#include "layout/SoftNetBind.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "layout/LayoutNode.h"
#include "layout/Path.h"
#include "layout/Resolve.h"
#include "ops/Command.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;
namespace sn = lila::deform::softnet;

struct BindOptions
{
    std::optional<double> reach, stiffness, spacing, maxDistance;
    std::string name = "SoftNet";
    std::vector<NodeId> extraMeshes;
};

/** Closest-point sampling on one mesh, in its object space. */
struct NetSurface
{
    const geo::Mesh& mesh;
    geo::MeshBvh bvh;
    explicit NetSurface(const geo::Mesh& m) : mesh(m), bvh(m) {}

    bool sample(const glm::dvec3& p, sn::SurfacePoint& out, double* distance = nullptr) const
    {
        geo::BvhClosest hit;
        if (!bvh.closestPoint(p, hit) || hit.triangle == geo::kInvalid)
            return false;
        const geo::Triangulation& tris = bvh.triangles();
        for (int k = 0; k < 3; ++k)
            out.points[size_t(k)] = uint32_t(mesh.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + size_t(k)]));
        out.bary = hit.bary;
        if (distance)
            *distance = hit.distance;
        return true;
    }

    double meanEdge() const
    {
        double sum = 0.0;
        size_t count = 0;
        for (geo::Index e = 0; e < mesh.edgeCapacity(); ++e)
            if (mesh.edgeValid(e))
            {
                sum += glm::length(mesh.position(mesh.edgePoint(e, 1)) - mesh.position(mesh.edgePoint(e, 0)));
                ++count;
            }
        return count ? sum / double(count) : 1.0;
    }
};

std::string sideSuffix(const glm::dvec3& origin, int axis, double tolerance)
{
    if (axis < 0 || std::abs(origin[axis]) <= tolerance)
        return {};
    return origin[axis] > 0.0 ? "_l" : "_r";
}

/** The run's direction leaving the given end, a short way along it. */
glm::dvec3 alongFrom(const Path& p, int end, double distance)
{
    const double len = p.length();
    const double s = std::clamp(distance, 0.0, len);
    return p.pointAt(end == 0 ? s : len - s);
}

struct Line
{
    const ResolvedRun* run = nullptr;
    int knot[2] = {-1, -1};
    int along[2] = {-1, -1}; // index into its knots' along lists
};

json bindNet(Graph& g, NodeId hubId, NodeId bodyId, Node& layoutNode, BindOptions o)
{
    LILA_ZONE();
    const std::shared_ptr<const Resolved> resolved = resolvedLayout(g, layoutNode.id());
    if (!resolved || resolved->runs().empty())
        throw CommandError("softNet: the Panel Layout has no lines");
    Node& body = *g.node(bodyId);
    const NodeType* hubType = types::find(kSoftNetKnotsType);
    Node* hub = hubId != kNoNode ? g.node(hubId) : nullptr;
    auto hubAttr = [&](const char* n) { return hubType->attrIndex(n); };
    if (hub)
    {
        if (!o.spacing)
            o.spacing = hub->as<double>(hubAttr("spacing"));
        if (!o.maxDistance)
            o.maxDistance = hub->as<double>(hubAttr("maxDistance"));
    }

    // The deformers: the body's first, then every mesh bound with it, existing ones kept.
    std::vector<NodeId> meshes{bodyId}, made;
    std::map<NodeId, NodeId> deformerOf;
    if (hub)
        for (NodeId d : netDeformers(g, hub->id()))
            if (const NodeId m = modifiers::meshOf(g, d); m != kNoNode)
            {
                deformerOf[m] = d;
                if (m != bodyId && std::find(meshes.begin(), meshes.end(), m) == meshes.end())
                    meshes.push_back(m);
            }
    for (NodeId m : o.extraMeshes)
        if (std::find(meshes.begin(), meshes.end(), m) == meshes.end())
            meshes.push_back(m);
    for (NodeId m : meshes)
        if (!deformerOf.contains(m))
        {
            const std::string name = m == bodyId ? o.name : o.name + "_" + g.node(m)->name();
            const std::vector<NodeId> d = deformers::addDeformer(g, kSoftNetType, {m}, name);
            if (d.empty())
                throw CommandError("softNet: could not add the deformer to " + g.node(m)->path());
            deformerOf[m] = d.front();
            made.push_back(d.front());
        }
    g.evaluate();
    Node& bodyDeformer = *g.node(deformerOf[bodyId]);
    const MeshPtr bodyIn = bodyDeformer.as<MeshPtr>(DeformerNode::kInMesh);
    if (!bodyIn)
        throw CommandError("softNet: " + body.path() + " has no mesh to bind");
    // The knot frames ride the surface the net shapes, not its result, so a knot moves its lines once.
    const std::optional<PlugRef> upstream = g.source({bodyDeformer.id(), DeformerNode::kInMesh, -1});
    if (!upstream || upstream->node == bodyId)
        throw CommandError("softNet: " + body.path() + " has nothing under the net to ride; add it after a skin, a modifier or the Panel Layout");

    // Knots: every junction a usable line touches, in id order (mirror images after their sources).
    std::map<uint32_t, const ResolvedJunction*> junctionById;
    for (const ResolvedJunction& j : resolved->junctions())
        junctionById[j.id] = &j;
    std::vector<const ResolvedRun*> runs;
    for (const ResolvedRun& r : resolved->runs())
        if (r.path.points.size() >= 2 && r.path.length() > 0.0 && junctionById.contains(r.a) && junctionById.contains(r.b))
            runs.push_back(&r);
    std::sort(runs.begin(), runs.end(), [](const ResolvedRun* a, const ResolvedRun* b) {
        return std::pair(a->source(), a->mirrored()) < std::pair(b->source(), b->mirrored());
    });
    if (runs.empty())
        throw CommandError("softNet: the Panel Layout has no lines between junctions");
    std::vector<uint32_t> junctions;
    for (const ResolvedRun* r : runs)
        for (uint32_t j : {r->a, r->b})
            if (std::find(junctions.begin(), junctions.end(), j) == junctions.end())
                junctions.push_back(j);
    std::sort(junctions.begin(), junctions.end(), [](uint32_t a, uint32_t b) { return std::pair(a & ~kMirrored, a & kMirrored) < std::pair(b & ~kMirrored, b & kMirrored); });
    std::map<uint32_t, int> knotOf;
    for (size_t k = 0; k < junctions.size(); ++k)
        knotOf[junctions[k]] = int(k);

    const NetSurface bodySurface(*bodyIn);
    const std::vector<glm::dvec3> bodyPts(bodyIn->positions().begin(), bodyIn->positions().end());
    const double edge = bodySurface.meanEdge();
    const double step = o.spacing.value_or(0.0) > 0.0 ? *o.spacing : std::max(resolved->spacing() * 2.0, edge * 0.5);
    const geo::Bounds box = bodyIn->bounds();
    const double autoReach = std::max(glm::length(box.max - box.min) / 6.0, edge);

    // Each knot's rig: its surface point, and a point a short way along every line end at it.
    NetBind net;
    net.knots.resize(junctions.size());
    std::vector<Line> lines(runs.size());
    for (size_t k = 0; k < junctions.size(); ++k)
    {
        sn::KnotRig& rig = net.knots[k];
        rig.origin = junctionById[junctions[k]]->position;
        if (!bodySurface.sample(rig.origin, rig.at))
            throw CommandError("softNet: a junction lies off " + body.path());
    }
    for (size_t r = 0; r < runs.size(); ++r)
        for (int e = 0; e < 2; ++e)
        {
            const int k = knotOf[e == 0 ? runs[r]->a : runs[r]->b];
            lines[r].run = runs[r];
            lines[r].knot[e] = k;
            sn::KnotRig& rig = net.knots[size_t(k)];
            const double d = std::min(runs[r]->path.length() * 0.25, std::max(step, edge * 2.0));
            sn::SurfacePoint p;
            bodySurface.sample(alongFrom(runs[r]->path, e, d), p);
            lines[r].along[e] = int(rig.along.size());
            rig.along.push_back(p);
        }
    std::vector<int> junctionKnots;
    for (size_t k = 0; k < net.knots.size(); ++k)
    {
        sn::KnotRig& rig = net.knots[k];
        rig.degree = int(rig.along.size());
        rig.rest = sn::restFrame(rig, bodyPts);
        const glm::dvec3 o3 = sn::at(rig.at, bodyPts);
        for (const sn::SurfacePoint& p : rig.along)
        {
            glm::dvec3 v = sn::at(p, bodyPts) - o3;
            v -= rig.rest[2] * glm::dot(v, rig.rest[2]);
            rig.restDirections.push_back(glm::length(v) > 1e-12 ? glm::transpose(rig.rest) * glm::normalize(v) : glm::dvec3(1, 0, 0));
        }
        if (rig.degree >= 3)
            junctionKnots.push_back(int(k));
    }
    // Knots off junctions take the turn of the nearest junctions, by inverse squared distance.
    for (size_t k = 0; k < net.knots.size(); ++k)
    {
        sn::KnotRig& rig = net.knots[k];
        if (rig.degree >= 3 || junctionKnots.empty())
            continue;
        std::vector<std::pair<double, int>> near;
        for (int j : junctionKnots)
            near.emplace_back(glm::length(net.knots[size_t(j)].origin - rig.origin), j);
        std::sort(near.begin(), near.end());
        near.resize(std::min<size_t>(near.size(), 4));
        double total = 0.0;
        for (const auto& [d, j] : near)
            total += 1.0 / std::max(d * d, 1e-12);
        for (const auto& [d, j] : near)
            rig.blend.emplace_back(j, (1.0 / std::max(d * d, 1e-12)) / total);
    }

    // Tangent handles: one per line end, a third of the line along its direction at the knot.
    MeshBind shared;
    shared.knots = net.knots.size();
    for (size_t r = 0; r < lines.size(); ++r)
    {
        sn::Run run;
        for (int e = 0; e < 2; ++e)
        {
            const int k = lines[r].knot[e];
            run.knot[size_t(e)] = k;
            run.tangent[size_t(e)] = int(shared.tangentRest.size());
            shared.tangentKnot.push_back(k);
            shared.tangentRest.push_back(net.knots[size_t(k)].restDirections[size_t(lines[r].along[e])] * (lines[r].run->path.length() / 3.0));
        }
        shared.runs.push_back(run);
    }

    // Every mesh's samples: the knots' and the lines', each on that mesh's own surface.
    const glm::dmat4 bodyWorld = worldOf(body);
    const double maxDistance = o.maxDistance.value_or(0.0) > 0.0 ? *o.maxDistance : o.reach.value_or(0.0) > 0.0 ? *o.reach * 0.5 : autoReach * 0.5;
    json perMesh = json::array();
    size_t bodySamples = 0;
    for (size_t mi = 0; mi < meshes.size(); ++mi)
    {
        Node& d = *g.node(deformerOf[meshes[mi]]);
        const MeshPtr in = d.as<MeshPtr>(DeformerNode::kInMesh);
        if (!in)
            throw CommandError("softNet: " + g.node(meshes[mi])->path() + " has no mesh to bind");
        const bool isBody = mi == 0;
        const glm::dmat4 toMesh = isBody ? glm::dmat4(1.0) : glm::inverse(worldOf(*g.node(meshes[mi]))) * bodyWorld;
        const NetSurface surface(*in);
        MeshBind b = shared;
        b.points = in->pointCapacity();
        b.edge = surface.meanEdge();
        b.autoReach = autoReach;
        auto add = [&](const glm::dvec3& bodySpace, sn::Sample s) {
            double dist = 0.0;
            if (isBody)
            {
                if (surface.sample(bodySpace, s.at))
                    b.samples.push_back(s);
                return;
            }
            if (surface.sample(glm::dvec3(toMesh * glm::dvec4(bodySpace, 1.0)), s.at, &dist) && dist <= maxDistance)
                b.samples.push_back(s);
        };
        for (size_t k = 0; k < net.knots.size(); ++k)
            add(net.knots[k].origin, {.run = -1, .knot = int(k)});
        for (size_t r = 0; r < lines.size(); ++r)
        {
            const Path& path = lines[r].run->path;
            const std::vector<glm::dvec3> along = resample(path.points, step);
            const double total = polylineLength(along);
            double s = 0.0;
            for (size_t i = 0; i < along.size(); ++i)
            {
                if (i > 0)
                    s += glm::length(along[i] - along[i - 1]);
                // The ends are the knots' own samples.
                if (i == 0 || i + 1 == along.size())
                    continue;
                add(along[i], {.run = int(r), .t = total > 0.0 ? s / total : 0.0});
            }
        }
        if (isBody && b.samples.empty())
            throw CommandError("softNet: no line sample lies on " + body.path());
        if (isBody)
            bodySamples = b.samples.size();
        g.setValue({d.id(), d.type().attrIndex("binding"), -1}, b.toJson().dump());
        g.setValue({d.id(), d.type().attrIndex("meshIndex"), -1}, int64_t(mi));
        if (o.reach || std::find(made.begin(), made.end(), d.id()) != made.end())
            g.setValue({d.id(), d.type().attrIndex("reach"), -1}, o.reach.value_or(0.0));
        if (o.stiffness || std::find(made.begin(), made.end(), d.id()) != made.end())
            g.setValue({d.id(), d.type().attrIndex("stiffness"), -1}, o.stiffness.value_or(1.0));
        perMesh.push_back({{"mesh", g.node(meshes[mi])->path()}, {"deformer", d.path()}, {"samples", b.samples.size()}});
    }

    // The knots group, standing where the body stands and riding the surface under the net.
    if (!hub)
        hub = g.createNode(*hubType, o.name + "_knots");
    g.setValue({hub->id(), hubAttr("binding"), -1}, net.toJson().dump());
    g.setValue({hub->id(), hubAttr("meshWorld"), -1}, bodyWorld);
    g.setValue({hub->id(), hubAttr("layout"), -1}, layoutNode.path());
    g.setValue({hub->id(), hubAttr("spacing"), -1}, o.spacing.value_or(0.0));
    g.setValue({hub->id(), hubAttr("maxDistance"), -1}, o.maxDistance.value_or(0.0));
    if (const auto src = g.source({hub->id(), hubAttr("inMesh"), -1}); !src || *src != *upstream)
        g.connect(*upstream, {hub->id(), hubAttr("inMesh"), -1}, true);

    // Knot controls, kept by junction so their values and keys survive a rebind.
    const NodeType* knotType = types::find(kSoftNetKnotType);
    const NodeType* tangentType = types::find(kSoftNetTangentType);
    const int kIndex = knotType->attrIndex("index"), kJunction = knotType->attrIndex("junction"), kDegree = knotType->attrIndex("degree"),
              kFrames = knotType->attrIndex("frames");
    std::map<uint32_t, NodeId> knotNodeOf;
    std::map<std::pair<int64_t, int64_t>, NodeId> tangentNodeOf;
    for (NodeId c : std::vector<NodeId>(g.children(hub->id())))
        if (const Node* n = g.node(c); n && n->type().name == kSoftNetKnotType)
        {
            knotNodeOf[uint32_t(n->as<int64_t>(kJunction))] = c;
            for (NodeId t : g.children(c))
                if (const Node* tn = g.node(t); tn && tn->type().name == kSoftNetTangentType)
                    tangentNodeOf[{tn->as<int64_t>(tangentType->attrIndex("run")), tn->as<int64_t>(tangentType->attrIndex("end"))}] = t;
        }
    const int axis = resolved->layout().mirrorAxis;
    const double sideTolerance = std::max(edge * 0.25, 1e-6);
    // A junction's partner across the mirror: its mirror image, else the knot standing at its reflection.
    auto partnered = [&](size_t k) {
        const uint32_t j = junctions[k];
        if (knotOf.contains(j ^ kMirrored))
            return true;
        if (axis < 0)
            return false;
        glm::dvec3 m = net.knots[k].origin;
        m[axis] = -m[axis];
        for (size_t o2 = 0; o2 < net.knots.size(); ++o2)
            if (o2 != k && glm::length(net.knots[o2].origin - m) < step)
                return true;
        return false;
    };
    std::vector<NodeId> knotNodes(junctions.size(), kNoNode);
    json knotsOut = json::array();
    const int showAll = hubAttr("showAllKnots");
    for (size_t k = 0; k < junctions.size(); ++k)
    {
        const uint32_t j = junctions[k];
        NodeId id = kNoNode;
        if (auto it = knotNodeOf.find(j); it != knotNodeOf.end())
        {
            id = it->second;
            knotNodeOf.erase(it);
        }
        else
        {
            const std::string suffix = partnered(k) ? sideSuffix(net.knots[k].origin, axis, sideTolerance) : std::string();
            id = g.createNode(*knotType, o.name + "_k" + std::to_string(j & ~kMirrored) + suffix, hub->id())->id();
        }
        knotNodes[k] = id;
        g.setValue({id, kIndex, -1}, int64_t(k));
        g.setValue({id, kJunction, -1}, int64_t(j));
        g.setValue({id, kDegree, -1}, int64_t(net.knots[k].degree));
        if (!g.source({id, kFrames, -1}))
            g.connect({hub->id(), hubAttr("frames"), -1}, {id, kFrames, -1});
        // Junctions and line ends show; knots inside a line show with showAllKnots. A lone loop's knot is its only control.
        bool loneLoop = false;
        for (const Line& l : lines)
            loneLoop = loneLoop || (l.knot[0] == int(k) && l.knot[1] == int(k) && net.knots[k].degree == 2);
        const bool exposed = net.knots[k].degree != 2 || loneLoop;
        const PlugRef vis{id, Transform::kVisibility, -1};
        const std::optional<PlugRef> visSrc = g.source(vis);
        if (exposed && visSrc)
            g.disconnect(vis);
        if (exposed && !visSrc)
            g.setValue(vis, true);
        if (!exposed && !visSrc)
            g.connect({hub->id(), showAll, -1}, vis);
        knotsOut.push_back(g.node(id)->path());
    }
    for (const auto& [j, id] : knotNodeOf)
        g.deleteNode(id);

    // Tangent handles under their knots, kept by run end.
    std::vector<NodeId> tangentNodes(shared.tangentRest.size(), kNoNode);
    for (size_t r = 0; r < lines.size(); ++r)
        for (int e = 0; e < 2; ++e)
        {
            const size_t t = size_t(shared.runs[r].tangent[size_t(e)]);
            const NodeId knotNode = knotNodes[size_t(lines[r].knot[e])];
            const std::pair<int64_t, int64_t> key{int64_t(lines[r].run->id), int64_t(e)};
            NodeId id = kNoNode;
            if (auto it = tangentNodeOf.find(key); it != tangentNodeOf.end())
            {
                if (g.node(it->second) && g.parent(it->second) == knotNode)
                    id = it->second;
                else if (g.node(it->second))
                    g.deleteNode(it->second);
                tangentNodeOf.erase(it);
            }
            if (id == kNoNode)
            {
                id = g.createNode(*tangentType, g.node(knotNode)->name() + "_t" + std::to_string(lines[r].run->source()) + (e ? "b" : "a") +
                                                    (lines[r].run->mirrored() ? "m" : ""),
                                  knotNode)
                         ->id();
                g.setValue({id, tangentType->attrIndex("run"), -1}, key.first);
                g.setValue({id, tangentType->attrIndex("end"), -1}, key.second);
                g.setValue({id, tangentType->attrIndex("drawSize"), -1}, std::max(edge, 0.25));
            }
            g.setValue({id, Transform::kOffsetParentMatrix, -1}, glm::translate(glm::dmat4(1.0), shared.tangentRest[t]));
            tangentNodes[t] = id;
        }
    for (const auto& [key, id] : tangentNodeOf)
        if (g.node(id))
            g.deleteNode(id);

    // Every deformer reads the frames and every control.
    for (NodeId m : meshes)
    {
        Node& d = *g.node(deformerOf[m]);
        const int frames = d.type().attrIndex("frames"), spaces = d.type().attrIndex("spaces");
        const int knotMatrix = d.type().attrIndex("knotMatrix"), tangentMatrix = d.type().attrIndex("tangentMatrix");
        if (!g.source({d.id(), frames, -1}))
            g.connect({hub->id(), hubAttr("frames"), -1}, {d.id(), frames, -1});
        if (!g.source({d.id(), spaces, -1}))
            g.connect({hub->id(), hubAttr("spaces"), -1}, {d.id(), spaces, -1});
        for (int attr : {knotMatrix, tangentMatrix})
        {
            std::vector<int> used;
            for (const auto& [i, v] : d.elements(attr))
                used.push_back(i);
            for (int i : used)
                g.disconnect({d.id(), attr, -1, i});
        }
        for (size_t k = 0; k < knotNodes.size(); ++k)
            g.connect({knotNodes[k], Transform::kMatrix, -1}, {d.id(), knotMatrix, -1, int(k)});
        for (size_t t = 0; t < tangentNodes.size(); ++t)
            g.connect({tangentNodes[t], Transform::kMatrix, -1}, {d.id(), tangentMatrix, -1, int(t)});
    }
    g.evaluate();
    return {{"softNet", g.node(deformerOf[bodyId])->path()},
            {"knotsGroup", hub->path()},
            {"knots", knotsOut},
            {"tangents", tangentNodes.size()},
            {"samples", bodySamples},
            {"meshes", perMesh}};
}

Node& meshArg(Graph& g, const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!n.type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + n.path() + " is not a mesh");
    (void)g;
    return n;
}
} // namespace

void registerSoftNetBindCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "softNet",
        .doc = "Make a Soft Net on a mesh from its Panel Layout: the layout's lines become Bezier curves bound to the surface as it "
               "is (no cuts), every junction a knot control riding the surface under the net, every line end a tangent handle "
               "under its knot. Added on top of the stack, so it shapes the skinned mesh. meshes binds garments and props to the "
               "same lines. On a mesh that has a net already it rebinds it (softNetRebind). Returns {softNet, knotsGroup, knots, "
               "tangents, samples, meshes}.",
        .positionalDoc = "mesh",
        .flags = {{"layout", "ly", T::String, nullptr, "The Panel Layout whose lines make the net; default the mesh's topmost."},
                  {"meshes", "ms", T::List, nullptr, "Other meshes the same lines move: garments, props."},
                  {"reach", "r", T::Float, nullptr, "How far from a line the surface follows it, cm; 0 is a sixth of the mesh's size.", {}, {}, 0.0},
                  {"stiffness", "st", T::Float, nullptr, "How closely the surface meets the lines (default 1).", {}, {}, 0.0},
                  {"spacing", "sp", T::Float, nullptr, "Distance between line samples, cm; 0 is twice the layout's.", {}, {}, 0.0},
                  {"maxDistance", "md", T::Float, nullptr, "How far a garment or prop may lie from a line and still follow it, cm; 0 is half the reach.", {}, {}, 0.0},
                  {"name", "n", T::String, nullptr, "Name for the deformer and its controls."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.objects().empty())
                throw CommandError("softNet: give a mesh");
            Node& mesh = meshArg(g, a.objects().front(), "softNet");
            BindOptions o;
            if (a.has("reach"))
                o.reach = a.get<double>("reach");
            if (a.has("stiffness"))
                o.stiffness = a.get<double>("stiffness");
            if (a.has("spacing"))
                o.spacing = a.get<double>("spacing");
            if (a.has("maxDistance"))
                o.maxDistance = a.get<double>("maxDistance");
            if (a.has("name"))
                o.name = a.get<std::string>("name");
            if (a.has("meshes"))
                for (const json& m : a.get<json>("meshes"))
                    o.extraMeshes.push_back(meshArg(g, m.get<std::string>(), "softNet").id());
            const NodeId hub = netOf(g, mesh.id());
            Node* layoutNode = findLayoutNode(g, a.has("layout") ? nodeNamed(a.get<std::string>("layout"), "softNet").id() : mesh.id());
            if (!layoutNode && hub != kNoNode)
                layoutNode = g.find(g.node(hub)->as<std::string>(g.node(hub)->type().attrIndex("layout")));
            if (!layoutNode)
                throw CommandError("softNet: " + mesh.path() + " has no Panel Layout; draw its lines with the Layout Pencil first");
            if (hub != kNoNode)
            {
                const std::vector<NodeId> ms = netMeshes(g, hub);
                if (!ms.empty() && ms.front() != mesh.id())
                    throw CommandError("softNet: " + mesh.path() + " is bound to the net on " + g.node(ms.front())->path() + "; rebind that one");
                o.name = g.node(hub)->name().ends_with("_knots") ? g.node(hub)->name().substr(0, g.node(hub)->name().size() - 6) : o.name;
            }
            return bindNet(g, hub, mesh.id(), *layoutNode, o);
        },
        .examples = {"softNet(\"Mochi\", reach=4.0)", "softNet(\"Body\", meshes=[\"Shirt\", \"Belt\"])"},
    });

    ops::registerCommand({
        .name = "softNetRebind",
        .doc = "Bind a Soft Net again after its Panel Layout changed: knot controls (with their values and keys) stay on the "
               "junctions that are still there, knots are added for new junctions and removed with removed ones, and every bound "
               "mesh gets its samples again. Returns what softNet returns.",
        .positionalDoc = "net (its knots group, a knot, the deformer or a mesh it binds)",
        .flags = {{"layout", "ly", T::String, nullptr, "Bind from this Panel Layout instead of the one it was made from."},
                  {"meshes", "ms", T::List, nullptr, "More meshes to bind to the same lines."},
                  {"spacing", "sp", T::Float, nullptr, "Distance between line samples, cm; 0 is twice the layout's.", {}, {}, 0.0},
                  {"maxDistance", "md", T::Float, nullptr, "How far a garment or prop may lie from a line and still follow it, cm.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.objects().empty())
                throw CommandError("softNetRebind: give a Soft Net");
            const NodeId hub = netOf(g, nodeNamed(a.objects().front(), "softNetRebind").id());
            if (hub == kNoNode)
                throw CommandError("softNetRebind: " + a.objects().front() + " is not part of a Soft Net");
            const std::vector<NodeId> ms = netMeshes(g, hub);
            if (ms.empty())
                throw CommandError("softNetRebind: the net binds no mesh");
            Node* layoutNode = a.has("layout") ? findLayoutNode(g, nodeNamed(a.get<std::string>("layout"), "softNetRebind").id())
                                               : g.find(g.node(hub)->as<std::string>(g.node(hub)->type().attrIndex("layout")));
            if (!layoutNode)
                layoutNode = findLayoutNode(g, ms.front());
            if (!layoutNode)
                throw CommandError("softNetRebind: no Panel Layout to bind from");
            BindOptions o;
            const std::string hubName = g.node(hub)->name();
            o.name = hubName.ends_with("_knots") ? hubName.substr(0, hubName.size() - 6) : hubName;
            if (a.has("spacing"))
                o.spacing = a.get<double>("spacing");
            if (a.has("maxDistance"))
                o.maxDistance = a.get<double>("maxDistance");
            if (a.has("meshes"))
                for (const json& m : a.get<json>("meshes"))
                    o.extraMeshes.push_back(meshArg(g, m.get<std::string>(), "softNetRebind").id());
            return bindNet(g, hub, ms.front(), *layoutNode, o);
        },
        .examples = {"softNetRebind(\"SoftNet_knots\")"},
    });
}
} // namespace lila::layout
