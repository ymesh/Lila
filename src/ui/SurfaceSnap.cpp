#include "ui/SurfaceSnap.h"

#include "core/Events.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Isolate.h"

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>

namespace lila::ui::surfaceSnap
{
namespace
{
using scene::NodeId;

struct Entry
{
    std::weak_ptr<const geo::Mesh> source;
    std::shared_ptr<const geo::MeshBvh> bvh;
};

/** Object-space BVH of a mesh snapshot; snapshots are immutable, so a new one after an edit misses and rebuilds. */
std::shared_ptr<const geo::MeshBvh> bvhFor(const scene::MeshPtr& mesh)
{
    // Never destroyed: at exit the meshes' owners (extension DLLs among them) may already be unloaded.
    static auto& cache = *new std::unordered_map<const geo::Mesh*, Entry>();
    auto it = cache.find(mesh.get());
    if (it != cache.end() && it->second.source.lock() == mesh)
        return it->second.bvh;
    if (cache.size() > 32)
        std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });
    auto bvh = std::make_shared<geo::MeshBvh>(*mesh);
    cache[mesh.get()] = {mesh, bvh};
    return bvh;
}

std::vector<NodeId>& designated()
{
    static std::vector<NodeId> ids;
    return ids;
}

std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> fns;
    return fns;
}

void notify()
{
    // A copy: a listener may remove itself.
    auto fns = listeners();
    for (const auto& [id, fn] : fns)
        fn();
}

bool contains(const std::vector<NodeId>& ids, NodeId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

struct Target
{
    NodeId id;
    scene::MeshPtr mesh;
    glm::dmat4 world;
};

std::optional<Target> target(const scene::Graph& g, NodeId id)
{
    const scene::Node* n = g.node(id);
    if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id) || !isolate::shows(scene::isolation::active(), id))
        return std::nullopt;
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh || mesh->numFaces() == 0)
        return std::nullopt;
    return Target{id, mesh, n->as<glm::dmat4>(scene::Transform::kWorldMatrix)};
}

/** The snap surfaces if any are set (and designated allows them), else every visible mesh, minus exclude. */
std::vector<Target> targets(const std::vector<NodeId>& exclude, bool designated = true)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<NodeId> ids = designated ? surfaces() : std::vector<NodeId>{};
    if (ids.empty())
        ids = g.nodes();
    std::vector<Target> out;
    for (NodeId id : ids)
        if (!contains(exclude, id))
            if (auto t = target(g, id))
                out.push_back(std::move(*t));
    return out;
}

glm::dvec3 worldNormal(const Target& t, const geo::MeshBvh& bvh, geo::Index triangle)
{
    const geo::Index* c = &bvh.triangles().corners[size_t(triangle) * 3];
    const geo::Mesh& m = *t.mesh;
    glm::dvec3 a = m.position(m.cornerPoint(c[0])), b = m.position(m.cornerPoint(c[1])), d = m.position(m.cornerPoint(c[2]));
    glm::dvec3 n = glm::transpose(glm::inverse(glm::dmat3(t.world))) * glm::cross(b - a, d - a);
    double len = glm::length(n);
    return len > 1e-300 ? n / len : glm::dvec3(0, 1, 0);
}
} // namespace

static std::optional<Hit> raycastAmong(const glm::dvec3& origin, const glm::dvec3& dir, const std::vector<NodeId>& exclude, bool designated);

std::optional<Hit> raycast(const glm::dvec3& origin, const glm::dvec3& dir, const std::vector<NodeId>& exclude)
{
    return raycastAmong(origin, dir, exclude, true);
}

std::optional<Hit> firstHit(const glm::dvec3& origin, const glm::dvec3& dir, const std::vector<NodeId>& exclude)
{
    return raycastAmong(origin, dir, exclude, false);
}

static std::optional<Hit> raycastAmong(const glm::dvec3& origin, const glm::dvec3& dir, const std::vector<NodeId>& exclude, bool designated)
{
    LILA_ZONE();

    std::optional<Hit> best;
    double bestDist = 1e300;
    for (const Target& t : targets(exclude, designated))
    {
        auto bvh = bvhFor(t.mesh);
        if (bvh->empty())
            continue;
        glm::dmat4 inv = glm::inverse(t.world);
        glm::dvec3 o(inv * glm::dvec4(origin, 1.0)), d(inv * glm::dvec4(dir, 0.0));
        geo::BvhHit h;
        if (!bvh->raycast(o, d, h))
            continue;
        glm::dvec3 p(t.world * glm::dvec4(h.point, 1.0));
        double dist = glm::dot(p - origin, dir);
        if (dist >= bestDist)
            continue;
        bestDist = dist;
        glm::dvec3 n = worldNormal(t, *bvh, h.triangle);
        best = Hit{p, glm::dot(n, dir) > 0.0 ? -n : n, t.id};
    }
    return best;
}

std::optional<Hit> underCursor(QPointF px, const ViewInfo& view, const std::vector<NodeId>& exclude)
{
    glm::dvec3 o, d;
    view.ray(px, o, d);
    return raycast(o, d, exclude);
}

std::optional<Hit> closest(const glm::dvec3& world, const std::vector<NodeId>& fallback)
{
    std::vector<NodeId> ids = surfaces();
    return closestAmong(world, ids.empty() ? fallback : ids);
}

std::optional<Hit> closestAmong(const glm::dvec3& world, const std::vector<NodeId>& meshes)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::optional<Hit> best;
    double bestDist = 1e300;
    for (NodeId id : meshes)
    {
        auto t = target(g, id);
        if (!t)
            continue;
        auto bvh = bvhFor(t->mesh);
        if (bvh->empty())
            continue;
        // Closest in object space; exact for rigid and uniformly scaled surfaces.
        glm::dvec3 local(glm::inverse(t->world) * glm::dvec4(world, 1.0));
        geo::BvhClosest c;
        if (!bvh->closestPoint(local, c))
            continue;
        glm::dvec3 p(t->world * glm::dvec4(c.point, 1.0));
        double dist = glm::length(p - world);
        if (dist >= bestDist)
            continue;
        bestDist = dist;
        best = Hit{p, worldNormal(*t, *bvh, c.triangle), id};
    }
    return best;
}

std::vector<NodeId> surfaces()
{
    const scene::Graph& g = scene::Graph::current();
    std::vector<NodeId> out;
    for (NodeId id : designated())
        if (const scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(id);
    return out;
}

bool active() { return !surfaces().empty(); }

bool isSurface(NodeId id) { return contains(surfaces(), id); }

void setSurfaces(std::vector<NodeId> ids)
{
    const scene::Graph& g = scene::Graph::current();
    std::erase_if(ids, [&](NodeId id) {
        const scene::Node* n = g.node(id);
        return !n || !n->type().isA("mesh");
    });
    designated() = std::move(ids);
    notify();
}

void clear()
{
    designated().clear();
    notify();
}

int addListener(std::function<void()> fn)
{
    static int next = 0;
    listeners()[++next] = std::move(fn);
    return next;
}

void removeListener(int id) { listeners().erase(id); }

bool alignToNormal() { return PrefStore::get().value<bool>("general", "/manipulator/snap/surface/alignToNormal", true); }

glm::dmat3 alignUp(const glm::dmat3& rotation, const glm::dvec3& up, const glm::dvec3& normal)
{
    glm::dvec3 from = glm::normalize(rotation * up);
    glm::dvec3 axis = glm::cross(from, normal);
    double s = glm::length(axis), c = glm::dot(from, normal);
    if (s < 1e-12)
    {
        if (c > 0.0)
            return rotation;
        // Opposite: half a turn about any axis perpendicular to from.
        glm::dvec3 perp = std::abs(from.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
        axis = glm::normalize(glm::cross(from, perp));
        return (2.0 * glm::outerProduct(axis, axis) - glm::dmat3(1.0)) * rotation;
    }
    axis /= s;
    glm::dmat3 k(0.0);
    k[1][0] = -axis.z, k[2][0] = axis.y;
    k[0][1] = axis.z, k[2][1] = -axis.x;
    k[0][2] = -axis.y, k[1][2] = axis.x;
    return (glm::dmat3(1.0) + s * k + (1.0 - c) * k * k) * rotation;
}
} // namespace lila::ui::surfaceSnap

namespace lila::ui::surfaceSnap
{
namespace
{
using json = nlohmann::json;

json paths(const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        if (const scene::Node* n = scene::Graph::current().node(id))
            out.push_back(n->path());
    return out;
}

/** The meshes among the named objects (the selection when none are named); throws when there are none. */
std::vector<NodeId> meshesOf(const Args& a, const char* cmd)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<NodeId> ids;
    for (const std::string& name : a.objects())
        if (const scene::Node* n = g.find(name); n && n->type().isA("mesh"))
            ids.push_back(n->id());
    if (ids.empty())
        throw CommandError(std::string(cmd) + ": select one or more meshes");
    return ids;
}
} // namespace

void registerCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    ops::registerCommand({
        .name = "setSnapSurface",
        .doc = "Make meshes the snap surface for this session: surface snapping hits only them, and the move tool keeps moved objects "
               "and components on them. Returns the snap surface.",
        .positionalDoc = "meshes (default: the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            setSurfaces(meshesOf(a, "setSnapSurface"));
            return paths(surfaces());
        },
    });
    ops::registerCommand({
        .name = "clearSnapSurface",
        .doc = "Clear the snap surface, so surface snapping hits every visible mesh again and moves stop conforming.",
        .undoable = false,
        .run = [](const Args&) -> json {
            clear();
            return nullptr;
        },
    });
    ops::registerCommand({
        .name = "snapSurface",
        .doc = "The meshes set as the snap surface; empty when none is set.",
        .undoable = false,
        .run = [](const Args&) -> json { return paths(surfaces()); },
    });
    // Ids are only meaningful within one scene.
    events::subscribe(events::Event::SceneNew, [](const events::EventData&) { clear(); });
    events::subscribe(events::Event::SceneOpened, [](const events::EventData&) { clear(); });

    actions::add({.id = "snapSurface.set", .label = "Set as Snap Surface", .category = "Snapping",
                  .doc = "Makes the selected meshes the snap surface: moved objects and components stay on it, and surface snapping hits "
                         "only it.",
                  .command = "setSnapSurface"});
    actions::add({.id = "snapSurface.clear", .label = "Clear Snap Surface", .category = "Snapping",
                  .doc = "Clears the snap surface, so moves no longer conform to it.", .command = "clearSnapSurface"});
    actions::add({.id = "snapSurface.toggle", .label = "Toggle Snap Surface", .category = "Snapping",
                  .run = [] {
                      const auto& sel = scene::Graph::current().selection();
                      bool same = !sel.empty() && std::all_of(sel.begin(), sel.end(), [](NodeId id) { return isSurface(id); });
                      actions::runWithOptions(active() && (sel.empty() || same) ? "clearSnapSurface" : "setSnapSurface", json::object());
                  },
                  .doc = "Sets the selected meshes as the snap surface, or clears it when they already are (or nothing is selected).",
                  .python = "setSnapSurface()"});
    actions::add({.id = "snapSurface.alignToNormal", .label = "Align to Surface Normal", .category = "Snapping",
                  .run = [] {
                      PrefStore::get().set("general", "/manipulator/snap/surface/alignToNormal", !alignToNormal());
                  },
                  .doc = "Objects slid along a surface (N held, or Ctrl+middle drag) turn their up axis to the surface normal. Hold "
                         "Shift during the slide to keep their orientation.",
                  .python = "lila.prefs.set(\"general\", \"/manipulator/snap/surface/alignToNormal\", True)",
                  .checked = [] { return alignToNormal(); }});
}
} // namespace lila::ui::surfaceSnap
