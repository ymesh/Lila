// Reverse foot and IK controllers in the viewport: a sole loop under each reverse foot and a ring around each IK goal
// without one, red while the key there is locked. Pinned joints get a red diamond. Clicking one selects its controller; right-clicking it (through the
// Spinner) locks or unlocks the key.

#include "gizmo/ControllerDraw.h"
#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/UI.h>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
constexpr int kSegments = 40;
// Data colours (linear): locked is a state worth seeing at a glance.
const glm::vec4 kLocked{0.85f, 0.06f, 0.04f, 1.0f};
const glm::vec4 kPlain{0.62f, 0.62f, 0.62f, 1.0f};

glm::dvec3 v3(Graph& g, const Node& n, const char* name) { return std::get<glm::dvec3>(g.value(plugOf(n, name))); }

double numberOr(const Node& n, const char* name, double fallback)
{
    const int a = n.type().attrIndex(name);
    return a >= 0 ? n.as<double>(a) : fallback;
}

/** The node whose drawThickness and drawRadius a chain's controller takes: its reverse foot when it has one. */
const Node* drawSettingsOf(Graph& g, NodeId chain)
{
    const NodeId f = footForChain(g, chain);
    return g.node(f ? f : chain);
}

double thicknessOf(Graph& g, NodeId chain)
{
    const Node* n = drawSettingsOf(g, chain);
    return n ? std::max(numberOr(*n, "drawThickness", 4.0), 1.0) : 4.0;
}

/** A closed loop as a camera-facing ribbon, widthPx wide on screen, mitred so the segments meet without gaps. */
void drawLoop(drawOverrides::DrawContext& c, const std::vector<glm::dvec3>& pts, const glm::vec4& color, double widthPx)
{
    const size_t n = pts.size();
    if (n < 2)
        return;
    const glm::dmat4& v = c.view.view;
    const glm::dvec3 fwd = -glm::dvec3(v[0][2], v[1][2], v[2][2]);
    std::vector<glm::dvec3> side(n);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 s = glm::cross(pts[(i + 1) % n] - pts[(i + n - 1) % n], fwd);
        side[i] = glm::length(s) > 1e-12 ? glm::normalize(s) * (0.5 * widthPx * c.view.unitsPerPixel(pts[i])) : glm::dvec3(0.0);
    }
    for (size_t i = 0; i < n; ++i)
    {
        const size_t j = (i + 1) % n;
        for (const glm::dvec3& p : {pts[i] - side[i], pts[j] - side[j], pts[j] + side[j], pts[i] - side[i], pts[j] + side[j], pts[i] + side[i]})
            c.frame.tris3d.push_back({glm::dvec3(p), color});
    }
}

glm::vec4 colorFor(Graph& g, NodeId chain, const drawOverrides::DrawContext& c)
{
    if (chainLocked(g, chain, g.time()))
        return kLocked;
    return c.highlight == drawOverrides::Highlight::Selected || c.highlight == drawOverrides::Highlight::Preselected ? c.color : kPlain;
}

double segmentDistance(QPointF p, QPointF a, QPointF b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    const double t = len2 > 0.0 ? std::clamp(((p - a).x() * ab.x() + (p - a).y() * ab.y()) / len2, 0.0, 1.0) : 0.0;
    const QPointF q = a + ab * t - p;
    return std::hypot(q.x(), q.y());
}

/** Closest approach in pixels of the projected loop to px; nullopt when none of it projects. */
std::optional<double> loopDistance(const ViewInfo& view, const std::vector<glm::dvec3>& pts, QPointF px)
{
    std::optional<double> best;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const auto a = view.project(pts[i]), b = view.project(pts[(i + 1) % pts.size()]);
        if (!a || !b)
            continue;
        const double d = segmentDistance(px, *a, *b);
        if (!best || d < *best)
            best = d;
    }
    return best;
}

/** Pixels from px to the drawn edge of the controller's line, 0 on it. */
std::optional<double> controllerDistance(Graph& g, const ViewInfo& view, NodeId chain, QPointF px)
{
    const auto d = loopDistance(view, controllerOutline(g, chain), px);
    return d ? std::optional(std::max(*d - 0.5 * thicknessOf(g, chain), 0.0)) : std::nullopt;
}

std::optional<double> pickChain(const drawOverrides::PickContext& c, NodeId chain)
{
    Graph& g = Graph::current();
    if (!c.click)
    {
        const QRectF box = QRectF(c.a, c.b).normalized();
        for (const glm::dvec3& p : controllerOutline(g, chain))
            if (auto s = c.view.project(p); s && box.contains(*s))
                return 0.0;
        return std::nullopt;
    }
    return controllerDistance(g, c.view, chain, c.a);
}

/** Rig mode shows where the foot pivots, so they can be checked against the sole. */
void drawPivots(drawOverrides::DrawContext& c, Graph& g, const Node& foot)
{
    if (modes::current() != kRigMode)
        return;
    const NodeId chain = chainFrom(g, foot.id());
    const glm::dmat4 control = chain ? lila::scene::rigel::controllerWorld(g, chain) : worldOf(g, foot.id());
    for (const char* name : {"heelPivot", "ballPivot", "toePivot", "innerPivot", "outerPivot"})
    {
        const glm::dvec3 p(control * glm::dvec4(v3(g, foot, name), 1.0));
        const double r = 4.0 * c.view.unitsPerPixel(p);
        for (int k = 0; k < 3; ++k)
        {
            glm::dvec3 d(0.0);
            d[k] = r;
            c.frame.linesOnTop.push_back({glm::dvec3(p - d), kPlain});
            c.frame.linesOnTop.push_back({glm::dvec3(p + d), kPlain});
        }
    }
}
/** A diamond around each joint full-body moves hold, in the same red as a locked controller. */
void drawPins(render::FrameState& frame, const ViewInfo& view)
{
    Graph& g = Graph::current();
    for (NodeId c : characters(g))
        for (NodeId j : pinsOf(g, c))
        {
            const glm::dvec3 p(worldOf(g, j)[3]);
            const double r = 7.0 * view.unitsPerPixel(p);
            const glm::dvec3 tips[6] = {{r, 0, 0}, {0, r, 0}, {-r, 0, 0}, {0, -r, 0}, {0, 0, r}, {0, 0, -r}};
            for (int a : {0, 1, 2, 3})
                for (int b : {4, 5})
                {
                    frame.linesOnTop.push_back({glm::dvec3(p + tips[a]), kLocked});
                    frame.linesOnTop.push_back({glm::dvec3(p + tips[b]), kLocked});
                    frame.linesOnTop.push_back({glm::dvec3(p + tips[a]), kLocked});
                    frame.linesOnTop.push_back({glm::dvec3(p + tips[(a + 1) % 4]), kLocked});
                }
        }
}
} // namespace

std::vector<glm::dvec3> controllerOutline(Graph& g, NodeId chain)
{
    std::vector<glm::dvec3> out;
    Node* cn = g.node(chain);
    if (!cn)
        return out;
    g.evaluate();
    const Node* settings = drawSettingsOf(g, chain);
    const double scale = std::max(settings ? numberOr(*settings, "drawRadius", 1.0) : 1.0, 0.01);
    if (NodeId f = footForChain(g, chain))
    {
        const Node& foot = *g.node(f);
        const glm::dvec3 heel = v3(g, foot, "heelPivot"), toe = v3(g, foot, "toePivot");
        const glm::dvec3 inner = v3(g, foot, "innerPivot"), outer = v3(g, foot, "outerPivot");
        const double cz = (heel.z + toe.z) * 0.5, cx = (inner.x + outer.x) * 0.5;
        const double hz = std::max(std::abs(toe.z - heel.z) * 0.5 * 1.08, 1.0) * scale;
        const double hx = std::max(std::abs(outer.x - inner.x) * 0.5 * 1.15, 1.0) * scale;
        // Drawn where grabbing it would put it: on the foot as posed, unless the solver holds it.
        const glm::dmat4 control = lila::scene::rigel::controllerWorld(g, chain);
        for (int i = 0; i < kSegments; ++i)
        {
            const double a = 2.0 * math::kPi * i / kSegments;
            out.push_back(glm::dvec3(control * glm::dvec4(cx + hx * std::cos(a), 0.0, cz + hz * std::sin(a), 1.0)));
        }
        return out;
    }
    Node* h = named(g, textOf(*cn, "handle"));
    Node* s = named(g, textOf(*cn, "startJoint"));
    Node* e = named(g, textOf(*cn, "endJoint"));
    if (!h || !s || !e)
        return out;
    const glm::dvec3 at(lila::scene::rigel::controllerWorld(g, chain)[3]);
    const glm::dvec3 start(worldOf(g, s->id())[3]);
    glm::dvec3 axis = at - glm::dvec3(worldOf(g, g.parent(e->id()))[3]);
    if (glm::length(axis) < 1e-9)
        axis = glm::dvec3(0, 1, 0);
    axis = glm::normalize(axis);
    const double r = std::max(glm::length(at - start) * 0.12, 1.0) * scale;
    glm::dvec3 u = glm::cross(axis, std::abs(axis.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
    u = glm::normalize(u);
    const glm::dvec3 v = glm::cross(axis, u);
    for (int i = 0; i < kSegments; ++i)
    {
        const double a = 2.0 * math::kPi * i / kSegments;
        out.push_back(at + (u * std::cos(a) + v * std::sin(a)) * r);
    }
    return out;
}

NodeId controllerAt(Graph& g, const ViewInfo& view, QPointF px, double tolerance)
{
    NodeId best = kNoNode;
    double bestD = tolerance;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kIkChainType))
            if (auto d = controllerDistance(g, view, id, px); d && *d <= bestD)
            {
                best = id;
                bestD = *d;
            }
    return best;
}

void registerControllerDrawing(ext::Context& ctx)
{
    viewportHooks::addDrawer(ctx, "puppeteer.pins", [](render::FrameState& frame, OverlayPainter&, const ViewInfo& view) { drawPins(frame, view); });
    drawOverrides::add(ctx, {.id = "puppeteer.reverseFoot",
                             .nodeType = kReverseFootType,
                             .draw =
                                 [](drawOverrides::DrawContext& c) {
                                     Graph& g = Graph::current();
                                     const NodeId chain = chainFrom(g, c.node.id());
                                     if (!chain)
                                         return;
                                     drawLoop(c, controllerOutline(g, chain), colorFor(g, chain, c), thicknessOf(g, chain));
                                     drawPivots(c, g, c.node);
                                 },
                             .pick = [](const drawOverrides::PickContext& c) -> std::optional<double> {
                                 const NodeId chain = chainFrom(Graph::current(), c.node.id());
                                 return chain ? pickChain(c, chain) : std::nullopt;
                             }});
    drawOverrides::add(ctx, {.id = "puppeteer.ikChain",
                             .nodeType = kIkChainType,
                             .draw =
                                 [](drawOverrides::DrawContext& c) {
                                     Graph& g = Graph::current();
                                     if (!footForChain(g, c.node.id()))
                                         drawLoop(c, controllerOutline(g, c.node.id()), colorFor(g, c.node.id(), c), thicknessOf(g, c.node.id()));
                                 },
                             .pick = [](const drawOverrides::PickContext& c) -> std::optional<double> {
                                 Graph& g = Graph::current();
                                 return footForChain(g, c.node.id()) ? std::nullopt : pickChain(c, c.node.id());
                             }});
}
} // namespace puppeteer
