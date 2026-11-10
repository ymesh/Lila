#include "ui/AnimProxyUi.h"

#include "core/Events.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/ProxySet.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/SplitMesh.h"
#include "ui/Actions.h"
#include "ui/MeshCache.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QElapsedTimer>
#include <QTimer>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>

namespace lila::ui
{
namespace
{
using scene::NodeId;
using scene::kNoNode;
namespace split = scene::split;
namespace proxy = scene::proxy;

constexpr const char* kSplitTool = "mesh.splitMeshTool";
constexpr const char* kProxyTool = "rig.animProxyTool";
constexpr const char* kHideHook = "lila.splitMeshPreview";

std::optional<std::vector<std::string>> redirectProxy(const selectionHandlers::Hit& h)
{
    if (h.node == kNoNode || selectionHandlers::selectAsBypassed())
        return std::nullopt;
    scene::Graph& g = scene::Graph::current();
    const NodeId joint = proxy::clickJoint(g, h.node);
    if (joint == kNoNode)
        return std::nullopt;
    return std::vector<std::string>{g.node(joint)->path()};
}

/** Distinct, data-display colours for the preview pieces (linear). */
glm::vec3 pieceColor(size_t i)
{
    static const glm::vec3 kPalette[] = {{0.85f, 0.32f, 0.22f}, {0.22f, 0.55f, 0.85f}, {0.35f, 0.75f, 0.30f}, {0.90f, 0.70f, 0.15f},
                                         {0.62f, 0.38f, 0.85f}, {0.20f, 0.75f, 0.72f}, {0.88f, 0.45f, 0.65f}, {0.55f, 0.55f, 0.55f}};
    return kPalette[i % std::size(kPalette)];
}

void leaveTool()
{
    // Not from inside the tool's own handler: set() destroys the tool.
    QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
}

json proxyNamesOf(const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        if (const scene::Node* n = scene::Graph::current().node(id))
            out.push_back(n->path());
    return out;
}

/** One split plane of the tool, with its mirror partner across the symmetry plane (or none). */
struct Guide
{
    split::Plane plane;
    int mirror = -1;
    /** Sits on the symmetry plane and is its own mirror image, so edits keep it there. */
    bool onSymmetry = false;
};

json guidesToJson(const std::vector<Guide>& guides)
{
    std::vector<split::Plane> planes;
    for (const Guide& gd : guides)
        planes.push_back(gd.plane);
    json out = split::planesToJson(planes);
    for (size_t i = 0; i < guides.size(); ++i)
    {
        if (guides[i].mirror >= 0)
            out[i]["mirror"] = guides[i].mirror;
        if (guides[i].onSymmetry)
            out[i]["onSymmetry"] = true;
    }
    return out;
}

std::vector<Guide> guidesFromJson(const json& j)
{
    std::vector<Guide> out;
    try
    {
        for (const split::Plane& p : split::planesFromJson(scene::Graph::current(), j))
            out.push_back({p});
    }
    catch (const std::exception&)
    {
        return {};
    }
    for (size_t i = 0; i < out.size(); ++i)
    {
        const int m = j[i].value("mirror", -1);
        out[i].mirror = m >= 0 && size_t(m) < out.size() && m != int(i) ? m : -1;
        out[i].onSymmetry = j[i].value("onSymmetry", false);
    }
    return out;
}

/** What the tool last made, so undoing it brings the tool back with these guides. */
struct Made
{
    NodeId mesh = kNoNode;
    bool animProxy = false;
    std::vector<Guide> guides;
};
std::optional<Made> gLastMade;
std::optional<Made> gRestore;
/** Guides per mesh, kept for the session so going back into the tool on a mesh picks them up. */
std::unordered_map<NodeId, std::vector<Guide>> gGuides;

bool madeExists(const Made& m)
{
    scene::Graph& g = scene::Graph::current();
    const scene::Node* n = g.node(m.mesh);
    if (!n)
        return false;
    if (m.animProxy)
        return !proxy::setsOfMesh(g, m.mesh).empty();
    return g.find(n->name() + "_piece1") != nullptr;
}

/** The mesh a selection means for the tool: a mesh, or the real mesh behind a selected proxy set or piece. */
NodeId toolTarget()
{
    scene::Graph& g = scene::Graph::current();
    for (NodeId id : g.selection())
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        const NodeId set = proxy::isSet(*n) ? id : proxy::setOfPiece(g, id);
        if (set != kNoNode)
            for (NodeId r : proxy::realOf(g, set))
                if (g.node(r)->type().isA("mesh"))
                    return r;
        if (n->type().isA("mesh") && set == kNoNode)
            return id;
    }
    return kNoNode;
}

double segmentPixels(QPointF p, QPointF a, QPointF b)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    const double t = len2 > 0.0 ? std::clamp(((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2, 0.0, 1.0) : 0.0;
    return std::hypot(p.x() - (a.x() + dx * t), p.y() - (a.y() + dy * t));
}

/** The plane's disc rim in view pixels. */
std::vector<QPointF> ringPixels(const split::Plane& pl, double radius, const ViewInfo& view)
{
    const glm::dvec3 u = glm::normalize(glm::cross(pl.normal, std::abs(pl.normal.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0)));
    const glm::dvec3 v = glm::cross(pl.normal, u);
    std::vector<QPointF> ring;
    for (int k = 0; k < 48; ++k)
    {
        const double a = 2.0 * 3.14159265358979 * k / 48.0;
        if (auto at = view.project(pl.origin + (u * std::cos(a) + v * std::sin(a)) * radius))
            ring.push_back(*at);
    }
    return ring;
}

constexpr double kRingPickPixels = 10.0;
constexpr double kCentrePickPixels = 12.0;

class SplitMeshTool;
SplitMeshTool* gActiveSplitTool = nullptr;

/**
 * Split Mesh: drag a line across the view to place a plane through it, edge on to the view, at the mesh's depth; the
 * pieces preview in colours as you go. Drag a plane's rim or centre to move it, Ctrl-drag to turn it, Ctrl-wheel over it to
 * size its disc. With symmetry on, each plane has a partner across the mesh's symmetry plane. Undo and redo step through
 * the plane edits first. Enter splits (or, as Anim Proxy, makes the proxy set).
 */
class SplitMeshTool final : public ViewportTool
{
public:
    explicit SplitMeshTool(bool animProxy) : mAnimProxy(animProxy) {}
    ~SplitMeshTool() override
    {
        if (gActiveSplitTool == this)
            gActiveSplitTool = nullptr;
    }

    const char* name() const override { return mAnimProxy ? "Anim Proxy" : "Split Mesh"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        scene::Graph& g = scene::Graph::current();
        if (gRestore)
        {
            mTarget = gRestore->mesh;
            mGuides = gRestore->guides;
            gRestore.reset();
        }
        else
            mTarget = toolTarget();
        if (!g.node(mTarget))
        {
            log::warn("{}: select a mesh first", name());
            leaveTool();
            return;
        }
        gActiveSplitTool = this;
        mSymmetry = split::symmetryPlane(g, mTarget);
        const geo::Bounds b = scene::worldBounds(g, mTarget);
        mTolerance = b.valid() ? glm::length(b.size()) * 0.02 : 1.0;
        if (mGuides.empty())
            mGuides = savedGuides();
        if (mGuides.empty() && scene::skin::find(g, mTarget) != kNoNode)
        {
            for (const split::Plane& p : split::suggestPlanes(g, mTarget))
                mGuides.push_back({p});
            linkSymmetry();
        }
        viewportHooks::setHideHook(kHideHook, [this](NodeId id) { return id == mTarget && !mPieces.empty(); });
        refresh();
    }

    void deactivate() override
    {
        if (gActiveSplitTool == this)
            gActiveSplitTool = nullptr;
        if (mTarget != kNoNode && !mGuides.empty())
            gGuides[mTarget] = mGuides;
        viewportHooks::removeHideHook(kHideHook);
        viewportTools::requestRedraw();
    }

    NodeId target() const { return mTarget; }

    std::vector<split::Plane> planes() const
    {
        std::vector<split::Plane> out;
        for (const Guide& gd : mGuides)
            out.push_back(gd.plane);
        return out;
    }

    /** The plane whose rim or centre is nearest to px within picking range, -1 for none. */
    int planeAt(QPointF px, const ViewInfo& view) const
    {
        int best = -1;
        double bestDist = 1e300;
        for (size_t i = 0; i < mGuides.size(); ++i)
        {
            double d = 1e300;
            if (auto at = view.project(mGuides[i].plane.origin))
                if (const double c = std::hypot(at->x() - px.x(), at->y() - px.y()); c <= kCentrePickPixels)
                    d = c;
            const std::vector<QPointF> ring = ringPixels(mGuides[i].plane, shownRadius(i), view);
            for (size_t k = 0; ring.size() > 2 && k < ring.size(); ++k)
                if (const double r = segmentPixels(px, ring[k], ring[(k + 1) % ring.size()]); r <= kRingPickPixels)
                    d = std::min(d, r);
            if (d < bestDist)
            {
                bestDist = d;
                best = int(i);
            }
        }
        return best;
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || mTarget == kNoNode)
            return false;
        mFrom = mTo = e->position();
        mView = view.camera;
        mSelected = planeAt(e->position(), view);
        mBefore = mGuides;
        if (mSelected >= 0)
        {
            mDrag = e->modifiers() & Qt::ControlModifier ? Drag::Rotate : Drag::Move;
            mStart = mGuides[size_t(mSelected)].plane;
        }
        else
            mDrag = Drag::Line;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mDrag == Drag::None)
        {
            // Hover lights the plane a press would take.
            if (const int h = planeAt(e->position(), view); h != mHover)
            {
                mHover = h;
                viewportTools::requestRedraw();
            }
            return false;
        }
        mTo = e->position();
        if (mDrag == Drag::Move || mDrag == Drag::Rotate)
            dragPlane(view);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || mDrag == Drag::None)
            return false;
        mTo = e->position();
        if (mDrag == Drag::Line && std::hypot(mTo.x() - mFrom.x(), mTo.y() - mFrom.y()) >= 4.0)
            addPlane(view);
        mDrag = Drag::None;
        if (guidesChanged(mBefore))
            pushUndo(std::move(mBefore));
        refresh();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo& view) override
    {
        // The plain wheel stays the camera's.
        if (!(e->modifiers() & Qt::ControlModifier))
            return false;
        const int at = planeAt(e->position(), view);
        if (at < 0)
            return false;
        // A run of wheel steps on one plane is one undo step.
        if (at != mWheelPlane || !mWheelClock.isValid() || mWheelClock.elapsed() > 700)
            pushUndo(mGuides);
        mWheelPlane = at;
        mWheelClock.restart();
        split::Plane& p = mGuides[size_t(at)].plane;
        const double r = p.radius > 0.0 ? p.radius : fitted(size_t(at));
        p.radius = std::max(0.01, r * std::pow(1.1, e->angleDelta().y() / 120.0));
        mSelected = at;
        propagate(size_t(at));
        refresh();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override { return key(e->key()); }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        // Plane edits undo first; with none left, undo falls through to the scene.
        if (action == "edit.undo")
            return undo();
        if (action == "edit.redo")
            return redo();
        if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier))
            return false;
        return key(e.key());
    }

    bool undo()
    {
        if (mUndo.empty())
            return false;
        mRedo.push_back(std::exchange(mGuides, std::move(mUndo.back())));
        mUndo.pop_back();
        mSelected = -1;
        mWheelPlane = -1;
        refresh();
        return true;
    }

    bool redo()
    {
        if (mRedo.empty())
            return false;
        mUndo.push_back(std::exchange(mGuides, std::move(mRedo.back())));
        mRedo.pop_back();
        mSelected = -1;
        refresh();
        return true;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        for (size_t i = 0; i < mPieces.size(); ++i)
            if (auto rm = renderMeshFor(mPieces[i]))
            {
                render::MeshDraw d;
                d.mesh = std::move(rm);
                d.baseColor = pieceColor(i);
                d.flags = render::DrawShaded | render::DrawWire;
                d.wireColor = {0.05f, 0.05f, 0.05f, 1.0f};
                f.meshes.push_back(std::move(d));
            }
        for (size_t i = 0; i < mGuides.size(); ++i)
            drawPlane(p, view, i);
        if (mDrag == Drag::Line && view.camera == mView)
            p.line(mFrom, mTo, 1.5f, {1.0f, 0.85f, 0.3f, 1.0f});
        drawToolStatus(p, view,
                       {.tool = name(),
                        .fields = {{"pieces", QString::number(mPieces.size())}},
                        .prompt = "drag across the mesh to add a plane",
                        .keys = {{"Rim-drag", "move"}, {"Ctrl-drag", "turn"}, {"Ctrl-wheel", "size"}, {"Delete", "remove"}, {"S", "suggest at joints"},
                                 {"X", "symmetry"}, {"Enter", mAnimProxy ? "make proxy" : "split"}, {"Esc", "cancel"}},
                        .symmetry = ToolSymmetry{mSymmetryOn}});
    }

private:
    enum class Drag
    {
        None,
        Line,
        Move,
        Rotate,
    };

    std::vector<Guide> savedGuides() const
    {
        if (auto it = gGuides.find(mTarget); it != gGuides.end())
            return it->second;
        scene::Graph& g = scene::Graph::current();
        for (NodeId s : proxy::setsOfMesh(g, mTarget))
            if (std::vector<Guide> out = guidesFromJson(proxy::planesOf(g, s)); !out.empty())
                return out;
        return {};
    }

    bool guidesChanged(const std::vector<Guide>& before) const
    {
        if (before.size() != mGuides.size())
            return true;
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i].plane.origin != mGuides[i].plane.origin || before[i].plane.normal != mGuides[i].plane.normal ||
                before[i].plane.radius != mGuides[i].plane.radius || before[i].mirror != mGuides[i].mirror)
                return true;
        return false;
    }

    void pushUndo(std::vector<Guide> before)
    {
        mUndo.push_back(std::move(before));
        mRedo.clear();
    }

    bool key(int k)
    {
        switch (k)
        {
        case Qt::Key_Escape: leaveTool(); return true;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (mSelected >= 0 && size_t(mSelected) < mGuides.size())
            {
                pushUndo(mGuides);
                removeGuide(size_t(mSelected));
                mSelected = -1;
                refresh();
            }
            return true;
        case Qt::Key_S:
            pushUndo(mGuides);
            for (const split::Plane& p : split::suggestPlanes(scene::Graph::current(), mTarget))
                mGuides.push_back({p});
            linkSymmetry();
            refresh();
            return true;
        case Qt::Key_X:
            mSymmetryOn = !mSymmetryOn;
            if (mSymmetryOn)
                linkSymmetry();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: confirm(); return true;
        default: return false;
        }
    }

    void removeGuide(size_t i)
    {
        std::vector<size_t> doomed{i};
        if (mGuides[i].mirror >= 0)
            doomed.push_back(size_t(mGuides[i].mirror));
        std::vector<int> remap(mGuides.size(), -1);
        std::vector<Guide> kept;
        for (size_t k = 0; k < mGuides.size(); ++k)
            if (std::ranges::find(doomed, k) == doomed.end())
            {
                remap[k] = int(kept.size());
                kept.push_back(mGuides[k]);
            }
        for (Guide& gd : kept)
            gd.mirror = gd.mirror >= 0 ? remap[size_t(gd.mirror)] : -1;
        mGuides = std::move(kept);
    }

    double symmetryDistance(const glm::dvec3& p) const { return glm::dot(p - mSymmetry.origin, mSymmetry.normal); }

    /** Pairs up unlinked guides that mirror each other, and pins those lying on the symmetry plane to it. */
    void linkSymmetry()
    {
        if (!mSymmetryOn)
            return;
        for (size_t i = 0; i < mGuides.size(); ++i)
        {
            if (mGuides[i].mirror >= 0 || mGuides[i].onSymmetry)
                continue;
            const split::Plane& a = mGuides[i].plane;
            if (std::abs(symmetryDistance(a.origin)) < mTolerance && std::abs(glm::dot(a.normal, mSymmetry.normal)) < 0.3)
            {
                mGuides[i].onSymmetry = true;
                propagate(i);
                continue;
            }
            if (std::abs(symmetryDistance(a.origin)) < mTolerance && std::abs(glm::dot(a.normal, mSymmetry.normal)) > 0.95)
                continue;
            const split::Plane want = split::reflect(a, mSymmetry);
            for (size_t j = 0; j < mGuides.size(); ++j)
                if (j != i && mGuides[j].mirror < 0 && !mGuides[j].onSymmetry &&
                    glm::length(mGuides[j].plane.origin - want.origin) < mTolerance * 2.0 && glm::dot(mGuides[j].plane.normal, want.normal) > 0.9)
                {
                    mGuides[i].mirror = int(j);
                    mGuides[j].mirror = int(i);
                    break;
                }
        }
    }

    /** Carries an edit of guide i to its partner, or keeps it on the symmetry plane. */
    void propagate(size_t i)
    {
        Guide& gd = mGuides[i];
        if (gd.onSymmetry)
        {
            gd.plane.origin -= symmetryDistance(gd.plane.origin) * mSymmetry.normal;
            const glm::dvec3 n = gd.plane.normal - glm::dot(gd.plane.normal, mSymmetry.normal) * mSymmetry.normal;
            if (glm::length(n) > 1e-6)
                gd.plane.normal = glm::normalize(n);
        }
        if (gd.mirror >= 0 && size_t(gd.mirror) < mGuides.size())
            mGuides[size_t(gd.mirror)].plane = split::reflect(gd.plane, mSymmetry);
    }

    void confirm()
    {
        scene::Graph& g = scene::Graph::current();
        const scene::Node* n = g.node(mTarget);
        if (!n || mGuides.empty())
            return;
        gGuides[mTarget] = mGuides;
        gLastMade = Made{mTarget, mAnimProxy, mGuides};
        actions::runWithOptions(mAnimProxy ? "animProxyFromMesh" : "splitMesh", {{"args", {n->path()}}, {"planes", guidesToJson(mGuides)}});
        leaveTool();
    }

    /** The world mesh split by the current planes, for the preview. */
    void refresh()
    {
        LILA_ZONE();

        mPieces.clear();
        mRadii.clear();
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        const scene::Node* n = g.node(mTarget);
        if (!n || mGuides.empty())
        {
            viewportTools::requestRedraw();
            return;
        }
        const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m)
            return;
        geo::Mesh world = *m;
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index p = 0; p < world.pointCapacity(); ++p)
            if (world.pointValid(p))
                world.setPosition(p, glm::dvec3(w * glm::dvec4(world.position(p), 1.0)));
        split::Result r = split::splitByPlanes(world, planes(), {.cap = false});
        mRadii = r.radii;
        for (geo::Mesh& piece : r.pieces)
            mPieces.push_back(std::make_shared<const geo::Mesh>(std::move(piece)));
        viewportTools::requestRedraw();
    }

    double fitted(size_t i) const { return i < mRadii.size() && mRadii[i] > 0.0 ? mRadii[i] : 10.0; }

    double shownRadius(size_t i) const { return mGuides[i].plane.radius > 0.0 ? mGuides[i].plane.radius : fitted(i); }

    void addPlane(const ViewInfo& view)
    {
        glm::dvec3 o1, d1, o2, d2;
        view.ray(mFrom, o1, d1);
        view.ray(mTo, o2, d2);
        const glm::dvec3 p1 = o1 + d1 * 100.0, p2 = o2 + d2 * 100.0;
        glm::dvec3 n = glm::cross(p2 - p1, d1);
        if (glm::length(n) < 1e-12)
            return;
        n = glm::normalize(n);
        // At the mesh's depth along the middle of the line, where the cut usually wants to be.
        glm::dvec3 om, dm;
        view.ray((mFrom + mTo) * 0.5, om, dm);
        const geo::Bounds b = scene::worldBounds(scene::Graph::current(), mTarget);
        const glm::dvec3 centre = b.valid() ? b.center() : om + dm * 100.0;
        const glm::dvec3 origin = om + dm * std::max(0.0, glm::dot(centre - om, dm));
        mGuides.push_back({{origin, n, 0.0}});
        const size_t i = mGuides.size() - 1;
        mSelected = int(i);
        if (!mSymmetryOn)
            return;
        if (std::abs(symmetryDistance(origin)) < mTolerance)
        {
            const double facing = std::abs(glm::dot(n, mSymmetry.normal));
            // Across the symmetry plane it is its own mirror image; lying in it, its mirror is itself.
            if (facing < 0.3)
            {
                mGuides[i].onSymmetry = true;
                propagate(i);
                return;
            }
            if (facing > 0.95)
                return;
        }
        mGuides.push_back({split::reflect(mGuides[i].plane, mSymmetry), int(i)});
        mGuides[i].mirror = int(mGuides.size()) - 1;
    }

    void dragPlane(const ViewInfo& view)
    {
        const size_t i = size_t(mSelected);
        split::Plane& p = mGuides[i].plane;
        if (mDrag == Drag::Move)
        {
            // Along the view plane through where the plane started.
            glm::dvec3 o0, d0, o1, d1;
            view.ray(mFrom, o0, d0);
            view.ray(mTo, o1, d1);
            const glm::dvec3 facing = view.viewDir(mStart.origin);
            auto onPlane = [&](const glm::dvec3& o, const glm::dvec3& d) {
                const double denom = glm::dot(d, facing);
                return std::abs(denom) < 1e-12 ? mStart.origin : o + d * (glm::dot(mStart.origin - o, facing) / denom);
            };
            p.origin = mStart.origin + onPlane(o1, d1) - onPlane(o0, d0);
        }
        else
        {
            // The normal tips toward the drag, about the view's own axes.
            const glm::dvec3 right = glm::dvec3(glm::inverse(view.view)[0]), up = glm::dvec3(glm::inverse(view.view)[1]);
            const double dx = glm::radians(mTo.x() - mFrom.x()) * 0.5, dy = glm::radians(mTo.y() - mFrom.y()) * 0.5;
            const glm::dmat4 r = glm::rotate(glm::dmat4(1.0), dx, up) * glm::rotate(glm::dmat4(1.0), dy, right);
            p.normal = glm::normalize(glm::dvec3(r * glm::dvec4(mStart.normal, 0.0)));
        }
        propagate(i);
    }

    void drawPlane(OverlayPainter& p, const ViewInfo& view, size_t i) const
    {
        const split::Plane& pl = mGuides[i].plane;
        const bool sel = int(i) == mSelected;
        const bool hot = int(i) == mHover && !sel;
        const glm::vec4 col = sel ? glm::vec4(1.0f, 0.85f, 0.3f, 1.0f) : hot ? glm::vec4(1.0f) : glm::vec4(0.85f, 0.85f, 0.85f, 0.8f);
        const double r = shownRadius(i);
        const std::vector<QPointF> ring = ringPixels(pl, r, view);
        if (ring.size() > 2)
            p.polyline(ring, sel || hot ? 2.5f : 1.5f, col, true);
        auto c = view.project(pl.origin);
        auto tip = view.project(pl.origin + pl.normal * r * 0.5);
        if (c && tip)
            p.line(*c, *tip, 1.5f, col);
        if (c)
            p.disc(*c, sel || hot ? 6.0f : 5.0f, col);
    }

    bool mAnimProxy = false;
    NodeId mTarget = kNoNode;
    std::vector<Guide> mGuides, mBefore;
    std::vector<std::vector<Guide>> mUndo, mRedo;
    std::vector<double> mRadii;
    std::vector<scene::MeshPtr> mPieces;
    split::Plane mSymmetry;
    bool mSymmetryOn = true;
    double mTolerance = 1.0;
    int mSelected = -1;
    int mHover = -1;
    int mWheelPlane = -1;
    QElapsedTimer mWheelClock;
    Drag mDrag = Drag::None;
    split::Plane mStart;
    QPointF mFrom, mTo;
    std::string mView;
};

/** Undoing what the tool made puts the tool back with its guides; redoing it while the tool is back leaves the tool. */
void watchUndo(const ExecuteEvent& e)
{
    if (!e.done || !e.ok || !gLastMade)
        return;
    if (e.name == "undo" && !gActiveSplitTool && !madeExists(*gLastMade))
    {
        gRestore = gLastMade;
        const char* tool = gLastMade->animProxy ? kProxyTool : kSplitTool;
        QTimer::singleShot(0, [tool] {
            if (gRestore)
                viewportTools::activate(tool);
        });
    }
    else if (e.name == "redo" && gActiveSplitTool && gActiveSplitTool->target() == gLastMade->mesh && madeExists(*gLastMade))
        leaveTool();
}

/** Selected joints, or the joints of the selected pieces. */
std::vector<NodeId> proxySelectedJoints()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        NodeId j = scene::rig::isJoint(*n) ? id : kNoNode;
        if (j == kNoNode)
            if (const NodeId s = proxy::setOfPiece(g, id); s != kNoNode)
                for (const proxy::Piece& p : proxy::pieces(g, s))
                    if (p.mesh == id)
                        j = p.joint;
        if (j != kNoNode && std::ranges::find(out, j) == out.end())
            out.push_back(j);
    }
    return out;
}

/** The selected joints' region between their true piece and the set's display. */
void togglePieceDisplay()
{
    scene::Graph& g = scene::Graph::current();
    for (NodeId j : proxySelectedJoints())
    {
        bool real = false;
        for (NodeId s : proxy::setsFor(g, j))
            for (const proxy::Piece& p : proxy::pieces(g, s))
                real |= p.joint == j && p.display == proxy::PieceDisplay::Real;
        try
        {
            ops::execute("setProxyDisplay", {{"piece", g.node(j)->path()}, {"pieceDisplay", real ? "follow" : "real"}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::warn("{}", e.what());
        }
    }
}
} // namespace

std::vector<scene::split::Plane> splitToolPlanes() { return gActiveSplitTool ? gActiveSplitTool->planes() : std::vector<scene::split::Plane>{}; }

int splitToolPlaneAt(QPointF px, const ViewInfo& view) { return gActiveSplitTool ? gActiveSplitTool->planeAt(px, view) : -1; }

void registerAnimProxyUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    ops::addExecuteListener(watchUndo);
    for (events::Event e : {events::Event::SceneNew, events::Event::SceneOpened})
        events::subscribe(e, [](const events::EventData&) {
            gGuides.clear();
            gLastMade.reset();
            gRestore.reset();
        });
    selectionHandlers::add({.id = "lila.proxySelect", .label = "Anim proxy piece selects its joint", .redirect = redirectProxy, .always = true});
    viewportTools::add({.id = kSplitTool, .label = "Split Mesh", .icon = "slice", .create = [] { return std::make_unique<SplitMeshTool>(false); }});
    viewportTools::add({.id = kProxyTool, .label = "Anim Proxy", .icon = "puzzle", .create = [] { return std::make_unique<SplitMeshTool>(true); }});

    actions::add({.id = kSplitTool,
                  .label = "Split Mesh Tool",
                  .category = "Mesh",
                  .run = [] { viewportTools::activate(kSplitTool); },
                  .doc = "Places split planes on the selected mesh, previews the pieces in colour, and cuts it into them on Enter.",
                  .python = "lila.ui.run_action(\"mesh.splitMeshTool\")"});
    actions::add({.id = "mesh.splitMesh",
                  .label = "Split Mesh",
                  .category = "Mesh",
                  .doc = "Cuts the selected skinned mesh into pieces at its joint boundaries (planes in the option box).",
                  .python = "splitMesh(suggest=True)",
                  .command = "splitMesh",
                  .call = {{"suggest", true}}});
    actions::add({.id = kProxyTool,
                  .label = "Anim Proxy Tool",
                  .category = "Rig",
                  .run = [] { viewportTools::activate(kProxyTool); },
                  .doc = "Places split planes on the selected skinned mesh (starting at its joint boundaries), previews the pieces, and "
                         "makes them an anim proxy on Enter.",
                  .python = "lila.ui.run_action(\"rig.animProxyTool\")"});
    actions::add({.id = "rig.createAnimProxy",
                  .label = "Create Anim Proxy",
                  .category = "Rig",
                  .doc = "Splits the selected skinned mesh at its joint boundaries, remeshes each piece and binds it to its joint as an "
                         "anim proxy. Remesh settings are in the option box.",
                  .python = "animProxyFromMesh()",
                  .command = "animProxyFromMesh"});
    actions::add({.id = "rig.createProxySet",
                  .label = "Create Empty Proxy Set",
                  .category = "Rig",
                  .doc = "Makes an empty proxy set for the selected meshes, to add hand-made proxies to.",
                  .python = "createProxySet()",
                  .command = "createProxySet"});
    actions::add({.id = "rig.addToProxySet",
                  .label = "Add Selected to Proxy Set",
                  .category = "Rig",
                  .doc = "Makes the selected meshes pieces of the selected proxy set, each following its nearest bone.",
                  .python = "addToProxySet()",
                  .command = "addToProxySet"});
    actions::add({.id = "rig.removeFromProxySet",
                  .label = "Remove from Proxy Set",
                  .category = "Rig",
                  .doc = "Stops the selected meshes being anim proxy pieces.",
                  .python = "removeFromProxySet()",
                  .command = "removeFromProxySet"});
    actions::add({.id = "rig.toggleAnimProxy",
                  .label = "Toggle Proxy/Real",
                  .category = "Rig",
                  .run =
                      [] {
                          try
                          {
                              scene::Graph& g = scene::Graph::current();
                              ops::execute("setProxyDisplay", {{"args", proxyNamesOf(g.selection())}}, CallSource::UI);
                          }
                          catch (const CommandError& e)
                          {
                              log::warn("{}", e.what());
                          }
                      },
                  .doc = "Shows the anim proxy of the selected character in place of its real meshes, or the real meshes again.",
                  .python = "setProxyDisplay()"});
    actions::add({.id = "rig.toggleProxyPiece",
                  .label = "Toggle Real Piece",
                  .category = "Rig",
                  .run = togglePieceDisplay,
                  .doc = "Shows the true (real surface) piece of the selected joints while the rest stays proxy, for facial animation on "
                         "the real head; again to follow the set.",
                  .python = "setProxyDisplay(piece=\"head\", pieceDisplay=\"real\")"});
}
} // namespace lila::ui
