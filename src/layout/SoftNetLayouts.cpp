#include "layout/SoftNetBind.h"

#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "layout/LayoutNode.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;

/** A mesh's surface in world space, for laying strokes on it. */
struct WorldSurface
{
    MeshPtr mesh;
    glm::dmat4 world{1.0}, toObject{1.0};
    std::unique_ptr<geo::MeshBvh> bvh;

    WorldSurface(Graph& g, NodeId meshNode)
    {
        g.evaluate();
        Node& n = *g.node(meshNode);
        mesh = n.as<MeshPtr>(MeshNode::kOutMesh);
        world = worldOf(n);
        toObject = glm::inverse(world);
        if (mesh)
            bvh = std::make_unique<geo::MeshBvh>(*mesh);
    }

    glm::dvec3 toWorld(const glm::dvec3& p) const { return glm::dvec3(world * glm::dvec4(p, 1.0)); }
    glm::dvec3 local(const glm::dvec3& p) const { return glm::dvec3(toObject * glm::dvec4(p, 1.0)); }

    /** The nearest surface hit along a world ray, or nothing. */
    std::optional<glm::dvec3> ray(const glm::dvec3& origin, const glm::dvec3& dir, double maxT) const
    {
        geo::BvhHit hit;
        const glm::dvec3 o = local(origin), d = glm::dvec3(toObject * glm::dvec4(dir, 0.0));
        if (!bvh || !bvh->raycast(o, d, hit, maxT))
            return std::nullopt;
        return toWorld(hit.point);
    }

    /** The closest surface point to a world point, and the surface normal there. */
    glm::dvec3 closest(const glm::dvec3& p, glm::dvec3* normal = nullptr) const
    {
        geo::BvhClosest hit;
        if (!bvh || !bvh->closestPoint(local(p), hit))
            return p;
        if (normal)
        {
            glm::dvec3 a, b, c;
            bvh->trianglePoints(hit.triangle, a, b, c);
            const glm::dvec3 n = glm::cross(b - a, c - a);
            *normal = glm::normalize(glm::dvec3(glm::transpose(toObject) * glm::dvec4(n, 0.0)));
        }
        return toWorld(hit.point);
    }
};

json strokeJson(const std::vector<glm::dvec3>& pts)
{
    json out = json::array();
    for (const glm::dvec3& p : pts)
        out.push_back({p.x, p.y, p.z});
    return out;
}

/** The world polyline a curve, a joint chain or a bend-bone chain gives: a curve's first spline, or a chain's joints from the given one down its first children. */
std::vector<glm::dvec3> spinePoints(Graph& g, Node& n, const char* cmd)
{
    g.evaluate();
    if (n.type().isA("curve") || n.type().isA("nurbsCurve") || dynamic_cast<curve::CurveShape*>(&n))
    {
        const curve::Splines& s = curve::splinesOf(n.value(curve::CurveShape::kWorldSpace));
        if (s.empty())
            throw CommandError(std::string(cmd) + ": " + n.path() + " has no curve");
        return curve::tessellate(s.front()).points;
    }
    if (rig::isJoint(n))
    {
        std::vector<glm::dvec3> pts{positionOf(n)};
        for (NodeId cur = n.id();;)
        {
            NodeId next = kNoNode;
            for (NodeId c : g.children(cur))
                if (rig::isJoint(*g.node(c)))
                {
                    next = c;
                    break;
                }
            if (next == kNoNode)
                break;
            pts.push_back(positionOf(*g.node(next)));
            cur = next;
        }
        if (pts.size() < 2)
            throw CommandError(std::string(cmd) + ": " + n.path() + " has no joint below it to run to");
        return pts;
    }
    throw CommandError(std::string(cmd) + ": " + n.path() + " is neither a curve nor a joint chain");
}

/** Parallel-transport frames along a polyline: tangent, and a normal and binormal turning as little as they can. */
struct SpineFrame
{
    glm::dvec3 p, t, n, b;
};

SpineFrame frameAt(const std::vector<glm::dvec3>& pts, double s)
{
    // The frame is transported from the start each time; spines are short.
    glm::dvec3 t0 = glm::normalize(pts[1] - pts[0]);
    glm::dvec3 n = glm::normalize(glm::cross(t0, std::abs(t0.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    double acc = 0.0;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const double len = glm::length(pts[i + 1] - pts[i]);
        const glm::dvec3 t = len > 0.0 ? (pts[i + 1] - pts[i]) / len : t0;
        // Carry n from the previous segment's direction onto this one.
        n = glm::normalize(n - t * glm::dot(n, t));
        if (acc + len >= s || i + 2 == pts.size())
        {
            const double f = len > 0.0 ? std::clamp((s - acc) / len, 0.0, 1.0) : 0.0;
            return {pts[i] + (pts[i + 1] - pts[i]) * f, t, n, glm::cross(t, n)};
        }
        acc += len;
        t0 = t;
    }
    return {pts.back(), t0, n, glm::cross(t0, n)};
}

/** Where a ray from the spine at angle a about it meets the surface; nothing when it doesn't. */
std::optional<glm::dvec3> around(const WorldSurface& surf, const SpineFrame& f, double a, double maxT)
{
    const glm::dvec3 dir = f.n * std::cos(a) + f.b * std::sin(a);
    return surf.ray(f.p, dir, maxT);
}

Node& layoutFor(Graph& g, Node& mesh, const Args& a, const char* cmd)
{
    if (a.has("layout"))
    {
        Node* l = findLayoutNode(g, nodeNamed(a.get<std::string>("layout"), cmd).id());
        if (!l)
            throw CommandError(std::string(cmd) + ": " + a.get<std::string>("layout") + " is not a Panel Layout");
        return *l;
    }
    if (Node* l = findLayoutNode(g, mesh.id()))
        return *l;
    const json made = ops::execute("panelLayout", {{"args", {mesh.path()}}, {"mirror", "off"}});
    Node* l = made.is_array() && !made.empty() ? g.find(made[0].get<std::string>()) : nullptr;
    if (!l)
        throw CommandError(std::string(cmd) + ": could not add a Panel Layout to " + mesh.path());
    // Lines made for a Soft Net shape the surface through the net only: they cut no grooves.
    ops::execute("panelLayoutLineSet", {{"args", {l->path()}}, {"set", "Major"}, {"output", "none"}});
    return *l;
}

/** Spoke spots on a closed loop: count of them by arc length, crowded toward corners as bias grows. */
std::vector<double> spokeSpots(const std::vector<glm::dvec3>& loop, int count, double bias)
{
    const size_t n = loop.size();
    std::vector<double> s(n + 1, 0.0), w(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i)
        s[i + 1] = s[i] + glm::length(loop[(i + 1) % n] - loop[i]);
    const double total = s[n];
    // Weight per segment: 1 plus bias times the turn at its start (radians), so corners collect spokes.
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 a = loop[i] - loop[(i + n - 1) % n], b = loop[(i + 1) % n] - loop[i];
        const double turn = glm::length(a) > 0 && glm::length(b) > 0 ? std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)) : 0.0;
        const double len = s[i + 1] - s[i];
        w[i + 1] = w[i] + len * (1.0 + bias * turn * double(n) / (2.0 * std::numbers::pi));
    }
    std::vector<double> out;
    for (int k = 0; k < count; ++k)
    {
        const double target = w[n] * (double(k) + 0.5) / double(count);
        size_t i = size_t(std::lower_bound(w.begin(), w.end(), target) - w.begin());
        i = std::clamp<size_t>(i, 1, n);
        const double f = w[i] > w[i - 1] ? (target - w[i - 1]) / (w[i] - w[i - 1]) : 0.0;
        out.push_back(s[i - 1] + (s[i] - s[i - 1]) * f);
    }
    (void)total;
    return out;
}

glm::dvec3 loopAt(const std::vector<glm::dvec3>& loop, double s, glm::dvec3* tangent)
{
    const size_t n = loop.size();
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 a = loop[i], b = loop[(i + 1) % n];
        const double len = glm::length(b - a);
        if (acc + len >= s || i + 1 == n)
        {
            if (tangent)
                *tangent = len > 0 ? (b - a) / len : glm::dvec3(1, 0, 0);
            return a + (b - a) * (len > 0 ? std::clamp((s - acc) / len, 0.0, 1.0) : 0.0);
        }
        acc += len;
    }
    return loop.front();
}

/** A spoke: from the loop point outward along the surface, staying on it. */
std::vector<glm::dvec3> spokeFrom(const WorldSurface& surf, const glm::dvec3& start, const glm::dvec3& tangent, const glm::dvec3& centre, double length)
{
    glm::dvec3 n;
    glm::dvec3 p = surf.closest(start, &n);
    glm::dvec3 dir = glm::cross(n, tangent);
    if (glm::dot(dir, p - centre) < 0.0)
        dir = -dir;
    std::vector<glm::dvec3> out{p};
    const int steps = 16;
    for (int i = 0; i < steps; ++i)
    {
        dir = dir - n * glm::dot(dir, n);
        if (glm::length(dir) < 1e-12)
            break;
        dir = glm::normalize(dir);
        p = surf.closest(p + dir * (length / steps), &n);
        out.push_back(p);
    }
    return out;
}
} // namespace

json loopWithSpokes(Graph& g, Node& layoutNode, const std::vector<glm::dvec3>& loopIn, int count, double spacing, double length, double bias, double tolerance)
{
    LILA_ZONE();
    const NodeId mesh = modifiers::meshOf(g, layoutNode.id());
    if (mesh == kNoNode)
        throw CommandError("panelLayoutLoopSpokes: the Panel Layout is on no mesh");
    std::vector<glm::dvec3> loop = loopIn;
    if (loop.size() > 2 && glm::length(loop.front() - loop.back()) < 1e-9)
        loop.pop_back();
    if (loop.size() < 3)
        throw CommandError("panelLayoutLoopSpokes: the loop needs at least three points");
    const WorldSurface surf(g, mesh);
    double perimeter = 0.0;
    glm::dvec3 centre(0.0);
    for (size_t i = 0; i < loop.size(); ++i)
    {
        perimeter += glm::length(loop[(i + 1) % loop.size()] - loop[i]);
        centre += loop[i];
    }
    centre /= double(loop.size());
    if (spacing > 0.0)
        count = std::max(3, int(std::lround(perimeter / spacing)));
    if (length <= 0.0)
        length = perimeter / (2.0 * std::numbers::pi) * 0.6;

    std::vector<glm::dvec3> closed = loop;
    closed.push_back(loop.front());
    ops::execute("panelLayoutStroke", {{"args", {layoutNode.path()}}, {"points", strokeJson(closed)}, {"tolerance", tolerance}});
    int made = 0;
    for (double s : spokeSpots(loop, count, bias))
    {
        glm::dvec3 tangent;
        const glm::dvec3 at = loopAt(loop, s, &tangent);
        const std::vector<glm::dvec3> spoke = spokeFrom(surf, at, tangent, centre, length);
        if (spoke.size() < 2)
            continue;
        ops::execute("panelLayoutStroke", {{"args", {layoutNode.path()}}, {"points", strokeJson(spoke)}, {"tolerance", tolerance}});
        ++made;
    }
    return {{"layout", layoutNode.path()}, {"spokes", made}, {"perimeter", perimeter}};
}

void registerSoftNetLayoutsCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "softNetLimbStations",
        .doc = "Lay a limb's Soft Net lines on its mesh's Panel Layout: rings round the limb at stations along a curve, a joint "
               "chain or a bend-bone chain, and rails running its length between them, so the chain drives the net and the "
               "net drives the surface. Makes a Panel Layout when the mesh has none. Returns {layout, rings, rails}.",
        .positionalDoc = "mesh",
        .flags = {{"spine", "sp", T::String, nullptr, "The curve, joint or bend-bone chain start the limb follows."},
                  {"rings", "rg", T::Int, 5, "Rings round the limb.", {}, {}, 1.0, 64.0},
                  {"rails", "rl", T::Int, 4, "Lines along the limb.", {}, {}, 0.0, 32.0},
                  {"start", "", T::Float, 0.05, "Where the first ring sits, as a fraction of the spine.", {}, {}, 0.0, 1.0},
                  {"end", "", T::Float, 0.95, "Where the last ring sits, as a fraction of the spine.", {}, {}, 0.0, 1.0},
                  {"twist", "tw", T::Float, 0.0, "Degrees the rails start turned about the spine.", {}, {}, -180.0, 180.0},
                  {"layout", "ly", T::String, nullptr, "The Panel Layout to draw on; default the mesh's topmost."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> objs = a.objects();
            if (objs.empty() || (!a.has("spine") && objs.size() < 2))
                throw CommandError("softNetLimbStations: give a mesh and spine= (or select the mesh, then the curve or joint chain)");
            const std::string spineName = a.has("spine") ? a.get<std::string>("spine") : objs[1];
            Node& mesh = nodeNamed(a.objects().front(), "softNetLimbStations");
            if (!mesh.type().isA("mesh"))
                throw CommandError("softNetLimbStations: " + mesh.path() + " is not a mesh");
            const std::vector<glm::dvec3> spine = spinePoints(g, nodeNamed(spineName, "softNetLimbStations"), "softNetLimbStations");
            Node& layout = layoutFor(g, mesh, a, "softNetLimbStations");
            const WorldSurface surf(g, mesh.id());
            if (!surf.bvh)
                throw CommandError("softNetLimbStations: " + mesh.path() + " has no surface");
            double total = 0.0;
            for (size_t i = 0; i + 1 < spine.size(); ++i)
                total += glm::length(spine[i + 1] - spine[i]);
            const geo::Bounds box = surf.mesh->bounds();
            const double reachOut = glm::length(box.max - box.min);
            const int rings = a.get<int>("rings"), rails = a.get<int>("rails");
            const double s0 = std::min(a.get<double>("start"), a.get<double>("end")) * total, s1 = std::max(a.get<double>("start"), a.get<double>("end")) * total;
            const double twist = glm::radians(a.get<double>("twist"));
            const double tolerance = std::max(total / 200.0, 1e-3);
            const int around = 48;
            int ringsMade = 0, railsMade = 0;
            for (int r = 0; r < rings; ++r)
            {
                const double s = rings == 1 ? (s0 + s1) * 0.5 : s0 + (s1 - s0) * double(r) / double(rings - 1);
                const SpineFrame f = frameAt(spine, s);
                std::vector<glm::dvec3> ring;
                for (int k = 0; k < around; ++k)
                    if (const auto p = layout::around(surf, f, twist + 2.0 * std::numbers::pi * double(k) / around, reachOut))
                        ring.push_back(*p);
                if (int(ring.size()) < around * 3 / 4)
                    continue;
                ring.push_back(ring.front());
                ops::execute("panelLayoutStroke", {{"args", {layout.path()}}, {"points", strokeJson(ring)}, {"tolerance", tolerance}});
                ++ringsMade;
            }
            // Rails run a little past the end rings so they cross them.
            const double pad = rings > 1 ? (s1 - s0) / double(rings - 1) * 0.25 : total * 0.05;
            for (int j = 0; j < rails; ++j)
            {
                const double ang = twist + 2.0 * std::numbers::pi * double(j) / double(rails);
                std::vector<glm::dvec3> rail;
                const int steps = std::max(8, rings * 6);
                for (int i = 0; i <= steps; ++i)
                {
                    const double s = std::clamp(s0 - pad + (s1 - s0 + 2.0 * pad) * double(i) / double(steps), 0.0, total);
                    if (const auto p = layout::around(surf, frameAt(spine, s), ang, reachOut))
                        rail.push_back(*p);
                }
                if (rail.size() < 2)
                    continue;
                ops::execute("panelLayoutStroke", {{"args", {layout.path()}}, {"points", strokeJson(rail)}, {"tolerance", tolerance}});
                ++railsMade;
            }
            return {{"layout", layout.path()}, {"rings", ringsMade}, {"rails", railsMade}};
        },
        .examples = {"softNetLimbStations(\"Body\", spine=\"upperarm_l\", rings=5, rails=4)"},
    });

    ops::registerCommand({
        .name = "panelLayoutLoopSpokes",
        .doc = "Draw a closed loop on a Panel Layout with spokes running out from it across the surface, for mouths and eyes "
               "under a Soft Net: count spokes (or one every spacing), each length long, crowded toward the loop's corners as "
               "cornerBias grows. The Layout Pencil's Loop with Spokes option draws through it. Returns {layout, spokes, perimeter}.",
        .positionalDoc = "layout (or its mesh)",
        .flags = {{"points", "p", T::List, nullptr, "The loop: world-space points on the surface, [[x, y, z], ...]; it closes itself."},
                  {"count", "c", T::Int, 8, "Spokes round the loop.", {}, {}, 3.0, 64.0},
                  {"spacing", "sp", T::Float, 0.0, "Distance between spokes along the loop, cm; overrides count when above 0.", {}, {}, 0.0},
                  {"length", "ln", T::Float, 0.0, "Spoke length, cm; 0 is 0.6 of the loop's radius.", {}, {}, 0.0},
                  {"cornerBias", "cb", T::Float, 1.0, "How strongly spokes crowd toward the loop's corners; 0 spaces them evenly.", {}, {}, 0.0, 10.0},
                  {"tolerance", "", T::Float, 0.1, "Fit tolerance, world units.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.objects().empty() || !a.has("points"))
                throw CommandError("panelLayoutLoopSpokes: give a layout and points=");
            Node* layout = findLayoutNode(g, nodeNamed(a.objects().front(), "panelLayoutLoopSpokes").id());
            if (!layout)
                throw CommandError("panelLayoutLoopSpokes: " + a.objects().front() + " has no Panel Layout");
            std::vector<glm::dvec3> pts;
            for (const json& p : a.get<json>("points"))
                if (p.is_array() && p.size() == 3)
                    pts.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
            return loopWithSpokes(g, *layout, pts, a.get<int>("count"), a.get<double>("spacing"), a.get<double>("length"), a.get<double>("cornerBias"),
                                  a.get<double>("tolerance"));
        },
        .examples = {"panelLayoutLoopSpokes(\"Mochi_panelLayout\", points=mouth, count=10, length=2.0)"},
    });
}
} // namespace lila::layout
