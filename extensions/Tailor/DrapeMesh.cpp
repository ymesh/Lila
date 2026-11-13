// tailorDrapeMesh: any mesh dropped as cloth onto the meshes under it, its own edge lengths as the rest shape.

#include "ClothGpu.h"
#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace tailor
{
using T = ArgType;

namespace
{
constexpr const char* kCmd = "tailorDrapeMesh";
constexpr size_t kGpuFrom = 400;

bool isMesh(const Node& n) { return n.type().isA("mesh"); }

const geo::Mesh* outMesh(Graph& g, const Node& n)
{
    const PlugValue& v = g.value({n.id(), MeshNode::kOutMesh});
    const MeshPtr* m = std::get_if<MeshPtr>(&v);
    return m && *m && !(*m)->empty() ? m->get() : nullptr;
}

glm::dmat4 worldOf(Graph& g, const Node& n) { return std::get<glm::dmat4>(g.value({n.id(), Transform::kWorldMatrix})); }

/** World bounds of a mesh, cm. */
std::pair<glm::dvec3, glm::dvec3> worldBounds(const geo::Mesh& m, const glm::dmat4& world)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 w(world * glm::dvec4(m.position(p), 1.0));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    return {lo, hi};
}

/** Visible meshes under the cloth's footprint: their horizontal bounds overlap and they start below the cloth's top. */
std::vector<Node*> meshesBelow(Graph& g, const Node& cloth, const std::pair<glm::dvec3, glm::dvec3>& cb)
{
    std::vector<Node*> out;
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || id == cloth.id() || !isMesh(*n) || !effectivelyVisible(g, id))
            continue;
        const geo::Mesh* m = outMesh(g, *n);
        if (!m)
            continue;
        const auto [lo, hi] = worldBounds(*m, worldOf(g, *n));
        if (lo.x <= cb.second.x && hi.x >= cb.first.x && lo.z <= cb.second.z && hi.z >= cb.first.z && lo.y <= cb.second.y)
            out.push_back(n);
    }
    return out;
}

/** The mesh's live points and triangles in world metres; toCloth maps each mesh point to its cloth vertex, -1 for none. */
ClothMesh clothFrom(const geo::Mesh& m, const glm::dmat4& world, std::vector<int>& toCloth)
{
    std::vector<Vec3> pts;
    ClothMesh cm;
    colliderTriangles(m, world, pts, cm.tris);
    toCloth.assign(pts.size(), -1);
    for (auto& t : cm.tris)
        for (int& v : t)
        {
            if (toCloth[v] < 0)
            {
                toCloth[v] = int(cm.rest3d.size());
                cm.rest3d.push_back(pts[v]);
            }
            v = toCloth[v];
        }
    return cm;
}

float meanEdge(const ClothMesh& cm)
{
    double sum = 0.0;
    for (const auto& t : cm.tris)
    {
        const Vec3 &p = cm.rest3d[t[0]], &q = cm.rest3d[t[1]];
        sum += std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    }
    return cm.tris.empty() ? 0.f : float(sum / double(cm.tris.size()));
}

/**
 * Where the cloth can reach: its bounds grown by its diagonal on every side, so a sheet sliding or hanging off an edge
 * still meets the colliders. The distance fields only cover this.
 */
std::pair<Vec3, Vec3> reach(const ClothMesh& cm)
{
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vec3& p : cm.rest3d)
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    const float grow = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) + 0.05f;
    for (int a = 0; a < 3; ++a)
    {
        lo[a] -= grow;
        hi[a] += grow;
    }
    return {lo, hi};
}

/** Cell size for the distance fields: fine enough for the cloth's edges, capped near 128 cells across the reach. */
float sdfCell(const std::pair<Vec3, Vec3>& box, float clothEdge)
{
    const float extent = std::max({box.second[0] - box.first[0], box.second[1] - box.first[1], box.second[2] - box.first[2]});
    return std::max({extent / 128.f, std::min(clothEdge * 0.5f, 0.02f), 0.002f});
}
/**
 * Runs step (one frame; null on failure) until no point moves more than half a millimetre a frame for a few frames
 * running, or frames run out. The positions it ended with, empty on failure.
 */
template <class Step>
std::vector<Vec3> settle(int frames, progress::Scope& scope, int& ran, bool& settled, Step step)
{
    std::vector<Vec3> prev;
    int still = 0;
    for (ran = 0, settled = false; ran < frames && !settled; ++ran)
    {
        scope.checkCancel();
        const std::vector<Vec3>* x = step();
        if (!x)
            return {};
        float most = 0.f;
        for (size_t i = 0; i < x->size() && i < prev.size(); ++i)
            most = std::max(most, std::abs((*x)[i][0] - prev[i][0]) + std::abs((*x)[i][1] - prev[i][1]) + std::abs((*x)[i][2] - prev[i][2]));
        prev = *x;
        still = ran > 20 && most < 5e-4f ? still + 1 : 0;
        settled = still >= 5;
        scope.step();
    }
    return prev;
}
} // namespace

void registerDrapeMesh(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = kCmd,
        .doc = "Drop a mesh as cloth onto the meshes under it and keep the settled shape. Its own edge lengths are the "
               "rest shape, so a plane keeps its size; a coarse mesh drapes as a finer copy and its points follow. With no colliders named or selected after it, every visible mesh "
               "below it is used. Undoable. Returns the frames run and whether it settled.",
        .positionalDoc = "mesh",
        .flags = {{"colliders", "col", T::List, json(), "Meshes to drape over; default: the rest of the selection, else every visible mesh below."},
                  {"fabric", "fab", T::String, "Cotton", "Fabric preset: how it stretches, bends and slides."},
                  {"frames", "f", T::Int, int64_t(300), "Most frames to run; stops earlier once settled.", {}, {}, 1.0, 5000.0},
                  {"gap", "gap", T::Float, 0.2, "Distance kept from the colliders, cm.", {}, {}, 0.0, 10.0},
                  {"detail", "d", T::Float, 3.0,
                   "Simulated edge length, cm: coarser meshes are subdivided toward it for the drape, then their points take the result. 0 drapes the mesh as it is.",
                   {}, {}, 0.0, 100.0},
                  {"keepDetail", "kd", T::Bool, false, "Keep the subdivided mesh the drape ran on instead of the original topology."},
                  {"selfCollision", "sc", T::Bool, true, "Keep the cloth from passing through itself."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();

            Node* cloth = nullptr;
            std::vector<Node*> colliders;
            if (!a.positional.empty() && a.positional[0].is_string())
            {
                cloth = g.find(a.positional[0].get<std::string>());
                if (!cloth)
                    throw CommandError(std::string(kCmd) + ": no node " + a.positional[0].get<std::string>());
            }
            for (NodeId id : g.selection())
                if (Node* n = g.node(id); n && isMesh(*n))
                {
                    if (!cloth)
                        cloth = n;
                    else if (n != cloth)
                        colliders.push_back(n);
                }
            if (!cloth || !isMesh(*cloth))
                throw CommandError(std::string(kCmd) + ": name a mesh or select one");
            if (a.has("colliders"))
            {
                colliders.clear();
                for (const json& c : a.get<json>("colliders"))
                {
                    Node* n = g.find(c.get<std::string>());
                    if (!n || !isMesh(*n))
                        throw CommandError(std::string(kCmd) + ": no mesh " + c.get<std::string>());
                    colliders.push_back(n);
                }
            }
            const Fabric* fabric = findFabric(a.get<std::string>("fabric"));
            if (!fabric)
                throw CommandError(std::string(kCmd) + ": no fabric preset " + a.get<std::string>("fabric"));

            const PlugRef base = modifiers::editPlug(g, cloth->id());
            g.evaluate();
            const MeshPtr& cur = std::get<MeshPtr>(g.storedValue(base));
            if (!cur || cur->empty())
                throw CommandError(std::string(kCmd) + ": " + cloth->path() + " has no points");
            const glm::dmat4 world = worldOf(g, *cloth);
            if (colliders.empty())
                colliders = meshesBelow(g, *cloth, worldBounds(*cur, world));
            if (colliders.empty())
                throw CommandError(std::string(kCmd) + ": nothing below " + cloth->path() + " to drape over");

            // The cage subdivided until its edges are near the detail size, so coarse faces can still fold.
            const double detail = a.get<double>("detail") / kCmPerM;
            std::vector<int> toCloth;
            ClothMesh cm = clothFrom(*cur, world, toCloth);
            if (cm.tris.empty())
                throw CommandError(std::string(kCmd) + ": " + cloth->path() + " has no faces");
            int levels = 0;
            for (const double edge = meanEdge(cm); detail > 0.0 && levels < 3 && edge / double(1 << levels) > 2.0 * detail;)
                ++levels;
            geo::SubdivideMap map;
            geo::Mesh fine;
            if (levels > 0)
            {
                fine = geo::subdivide(*cur, {.levels = levels, .sharpBoundary = true}, &map);
                cm = clothFrom(fine, world, toCloth);
            }
            const std::pair<Vec3, Vec3> box = reach(cm);
            const float cell = sdfCell(box, meanEdge(cm));

            const int frames = int(a.get<int64_t>("frames"));
            progress::Scope scope("Draping " + cloth->path(), double(frames) + 1.0, progress::Cancellable);
            std::vector<Sdf> sdfs;
            sdfs.reserve(colliders.size());
            for (Node* c : colliders)
                if (const geo::Mesh* m = outMesh(g, *c))
                {
                    std::vector<Vec3> pts;
                    std::vector<std::array<int, 3>> tris;
                    colliderTriangles(*m, worldOf(g, *c), pts, tris);
                    if (Sdf sdf = sdfFromMesh(pts, tris, cell, 0.05f, &box.first, &box.second); sdf.nx > 0)
                        sdfs.push_back(std::move(sdf));
                }
            std::vector<const Sdf*> sdfPtrs;
            for (const Sdf& s : sdfs)
                sdfPtrs.push_back(&s);
            scope.step();

            Garment garment;
            garment.fabrics = {*fabric};
            SolverSettings s;
            s.substeps = 4;
            s.collisionThickness = float(a.get<double>("gap") / kCmPerM);
            s.selfCollision = a.get<bool>("selfCollision");
            int ran = 0;
            bool settled = false, gpu = false;
            std::vector<Vec3> x;
            // A GPU frame costs about the same whatever the size, so small cloths are quicker on the CPU.
            if (cm.rest3d.size() > kGpuFrom && ClothGpu::available())
            {
                ClothGpu solver;
                std::string err;
                solver.setColliders(sdfPtrs);
                bool ok = solver.reset(cm, garment, s, &err);
                if (ok)
                    x = settle(frames, scope, ran, settled, [&]() -> const std::vector<Vec3>* {
                        ok = solver.step(&err);
                        return ok ? &solver.positions() : nullptr;
                    });
                gpu = ok;
                if (!ok)
                    log::warn("{}: GPU drape failed, using the CPU solver: {}", kCmd, err);
            }
            if (!gpu)
            {
                ClothSolver solver;
                solver.reset(cm, garment, s);
                solver.setColliders(sdfPtrs);
                x = settle(frames, scope, ran, settled, [&] {
                    solver.step();
                    return &solver.positions();
                });
            }

            const glm::dmat4 toLocal = glm::inverse(world);
            auto placed = [&](geo::Index finePoint, glm::dvec3& out) {
                const int i = finePoint < toCloth.size() ? toCloth[finePoint] : -1;
                if (i < 0 || !std::isfinite(x[i][0]) || !std::isfinite(x[i][1]) || !std::isfinite(x[i][2]))
                    return false;
                out = glm::dvec3(toLocal * glm::dvec4(glm::dvec3(x[i][0], x[i][1], x[i][2]) * kCmPerM, 1.0));
                return true;
            };
            const bool keepDetail = levels > 0 && a.get<bool>("keepDetail");
            auto edited = std::make_shared<geo::Mesh>(keepDetail ? std::move(fine) : *cur);
            glm::dvec3 q;
            for (geo::Index p = 0; p < edited->pointCapacity(); ++p)
                if (edited->pointValid(p) && placed(keepDetail || levels == 0 ? p : map.points[p], q))
                    edited->setPosition(p, q);
            g.setValue(base, MeshPtr(std::move(edited)));

            json names = json::array();
            for (Node* c : colliders)
                names.push_back(c->path());
            return {{"mesh", cloth->path()}, {"colliders", names}, {"frames", ran}, {"settled", settled}, {"detailLevels", levels}, {"gpu", gpu}};
        },
        .examples = {"tailorDrapeMesh('Plane')", "tailorDrapeMesh('Tablecloth', colliders=['Table'], fabric='Linen')"},
    });
}
} // namespace tailor
