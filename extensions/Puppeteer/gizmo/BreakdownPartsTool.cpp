// Breakdown Parts: brush over a character's mesh to set how much of the breakdown slider's weight each part takes. The
// part under the brush is the joint that dominates the face there, with its children.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Mesh.h>
#include <lila/Rig.h>
#include <lila/UI.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
const glm::vec4 kText{0.95f, 0.95f, 0.95f, 1.0f};
const glm::vec4 kDot{0.95f, 0.95f, 0.95f, 0.85f};

/** The joint dominating the skinned mesh face under px, or kNoNode. */
NodeId jointUnder(QPointF px, const ViewInfo& view)
{
    const auto hit = surfaceSnap::underCursor(px, view);
    if (!hit)
        return kNoNode;
    Graph& g = Graph::current();
    Node* n = g.node(hit->node);
    if (!n || !n->type().isA("mesh"))
        return kNoNode;
    const PlugValue v = g.value({hit->node, n->type().attrIndex("outMesh"), -1});
    const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
    if (!mesh || !*mesh)
        return kNoNode;
    const glm::dvec3 local(glm::inverse(worldOf(g, hit->node)) * glm::dvec4(hit->position, 1.0));
    uint32_t best = selectionHandlers::kNone;
    double bestD = 0.0;
    for (geo::Index f = 0; f < (*mesh)->faceCapacity(); ++f)
        if ((*mesh)->faceValid(f))
            if (const double d = glm::length((*mesh)->faceCentroid(f) - local); best == selectionHandlers::kNone || d < bestD)
            {
                best = f;
                bestD = d;
            }
    if (best == selectionHandlers::kNone)
        return kNoNode;
    const NodeId j = rig::dominantJoint(g, hit->node, best);
    return j && characterFor(g, j) ? j : kNoNode;
}

std::map<std::string, double> partsNow()
{
    std::map<std::string, double> out;
    try
    {
        const json parts = ops::execute("stringsBreakdownParts", json::object());
        for (const auto& [joint, f] : parts.items())
            out[joint] = f.get<double>();
    }
    catch (const std::exception&)
    {
    }
    return out;
}

class BreakdownPartsTool final : public ViewportTool
{
public:
    const char* name() const override { return "Breakdown Parts"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }
    void deactivate() override { viewportTools::requestRedraw(); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mPainting = true;
        mClearing = e->modifiers() & Qt::ControlModifier;
        mDone.clear();
        dab(e->position(), view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = jointUnder(e->position(), view);
        if (mPainting)
            dab(e->position(), view);
        viewportTools::requestRedraw();
        return mPainting;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mPainting)
            return false;
        mPainting = false;
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ControlModifier))
            return false;
        mFactor = std::clamp(std::round((mFactor + (e->angleDelta().y() > 0 ? 0.1 : -0.1)) * 10.0) / 10.0, 0.0, 1.0);
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        Graph& g = Graph::current();
        for (const auto& [path, f] : partsNow())
            if (Node* n = g.find(path))
                if (const auto at = view.project(glm::dvec3(worldOf(g, n->id())[3])))
                {
                    p.disc(*at, 4.0f, kDot, 12);
                    p.label(*at + QPointF(7.0, -7.0), percent(f), kText);
                }
        QString part = "none";
        if (Node* h = g.node(mHover))
            part = QString::fromStdString(h->name());
        drawToolStatus(p, view,
                       ToolStatus{.tool = "Breakdown Parts",
                                  .subject = part,
                                  .fields = {{"weight", percent(mFactor)}},
                                  .prompt = "brush over the mesh: the part under it takes this share of the breakdown",
                                  .keys = {{"Ctrl+wheel", "weight"}, {"Ctrl+drag", "clear"}, {"Esc", "exit"}}});
    }

private:
    static QString percent(double f) { return QString("%1%").arg(int(std::lround(f * 100.0))); }

    void dab(QPointF px, const ViewInfo& view)
    {
        const NodeId j = jointUnder(px, view);
        if (!j || std::find(mDone.begin(), mDone.end(), j) != mDone.end())
            return;
        mDone.push_back(j);
        const std::string path = Graph::current().node(j)->path();
        runCommand("stringsBreakdownParts", mClearing ? json{{"args", {path}}, {"remove", true}} : json{{"args", {path}}, {"factor", mFactor}});
    }

    double mFactor = 0.0;
    bool mPainting = false, mClearing = false;
    std::vector<NodeId> mDone; // parts set this stroke, so a stroke sets each once
    NodeId mHover = kNoNode;
};
} // namespace

void registerBreakdownPartsTool(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = "puppeteer.breakdownParts", .label = "Breakdown Parts", .icon = "brush",
                             .create = [] { return std::make_unique<BreakdownPartsTool>(); }});
    actions::add(ctx, {.id = "puppeteer.strings.breakdownParts",
                       .label = "Breakdown Parts",
                       .category = "Puppeteer Strings",
                       .run = [] { viewportTools::activate("puppeteer.breakdownParts"); },
                       .doc = "Brush over a character's mesh to set how much of the breakdown slider each part takes: 0% holds the "
                              "part in its pose, 100% follows the slider. Ctrl + wheel sets the weight, Ctrl + drag clears.",
                       .icon = "brush",
                       .keywords = {"breakdown", "tween", "per part", "override", "mask", "paint"}});
    actions::add(ctx, {.id = "puppeteer.strings.clearBreakdownParts",
                       .label = "Clear Breakdown Parts",
                       .category = "Puppeteer Strings",
                       .doc = "Every part follows the breakdown slider again.",
                       .command = "stringsBreakdownParts",
                       .call = {{"remove", true}},
                       .icon = "eraser"});
}
} // namespace puppeteer
