// Draw Channel, Draw Dam and Draw Airstrip: click on the terrain to lay a channel's line or a dam's or runway's two ends,
// the way the curve tools place points; the command runs with the option box's remembered settings.

#include "Analysis.h"
#include "Coast.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/ExtensionScope.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Transform.h>
#include <lila/ViewportTool.h>

#include <QTimer>

#include <cmath>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
namespace coast_tools
{
using json = nlohmann::json;

struct Surface
{
    std::string mesh;
    glm::dmat4 world{1.0};
    std::shared_ptr<const Heightfield> h;
};

/** Terrains to draw on: the selected ones, else every terrain in the scene. */
std::vector<Surface> surfaces()
{
    scene::Graph& g = scene::Graph::current();
    auto of = [&](scene::NodeId id) -> std::optional<Surface> {
        scene::Node* n = g.node(id);
        if (n && gn::isGnModifier(*n))
            if (scene::NodeId m = scene::modifiers::meshOf(g, id); m != scene::kNoNode)
                n = g.node(m);
        if (!n || !n->type().isA("mesh"))
            return std::nullopt;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (scene::Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()); h && !h->empty())
                    return Surface{n->path(), n->as<glm::dmat4>(scene::Transform::kWorldMatrix), std::move(h)};
        return std::nullopt;
    };
    std::vector<Surface> out;
    for (scene::NodeId id : g.selection())
        if (auto s = of(id))
            out.push_back(std::move(*s));
    if (out.empty())
        for (scene::NodeId id : g.nodes())
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                if (auto s = of(id))
                    out.push_back(std::move(*s));
    return out;
}

/** The nearest terrain point under px, and which terrain it is on. */
std::optional<glm::dvec3> pick(const std::vector<Surface>& on, QPointF px, const ViewInfo& view, std::string* mesh)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    std::optional<glm::dvec3> best;
    double bestD = 1e300;
    for (const Surface& s : on)
    {
        const glm::dmat4 inv = glm::inverse(s.world);
        glm::dvec3 at;
        if (!raycast(*s.h, glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), &at))
            continue;
        const glm::dvec3 w(s.world * glm::dvec4(at, 1.0));
        if (const double dist = glm::length(w - o); dist < bestD)
        {
            bestD = dist;
            best = w;
            if (mesh)
                *mesh = s.mesh;
        }
    }
    return best;
}

QString lengthText(double cm) { return cm >= 100000.0 ? QString("%1 km").arg(cm / 100000.0, 0, 'f', 2) : QString("%1 m").arg(cm / 100.0, 0, 'f', 0); }

enum class Kind
{
    Channel,
    Dam,
    Airstrip,
};

const glm::vec4 kLine(0.95f, 0.95f, 0.95f, 0.95f), kGhost(0.95f, 0.95f, 0.95f, 0.45f), kRim(0.05f, 0.05f, 0.05f, 0.9f);
const glm::vec4 kWarn(1.0f, 0.62f, 0.2f, 1.0f);

/** Click points on the terrain: a channel's line (Enter finishes), or two ends (the second click makes the dam or runway). */
class DrawTool final : public ViewportTool
{
public:
    explicit DrawTool(Kind k, ext::Context* ctx) : mKind(k), mCtx(ctx) {}

    const char* name() const override { return mKind == Kind::Channel ? "Draw Channel" : mKind == Kind::Dam ? "Draw Dam" : "Draw Airstrip"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void activate() override { mSurfaces = surfaces(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mSurfaces = surfaces();
        std::string mesh;
        const auto p = pick(mSurfaces, e->position(), view, &mesh);
        if (!p)
        {
            mNote = mSurfaces.empty() ? "no terrain in the scene" : "click on the terrain";
            viewportTools::requestRedraw();
            return true;
        }
        if (mPts.empty())
            mMesh = mesh;
        mNote.clear();
        mPts.push_back(*p);
        if (mKind != Kind::Channel && mPts.size() == 2)
            finish();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = pick(mSurfaces, e->position(), view, nullptr);
        mHoverView = view.camera;
        viewportTools::requestRedraw();
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Return:
        case Qt::Key_Enter: finish(); return true;
        case Qt::Key_Backspace: undoPoint(); return true;
        case Qt::Key_Escape:
            if (mPts.empty())
                exit();
            mPts.clear();
            viewportTools::requestRedraw();
            return true;
        default: return false;
        }
    }

    bool hotkey(const QKeyEvent&, std::string_view action) override
    {
        if (action != "edit.undo" || mPts.empty())
            return false;
        undoPoint();
        return true;
    }

    bool cancelStep() override
    {
        if (mPts.empty())
            return false;
        mPts.clear();
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        std::vector<glm::dvec3> pts = mPts;
        const bool hover = mHover && view.camera == mHoverView;
        if (hover)
            pts.push_back(*mHover);
        for (size_t i = 1; i < pts.size(); ++i)
        {
            const glm::vec4& c = i < mPts.size() ? kLine : kGhost;
            f.linesOnTop.push_back({pts[i - 1], c});
            f.linesOnTop.push_back({pts[i], c});
        }
        for (const glm::dvec3& q : mPts)
            if (auto s = view.project(q))
            {
                p.disc(*s, 5.5f, kRim, 16);
                p.disc(*s, 4.0f, kLine, 16);
            }
        if (hover)
            if (auto s = view.project(*mHover))
                p.disc(*s, 3.0f, kGhost, 16);

        double length = 0.0;
        for (size_t i = 1; i < pts.size(); ++i)
            length += glm::length(pts[i] - pts[i - 1]);
        const json opts = ops::options::values(command());
        ToolStatus st{.tool = name(), .subject = QString::fromStdString(mMesh.empty() ? std::string() : mMesh.substr(mMesh.rfind('|') + 1))};
        std::optional<QString> warning;
        if (mKind == Kind::Channel)
        {
            st.mode = QString::fromStdString(opts.value("craft", std::string("skiff")));
            st.prompt = mPts.empty() ? "click the channel's start" : "click along the water";
            st.keys = {{"Enter", "finish"}};
        }
        else if (mKind == Kind::Dam)
        {
            st.prompt = mPts.empty() ? "click one end of the dam" : "click the other end";
        }
        else
        {
            const std::string aircraft = opts.value("aircraft", std::string("light"));
            st.mode = QString::fromStdString(aircraft);
            st.prompt = mPts.empty() ? "click one end of the runway" : "click the other end";
            if (const CraftClass* c = craftClass(aircraft); c && pts.size() >= 2)
            {
                st.fields.push_back({"needs", lengthText(c->runway)});
                if (length < c->runway)
                    warning = QString("Runway shorter than a %1 aircraft needs").arg(QString::fromStdString(aircraft));
            }
        }
        if (pts.size() >= 2)
            st.fields.insert(st.fields.begin(), {"length", lengthText(length)});
        st.keys.push_back({"Z", "undo point"});
        st.keys.push_back({"Esc", mPts.empty() ? "exit" : "clear"});
        drawToolStatus(p, view, st);
        if (!mNote.empty())
            drawToolMessage(p, view, QString::fromStdString(mNote), kWarn);
        else if (warning)
            drawToolMessage(p, view, *warning, kWarn);
    }

private:
    const char* command() const { return mKind == Kind::Channel ? "terrainAddChannel" : mKind == Kind::Dam ? "terrainAddReservoir" : "terrainAddAirstrip"; }

    void undoPoint()
    {
        if (!mPts.empty())
            mPts.pop_back();
        viewportTools::requestRedraw();
    }

    void exit()
    {
        // Not from inside the tool's own handler: set() destroys the tool.
        QTimer::singleShot(0, mCtx ? ui::scope(*mCtx) : nullptr, [] { viewportTools::set(nullptr); });
    }

    void finish()
    {
        if (mPts.size() < 2)
            return;
        json call = {{"args", {mMesh}}};
        auto pt = [](const glm::dvec3& w) { return json::array({w.x, w.y, w.z}); };
        if (mKind == Kind::Channel)
        {
            json path = json::array();
            for (const glm::dvec3& q : mPts)
                path.push_back(pt(q));
            call["path"] = path;
        }
        else
        {
            call["a"] = pt(mPts[0]);
            call["b"] = pt(mPts[1]);
        }
        mPts.clear();
        try
        {
            const json r = ops::execute(command(), ops::options::callWith(command(), call));
            log::info("{}: {}", name(), r.is_object() ? r.value("id", std::string()) : std::string());
        }
        catch (const std::exception& e)
        {
            mNote = e.what();
            log::error("{}", e.what());
        }
        viewportTools::requestRedraw();
    }

    Kind mKind;
    ext::Context* mCtx;
    std::vector<Surface> mSurfaces;
    std::vector<glm::dvec3> mPts;
    std::string mMesh;
    std::optional<glm::dvec3> mHover;
    std::string mHoverView;
    std::string mNote;
};
} // namespace coast_tools

void registerCoastTools(ext::Context& ctx)
{
    using namespace coast_tools;
    ext::Context* c = &ctx;
    viewportTools::add(ctx, {.id = "terrain.drawChannel", .label = "Draw Channel", .icon = "git-branch", .create = [c] { return std::make_unique<DrawTool>(Kind::Channel, c); }});
    viewportTools::add(ctx, {.id = "terrain.drawDam", .label = "Draw Dam", .icon = "wall", .create = [c] { return std::make_unique<DrawTool>(Kind::Dam, c); }});
    viewportTools::add(ctx, {.id = "terrain.drawAirstrip", .label = "Draw Airstrip", .icon = "plane", .create = [c] { return std::make_unique<DrawTool>(Kind::Airstrip, c); }});
}
} // namespace terrain
