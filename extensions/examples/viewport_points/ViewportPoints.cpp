// One of each viewport extension point, each registered through the Context so unloading removes it: a node type with
// its own draw override, a gizmo provider, a cursor badge with its own PNG icon, a selection handler, a render pass
// and a viewport tool that places the node type.

#include <lila/Command.h>
#include <lila/CursorBadges.h>
#include <lila/Extension.h>
#include <lila/Gizmo.h>
#include <lila/Graph.h>
#include <lila/Node.h>
#include <lila/Relocate.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QMouseEvent>

#include <cmath>
#include <optional>

using namespace lila;
using namespace lila::ui;

namespace
{
constexpr const char* kMarker = "viewportPointsMarker";
constexpr const char* kTool = "viewportPoints.place";
constexpr const char* kToolName = "viewportPointsPlace";

double markerSize(const ViewInfo& view, const glm::dvec3& at) { return 8.0 * view.unitsPerPixel(at); }

/** A diamond facing the camera, a constant 8 px across. */
void drawMarker(drawOverrides::DrawContext& c)
{
    glm::dvec3 p(c.world[3]);
    double r = markerSize(c.view, p);
    glm::dvec3 right(c.view.view[0][0], c.view.view[1][0], c.view.view[2][0]), up(c.view.view[0][1], c.view.view[1][1], c.view.view[2][1]);
    glm::dvec3 pts[4] = {p + right * r, p + up * r, p - right * r, p - up * r};
    for (int k = 0; k < 4; ++k)
    {
        c.frame.lines.push_back({glm::dvec3(pts[k]), c.color});
        c.frame.lines.push_back({glm::dvec3(pts[(k + 1) % 4]), c.color});
    }
}

std::optional<double> pickMarker(const drawOverrides::PickContext& c)
{
    std::optional<QPointF> at = c.view.project(glm::dvec3(c.world[3]));
    if (!at)
        return std::nullopt;
    if (!c.click)
        return QRectF(c.a, c.b).normalized().contains(*at) ? std::optional<double>(0.0) : std::nullopt;
    return std::max(0.0, std::hypot(at->x() - c.a.x(), at->y() - c.a.y()) - 8.0);
}

/** Where a ray meets the ground (y = 0), or nullopt when it runs parallel or away. */
std::optional<glm::dvec3> ground(const ViewInfo& view, QPointF px)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    if (std::abs(d.y) < 1e-9 || -o.y / d.y < 0.0)
        return std::nullopt;
    return o + d * (-o.y / d.y);
}

/** Click to place a marker on the ground; Esc leaves. */
class PlaceTool final : public ViewportTool
{
public:
    const char* name() const override { return kToolName; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (std::optional<glm::dvec3> p = ground(view, e->position()))
        {
            UndoChunk chunk("Place Marker");
            json made = ops::execute("createNode", {{"args", {kMarker}}}, CallSource::UI);
            ops::execute("move", {{"args", {made}}, {"to", {p->x, p->y, p->z}}}, CallSource::UI);
        }
        return true;
    }
    bool mouseRelease(QMouseEvent*, const ViewInfo&) override { return true; }
    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape)
            return false;
        viewportTools::set(nullptr);
        return true;
    }
};

/**
 * A move-only gizmo: a square on the last selected node that drags it across the ground plane, committed as one
 * move command on release.
 */
class GroundGizmo final : public Gizmo
{
public:
    void setView(const ViewInfo& v) override { mView = v; }
    bool supports(std::string_view mode) const override { return mode == "move"; }

    bool visible() override
    {
        scene::Graph& g = scene::Graph::current();
        mTarget.clear();
        if (g.selection().empty())
            return false;
        scene::Node* n = g.node(g.selection().back());
        int world = n ? n->type().attrIndex("worldMatrix") : -1;
        if (world < 0)
            return false;
        mTarget = n->path();
        mPivot = glm::dvec3(n->as<glm::dmat4>(world)[3]);
        return true;
    }

    void draw(OverlayPainter& p) override
    {
        if (!mDrag && !visible())
            return;
        std::optional<QPointF> c = mView.project(mPivot + mDelta);
        if (!c)
            return;
        float shade = mDrag || mHover ? 1.0f : 0.7f;
        p.rect(QRectF(c->x() - 7, c->y() - 7, 14, 14), {shade, shade, shade, 0.9f});
        if (mDrag)
            p.label(*c + QPointF(14, -10), readout());
    }

    bool hover(QPointF px) override
    {
        bool over = !mDrag && visible() && near(px);
        bool changed = over != mHover;
        mHover = over;
        return changed;
    }

    bool press(QPointF px, Qt::MouseButton button, Qt::KeyboardModifiers) override
    {
        if (button != Qt::LeftButton || !visible() || !near(px))
            return false;
        mStart = ground(mView, px);
        mDrag = mStart.has_value();
        mDelta = glm::dvec3(0.0);
        return mDrag;
    }

    void drag(QPointF px, Qt::KeyboardModifiers) override
    {
        if (std::optional<glm::dvec3> now = mDrag ? ground(mView, px) : std::nullopt)
            mDelta = *now - *mStart;
    }

    void release() override
    {
        if (!mDrag)
            return;
        mDrag = false;
        if (glm::length(mDelta) > 1e-9)
            ops::execute("move", {{"args", {mTarget}}, {"by", {mDelta.x, mDelta.y, mDelta.z}}}, CallSource::UI);
        mDelta = glm::dvec3(0.0);
    }

    void cancel() override
    {
        mDrag = false;
        mDelta = glm::dvec3(0.0);
    }

    bool dragging() const override { return mDrag; }
    bool hovering() const override { return mHover; }
    QString readout() const override { return mDrag ? QString("%1 cm").arg(glm::length(mDelta), 0, 'f', 2) : QString(); }

private:
    bool near(QPointF px) const
    {
        std::optional<QPointF> c = mView.project(mPivot);
        return c && std::abs(c->x() - px.x()) < 10 && std::abs(c->y() - px.y()) < 10;
    }

    ViewInfo mView;
    std::string mTarget;
    glm::dvec3 mPivot{0.0};
    glm::dvec3 mDelta{0.0};
    std::optional<glm::dvec3> mStart;
    bool mDrag = false;
    bool mHover = false;
};
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    const scene::NodeType* locator = scene::types::find("locator");
    if (!locator)
        return 1;
    if (!scene::types::registerType(ctx, {.name = kMarker,
                                          .base = locator,
                                          .dag = true,
                                          .doc = "A point drawn as a camera-facing diamond: the viewport points example's node type.",
                                          .icon = "point",
                                          .create = locator->create}))
        return 1;

    lila::scene::relocate::registerNeutral(kMarker);
    drawOverrides::add(ctx, {.id = "viewportPoints.marker", .nodeType = kMarker, .draw = drawMarker, .pick = pickMarker});

    gizmos::add(ctx, {.id = "viewportPoints.ground", .label = "Ground Move", .create = [] { return std::make_unique<GroundGizmo>(); }});

    viewportTools::add(ctx, {.id = kTool, .label = "Place Marker", .icon = "point", .create = [] { return std::make_unique<PlaceTool>(); }});

    overlayIcons::add(ctx, "viewportPoints.icon", {.file = QString::fromStdString((ctx.resources() / "marker.png").generic_string())});
    cursorBadges::add(ctx, {.id = "viewportPoints.placing",
                            .icon = "viewportPoints.icon",
                            .active = [](const ViewportState& v) { return v.tool == kToolName; },
                            .message = "Click the ground to place a marker"});

    // A click on "<name>_proxy" selects <name>; hovering a mesh shades the face under the cursor.
    selectionHandlers::add(ctx, {.id = "viewportPoints.proxy",
                                 .label = "Proxy to target",
                                 .redirect = [](const selectionHandlers::Hit& h) -> std::optional<std::vector<std::string>> {
                                     const std::string suffix = "_proxy";
                                     if (h.name.size() <= suffix.size() || h.name.compare(h.name.size() - suffix.size(), suffix.size(), suffix) != 0)
                                         return std::nullopt;
                                     return std::vector<std::string>{h.name.substr(0, h.name.size() - suffix.size())};
                                 },
                                 .hover = [](const selectionHandlers::Hit& h) -> std::optional<selectionHandlers::Highlight> {
                                     if (h.face == selectionHandlers::kNone)
                                         return std::nullopt;
                                     selectionHandlers::Highlight out;
                                     out.mesh = h.node;
                                     out.weights.assign(h.face + 1, 0.0f);
                                     out.weights[h.face] = 1.0f;
                                     return out;
                                 }});

    // While placing, a crosshair at the view's centre.
    renderPasses::add(ctx, {.id = "viewportPoints.crosshair", .stage = renderPasses::Stage::Overlay, .run = [](renderPasses::Context& c) {
                                if (viewportTools::activeId() != kTool)
                                    return;
                                QPointF m(c.view.width / 2.0, c.view.height / 2.0);
                                const glm::vec4 ink = c.painter.ink(0.85f);
                                c.painter.line(m - QPointF(10, 0), m + QPointF(10, 0), 1.0f, ink);
                                c.painter.line(m - QPointF(0, 10), m + QPointF(0, 10), 1.0f, ink);
                            }});
    return 0;
}
