#include "scene/FloatPrecision.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_set>

namespace lila::scene::floatPrecision
{
namespace
{
constexpr const char* kPrecisionSource = "floatPrecision";

/** The last reach worked out for a node, kept while its mesh and its world rotation and scale stay the same. */
struct PrecisionBounds
{
    const geo::Mesh* mesh = nullptr;
    geo::Bounds bounds;
    glm::dmat3 linear{0.0};
    double reach = -1.0; // for linear; below 0 until worked out
};

struct PrecisionWatch
{
    std::map<NodeId, PrecisionBounds> bounds;
    std::unordered_set<NodeId> pending;
    bool all = false;
    bool queued = false;
    bool running = false; // checks evaluate meshes; their own notifications don't queue more
};

PrecisionWatch& precisionWatch()
{
    static PrecisionWatch w;
    return w;
}

MeshPtr precisionMesh(Graph& g, NodeId id)
{
    const PlugValue v = g.value({id, MeshNode::kOutMesh, -1});
    const MeshPtr* m = std::get_if<MeshPtr>(&v);
    return m ? *m : nullptr;
}

std::string distanceText(double cm) { return cm >= 1.0e5 ? std::format("{:.0f} km", cm / 1.0e5) : std::format("{:.0f} m", cm / 100.0); }

/** An upper bound on reach from the mesh's bounds alone, so most meshes never walk their points. */
double reachBound(Graph& g, NodeId id)
{
    const MeshPtr mesh = precisionMesh(g, id);
    if (!mesh)
        return 0.0;
    PrecisionBounds& cache = precisionWatch().bounds[id];
    if (cache.mesh != mesh.get())
        cache = {mesh.get(), mesh->bounds()};
    if (!cache.bounds.valid())
        return 0.0;
    const glm::dmat3 linear(std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})));
    double r = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        const glm::dvec3 corner((i & 1) ? cache.bounds.max.x : cache.bounds.min.x, (i & 2) ? cache.bounds.max.y : cache.bounds.min.y,
                                (i & 4) ? cache.bounds.max.z : cache.bounds.min.z);
        r = std::max(r, glm::length(linear * corner));
    }
    return r;
}

void checkPrecision(Graph& g, NodeId id)
{
    diagnostics::clear(kPrecisionSource, id);
    const Node* n = g.node(id);
    if (!n || !dynamic_cast<const MeshNode*>(n))
    {
        precisionWatch().bounds.erase(id);
        return;
    }
    if (reachBound(g, id) <= kWarnReach)
        return;
    const double r = reach(g, id);
    if (r <= kWarnReach)
        return;
    diagnostics::Issue issue;
    issue.source = kPrecisionSource;
    issue.severity = diagnostics::Severity::Warning;
    issue.target = n->path();
    issue.message = std::format("Reaches {} from its origin, so the viewport rounds its surface to about {:.1f} mm and it "
                                "jitters as the camera moves. Mesh > Split into Tiles fixes it.",
                                distanceText(r), jitter(r) * 10.0);
    issue.fix = std::format("tileMesh(\"{}\")", n->path());
    diagnostics::report(std::move(issue));
}

void runPrecisionCheck()
{
    LILA_ZONE();
    PrecisionWatch& w = precisionWatch();
    w.queued = false;
    w.running = true;
    struct Done
    {
        bool& flag;
        ~Done() { flag = false; }
    } done{w.running};
    Graph& g = Graph::current();
    if (w.all)
    {
        w.all = false;
        w.pending.clear();
        diagnostics::clear(kPrecisionSource);
        for (NodeId id : g.nodes())
            if (const Node* n = g.node(id); n && dynamic_cast<const MeshNode*>(n))
                checkPrecision(g, id);
        return;
    }
    std::unordered_set<NodeId> ids = std::move(w.pending);
    w.pending.clear();
    for (NodeId id : ids)
        checkPrecision(g, id);
}

void schedulePrecisionCheck()
{
    PrecisionWatch& w = precisionWatch();
    if (w.queued)
        return;
    w.queued = true;
    mainq::post("float precision check", runPrecisionCheck);
}

void watchPrecision()
{
    Graph::current().addListener([](Change c, NodeId id) {
        PrecisionWatch& w = precisionWatch();
        if (w.running)
            return;
        switch (c)
        {
        case Change::Reset:
        case Change::Cleared:
            w.all = true;
            w.bounds.clear();
            break;
        case Change::Deleted:
            w.bounds.erase(id);
            diagnostics::clear(kPrecisionSource, id);
            return;
        case Change::Created:
        case Change::Reparented:
        case Change::Value:
        {
            Graph& g = Graph::current();
            const Node* n = g.node(id);
            if (!n || !n->type().dag)
                return;
            w.pending.insert(id);
            // A transform's change moves everything under it.
            if (c != Change::Created)
                for (NodeId d : g.descendants(id))
                    w.pending.insert(d);
            break;
        }
        default:
            return;
        }
        schedulePrecisionCheck();
    });
}

/** The cell of a grid with that edge holding p. */
glm::ivec3 tileCellOf(const glm::dvec3& p, const glm::dvec3& origin, double size)
{
    return glm::ivec3(glm::floor((p - origin) / size));
}

struct TileCellLess
{
    bool operator()(const glm::ivec3& a, const glm::ivec3& b) const
    {
        return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
    }
};

/** The mesh in world space, in double, facing out under a mirrored transform. */
geo::Mesh tileWorldMesh(const geo::Mesh& local, const glm::dmat4& world)
{
    geo::Mesh m = local;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(world * glm::dvec4(m.position(p), 1.0)));
    if (glm::determinant(glm::dmat3(world)) < 0.0)
    {
        std::vector<geo::Index> all;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                all.push_back(f);
        geo::flipNormals(m, all);
    }
    return m;
}

/**
 * Cells the faces' bounds cover, counted up to limit, so a mesh that would make too many tiles is refused before any
 * cutting. An upper bound on the tiles the cut makes.
 */
int64_t tileCountAtLeast(const geo::Mesh& m, const glm::dvec3& origin, double size, const glm::ivec3& cells, int64_t limit)
{
    std::set<glm::ivec3, TileCellLess> seen;
    for (geo::Index f = 0; f < m.faceCapacity() && int64_t(seen.size()) < limit; ++f)
    {
        if (!m.faceValid(f))
            continue;
        geo::Bounds fb;
        for (geo::Index c : m.faceCorners(f))
            fb.extend(m.position(m.cornerPoint(c)));
        const glm::ivec3 lo = glm::clamp(tileCellOf(fb.min, origin, size), glm::ivec3(0), cells - 1);
        const glm::ivec3 hi = glm::clamp(tileCellOf(fb.max, origin, size), glm::ivec3(0), cells - 1);
        if (int64_t(hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1) >= limit)
            return limit;
        for (int x = lo.x; x <= hi.x; ++x)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int z = lo.z; z <= hi.z; ++z)
                    seen.insert({x, y, z});
    }
    return int64_t(seen.size());
}

glm::dvec3 tileFaceCentre(const geo::Mesh& m, geo::Index f)
{
    glm::dvec3 sum(0.0);
    const std::vector<geo::Index> corners = m.faceCorners(f);
    for (geo::Index c : corners)
        sum += m.position(m.cornerPoint(c));
    return sum / double(std::max<size_t>(corners.size(), 1));
}
} // namespace

double reach(Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    if (!n || !dynamic_cast<const MeshNode*>(n))
        return 0.0;
    const MeshPtr mesh = precisionMesh(g, id);
    if (!mesh)
        return 0.0;
    const glm::dmat3 linear(std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})));
    PrecisionBounds& cache = precisionWatch().bounds[id];
    if (cache.mesh != mesh.get())
        cache = {mesh.get(), mesh->bounds()};
    else if (cache.linear == linear && cache.reach >= 0.0)
        return cache.reach;
    cache.linear = linear;
    double r2 = 0.0;
    for (geo::Index p = 0; p < mesh->pointCapacity(); ++p)
        if (mesh->pointValid(p))
        {
            const glm::dvec3 q = linear * mesh->position(p);
            r2 = std::max(r2, glm::dot(q, q));
        }
    cache.reach = std::sqrt(r2);
    return cache.reach;
}

double jitter(double reach) { return reach * std::ldexp(1.0, -23); }

void registerCommands()
{
    if (ops::find("tileMesh"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "tileMesh",
        .doc = "Split meshes into a grid of tile objects, each with its origin at its centre, so none reaches far enough from its "
               "origin for the viewport's float precision to make it jitter. Faces crossing a tile edge are cut there. The tiles "
               "go in a new group at the top of the scene, with the mesh's transform baked in. Returns the tiles.",
        .positionalDoc = "objects",
        .flags = {
            {"tileSize", "ts", T::Float, kDefaultTileSize, "Tile edge, cm.", {}, {}, 1.0},
            {"maxTiles", "mt", T::Int, 4096, "Refuse a mesh that would make more tiles than this.", {}, {}, 1.0},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            const double size = a.get<double>("tileSize", kDefaultTileSize);
            const int64_t maxTiles = a.get<int64_t>("maxTiles", 4096);
            json out = json::array();
            std::vector<NodeId> groups;
            g.evaluate();
            for (const std::string& name : meshObjects(a, "tileMesh"))
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("tileMesh: no object " + name);
                const MeshPtr local = precisionMesh(g, n->id());
                if (!local)
                    throw CommandError("tileMesh: " + name + " has no mesh");
                geo::Mesh m = tileWorldMesh(*local, std::get<glm::dmat4>(g.value({n->id(), Transform::kWorldMatrix, -1})));
                const geo::Bounds b = m.bounds();
                if (!b.valid())
                    continue;
                if (glm::any(glm::greaterThan(b.size() / size, glm::dvec3(1.0e5))))
                    throw CommandError(std::format("tileMesh: {} is too big for tiles of {}; raise tileSize", name, distanceText(size)));
                const glm::dvec3 origin = b.min;
                const glm::ivec3 cells = glm::max(glm::ivec3(glm::ceil(b.size() / size)), glm::ivec3(1));
                if (tileCountAtLeast(m, origin, size, cells, maxTiles + 1) > maxTiles)
                    throw CommandError(std::format("tileMesh: {} would make more than {} tiles of {}; raise tileSize or maxTiles", name, maxTiles,
                                                   distanceText(size)));
                for (int axis = 0; axis < 3; ++axis)
                    for (int k = 1; k < cells[axis]; ++k)
                    {
                        glm::dvec3 at = origin, normal(0.0);
                        at[axis] += k * size;
                        normal[axis] = 1.0;
                        geo::cutAlongPlane(m, at, normal);
                    }

                std::map<glm::ivec3, std::vector<geo::Index>, TileCellLess> byCell;
                for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                    if (m.faceValid(f))
                        byCell[glm::clamp(tileCellOf(tileFaceCentre(m, f), origin, size), glm::ivec3(0), cells - 1)].push_back(f);

                const std::string base = n->name();
                Node* group = g.createNode(*types::find("transform"), base + "Tiles", kNoNode, true);
                std::vector<NodeId> made;
                for (auto& [cell, faces] : byCell)
                {
                    geo::Mesh tile = geo::extractFaces(m, faces);
                    const glm::dvec3 centre = tile.bounds().center();
                    for (geo::Index p = 0; p < tile.pointCapacity(); ++p)
                        if (tile.pointValid(p))
                            tile.setPosition(p, tile.position(p) - centre);
                    Node* t = g.createNode(n->type(), base + "Tile", group->id(), true);
                    g.setValue({t->id(), Transform::kTranslate, -1}, centre);
                    g.setValue({t->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(tile))));
                    made.push_back(t->id());
                    out.push_back(t->path());
                }
                isolation::inherit(made, {n->id()});
                g.deleteNode(n->id());
                groups.push_back(group->id());
            }
            g.select(groups);
            return out;
        },
    });
    watchPrecision();
}
} // namespace lila::scene::floatPrecision
