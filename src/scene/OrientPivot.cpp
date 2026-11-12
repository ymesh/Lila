#include "scene/OrientPivot.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene::orientpivot
{
namespace
{
using geo::Index;

ManipulatorSink& sink()
{
    static ManipulatorSink s;
    return s;
}

glm::dvec3 unitOrZero(const glm::dvec3& v)
{
    double l = glm::length(v);
    return l > 1e-12 ? v / l : glm::dvec3(0.0);
}

bool parallel(const glm::dvec3& a, const glm::dvec3& b) { return glm::length(glm::cross(unitOrZero(a), unitOrZero(b))) < 1e-3; }

int root(std::vector<int>& parent, int i)
{
    while (parent[i] != i)
        i = parent[i] = parent[parent[i]];
    return i;
}

/** Direction of the face's longest edge in winding order; the first such edge on ties, so a square takes its first. */
glm::dvec3 longestEdge(const geo::Mesh& m, Index f)
{
    glm::dvec3 best(0.0);
    double bestLen = 0.0;
    m.forEachFaceCorner(f, [&](Index c) {
        glm::dvec3 d = m.position(m.cornerNextPoint(c)) - m.position(m.cornerPoint(c));
        double l = glm::length(d);
        if (l > bestLen * (1.0 + 1e-9))
        {
            bestLen = l;
            best = d;
        }
    });
    return unitOrZero(best);
}

/** From the point along its longest edge. */
glm::dvec3 longestEdgeFrom(const geo::Mesh& m, Index p)
{
    glm::dvec3 best(0.0);
    double bestLen = 0.0;
    m.forEachPointEdge(p, [&](Index e) {
        glm::dvec3 d = m.position(m.edgeOtherPoint(e, p)) - m.position(p);
        double l = glm::length(d);
        if (l > bestLen * (1.0 + 1e-9))
        {
            bestLen = l;
            best = d;
        }
    });
    return unitOrZero(best);
}

glm::dvec3 newell(const geo::Mesh& m, const std::vector<Index>& loop)
{
    glm::dvec3 n(0.0);
    for (size_t i = 0; i < loop.size(); ++i)
        n += glm::cross(m.position(loop[i]), m.position(loop[(i + 1) % loop.size()]));
    return n;
}

Piece makePiece(const geo::Mesh& m, ComponentMode kind, const std::vector<Pick>& picks)
{
    Piece pc;
    pc.rank = picks.front().rank;

    std::vector<Index> pts;
    std::unordered_set<Index> seenPts;
    auto addPoint = [&](Index p) {
        if (seenPts.insert(p).second)
            pts.push_back(p);
    };
    for (const Pick& k : picks)
    {
        if (kind == ComponentMode::Vertex)
            addPoint(k.id);
        else if (kind == ComponentMode::Edge)
        {
            addPoint(m.edgePoint(k.id, 0));
            addPoint(m.edgePoint(k.id, 1));
        }
        else
            for (Index p : m.facePoints(k.id))
                addPoint(p);
    }
    for (Index p : pts)
        pc.pointSum += m.position(p);
    pc.pointCount = pts.size();
    pc.centre = pc.pointSum / double(pts.size());

    glm::dvec3 normal(0.0);
    double normalWeight = 0.0;
    std::unordered_set<Index> faces;
    if (kind == ComponentMode::Face)
    {
        glm::dvec3 weighted(0.0);
        double area = 0.0;
        for (const Pick& k : picks)
        {
            double a = m.faceArea(k.id);
            weighted += m.faceCentroid(k.id) * a;
            area += a;
            normal += m.faceAreaVector(k.id);
            normalWeight += glm::length(m.faceAreaVector(k.id));
        }
        if (area > 1e-12)
            pc.centre = weighted / area;
    }
    else if (kind == ComponentMode::Edge)
    {
        for (const Pick& k : picks)
            m.forEachEdgeCorner(k.id, [&](Index c) { faces.insert(m.cornerFace(c)); });
    }
    else
        for (Index p : pts)
            for (Index f : m.pointFaces(p))
                faces.insert(f);
    for (Index f : faces)
    {
        normal += m.faceAreaVector(f);
        normalWeight += glm::length(m.faceAreaVector(f));
    }
    // Faces that cancel out (all the way round a tube) leave no normal rather than rounding noise.
    pc.normal = glm::length(normal) > 1e-6 * normalWeight ? unitOrZero(normal) : glm::dvec3(0.0);
    pc.toStart = unitOrZero(m.position(pts.front()) - pc.centre);

    if (kind == ComponentMode::Face)
    {
        pc.aim = pc.normal;
        pc.aimIsNormal = true;
        pc.up = longestEdge(m, picks.front().id);
        return pc;
    }
    if (picks.size() == 1 && kind == ComponentMode::Vertex)
    {
        pc.aim = pc.normal;
        pc.aimIsNormal = true;
        pc.up = longestEdgeFrom(m, picks.front().id);
        return pc;
    }
    if (picks.size() == 1)
    {
        pc.aim = unitOrZero(m.position(m.edgePoint(picks.front().id, 1)) - m.position(m.edgePoint(picks.front().id, 0)));
        pc.up = pc.normal;
        pc.upIsNormal = true;
        return pc;
    }

    // Several points or edges: a closed loop aims through its plane, an open chain from end to end.
    std::vector<Index> edges;
    if (kind == ComponentMode::Edge)
        for (const Pick& k : picks)
            edges.push_back(k.id);
    else
        for (Index p : pts)
            m.forEachPointEdge(p, [&](Index e) {
                Index o = m.edgeOtherPoint(e, p);
                if (p < o && seenPts.contains(o))
                    edges.push_back(e);
            });
    std::vector<geo::EdgePath> paths = geo::chainEdges(m, edges);
    if (paths.size() == 1 && paths.front().points.size() == pts.size())
    {
        const geo::EdgePath& path = paths.front();
        if (path.closed)
        {
            glm::dvec3 n = unitOrZero(newell(m, path.points));
            // The surface decides which side the loop faces; a loop whose surface cancels out (a tube's ring) faces its
            // largest positive axis, so the result does not depend on how the loop was walked.
            double side = glm::dot(n, pc.normal);
            if (std::abs(side) < 1e-6)
            {
                int big = 0;
                for (int i = 1; i < 3; ++i)
                    if (std::abs(n[i]) > std::abs(n[big]) + 1e-9)
                        big = i;
                side = n[big];
            }
            pc.aim = side < 0.0 ? -n : n;
            pc.aimIsNormal = true;
            pc.up = pc.toStart;
            return pc;
        }
        glm::dvec3 a = m.position(path.points.front()), b = m.position(path.points.back());
        glm::dvec3 start = m.position(pts.front());
        if (glm::length(start - b) < glm::length(start - a))
            std::swap(a, b);
        pc.aim = unitOrZero(b - a);
        pc.up = pc.normal;
        pc.upIsNormal = true;
        return pc;
    }
    pc.aim = pc.normal;
    pc.aimIsNormal = true;
    pc.up = pc.toStart;
    return pc;
}

/** Sets the object's rotation and origin to the frame, its points moved back so nothing moves in the world. */
void unfreeze(Graph& g, NodeId id, const glm::dmat3& rotation, const glm::dvec3& origin)
{
    LILA_ZONE();

    g.evaluate();
    Node* n = g.node(id);
    const glm::dmat4 before = n->as<glm::dmat4>(Transform::kWorldMatrix);
    std::vector<std::pair<NodeId, glm::dmat4>> children;
    for (NodeId c : g.children(id))
        children.push_back({c, g.node(c)->as<glm::dmat4>(Transform::kWorldMatrix)});
    const glm::dmat4 parent = n->as<glm::dmat4>(Transform::kParentMatrix);
    const glm::dvec3 scale = n->as<glm::dvec3>(Transform::kScale);

    for (int attr : {Transform::kRotatePivot, Transform::kScalePivot, Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate,
                     Transform::kPivotOrient, Transform::kRotateAxis})
        g.setValue({id, attr, -1}, glm::dvec3(0.0));
    glm::dmat4 world = glm::translate(glm::dmat4(1.0), origin) * glm::dmat4(rotation) * glm::scale(glm::dmat4(1.0), scale);
    Transform::setLocalMatrix(g, id, glm::inverse(parent) * world);
    g.evaluate();
    xformops::bakeIntoGeometry(g, id, glm::inverse(n->as<glm::dmat4>(Transform::kWorldMatrix)) * before);
    g.evaluate();
    for (auto& [c, w] : children)
        Transform::setLocalMatrix(g, c, glm::inverse(n->as<glm::dmat4>(Transform::kWorldMatrix)) * w);
}

json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/** Curve points: each pick is its own piece; a single one aims along its tangent, up along its normal. */
std::vector<Piece> providedPieces(Graph& g, NodeId id, const components::PointProvider& pp, const std::vector<Pick>& picks)
{
    std::unordered_map<uint32_t, glm::dvec3> at;
    for (const auto& [k, pos] : pp.points(g, id))
        at[k] = pos;
    components::Ids ids;
    for (const Pick& k : picks)
        ids.push_back(k.id);
    const std::vector<glm::dmat3> frames = pp.frames ? pp.frames(g, id, ids) : std::vector<glm::dmat3>{};
    std::vector<Piece> out;
    for (size_t i = 0; i < picks.size(); ++i)
    {
        auto it = at.find(picks[i].id);
        if (it == at.end())
            continue;
        Piece pc;
        pc.rank = picks[i].rank;
        pc.centre = pc.pointSum = it->second;
        pc.pointCount = 1;
        if (i < frames.size())
        {
            pc.aim = unitOrZero(frames[i][0]);
            pc.normal = unitOrZero(frames[i][2]);
            pc.up = pc.normal != glm::dvec3(0.0) ? pc.normal : unitOrZero(frames[i][1]);
            pc.upIsNormal = pc.normal != glm::dvec3(0.0);
        }
        out.push_back(pc);
    }
    std::stable_sort(out.begin(), out.end(), [](const Piece& x, const Piece& y) { return x.rank < y.rank; });
    return out;
}

/**
 * The objects that turn: each picked node, or the selected group it was reached through when only the group is
 * selected, so picking a child's components orients the group.
 */
std::vector<NodeId> selectedOwners(Graph& g, const std::vector<NodeId>& owners)
{
    const std::vector<NodeId>& sel = g.selection();
    auto selected = [&](NodeId id) { return std::find(sel.begin(), sel.end(), id) != sel.end(); };
    std::vector<NodeId> out;
    for (NodeId id : owners)
    {
        NodeId target = id;
        if (!selected(id))
            for (NodeId up = g.parent(id); up != kNoNode; up = g.parent(up))
                if (selected(up))
                {
                    target = up;
                    break;
                }
        if (std::find(out.begin(), out.end(), target) == out.end())
            out.push_back(target);
    }
    return out;
}

/** Object mode: the other objects take the first one's pivot axes (and, for position='first', its pivot position). */
json fromFirstObject(const std::vector<std::string>& names, const std::string& mode, const std::string& position)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::vector<NodeId> ids;
    for (const std::string& name : names)
        if (Node* n = g.find(name); n && n->type().isA("transform"))
            ids.push_back(n->id());
    if (ids.size() < 2)
        throw CommandError("orientPivot: pick components, or select the object to take the pivot from, then the ones to change");
    if (mode == "manipulator")
        throw CommandError("orientPivot: mode='manipulator' needs a component selection");
    if (mode != "pivot" && mode != "transform")
        throw CommandError("orientPivot: mode must be pivot, transform or manipulator");
    if (position != "first" && position != "center" && position != "keep")
        throw CommandError("orientPivot: position must be first, center or keep");

    g.evaluate();
    const NodeId source = ids.front();
    ids.erase(ids.begin());
    const glm::dmat3 axes = xformops::pivotFrame(g, source);
    std::optional<glm::dvec3> place;
    if (position == "first")
        place = xformops::worldPivot(g, source);
    else if (position == "center")
    {
        glm::dvec3 sum = xformops::worldPivot(g, source);
        for (NodeId id : ids)
            sum += xformops::worldPivot(g, id);
        place = sum / double(ids.size() + 1);
    }

    if (mode == "pivot")
        for (NodeId id : ids)
        {
            if (place)
                xformops::setWorldPivot(g, id, *place);
            xformops::setWorldPivotFrame(g, id, axes);
        }
    else
        for (NodeId id : xformops::topLevel(g, ids))
            unfreeze(g, id, axes, place ? *place : xformops::worldPivot(g, id));

    const glm::dvec3 at = place ? *place : xformops::worldPivot(g, ids.front());
    return {{"position", vecJson(at)}, {"x", vecJson(axes[0])}, {"y", vecJson(axes[1])}, {"z", vecJson(axes[2])}};
}
} // namespace

std::vector<Piece> pieces(const geo::Mesh& m, ComponentMode kind, const std::vector<Pick>& picks, bool split)
{
    LILA_ZONE();

    std::vector<Pick> valid;
    for (const Pick& k : picks)
        if (kind == ComponentMode::Vertex ? m.pointValid(k.id) : kind == ComponentMode::Edge ? m.edgeValid(k.id) : m.faceValid(k.id))
            valid.push_back(k);
    std::stable_sort(valid.begin(), valid.end(), [](const Pick& a, const Pick& b) { return a.rank < b.rank; });

    std::vector<int> parent(valid.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto unite = [&](int a, int b) { parent[root(parent, a)] = root(parent, b); };
    if (!split)
    {
        // Each shared element (a point, or an edge between faces) joins whatever picks touch it.
        std::unordered_map<Index, int> owner;
        auto touch = [&](Index key, int i) {
            auto [it, fresh] = owner.try_emplace(key, i);
            if (!fresh)
                unite(i, it->second);
        };
        for (int i = 0; i < int(valid.size()); ++i)
        {
            const Index id = valid[i].id;
            if (kind == ComponentMode::Vertex)
            {
                touch(id, i);
                m.forEachPointEdge(id, [&](Index e) {
                    if (auto it = owner.find(m.edgeOtherPoint(e, id)); it != owner.end())
                        unite(i, it->second);
                });
            }
            else if (kind == ComponentMode::Edge)
            {
                touch(m.edgePoint(id, 0), i);
                touch(m.edgePoint(id, 1), i);
            }
            else
                m.forEachFaceCorner(id, [&](Index c) { touch(m.cornerEdge(c), i); });
        }
    }

    std::vector<std::vector<Pick>> groups;
    std::unordered_map<int, size_t> groupOf;
    for (int i = 0; i < int(valid.size()); ++i)
    {
        auto [it, fresh] = groupOf.try_emplace(root(parent, i), groups.size());
        if (fresh)
            groups.emplace_back();
        groups[it->second].push_back(valid[i]);
    }
    std::vector<Piece> out;
    for (const auto& group : groups)
        out.push_back(makePiece(m, kind, group));
    return out;
}

Piece toWorld(const Piece& p, const glm::dmat4& world)
{
    const glm::dmat3 linear(world);
    const glm::dmat3 normals = glm::inverseTranspose(linear);
    auto dir = [&](const glm::dvec3& v, bool normal) { return unitOrZero((normal ? normals : linear) * v); };
    Piece w = p;
    w.centre = glm::dvec3(world * glm::dvec4(p.centre, 1.0));
    w.pointSum = glm::dvec3(world * glm::dvec4(p.pointSum / double(std::max<size_t>(p.pointCount, 1)), 1.0)) * double(p.pointCount);
    w.aim = dir(p.aim, p.aimIsNormal);
    w.up = dir(p.up, p.upIsNormal);
    w.normal = dir(p.normal, true);
    w.toStart = dir(p.toStart, false);
    return w;
}

Aim solve(const std::vector<Piece>& sorted)
{
    if (sorted.empty())
        throw std::invalid_argument("nothing to aim from");
    Aim a;
    if (sorted.size() == 1)
    {
        const Piece& p = sorted.front();
        a.aim = p.aim;
        a.ups = {p.up, p.normal, p.toStart};
        if (glm::length(a.aim) < 1e-12)
            throw std::invalid_argument("the selection has no direction to aim along");
        return a;
    }
    const Piece& first = sorted.front();
    const Piece& last = sorted.back();
    a.aim = unitOrZero(last.centre - first.centre);
    if (glm::length(a.aim) < 1e-12)
        throw std::invalid_argument("the first and last picked components are at the same place");
    a.ups = {first.normal, last.normal, first.toStart, last.toStart};
    return a;
}

Axis parseAxis(const std::string& name)
{
    bool negative = !name.empty() && name[0] == '-';
    std::string axis = negative ? name.substr(1) : name;
    if (axis.size() != 1 || axis[0] < 'x' || axis[0] > 'z')
        throw std::invalid_argument("axis must be x, -x, y, -y, z or -z");
    return {axis[0] - 'x', negative ? -1.0 : 1.0};
}

glm::dmat3 aimFrame(const glm::dvec3& aim, const std::vector<glm::dvec3>& ups, Axis aimAxis, Axis upAxis)
{
    if (aimAxis.index == upAxis.index)
        throw std::invalid_argument("the aim and up axes must differ");
    const glm::dvec3 a = unitOrZero(aim);
    if (glm::length(a) == 0.0)
        throw std::invalid_argument("nothing to aim along");
    glm::dvec3 u(0.0);
    for (const glm::dvec3& candidate : ups)
        if (glm::length(candidate) > 1e-12 && !parallel(candidate, a))
        {
            u = candidate;
            break;
        }
    if (u == glm::dvec3(0.0))
    {
        int least = 0;
        for (int i = 1; i < 3; ++i)
            if (std::abs(a[i]) < std::abs(a[least]))
                least = i;
        u[least] = 1.0;
    }
    u = glm::normalize(u - a * glm::dot(u, a));

    glm::dmat3 f(0.0);
    f[aimAxis.index] = a * aimAxis.sign;
    f[upAxis.index] = u * upAxis.sign;
    const int k = 3 - aimAxis.index - upAxis.index;
    f[k] = glm::cross(f[(k + 1) % 3], f[(k + 2) % 3]);
    return f;
}

void setManipulatorSink(ManipulatorSink s) { sink() = std::move(s); }

void registerCommand()
{
    using T = ArgType;
    const std::vector<std::string> axes = {"x", "-x", "y", "-y", "z", "-z"};

    ops::registerCommand({
        .name = "orientPivot",
        .doc = "Aim the pivot from the first picked component to the second, or along a single one.\n\n"
               "The component selection splits into connected pieces (two edge loops are two pieces); with exactly two "
               "components picked, each is its own piece. With several pieces the aim runs from the first picked piece's "
               "centre to the last's. One piece aims along itself: a face or face region along its normal (up from the first "
               "face's longest edge), an edge from its first point to its second (up from its faces' normal), a closed loop "
               "through its plane (up toward its first picked point), an open chain from the end nearer the first pick to "
               "the other, a vertex along its normal (up along its longest edge).\n\n"
               "A curve point aims along the curve, up along its normal. Picks on a mesh reached through a selected group turn the "
               "group. In object mode the other selected objects take the first one's pivot axes and position.",
        .flags = {
            {"aim", "", T::String, "x", "Pivot axis that points along the aim.", axes},
            {"up", "", T::String, "y", "Pivot axis that points toward the up reference.", axes},
            {"upReference", "", T::String, "component",
             "What the up axis leans toward: the selection's own up (component), worldUp (world), or the object's own matching "
             "axis (object). Falls back to the others where it lines up with the aim.",
             {"component", "world", "object"}},
            {"worldUp", "", T::Vec3, nullptr, "Up direction for upReference='world'; the scene up when left out."},
            {"position", "", T::String, "first",
             "Where the pivot goes: the first picked piece's centre (first), the centre of everything selected (center), or "
             "where it is (keep).",
             {"first", "center", "keep"}},
            {"mode", "", T::String, "transform",
             "transform: turn and move the object itself onto the aim, the mesh moved back so it stays put (undoes a freeze's "
             "lost rotation), so Object space then works along it. pivot: orient the object's pivot, leaving object and mesh "
             "alone. manipulator: pin only the component manipulator's axes, through selection changes, until "
             "resetComponentPivot.",
             {"pivot", "transform", "manipulator"}},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const ComponentMode kind = components::mode();
            const std::string mode = a.get<std::string>("mode", "transform");
            const std::string position = a.get<std::string>("position", "first");
            if (kind == ComponentMode::Object)
                return fromFirstObject(a.objects(), mode, position);
            const std::string upRef = a.get<std::string>("upReference", "component");
            Axis aimAxis, upAxis;
            try
            {
                aimAxis = parseAxis(a.get<std::string>("aim", "x"));
                upAxis = parseAxis(a.get<std::string>("up", "y"));
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("orientPivot: ") + e.what());
            }
            if (aimAxis.index == upAxis.index)
                throw CommandError("orientPivot: the aim and up axes must differ");

            Graph& g = Graph::current();
            g.evaluate();
            const auto ordered = components::ordered(kind);
            std::map<NodeId, std::vector<Pick>> byNode;
            std::vector<NodeId> nodes;
            for (size_t i = 0; i < ordered.size(); ++i)
            {
                auto [node, id] = ordered[i];
                if (!byNode.contains(node))
                    nodes.push_back(node);
                byNode[node].push_back({id, i});
            }
            std::vector<Piece> all;
            std::vector<NodeId> owners;
            for (NodeId node : nodes)
            {
                Node* n = g.node(node);
                if (!n)
                    continue;
                std::vector<Piece> found;
                if (MeshPtr cage = n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr)
                    found = pieces(*cage, kind, byNode[node], ordered.size() == 2);
                else if (const components::PointProvider* pp = components::pointProvider(*n); pp && kind == ComponentMode::Vertex)
                    found = providedPieces(g, node, *pp, byNode[node]);
                if (found.empty())
                    continue;
                const glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
                for (const Piece& p : found)
                    all.push_back(toWorld(p, world));
                owners.push_back(node);
            }
            if (all.empty())
                throw CommandError("orientPivot: select mesh vertices, edges or faces, or curve points");
            const std::vector<NodeId> targets = selectedOwners(g, owners);
            std::stable_sort(all.begin(), all.end(), [](const Piece& x, const Piece& y) { return x.rank < y.rank; });

            Aim aim;
            try
            {
                aim = solve(all);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("orientPivot: ") + e.what());
            }

            std::optional<glm::dvec3> place;
            if (position == "first")
                place = all.front().centre;
            else if (position == "center")
            {
                glm::dvec3 sum(0.0);
                size_t count = 0;
                for (const Piece& p : all)
                {
                    sum += p.pointSum;
                    count += p.pointCount;
                }
                place = sum / double(count);
            }
            else if (position != "keep")
                throw CommandError("orientPivot: position must be first, center or keep");

            const glm::dvec3 worldUp = a.has("worldUp") ? glm::dvec3(a.flags["worldUp"][0].get<double>(), a.flags["worldUp"][1].get<double>(),
                                                                      a.flags["worldUp"][2].get<double>())
                                                        : sceneUp();
            auto frameFor = [&](NodeId id) {
                std::vector<glm::dvec3> ups;
                if (upRef == "world")
                    ups.push_back(worldUp * upAxis.sign);
                else if (upRef == "object")
                    ups.push_back(xformops::frame(g, id, Space::Object)[upAxis.index] * upAxis.sign);
                else if (upRef != "component")
                    throw CommandError("orientPivot: upReference must be component, world or object");
                ups.insert(ups.end(), aim.ups.begin(), aim.ups.end());
                ups.push_back(sceneUp());
                return aimFrame(aim.aim, ups, aimAxis, upAxis);
            };

            const glm::dmat3 lead = frameFor(targets.front());
            if (mode == "pivot")
                for (NodeId id : targets)
                {
                    if (place)
                        xformops::setWorldPivot(g, id, *place);
                    xformops::setWorldPivotFrame(g, id, frameFor(id));
                }
            else if (mode == "transform")
                for (NodeId id : xformops::topLevel(g, targets))
                    unfreeze(g, id, frameFor(id), place ? *place : xformops::worldPivot(g, id));
            else if (mode != "manipulator")
                throw CommandError("orientPivot: mode must be pivot, transform or manipulator");
            else if (!sink())
                throw CommandError("orientPivot: mode='manipulator' needs a viewport");

            const glm::dvec3 at = place ? *place : mode == "manipulator" ? symmetricSelectionCenter() : xformops::worldPivot(g, targets.front());
            if (sink())
                sink()(at, lead);
            return {{"position", vecJson(at)}, {"x", vecJson(lead[0])}, {"y", vecJson(lead[1])}, {"z", vecJson(lead[2])}};
        },
        .adjustable = true,
        .examples = {"orientPivot()  # two verts picked: the object turns so its X aims from the first to the second",
                     "orientPivot(mode='manipulator')  # pin the component manipulator's axes instead"},
    });
}
} // namespace lila::scene::orientpivot
