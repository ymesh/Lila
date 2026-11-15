// Tailor: the tailorGarment node, a garment (panels, seams, fabrics) kept as JSON, meshed at its particle distance
// and draped by the cloth solver onto an optional collider. Its outMesh feeds an ordinary mesh.

#include "Tailor.h"
#include "ClothGpu.h"
#include "TailorCore.h"

#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Plug.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Simulation.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/Uv.h>
#include <lila/ViewportTool.h>

#include <QCoreApplication>

#include <algorithm>
#include <map>
#include <atomic>
#include <functional>
#include <mutex>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace tailor
{
namespace
{
constexpr const char* kStateNames[] = {"active", "frozen", "deactivated", "strengthened"};

/**
 * A panel's edge length under the garment's particleDistance (cm): the garment's, except that a panel drafted finer than
 * the standard 2 cm (a finger, a hood) keeps its share of it, so small pieces still mesh with enough cloth to bend.
 */
float scaledParticleDistance(const Panel& p, double garmentCm)
{
    return float(garmentCm / kCmPerM) * std::min(1.f, p.particleDistance / 0.02f);
}

json vec2Json(const Vec2& v) { return json::array({v[0], v[1]}); }
json vec3Json(const Vec3& v) { return json::array({v[0], v[1], v[2]}); }
Vec2 jsonVec2(const json& j) { return {j.at(0).get<float>(), j.at(1).get<float>()}; }
Vec3 jsonVec3(const json& j) { return {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()}; }

json polyJson(const std::vector<Vec2>& pts)
{
    json a = json::array();
    for (const Vec2& p : pts)
        a.push_back(vec2Json(p));
    return a;
}

std::vector<Vec2> jsonPoly(const json& j)
{
    std::vector<Vec2> out;
    for (const json& p : j)
        out.push_back(jsonVec2(p));
    return out;
}

json fabricJson(const Fabric& f)
{
    return {{"name", f.name},         {"stretchWarp", f.stretchWarp}, {"stretchWeft", f.stretchWeft}, {"shear", f.shear},
            {"bendWarp", f.bendWarp}, {"bendWeft", f.bendWeft},       {"bendBias", f.bendBias},       {"strainLimit", f.strainLimit},
            {"density", f.density},   {"thickness", f.thickness},     {"friction", f.friction},       {"damping", f.damping},
            {"shrinkWarp", f.shrinkWarp}, {"shrinkWeft", f.shrinkWeft}};
}

Fabric jsonFabric(const json& j)
{
    Fabric f;
    if (const Fabric* preset = findFabric(j.value("name", f.name)))
        f = *preset;
    f.name = j.value("name", f.name);
    f.stretchWarp = j.value("stretchWarp", f.stretchWarp);
    f.stretchWeft = j.value("stretchWeft", f.stretchWeft);
    f.shear = j.value("shear", f.shear);
    f.bendWarp = j.value("bendWarp", f.bendWarp);
    f.bendWeft = j.value("bendWeft", f.bendWeft);
    f.bendBias = j.value("bendBias", f.bendBias);
    f.strainLimit = j.value("strainLimit", f.strainLimit);
    f.density = j.value("density", f.density);
    f.thickness = j.value("thickness", f.thickness);
    f.friction = j.value("friction", f.friction);
    f.damping = j.value("damping", f.damping);
    f.shrinkWarp = j.value("shrinkWarp", f.shrinkWarp);
    f.shrinkWeft = j.value("shrinkWeft", f.shrinkWeft);
    return f;
}

json sidesJson(const std::vector<SeamSide>& sides)
{
    json a = json::array();
    for (const SeamSide& s : sides)
        a.push_back({{"panel", s.panel}, {"from", s.from}, {"to", s.to}});
    return a;
}

std::vector<SeamSide> jsonSides(const json& j)
{
    std::vector<SeamSide> out;
    for (const json& s : j)
        out.push_back({s.value("panel", -1), s.value("from", 0), s.value("to", 0)});
    return out;
}

/** Attr order of the tailorGarment type; it has no base, so these are the indices. */
enum GarmentAttr
{
    kGarment,
    kBlock,
    kFabric,
    kParticleDistance,
    kSimulate,
    kFrames,
    kColliderMesh,
    kColliderMatrix,
    kOutMesh,
    kWarning,
    kWeldSeams,
    kThickness,
    kGpu,
    kLive,
    kFloor,
    kAnimate,
    kStartFrame,
    kFitMap,
};

void hashMix(uint64_t& h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }

Sdf colliderSdf(const geo::Mesh& m, const glm::dmat4& world)
{
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    colliderTriangles(m, world, pts, tris);
    return sdfFromMesh(pts, tris, 0.01f, 0.05f);
}
} // namespace

void colliderTriangles(const geo::Mesh& m, const glm::dmat4& world, std::vector<Vec3>& pts, std::vector<std::array<int, 3>>& tris)
{
    pts.assign(m.pointCapacity(), Vec3{0.f, 0.f, 0.f});
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        const glm::dvec3 w = glm::dvec3(world * glm::dvec4(m.position(p), 1.0)) / kCmPerM;
        pts[p] = {float(w.x), float(w.y), float(w.z)};
    }
    tris.clear();
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> c = m.faceCorners(f);
        for (size_t k = 1; k + 1 < c.size(); ++k)
            tris.push_back({int(m.cornerPoint(c[0])), int(m.cornerPoint(c[k])), int(m.cornerPoint(c[k + 1]))});
    }
}

namespace
{

/** The surface as a scene mesh in cm: uv0 the packed panels, uv1 the flat pattern (one unit per metre, so a fabric
 *  texture tiles at its real size), and face ints panel and side (0 outer, 1 inner, 2 rim). */
/**
 * The fit map as point colours: red where the cloth is stretched past its pattern (full red at 15%), blue where it is
 * compressed (full blue at 15%), white where it lies at rest. Each point takes its triangles' extremes.
 */
void writeFitMap(const GarmentSurface& s, geo::Mesh& mesh)
{
    std::vector<float> stretch(s.points.size(), 1.f), squash(s.points.size(), 1.f);
    for (size_t f = 0; f < s.tris.size(); ++f)
    {
        if (s.triSide[f] != GarmentSurface::Outer)
            continue;
        const auto& t = s.tris[f];
        const auto& uv = s.fabricUv[f];
        const glm::dvec2 d1(uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]), d2(uv[2][0] - uv[0][0], uv[2][1] - uv[0][1]);
        const double det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12)
            continue;
        auto p = [&](int i) { return glm::dvec3(s.points[size_t(t[size_t(i)])][0], s.points[size_t(t[size_t(i)])][1], s.points[size_t(t[size_t(i)])][2]); };
        const glm::dvec3 e1 = p(1) - p(0), e2 = p(2) - p(0);
        // Deformation gradient columns: the pattern's x and y axes as they lie on the drape.
        const glm::dvec3 fx = (e1 * d2.y - e2 * d1.y) / det, fy = (e2 * d1.x - e1 * d2.x) / det;
        const double a = glm::dot(fx, fx), b = glm::dot(fx, fy), c = glm::dot(fy, fy);
        const double half = 0.5 * (a + c), disc = std::sqrt(std::max(0.0, half * half - (a * c - b * b)));
        const float hi = float(std::sqrt(std::max(0.0, half + disc))), lo = float(std::sqrt(std::max(0.0, half - disc)));
        for (int v : t)
        {
            stretch[size_t(v)] = std::max(stretch[size_t(v)], hi);
            squash[size_t(v)] = std::min(squash[size_t(v)], lo);
        }
    }
    std::span<glm::vec4> colour = mesh.pointAttrs().getOrAdd<glm::vec4>(geo::attr::kColor);
    for (size_t i = 0; i < colour.size() && i < stretch.size(); ++i)
    {
        const float red = std::clamp((stretch[i] - 1.f) / 0.15f, 0.f, 1.f), blue = std::clamp((1.f - squash[i]) / 0.15f, 0.f, 1.f);
        colour[i] = red >= blue ? glm::vec4(1.f, 1.f - red, 1.f - red, 1.f) : glm::vec4(1.f - blue, 1.f - blue, 1.f, 1.f);
    }
}

/**
 * Strain along the pattern's two axes as the drape has it: point attribute strainUV (length along the axis over its
 * pattern length, minus 1) and colour set "wrinkle", compression along U in red and along V in green, full at 25%. uv0
 * keeps the pattern's axes (panels only turn by half turns), so wrinkle maps tiled in uv0 fold across the right way.
 */
void writeWrinkleMask(const GarmentSurface& s, geo::Mesh& mesh)
{
    std::vector<glm::dvec2> sum(s.points.size(), glm::dvec2(0.0));
    std::vector<double> weight(s.points.size(), 0.0);
    for (size_t f = 0; f < s.tris.size(); ++f)
    {
        if (s.triSide[f] != GarmentSurface::Outer)
            continue;
        const auto& t = s.tris[f];
        const auto& uv = s.fabricUv[f];
        const glm::dvec2 d1(uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]), d2(uv[2][0] - uv[0][0], uv[2][1] - uv[0][1]);
        const double det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12)
            continue;
        auto p = [&](int i) { return glm::dvec3(s.points[size_t(t[size_t(i)])][0], s.points[size_t(t[size_t(i)])][1], s.points[size_t(t[size_t(i)])][2]); };
        const glm::dvec3 e1 = p(1) - p(0), e2 = p(2) - p(0);
        const glm::dvec2 strain(glm::length((e1 * d2.y - e2 * d1.y) / det) - 1.0, glm::length((e2 * d1.x - e1 * d2.x) / det) - 1.0);
        const double area = 0.5 * std::abs(det);
        for (int v : t)
        {
            sum[size_t(v)] += strain * area;
            weight[size_t(v)] += area;
        }
    }
    std::span<glm::vec2> strain = mesh.pointAttrs().getOrAdd<glm::vec2>("strainUV");
    std::vector<glm::vec4> mask(mesh.pointCapacity(), glm::vec4(0.f, 0.f, 0.f, 1.f));
    for (size_t i = 0; i < strain.size() && i < sum.size(); ++i)
    {
        strain[i] = weight[i] > 0.0 ? glm::vec2(sum[i] / weight[i]) : glm::vec2(0.f);
        mask[i] = {std::clamp(-strain[i].x * 4.f, 0.f, 1.f), std::clamp(-strain[i].y * 4.f, 0.f, 1.f), 0.f, 1.f};
    }
    geo::colors::add(mesh, "wrinkle", glm::vec4(0.f, 0.f, 0.f, 1.f));
    geo::colors::setFromPoints(mesh, "wrinkle", mask);
}

MeshPtr surfaceToMesh(const GarmentSurface& s)
{
    std::vector<glm::dvec3> pts(s.points.size());
    for (size_t k = 0; k < s.points.size(); ++k)
        pts[k] = glm::dvec3(s.points[k][0], s.points[k][1], s.points[k][2]) * kCmPerM;
    std::vector<geo::Index> sizes(s.tris.size(), 3), idx;
    idx.reserve(s.tris.size() * 3);
    for (const auto& t : s.tris)
        for (int v : t)
            idx.push_back(geo::Index(v));
    auto mesh = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(pts, sizes, idx));

    std::span<glm::vec2> uv = mesh->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    std::span<glm::vec2> fabricUv = mesh->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(1));
    std::span<int32_t> panel = mesh->faceAttrs().getOrAdd<int32_t>("panel");
    std::span<int32_t> side = mesh->faceAttrs().getOrAdd<int32_t>("side");
    for (geo::Index f = 0; f < mesh->faceCapacity() && f < s.tris.size(); ++f)
    {
        if (!mesh->faceValid(f))
            continue;
        panel[f] = s.triPanel[f];
        side[f] = int32_t(s.triSide[f]);
        const std::vector<geo::Index> corners = mesh->faceCorners(f);
        for (size_t k = 0; k < corners.size() && k < 3; ++k)
        {
            uv[corners[k]] = glm::vec2(s.uv[f][k][0], s.uv[f][k][1]);
            fabricUv[corners[k]] = glm::vec2(s.fabricUv[f][k][0], s.fabricUv[f][k][1]);
        }
    }
    return mesh;
}

/** The panels packed into uv 0-1 by surface area, turned only by half turns so the grain stays vertical. Per triangle corner. */
std::vector<Vec2> packPattern(const ClothMesh& cm)
{
    const GarmentSurface flat = garmentSurface(cm, cm.rest3d, {}, {.weldSeams = false});
    MeshPtr mesh = surfaceToMesh(flat);
    geo::Mesh& m = const_cast<geo::Mesh&>(*mesh);
    uv::PackOptions opts;
    opts.rotateStep = 0;
    opts.angles = {0.0, 180.0};
    opts.preOrient = false;
    opts.padding = 8.0;
    uv::packIslands(m, {}, geo::attr::uvName(0), opts);
    const std::span<const glm::vec2> packed = std::as_const(m).cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    std::vector<Vec2> out(cm.tris.size() * 3, Vec2{0.f, 0.f});
    for (geo::Index f = 0; f < m.faceCapacity() && f < flat.tris.size(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> corners = m.faceCorners(f);
        for (size_t k = 0; k < corners.size() && k < 3; ++k)
            out[3 * f + k] = {packed[corners[k]].x, packed[corners[k]].y};
    }
    return out;
}

/** One drape on a worker thread. It publishes its positions every few frames; the garment node shows the latest. */
struct DrapeJob
{
    uint64_t key = 0;
    std::mutex mutex;
    std::vector<Vec3> latest; // guarded by mutex
    bool finished = false;    // guarded by mutex
    std::atomic<bool> refreshQueued{false};
    tasks::Job job;
};

/** Jobs still running, so unloading can stop them. Main thread only. */
std::vector<std::shared_ptr<DrapeJob>>& drapeJobs()
{
    static std::vector<std::shared_ptr<DrapeJob>> jobs;
    return jobs;
}

constexpr int64_t kPublishEvery = 5;

std::atomic<int> gDrapeNow{0};

/** Batch runs (no Qt application) and scripts holding a DrapeNow get the finished drape from the evaluation itself. */
bool drapeInBackground() { return gDrapeNow.load() == 0 && QCoreApplication::instance() != nullptr; }

/** Re-evaluates a garment whose drape moved on, from the main thread. */
void refreshGarment(NodeId id)
{
    Graph& g = Graph::current();
    if (!g.node(id))
        return;
    g.touch(id);
    g.evaluate();
    ui::viewportTools::requestRedraw();
}

class GarmentNode final : public Node
{
public:
    ~GarmentNode() override
    {
        if (mJob)
            mJob->job.cancel();
        if (mLive)
            registerLive(id(), nullptr);
    }

    void compute(const EvalContext& ctx) override
    {
        LILA_ZONE();
        mTime = ctx.time;
        std::string warning;
        try
        {
            setOutput(kOutMesh, build());
        }
        catch (const std::exception& e)
        {
            warning = e.what();
            setOutput(kOutMesh, MeshPtr());
        }
        if (!warning.empty() && warning != as<std::string>(kWarning))
            log::warn("{}: {}", name(), warning);
        setOutput(kWarning, warning);
    }

private:
    MeshPtr output(const ClothMesh& cm, const std::vector<Vec3>& positions) const
    {
        SurfaceOptions o;
        o.weldSeams = as<bool>(kWeldSeams);
        o.thickness = float(std::max(0.0, as<double>(kThickness)) / kCmPerM);
        const GarmentSurface surface = garmentSurface(cm, positions, mPackedUv, o);
        MeshPtr mesh = surfaceToMesh(surface);
        writeWrinkleMask(surface, const_cast<geo::Mesh&>(*mesh));
        if (as<bool>(kFitMap))
            writeFitMap(surface, const_cast<geo::Mesh&>(*mesh));
        return mesh;
    }

    MeshPtr build()
    {
        const std::string& text = as<std::string>(kGarment);
        if (text.empty())
            return MeshPtr();
        Garment g = garmentFromJson(json::parse(text));
        if (const std::string& preset = as<std::string>(kFabric); !preset.empty())
        {
            const Fabric* f = findFabric(preset);
            if (!f)
                throw std::runtime_error("unknown fabric preset '" + preset + "'");
            g.fabrics.at(0) = *f;
        }
        if (const double pd = as<double>(kParticleDistance); pd > 0.0)
            for (Panel& p : g.panels)
                p.particleDistance = scaledParticleDistance(p, pd);

        uint64_t meshKey = std::hash<std::string>{}(text);
        hashMix(meshKey, std::hash<double>{}(as<double>(kParticleDistance)));
        if (meshKey != mClothKey || !mCloth)
        {
            mCloth = std::make_shared<const ClothMesh>(buildClothMesh(g));
            mPackedUv = packPattern(*mCloth);
            mClothKey = meshKey;
        }
        const ClothMesh& cm = *mCloth;
        const int64_t frames = as<int64_t>(kFrames);
        if (!as<bool>(kSimulate) || frames <= 0 || cm.rest3d.empty())
            return output(cm, cm.rest3d);

        const MeshPtr& collider = as<MeshPtr>(kColliderMesh);
        const glm::dmat4& colliderWorld = as<glm::dmat4>(kColliderMatrix);
        uint64_t key = meshKey;
        hashMix(key, std::hash<std::string>{}(as<std::string>(kFabric)));
        hashMix(key, uint64_t(frames));
        hashMix(key, uint64_t(as<bool>(kGpu)));
        hashMix(key, uint64_t(as<bool>(kFloor)));
        hashMix(key, uint64_t(uintptr_t(collider.get())));
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                hashMix(key, std::hash<double>{}(colliderWorld[c][r]));

        SolverSettings settings;
        if (as<bool>(kFloor) && collider && !collider->empty())
        {
            double lowest = 1e30;
            for (geo::Index p = 0; p < collider->pointCapacity(); ++p)
                lowest = std::min(lowest, (colliderWorld * glm::dvec4(collider->position(p), 1.0)).y);
            settings.floorY = float(lowest / kCmPerM);
        }

        if (as<bool>(kAnimate))
        {
            // The simulated frame at the time evaluated (exports sample other times); else the last one shown.
            if (const auto f = sim::cached(Graph::current(), id(), sim::frameOf(mTime)); f && f->state.size() == 6 * cm.rest3d.size())
            {
                std::vector<Vec3> x(cm.rest3d.size());
                for (size_t i = 0; i < x.size(); ++i)
                    x[i] = {float(f->state[3 * i]), float(f->state[3 * i + 1]), float(f->state[3 * i + 2])};
                return output(cm, x);
            }
            if (const AnimatedFrame* f = animatedFrame(id()); f && f->positions.size() == cm.rest3d.size())
                return output(cm, f->positions);
        }

        if (as<bool>(kLive) && drapeInBackground())
        {
            if (!mLive || mLive->key() != key)
            {
                std::vector<Vec3> start = mLive ? mLive->positions() : mDrape;
                if (start.size() != cm.rest3d.size())
                    start = cm.rest3d;
                Sdf sdf;
                if (collider && !collider->empty())
                    sdf = colliderSdf(*collider, colliderWorld);
                mLive.reset();
                mLive = std::make_shared<LiveDrape>(id(), key, mCloth, g, std::move(sdf), as<bool>(kGpu), settings, std::move(start));
                registerLive(id(), mLive);
            }
            return output(cm, mLive->positions());
        }
        if (mLive)
        {
            // Turning live off keeps the cloth where it got to.
            if (mLive->key() == key)
            {
                mDrape = mLive->positions();
                mDrapeKey = key;
            }
            mLive.reset();
            registerLive(id(), nullptr);
        }

        if (key == mDrapeKey && mDrape.size() == cm.rest3d.size())
            return output(cm, mDrape);

        if (!drapeInBackground())
        {
            if (mJob)
                mJob->job.cancel();
            mJob.reset();
            mDrape = drape(cm, g, collider, colliderWorld, frames, as<bool>(kGpu), settings, nullptr);
            mDrapeKey = key;
            return output(cm, mDrape);
        }

        if (!mJob || mJob->key != key)
            startDrape(key, g, collider, colliderWorld, frames, settings);
        std::vector<Vec3> shown;
        {
            std::lock_guard lock(mJob->mutex);
            if (mJob->finished && mJob->latest.size() == cm.rest3d.size())
            {
                mDrape = mJob->latest;
                mDrapeKey = key;
            }
            shown = mJob->latest;
        }
        if (mDrapeKey == key)
            mJob.reset();
        return output(cm, shown.size() == cm.rest3d.size() ? shown : cm.rest3d);
    }

    /**
     * Runs the solver, on the GPU when asked and the renderer runs (falling back to the CPU solver if the GPU fails);
     * with a task, reports progress to it, publishes every few frames and stops when cancelled.
     */
    static std::vector<Vec3> drape(const ClothMesh& cm, const Garment& g, const MeshPtr& collider,
                                   const glm::dmat4& colliderWorld, int64_t frames, bool useGpu, const SolverSettings& settings,
                                   tasks::Task* task,
                                   const std::function<void(const std::vector<Vec3>&)>& publish = {})
    {
        Sdf sdf;
        std::vector<const Sdf*> colliders;
        std::vector<Vec3> bodyPts;
        std::vector<std::array<int, 3>> bodyTris;
        if (collider && !collider->empty())
        {
            colliderTriangles(*collider, colliderWorld, bodyPts, bodyTris);
            sdf = sdfFromMesh(bodyPts, bodyTris, 0.01f, 0.05f);
            colliders.push_back(&sdf);
        }
        const std::vector<Pin> pins = resolvePins(cm, g, &bodyPts, &bodyTris);
        auto tick = [&](int64_t k, const std::vector<Vec3>& positions) {
            if (publish && (k + 1) % kPublishEvery == 0 && k + 1 < frames)
                publish(positions);
        };
        auto beginFrame = [&] {
            if (task)
            {
                task->checkCancel();
                task->progress.step(1.0);
            }
        };

        if (useGpu && ClothGpu::available())
        {
            ClothGpu solver;
            std::string err;
            solver.setColliders(colliders);
            bool ok = solver.reset(cm, g, settings, &err);
            solver.setPins(pins);
            for (int64_t k = 0; ok && k < frames; ++k)
            {
                beginFrame();
                ok = solver.step(&err);
                if (ok)
                    tick(k, solver.positions());
            }
            if (ok)
                return solver.positions();
            log::warn("Tailor: GPU drape failed, using the CPU solver: {}", err);
        }

        ClothSolver solver;
        solver.reset(cm, g, settings);
        solver.setColliders(colliders);
        solver.setPins(pins);
        for (int64_t k = 0; k < frames; ++k)
        {
            beginFrame();
            solver.step();
            tick(k, solver.positions());
        }
        return solver.positions();
    }

    void startDrape(uint64_t key, const Garment& g, const MeshPtr& collider, const glm::dmat4& colliderWorld, int64_t frames,
                    const SolverSettings& settings)
    {
        const bool useGpu = as<bool>(kGpu);
        if (mJob)
            mJob->job.cancel();
        auto job = std::make_shared<DrapeJob>();
        job->key = key;
        const NodeId id = this->id();
        auto publish = [job, id](const std::vector<Vec3>& positions) {
            {
                std::lock_guard lock(job->mutex);
                job->latest = positions;
            }
            if (!job->refreshQueued.exchange(true))
                tasks::postToMain([job, id] {
                    job->refreshQueued = false;
                    refreshGarment(id);
                });
        };
        job->job = tasks::runInBackground(
            "Draping " + name(), double(frames),
            [job, cloth = mCloth, g, collider, colliderWorld, frames, useGpu, settings, publish](tasks::Task& t) {
                std::vector<Vec3> result = drape(*cloth, g, collider, colliderWorld, frames, useGpu, settings, &t, publish);
                std::lock_guard lock(job->mutex);
                job->latest = std::move(result);
                job->finished = true;
            },
            [job, id](std::exception_ptr) {
                auto& jobs = drapeJobs();
                jobs.erase(std::remove(jobs.begin(), jobs.end(), job), jobs.end());
                bool finished;
                {
                    std::lock_guard lock(job->mutex);
                    finished = job->finished;
                }
                if (finished)
                    refreshGarment(id);
            });
        drapeJobs().push_back(job);
        mJob = std::move(job);
    }

    uint64_t mClothKey = 0;
    std::shared_ptr<const ClothMesh> mCloth;
    std::vector<Vec2> mPackedUv;
    uint64_t mDrapeKey = 0;
    std::vector<Vec3> mDrape;
    std::shared_ptr<DrapeJob> mJob;
    std::shared_ptr<LiveDrape> mLive;
    double mTime = 0.0;
};

constexpr uint32_t kGarmentIn = AttrInput | AttrStorable;

/** Points of a mesh and its faces fanned into triangles, as the binding takes them. */
void meshTriangles(const geo::Mesh& m, std::vector<Vec3>& pts, std::vector<std::array<int, 3>>& tris)
{
    pts.assign(m.pointCapacity(), Vec3{0.f, 0.f, 0.f});
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        const glm::dvec3 q = m.position(p);
        pts[p] = {float(q.x), float(q.y), float(q.z)};
    }
    tris.clear();
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> c = m.faceCorners(f);
        for (size_t k = 1; k + 1 < c.size(); ++k)
            tris.push_back({int(m.cornerPoint(c[0])), int(m.cornerPoint(c[k])), int(m.cornerPoint(c[k + 1]))});
    }
}

enum BindingAttr
{
    kRestMesh,
    kBindSim,
    kSimMesh,
    kBoundMesh,
    kBindWarning,
    kRigid,
};

/** A render mesh latched to a garment's cloth: bound once against the cloth as it was, then moved with it. */
class BindingNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        const MeshPtr& rest = as<MeshPtr>(kRestMesh);
        const MeshPtr& bindSim = as<MeshPtr>(kBindSim);
        const MeshPtr& sim = as<MeshPtr>(kSimMesh);
        std::string warning;
        if (!rest || rest->empty())
        {
            setOutput(kBoundMesh, MeshPtr());
            setOutput(kBindWarning, std::string("no render mesh"));
            return;
        }
        if (bindSim && sim && bindSim->pointCapacity() != sim->pointCapacity() && as<bool>(kRigid))
        {
            // Re-meshed since binding: rigid pieces bind again where they sit, each centred back on its cloth point.
            if (sim->pointCapacity() != mReboundPoints || rest.get() != mRestKey)
            {
                std::vector<Vec3> simPts, restPts;
                std::vector<std::array<int, 3>> restTris;
                meshTriangles(*sim, simPts, mTris);
                meshTriangles(*rest, restPts, restTris);
                RenderBinding again = bindRigidPieces(simPts, mTris, restPts, restTris);
                std::map<int, std::array<float, 3>> centre;
                std::map<int, int> count;
                for (size_t i = 0; i < again.tri.size(); ++i)
                {
                    auto& c = centre[again.tri[i]];
                    for (int k = 0; k < 2; ++k)
                        c[size_t(k)] += again.local[i][size_t(k)];
                    ++count[again.tri[i]];
                }
                for (size_t i = 0; i < again.tri.size(); ++i)
                {
                    const auto& c = centre[again.tri[i]];
                    const float n = float(count[again.tri[i]]);
                    again.local[i][0] -= c[0] / n;
                    again.local[i][1] -= c[1] / n;
                    if (i < mBinding.local.size() && mBinding.tri.size() == again.tri.size())
                        again.local[i][2] = mBinding.local[i][2];
                }
                mBinding = std::move(again);
                mReboundPoints = sim->pointCapacity();
                mSimKey = nullptr;
                mRestKey = rest.get();
                mRigid = true;
            }
            std::vector<Vec3> now;
            std::vector<std::array<int, 3>> tris;
            meshTriangles(*sim, now, tris);
            const std::vector<Vec3> moved = deformRenderMesh(mBinding, now, mTris);
            auto out = std::make_shared<geo::Mesh>(*rest);
            std::span<glm::dvec3> pos = out->positions();
            for (size_t i = 0; i < moved.size() && i < pos.size(); ++i)
                pos[i] = glm::dvec3(moved[i][0], moved[i][1], moved[i][2]);
            setOutput(kBoundMesh, MeshPtr(std::move(out)));
            setOutput(kBindWarning, std::string());
            return;
        }
        if (!bindSim || !sim || bindSim->pointCapacity() != sim->pointCapacity())
        {
            // The garment was re-meshed since binding (a pattern edit): show the render mesh as it was bound.
            setOutput(kBoundMesh, rest);
            setOutput(kBindWarning, std::string(sim ? "the garment changed shape since binding; bind again" : "no garment connected"));
            return;
        }
        if (rest.get() != mRestKey || bindSim.get() != mSimKey || as<bool>(kRigid) != mRigid)
        {
            std::vector<Vec3> simPts, restPts;
            std::vector<std::array<int, 3>> restTris;
            meshTriangles(*bindSim, simPts, mTris);
            meshTriangles(*rest, restPts, restTris);
            mBinding = as<bool>(kRigid) ? bindRigidPieces(simPts, mTris, restPts, restTris) : bindRenderMesh(simPts, mTris, restPts);
            mRestKey = rest.get();
            mSimKey = bindSim.get();
            mRigid = as<bool>(kRigid);
        }
        std::vector<Vec3> now;
        std::vector<std::array<int, 3>> tris;
        meshTriangles(*sim, now, tris);
        const std::vector<Vec3> moved = deformRenderMesh(mBinding, now, mTris);
        auto out = std::make_shared<geo::Mesh>(*rest);
        std::span<glm::dvec3> pos = out->positions();
        for (size_t i = 0; i < moved.size() && i < pos.size(); ++i)
            pos[i] = glm::dvec3(moved[i][0], moved[i][1], moved[i][2]);
        setOutput(kBoundMesh, MeshPtr(std::move(out)));
        setOutput(kBindWarning, warning);
    }

private:
    const void* mRestKey = nullptr;
    const void* mSimKey = nullptr;
    bool mRigid = false;
    geo::Index mReboundPoints = 0;
    RenderBinding mBinding;
    std::vector<std::array<int, 3>> mTris;
};

NodeType bindingType()
{
    NodeType t;
    t.name = kBindingType;
    t.icon = "link";
    t.doc = "A render mesh latched to a garment's cloth (latch to nearest): each point follows its nearest cloth triangle, "
            "offset kept, so a thick or separately modelled mesh moves with the drape. Feeds the render mesh's inMesh.";
    t.ownAttrs = {
        {.name = "restMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The render mesh as it was bound."},
        {.name = "bindSim", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The garment's cloth when it was bound."},
        {.name = "simMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The garment's cloth now: connect the garment's outMesh."},
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "The render mesh moved with the cloth."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the binding."},
        {.name = "rigid", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
         .doc = "Each connected piece (a button, a buckle) follows the cloth at its middle without bending."},
    };
    t.create = [] { return std::make_unique<BindingNode>(); };
    return t;
}


NodeType garmentType()
{
    NodeType t;
    t.name = kGarmentType;
    t.icon = "shirt";
    // Animated garments show the simulated frame at the current time.
    t.timeDependent = true;
    t.doc = "A garment: pattern panels sewn by seams, meshed at a particle distance and draped onto a collider. Feeds a mesh's inMesh.";
    t.ownAttrs = {
        {.name = "garment", .type = PlugType::String, .defaultValue = std::string(), .flags = kGarmentIn | AttrHidden | AttrMultiline,
         .doc = "Panels, seams and fabrics, JSON. Pattern space and placement are in metres."},
        {.name = "block", .type = PlugType::String, .defaultValue = std::string(), .flags = kGarmentIn,
         .doc = "The block the garment was started from, if any.", .category = "Garment"},
        {.name = "fabric", .type = PlugType::String, .defaultValue = std::string(), .flags = kGarmentIn,
         .doc = "Fabric preset for panels on the garment's first fabric; empty keeps the garment's own.", .category = "Garment"},
        {.name = "particleDistance", .type = PlugType::Float, .defaultValue = 2.0, .flags = kGarmentIn | AttrKeyable,
         .doc = "Target cloth edge length, cm, with small pieces (fingers, hoods) proportionally finer; 0 uses each panel's own.", .minValue = 0.0, .softMin = 0.3, .softMax = 5.0, .category = "Garment"},
        {.name = "simulate", .type = PlugType::Bool, .defaultValue = true, .flags = kGarmentIn,
         .doc = "Drape the garment; off shows the panels where they were placed.", .category = "Simulation"},
        {.name = "frames", .type = PlugType::Int, .defaultValue = int64_t(90), .flags = kGarmentIn,
         .doc = "Solver frames run to settle the drape.", .minValue = 0.0, .softMax = 600.0, .category = "Simulation"},
        {.name = "colliderMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The body the garment drapes onto: connect a mesh's outMesh."},
        {.name = "colliderMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The collider's world matrix: connect its worldMatrix."},
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "The draped garment, cm: uv0 the packed panels, uv1 the flat pattern in metres, face ints panel and side."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput,
         .doc = "Problems with the last build."},
        {.name = "weldSeams", .type = PlugType::Bool, .defaultValue = true, .flags = kGarmentIn,
         .doc = "Merge sewn points so the garment is one connected mesh; off keeps each panel separate.", .category = "Output"},
        {.name = "thickness", .type = PlugType::Float, .defaultValue = 0.0, .flags = kGarmentIn,
         .doc = "Shell thickness, cm, grown towards the wearer with rims on open edges; 0 is a single sheet.", .minValue = 0.0,
         .softMax = 2.0, .category = "Output"},
        {.name = "gpu", .type = PlugType::Bool, .defaultValue = true, .flags = kGarmentIn,
         .doc = "Drape on the GPU when the viewport is running; the CPU solver otherwise, and in batch runs.", .category = "Simulation"},
        {.name = "live", .type = PlugType::Bool, .defaultValue = false, .flags = kGarmentIn,
         .doc = "Keep simulating in real time, so the cloth answers edits and the Grab Cloth tool as they happen. Off keeps "
                "the cloth where it got to.", .category = "Simulation"},
        {.name = "floor", .type = PlugType::Bool, .defaultValue = true, .flags = kGarmentIn,
         .doc = "A ground the cloth rests on, level with the collider's lowest point (its feet).", .category = "Simulation"},
        {.name = "animate", .type = PlugType::Bool, .defaultValue = false, .flags = kGarmentIn,
         .doc = "Simulate over the timeline against the collider as it moves (a skinned or animated body), cached per frame "
                "like other simulations. Starts draped at startFrame.", .category = "Simulation"},
        {.name = "startFrame", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = kGarmentIn,
         .doc = "The frame an animated garment starts from, draped onto the collider as it stands there.", .category = "Simulation"},
        {.name = "fitMap", .type = PlugType::Bool, .defaultValue = false, .flags = kGarmentIn,
         .doc = "Colour the drape by fit: red where the cloth is stretched past its pattern, blue where it is compressed, "
                "white at rest. Show it with the viewport's vertex colours.", .category = "Output"},
    };
    t.create = [] { return std::make_unique<GarmentNode>(); };
    return t;
}
} // namespace

const char* panelStateName(PanelState s) { return kStateNames[size_t(s)]; }

bool parsePanelState(const std::string& name, PanelState* out)
{
    for (size_t k = 0; k < std::size(kStateNames); ++k)
        if (name == kStateNames[k])
        {
            *out = PanelState(k);
            return true;
        }
    return false;
}

json garmentToJson(const Garment& g)
{
    json panels = json::array();
    for (const Panel& p : g.panels)
    {
        json holes = json::array(), lines = json::array(), angles = json::array(), spots = json::array();
        for (const auto& h : p.holes)
            holes.push_back(polyJson(h));
        for (const auto& l : p.internalLines)
            lines.push_back(polyJson(l));
        for (float a : p.lineAngles)
            angles.push_back(std::isfinite(a) ? json(a) : json());
        for (const ShrinkSpot& s : p.shrinkSpots)
            spots.push_back({{"at", {s.at[0], s.at[1]}}, {"radius", s.radius}, {"scale", s.scale}});
        panels.push_back({{"name", p.name},
                          {"outline", polyJson(p.outline)},
                          {"holes", holes},
                          {"internalLines", lines},
                          {"grainAngle", p.grainAngle},
                          {"fabric", p.fabric},
                          {"layer", p.layer},
                          {"state", panelStateName(p.state)},
                          {"particleDistance", p.particleDistance},
                          {"origin", vec3Json(p.origin)},
                          {"right", vec3Json(p.right)},
                          {"up", vec3Json(p.up)},
                          {"wrapRadius", p.wrapRadius},
                          {"lineAngles", angles},
                          {"shrinkSpots", spots},
                          {"notches", p.notches},
                          {"twin", p.twin}});
    }
    json seams = json::array();
    for (const Seam& s : g.seams)
        seams.push_back({{"a", sidesJson(s.a)}, {"b", sidesJson(s.b)}, {"reversed", s.reversed}, {"foldAngle", s.foldAngle}, {"shrink", s.shrink}});
    json fabrics = json::array();
    for (const Fabric& f : g.fabrics)
        fabrics.push_back(fabricJson(f));
    json pins = json::array();
    for (const GarmentPin& p : g.pins)
    {
        json pj = {{"panel", p.panel}, {"at", {p.at[0], p.at[1]}}, {"target", vec3Json(p.target)}};
        if (p.bodyTri >= 0)
            pj["body"] = {{"tri", p.bodyTri}, {"bary", p.bodyBary}, {"local", vec3Json(p.bodyLocal)}, {"tris", p.bodyTris}};
        pins.push_back(std::move(pj));
    }
    json tacks = json::array();
    for (const Tack& t : g.tacks)
        tacks.push_back({t.panelA, t.atA[0], t.atA[1], t.panelB, t.atB[0], t.atB[1]});
    return {{"version", 1}, {"panels", panels}, {"seams", seams}, {"fabrics", fabrics}, {"pins", pins}, {"tacks", tacks}};
}

Garment garmentFromJson(const json& j)
{
    Garment g;
    for (const json& pj : j.value("panels", json::array()))
    {
        Panel p;
        p.name = pj.value("name", std::string());
        p.outline = jsonPoly(pj.value("outline", json::array()));
        for (const json& h : pj.value("holes", json::array()))
            p.holes.push_back(jsonPoly(h));
        for (const json& l : pj.value("internalLines", json::array()))
            p.internalLines.push_back(jsonPoly(l));
        p.grainAngle = pj.value("grainAngle", p.grainAngle);
        p.fabric = pj.value("fabric", p.fabric);
        p.layer = pj.value("layer", p.layer);
        if (!parsePanelState(pj.value("state", std::string("active")), &p.state))
            throw std::runtime_error("panel '" + p.name + "': unknown state");
        p.particleDistance = pj.value("particleDistance", p.particleDistance);
        if (pj.contains("origin"))
            p.origin = jsonVec3(pj["origin"]);
        if (pj.contains("right"))
            p.right = jsonVec3(pj["right"]);
        if (pj.contains("up"))
            p.up = jsonVec3(pj["up"]);
        p.wrapRadius = pj.value("wrapRadius", p.wrapRadius);
        p.notches = pj.value("notches", std::vector<int>{});
        for (const json& a : pj.value("lineAngles", json::array()))
            p.lineAngles.push_back(a.is_number() ? a.get<float>() : std::nanf(""));
        for (const json& s : pj.value("shrinkSpots", json::array()))
        {
            const json at = s.value("at", json::array({0.0, 0.0}));
            p.shrinkSpots.push_back({{at[0].get<float>(), at[1].get<float>()}, s.value("radius", 0.05f), s.value("scale", 1.f)});
        }
        p.twin = pj.value("twin", -1);
        g.panels.push_back(std::move(p));
    }
    for (const json& sj : j.value("seams", json::array()))
    {
        Seam s;
        s.a = jsonSides(sj.value("a", json::array()));
        s.b = jsonSides(sj.value("b", json::array()));
        s.reversed = sj.value("reversed", s.reversed);
        s.foldAngle = sj.value("foldAngle", s.foldAngle);
        s.shrink = sj.value("shrink", s.shrink);
        g.seams.push_back(std::move(s));
    }
    if (j.contains("fabrics") && !j["fabrics"].empty())
    {
        g.fabrics.clear();
        for (const json& fj : j["fabrics"])
            g.fabrics.push_back(jsonFabric(fj));
    }
    for (const json& pj : j.value("pins", json::array()))
    {
        const json at = pj.value("at", json::array({0.0, 0.0}));
        GarmentPin p{pj.value("panel", -1), {at[0].get<float>(), at[1].get<float>()}, {0.f, 0.f, 0.f}};
        if (pj.contains("target"))
            p.target = jsonVec3(pj["target"]);
        if (const json body = pj.value("body", json()); body.is_object())
        {
            p.bodyTri = body.value("tri", -1);
            p.bodyBary = body.value("bary", std::array<float, 3>{1.f, 0.f, 0.f});
            if (body.contains("local"))
                p.bodyLocal = jsonVec3(body["local"]);
            p.bodyTris = body.value("tris", 0);
        }
        if (p.panel >= 0 && p.panel < int(g.panels.size()))
            g.pins.push_back(p);
    }
    for (const json& t : j.value("tacks", json::array()))
        if (t.is_array() && t.size() == 6)
        {
            const Tack k{t[0].get<int>(), {t[1].get<float>(), t[2].get<float>()}, t[3].get<int>(), {t[4].get<float>(), t[5].get<float>()}};
            if (k.panelA >= 0 && k.panelB >= 0 && k.panelA < int(g.panels.size()) && k.panelB < int(g.panels.size()))
                g.tacks.push_back(k);
        }
    for (Panel& p : g.panels)
    {
        p.fabric = std::clamp(p.fabric, 0, int(g.fabrics.size()) - 1);
        if (p.twin >= int(g.panels.size()))
            p.twin = -1;
    }
    return g;
}

DrapeNow::DrapeNow() { ++gDrapeNow; }
DrapeNow::~DrapeNow() { --gDrapeNow; }

void stopDrapes()
{
    for (lila::scene::NodeId id : liveGarments())
        if (auto live = liveDrape(id))
            live->stop();
    lila::mainq::drain();
    std::vector<std::shared_ptr<DrapeJob>> jobs = drapeJobs();
    for (const auto& j : jobs)
        j->job.cancel();
    for (const auto& j : jobs)
        j->job.wait();
    drapeJobs().clear();
}

GarmentInputs garmentInputs(Graph& g, const Node& op)
{
    GarmentInputs in;
    const std::string& text = op.as<std::string>(kGarment);
    if (text.empty())
        throw std::invalid_argument("the garment has no pattern");
    in.garment = garmentFromJson(json::parse(text));
    if (const std::string& preset = op.as<std::string>(kFabric); !preset.empty())
    {
        const Fabric* f = findFabric(preset);
        if (!f)
            throw std::invalid_argument("unknown fabric preset '" + preset + "'");
        in.garment.fabrics.at(0) = *f;
    }
    if (const double pd = op.as<double>(kParticleDistance); pd > 0.0)
        for (Panel& p : in.garment.panels)
            p.particleDistance = scaledParticleDistance(p, pd);
    in.cloth = buildClothMesh(in.garment);
    in.frames = op.as<int64_t>(kFrames);
    in.key = std::hash<std::string>{}(text);
    hashMix(in.key, std::hash<std::string>{}(op.as<std::string>(kFabric)));
    hashMix(in.key, std::hash<double>{}(op.as<double>(kParticleDistance)));
    hashMix(in.key, uint64_t(in.frames));
    hashMix(in.key, uint64_t(op.as<int64_t>(kStartFrame)));
    if (const std::optional<PlugRef> src = g.source({op.id(), kColliderMesh}))
    {
        in.collider = src->node;
        if (op.as<bool>(kFloor))
        {
            const PlugValue v = g.value({src->node, MeshNode::kOutMesh});
            const PlugValue w = g.value({src->node, Transform::kWorldMatrix});
            if (const MeshPtr* m = std::get_if<MeshPtr>(&v); m && *m && std::get_if<glm::dmat4>(&w))
            {
                double lowest = 1e30;
                for (geo::Index p = 0; p < (*m)->pointCapacity(); ++p)
                    lowest = std::min(lowest, (std::get<glm::dmat4>(w) * glm::dvec4((*m)->position(p), 1.0)).y);
                in.settings.floorY = float(lowest / kCmPerM);
            }
        }
    }
    return in;
}

bool registerNodes(ext::Context& ctx)
{
    relocate::registerNeutral(kGarmentType);
    relocate::registerNeutral(kBindingType);
    return types::registerType(ctx, garmentType()) != nullptr && types::registerType(ctx, bindingType()) != nullptr;
}
} // namespace tailor

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!tailor::registerNodes(ctx))
        return 1;
    tailor::registerCommands(ctx);
    tailor::registerDrapeMesh(ctx);
    tailor::registerUi(ctx);
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&)
{
    tailor::stopDrapes();
}
