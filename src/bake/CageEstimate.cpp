#include "bake/CageEstimate.h"

#include "bake/AutoDistance.h"
#include "bake/Backend.h"
#include "bake/BakeCommon.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/Maps.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Triangulate.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace lila::bake
{
using Clock = std::chrono::steady_clock;

namespace
{
using detail::edgeFunction;
using detail::meshByName;
using detail::mix3;
using detail::worldOf;


/** Adds the seconds since t0 to timings[key]. */
void addStageTiming(json& timings, const char* key, Clock::time_point t0)
{
    timings[key] = timings.value(key, 0.0) + std::chrono::duration<double>(Clock::now() - t0).count();
}

/** The ray direction a bake's auto cage uses at w in triangle t (TexelRaster's), pointing out of the low. */
glm::vec3 cageDirection(const LowInput& low, size_t t, const glm::dvec3& w, float groupSkew)
{
    const size_t c0 = t * 3;
    glm::vec3 n = mix3(&low.cageNormals[c0], w);
    const float l = glm::length(n);
    n = l > 0.0f ? n / l : glm::vec3(0.0f, 0.0f, 1.0f);
    const float skew = std::clamp(groupSkew + (low.cageSkew.size() == low.soup.positions.size() ? mix3(&low.cageSkew[c0], w) : 0.0f), 0.0f, 1.0f);
    if (skew > 0.0f)
    {
        const glm::vec3* p = &low.soup.positions[c0];
        const glm::vec3 g = glm::cross(p[1] - p[0], p[2] - p[0]);
        const float gl = glm::length(g);
        if (gl > 0.0f)
        {
            const glm::vec3 d = glm::mix(n, g / gl, skew);
            if (const float dl = glm::length(d); dl > 1e-6f)
                n = d / dl;
        }
    }
    return n;
}

/** Barycentric weights of a raster sample in its low triangle, from its UV. */
glm::dvec3 sampleBary(const LowInput& low, const TexelSample& s)
{
    const glm::vec2* uv = &low.soup.uvs[size_t(s.lowTriangle) * 3];
    const glm::dvec2 a(uv[0]), b(uv[1]), c(uv[2]), p(s.uv[0], s.uv[1]);
    const double area = edgeFunction(a, b, c);
    if (std::abs(area) < 1e-18)
        return glm::dvec3(1.0 / 3.0);
    glm::dvec3 w(edgeFunction(b, c, p) / area, edgeFunction(c, a, p) / area, edgeFunction(a, b, p) / area);
    w = glm::max(w, glm::dvec3(0.0));
    return w / std::max(1e-12, w.x + w.y + w.z);
}

/** What the walks along one sample's cage line found. */
struct Need
{
    /** Push needed to start outside the high material the low sits in; 0 when it sits outside. */
    float outward = 0.0f;
    /** Distance to the high below the low (for rear), negative when there is none within reach. */
    float inward = -1.0f;
    bool anyHit = false;
};

/** Per triangle of a soup, its shell: triangles joined through corners at the same position share one id. */
std::vector<uint32_t> soupShells(const MeshSoup& soup)
{
    const size_t tris = soup.triangleCount();
    std::vector<uint32_t> parent(tris);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    struct Key
    {
        std::array<uint32_t, 3> bits;
        bool operator==(const Key&) const = default;
    };
    struct Hash
    {
        size_t operator()(const Key& k) const { return size_t(k.bits[0]) * 73856093u ^ size_t(k.bits[1]) * 19349663u ^ size_t(k.bits[2]) * 83492791u; }
    };
    std::unordered_map<Key, uint32_t, Hash> seen;
    seen.reserve(tris * 2);
    for (uint32_t t = 0; t < tris; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const glm::vec3& q = soup.positions[size_t(t) * 3 + size_t(k)];
            const auto [it, fresh] = seen.try_emplace(Key{{std::bit_cast<uint32_t>(q.x), std::bit_cast<uint32_t>(q.y), std::bit_cast<uint32_t>(q.z)}}, t);
            if (!fresh)
                parent[find(t)] = find(it->second);
        }
    for (uint32_t t = 0; t < tris; ++t)
        parent[t] = find(t);
    return parent;
}

/** The shell of a self high a sample lies on: hits on that high's other shells are skipped. */
struct SelfShell
{
    uint32_t high = kNoHit;
    uint32_t shell = kNoHit;
};

class Walker
{
public:
    /** shells[h] holds high h's triangle shells when it is a low baking onto itself, else nothing. */
    Walker(const Tracer& tracer, size_t group, const std::vector<std::vector<uint32_t>>& shells) : mTracer(tracer), mGroup(group), mShells(shells) {}

    /**
     * cap bounds the walk: far layers of the same mesh would otherwise read as material the low starts inside. own is
     * the sample's shell when the low bakes onto itself: its other shells hide what they cross.
     */
    Need at(uint32_t low, const glm::vec3& p, const glm::vec3& dir, float mCap, float mEps, const SelfShell& own = {}) const
    {
        Need n;
        // A ray through a shared edge or vertex meets each triangle there: hits this close repeat one crossing.
        const float same = 4.0f * mEps;
        // Parity from below, reaching past the walk so a large body still shows its far side: only a back face under the
        // low means it starts inside. Otherwise (outside, or an open mesh seen from behind) an exit ahead is a missed entry.
        TraceHit h;
        const bool below = next(low, p, -dir, 4.0f * mCap, mEps, own, h);
        if (below && h.distance <= mCap)
        {
            n.anyHit = true;
            if (!h.backface)
                n.inward = h.distance;
        }
        const bool outside = !below || !h.backface;
        bool left = false;
        // Exits pair off with the entries before them: an unpaired exit leaves material the walk started in.
        float travelled = 0.0f, last = -1.0f;
        bool lastBack = false;
        int entered = 0;
        for (int k = 0; k < 16 && travelled < mCap; ++k)
        {
            if (!next(low, p + dir * travelled, dir, mCap - travelled, mEps, own, h))
                break;
            n.anyHit = true;
            travelled += h.distance;
            const bool repeat = last >= 0.0f && travelled - last < same && h.backface == lastBack;
            last = travelled;
            lastBack = h.backface;
            if (!repeat)
            {
                if (!h.backface)
                    ++entered;
                else if (entered > 0)
                    --entered;
                else if (!left && (!outside || travelled < same))
                {
                    // Past the first unpaired exit the walk is outside; another one is an entry the ray slipped past.
                    n.outward = travelled;
                    left = true;
                }
            }
            travelled += mEps;
        }
        return n;
    }

private:
    bool trace(uint32_t low, const glm::vec3& o, const glm::vec3& dir, float tMax, TraceHit& h) const
    {
        return mTracer.closestHit(mGroup, OccluderScope::Group, kNoHit, low, RayKind::Projection, o, dir, tMax, h);
    }

    /** Cosine below which a hit grazes the surface. */
    static constexpr float kGrazing = 0.1f;

    /** The nearest hit within tMax, stepping past the own high's other shells and grazing hits; distance is from o. */
    bool next(uint32_t low, const glm::vec3& o, const glm::vec3& dir, float tMax, float eps, const SelfShell& own, TraceHit& h) const
    {
        float skipped = 0.0f;
        for (int k = 0; k < 32 && skipped < tMax; ++k)
        {
            if (!trace(low, o + dir * skipped, dir, tMax - skipped, h))
                return false;
            const bool foreign = own.shell != kNoHit && h.high == own.high && h.triangle < mShells[own.high].size() &&
                                 mShells[own.high][h.triangle] != own.shell;
            // A ray sliding along a surface (a thin part's rim walking its own face) crosses nothing it can count.
            const bool grazing = glm::dot(h.normal, h.normal) > 0.0f && std::abs(glm::dot(h.normal, dir)) < kGrazing;
            if (!foreign && !grazing)
            {
                h.distance += skipped;
                return true;
            }
            skipped += h.distance + eps;
        }
        return false;
    }

    const Tracer& mTracer;
    size_t mGroup;
    const std::vector<std::vector<uint32_t>>& mShells;
};

/** Every triangle's interpolated push must reach need. */
struct Constraint
{
    uint32_t tri;
    glm::vec3 w;
    float need;
};

struct LowState
{
    const CageLow* source = nullptr;
    LowInput input;
    /** Point of each soup corner. */
    std::vector<geo::Index> cornerPoint;
    std::vector<Constraint> constraints;
    /** World push per point slot. */
    std::vector<float> push;
    /** Most push per point slot: just short of another low in front of it. */
    std::vector<float> clear;
    /** From this low's own bounds diagonal: the gap kept to the high, the walk's reach, its step past a hit, sample spacing. */
    float margin = 0.0f;
    float cap = 0.0f;
    float eps = 0.0f;
    float spacing = 1.0f;
    /** The low bakes onto itself. */
    bool self = false;

    /**
     * How far out of its high a sample in triangle t can need to start: unbounded unless the low bakes onto itself,
     * where the high strays from the low only by smoothing, about a triangle's size. An exit further out crosses the
     * low's own fold or tight bend.
     */
    float reach(size_t t) const
    {
        if (!self)
            return std::numeric_limits<float>::max();
        const glm::vec3* p = &input.soup.positions[t * 3];
        return kSelfReach * (glm::length(p[1] - p[0]) + glm::length(p[2] - p[1]) + glm::length(p[0] - p[2])) / 3.0f;
    }

    /** A self-baking low's reach in mean triangle edges. */
    static constexpr float kSelfReach = 1.0f;

    /** Its self high and, per triangle, the shell of that high it smooths into. */
    uint32_t selfHigh = kNoHit;
    std::vector<uint32_t> highShell;

    SelfShell own(size_t t) const { return t < highShell.size() ? SelfShell{selfHigh, highShell[t]} : SelfShell{}; }
};

/**
 * Fills s's highShell: each of its shells takes the shell of its self high (shells per high triangle) whose bounds
 * match its own best, since smoothing keeps a shell in place and only rounds it.
 */
void matchShells(LowState& s, uint32_t high, const MeshSoup& soup, std::span<const uint32_t> shells)
{
    struct Box
    {
        glm::vec3 lo{std::numeric_limits<float>::max()};
        glm::vec3 hi{std::numeric_limits<float>::lowest()};
    };
    auto boxes = [](const MeshSoup& m, std::span<const uint32_t> ids) {
        std::unordered_map<uint32_t, Box> out;
        for (size_t t = 0; t < ids.size(); ++t)
        {
            Box& b = out[ids[t]];
            for (int k = 0; k < 3; ++k)
            {
                b.lo = glm::min(b.lo, m.positions[t * 3 + size_t(k)]);
                b.hi = glm::max(b.hi, m.positions[t * 3 + size_t(k)]);
            }
        }
        return out;
    };
    const std::vector<uint32_t> lowShells = soupShells(s.input.soup);
    const auto lowBoxes = boxes(s.input.soup, lowShells), highBoxes = boxes(soup, shells);
    std::unordered_map<uint32_t, uint32_t> match;
    for (const auto& [ls, lb] : lowBoxes)
    {
        float best = std::numeric_limits<float>::max();
        for (const auto& [hs, hb] : highBoxes)
            if (const float d = glm::length(lb.lo - hb.lo) + glm::length(lb.hi - hb.hi); d < best)
            {
                best = d;
                match[ls] = hs;
            }
    }
    s.selfHigh = high;
    s.highShell.resize(lowShells.size());
    for (size_t t = 0; t < lowShells.size(); ++t)
        s.highShell[t] = match[lowShells[t]];
}

/** The smallest per-point push (from push as it stands) meeting every constraint, by repeated projection. */
void solve(LowState& s, float floor)
{
    LILA_ZONE();

    std::vector<float>& x = s.push;
    for (geo::Index p = 0; p < x.size(); ++p)
        x[p] = std::max(x[p], floor);
    auto pts = [&](const Constraint& c) {
        const geo::Index* cp = &s.cornerPoint[size_t(c.tri) * 3];
        return std::array<geo::Index, 3>{cp[0], cp[1], cp[2]};
    };
    // Seed: each constraint lifts the point it sits closest to.
    for (const Constraint& c : s.constraints)
    {
        const auto p = pts(c);
        const int k = c.w.x >= c.w.y && c.w.x >= c.w.z ? 0 : c.w.y >= c.w.z ? 1 : 2;
        x[p[size_t(k)]] = std::max(x[p[size_t(k)]], c.need);
    }
    for (int pass = 0; pass < 24; ++pass)
    {
        bool moved = false;
        for (const Constraint& c : s.constraints)
        {
            const auto p = pts(c);
            const float cur = x[p[0]] * c.w.x + x[p[1]] * c.w.y + x[p[2]] * c.w.z;
            if (cur >= c.need * 0.9999f)
                continue;
            const float denom = c.w.x * c.w.x + c.w.y * c.w.y + c.w.z * c.w.z;
            const float d = (c.need - cur) / std::max(denom, 1e-6f);
            for (int k = 0; k < 3; ++k)
                x[p[size_t(k)]] += d * c.w[k];
            moved = true;
        }
        if (!moved)
            break;
    }
    // Anything projection left short lifts its whole triangle.
    for (const Constraint& c : s.constraints)
    {
        const auto p = pts(c);
        if (x[p[0]] * c.w.x + x[p[1]] * c.w.y + x[p[2]] * c.w.z < c.need * 0.999f)
            for (geo::Index q : p)
                x[q] = std::max(x[q], c.need);
    }
}

/** How much of the gap to another low in front a cage may cross. */
constexpr float kClearanceShare = 0.9f;

/** Holds every point to its clearance. */
void clampToClearance(LowState& s)
{
    for (size_t p = 0; p < s.push.size(); ++p)
        s.push[p] = std::min(s.push[p], s.clear[p]);
}

/** Raises points below their neighbours' mean halfway toward it: a smoother shell that never pulls under a need. */
void smoothUp(LowState& s)
{
    const geo::Mesh& m = *s.source->mesh;
    std::vector<float> next = s.push;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        double sum = 0.0;
        int n = 0;
        m.forEachPointEdge(p, [&](geo::Index e) {
            sum += s.push[m.edgeOtherPoint(e, p)];
            ++n;
        });
        if (n)
            next[p] = std::max(s.push[p], 0.5f * (s.push[p] + float(sum / n)));
    }
    s.push = std::move(next);
}

float quantile(std::vector<float> v, double q)
{
    if (v.empty())
        return 0.0f;
    const size_t i = std::min(v.size() - 1, size_t(double(v.size() - 1) * q));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(i), v.end());
    return v[i];
}

/** Shares of an auto paint's progress bar, roughly in proportion to their time on a large high. */
constexpr double kReadSpan = 2.0;
constexpr double kHighsSpan = 3.0;
constexpr double kBeforeSpan = 1.0;
constexpr double kEstimateSpan = 5.0;
constexpr double kApplySpan = 1.0;
constexpr double kRemeasureSpan = 1.0;
constexpr double kTotalSpan = kReadSpan + kHighsSpan + kBeforeSpan + kEstimateSpan + kApplySpan + kRemeasureSpan;

/** Coverage of every low of a prepared group with cp, summed. */
CageCoverage sumCoverage(const GroupInputs& gi, const CageParams& cp, const Backend& backend, int resolution)
{
    CageCoverage total;
    for (const LowInput& low : gi.lows)
    {
        const CageCoverage c = cageCoverage(low, uint32_t(&low - gi.lows.data()), cp, 0, backend, resolution);
        total.samples += c.samples;
        total.misses += c.misses;
        total.backfaces += c.backfaces;
    }
    return total;
}

/** The group's inputs with highs, a prepared CPU backend and its Auto distances measured, as a bake starts. */
struct Prepared
{
    BakeInputs inputs;
    std::unique_ptr<Backend> backend;
};

std::unique_ptr<Prepared> prepare(scene::Graph& g, scene::NodeId group, json* timings = nullptr)
{
    scene::Node* grp = g.node(group);
    if (!grp || !isGroup(*grp))
        throw std::runtime_error("not a bake group");
    scene::Node* project = groupProject(g, *grp);
    if (!project)
        throw std::runtime_error(grp->name() + " is in no bake project");
    auto t0 = Clock::now();
    auto p = std::make_unique<Prepared>();
    {
        progress::Scope stage("Reading meshes", 0.0, progress::None, kReadSpan);
        g.evaluate();
        p->inputs = buildInputs(g, project->id(), {group}, true);
    }
    if (timings)
        addStageTiming(*timings, "prepare:inputs", t0);
    if (p->inputs.groups.size() != 1)
        throw std::runtime_error(grp->name() + ": nothing to bake");
    if (p->inputs.groups[0].highs.empty())
        throw std::runtime_error(grp->name() + " has no highs");
    progress::checkCancel();
    t0 = Clock::now();
    {
        progress::Scope stage("Preparing highs", 0.0, progress::None, kHighsSpan);
        p->backend = createBackend("cpu");
        progress::runOnWorker([&] {
            p->backend->prepare(p->inputs);
            resolveAutoDistances(p->inputs, *p->backend);
        });
    }
    if (timings)
        addStageTiming(*timings, "prepare:highs", t0);
    return p;
}
} // namespace

std::vector<geo::Index> soupCornerPoints(const geo::Mesh& mesh, const glm::dmat4& world)
{
    const geo::Triangulation tris = geo::triangulate(mesh);
    const bool mirrored = glm::determinant(glm::dmat3(world)) < 0.0;
    const int order[3] = {0, mirrored ? 2 : 1, mirrored ? 1 : 2};
    std::vector<geo::Index> out(tris.corners.size());
    for (size_t t = 0; t < tris.faces.size(); ++t)
        for (int k = 0; k < 3; ++k)
            out[t * 3 + size_t(k)] = mesh.cornerPoint(tris.corners[t * 3 + size_t(order[k])]);
    return out;
}

json CageCoverage::toJson() const { return {{"samples", samples}, {"misses", misses}, {"backfaces", backfaces}, {"missRate", missRate()}}; }

json CageEstimate::toJson() const
{
    return {{"frontal", frontal},       {"rear", rear},
            {"cageOffset", cageOffset}, {"after", after.toJson()},
            {"iterations", iterations}, {"unreachable", unreachable},
            {"timings", timings}};
}

CageCoverage cageCoverage(const LowInput& low, uint32_t lowIndex, const CageParams& cage, size_t group, const Backend& backend, int resolution,
                          std::vector<TexelSample>* missed)
{
    LILA_ZONE();

    CageCoverage out;
    if (low.soup.uvs.empty())
        return out;
    RasterSettings rs;
    rs.width = rs.height = std::max(8, resolution);
    rs.supersampling = 1;
    rs.bandRows = 32;
    const TexelRaster raster(low, rs);
    progress::Scope scope("", double(raster.udims().size() * raster.bandCount()));
    std::atomic<uint64_t> samples = 0, misses = 0, backs = 0;
    std::mutex mutex;
    for (int udim : raster.udims())
        tbb::parallel_for(size_t(0), raster.bandCount(), [&](size_t b) {
            scope.checkCancel();
            std::vector<TexelSample> s;
            raster.band(udim, b, cage, s);
            if (s.empty())
            {
                scope.step();
                return;
            }
            for (TexelSample& t : s)
                t.low = lowIndex;
            std::vector<HitRecord> hits(s.size());
            backend.traceProjection(group, s, hits);
            backend.endBand(s);
            uint64_t m = 0, bf = 0;
            std::vector<TexelSample> local;
            for (size_t i = 0; i < s.size(); ++i)
                // A probed hit is a miss for the cage: no ray along the normal reached the high.
                if (!(hits[i].flags & kHitValid) || (hits[i].flags & kHitProbed))
                {
                    ++m;
                    bf += (hits[i].flags & kHitBackface) ? 1 : 0;
                    if (missed)
                    {
                        // Carries the backface flag to the caller in the otherwise unused sub index.
                        TexelSample t = s[i];
                        t.sub = (hits[i].flags & kHitBackface) ? 1u : 0u;
                        local.push_back(t);
                    }
                }
            samples += s.size();
            misses += m;
            backs += bf;
            if (missed && !local.empty())
            {
                std::scoped_lock lock(mutex);
                missed->insert(missed->end(), local.begin(), local.end());
            }
            scope.step();
        });
    out.samples = samples;
    out.misses = misses;
    out.backfaces = backs;
    return out;
}

CageEstimate estimateCage(std::span<const CageLow> lows, const GroupSettings& settings, const BakeInputs& inputs, size_t group, const Backend& backend,
                          const CageEstimateOptions& options)
{
    LILA_ZONE();

    const Tracer* tracer = backend.tracer();
    if (!tracer)
        throw std::runtime_error("cage estimate: the backend has no tracer");
    CageEstimate est;
    est.cageOffset = settings.cageOffset > 1e-6 ? settings.cageOffset : 1.0;
    const float skew = float(settings.skew);
    auto t0 = Clock::now();
    // Sampling, the first coverage check, then each refining pass.
    progress::Scope whole("Estimating cage", 2.0 + double(options.iterations));

    const GroupInputs& gi = inputs.groups[group];
    std::vector<LowState> states(lows.size());
    float margin = 0.0f;
    for (size_t i = 0; i < lows.size(); ++i)
    {
        LowState& s = states[i];
        s.source = &lows[i];
        s.input = lowFromMesh(*lows[i].mesh, lows[i].world, lows[i].name);
        s.cornerPoint = soupCornerPoints(*lows[i].mesh, lows[i].world);
        s.push.assign(lows[i].mesh->pointCapacity(), 0.0f);
        s.clear.assign(lows[i].mesh->pointCapacity(), std::numeric_limits<float>::max());
        if (!s.input.soup.triangleCount())
            continue;
        // Samples on a barycentric grid about diag / 128 apart (2 to 8 steps per triangle side).
        const float diag = std::max(glm::length(s.input.soup.boundsMax - s.input.soup.boundsMin), 1e-4f);
        s.margin = float(options.margin) * diag;
        s.cap = 0.25f * diag;
        s.eps = 1e-5f * diag;
        s.spacing = diag / 128.0f;
        s.self = std::ranges::find(gi.highSelfLow, int32_t(i)) != gi.highSelfLow.end();
        margin = std::max(margin, s.margin);
    }
    if (std::ranges::none_of(states, [](const LowState& s) { return s.input.soup.triangleCount() > 0; }))
        throw std::runtime_error("cage estimate: the lows have no faces");
    // Shells of the highs that are lows baking onto themselves.
    std::vector<std::vector<uint32_t>> shells(gi.highs.size());
    tbb::parallel_for(size_t(0), gi.highs.size(), [&](size_t h) {
        if (h < gi.highSelfLow.size() && gi.highSelfLow[h] >= 0)
            shells[h] = soupShells(gi.highs[h].soup);
    });
    for (size_t h = 0; h < gi.highSelfLow.size(); ++h)
        if (const int32_t k = gi.highSelfLow[h]; k >= 0 && size_t(k) < states.size() && !shells[h].empty())
            matchShells(states[size_t(k)], uint32_t(h), gi.highs[h].soup, shells[h]);
    const Walker walk(*tracer, group, shells);
    addStageTiming(est.timings, "lowInputs", t0);
    t0 = Clock::now();

    // The other lows, for clearance: rays go into each one's own space, where t stays the world distance.
    struct Other
    {
        geo::MeshBvh bvh;
        glm::dmat4 toLocal;
    };
    std::vector<std::unique_ptr<Other>> others(lows.size());
    if (options.clearance && lows.size() > 1)
        tbb::parallel_for(size_t(0), lows.size(), [&](size_t i) {
            if (lows[i].mesh && states[i].input.soup.triangleCount())
                others[i] = std::make_unique<Other>(Other{geo::MeshBvh(*lows[i].mesh), glm::inverse(lows[i].world)});
        });
    auto gapAhead = [&](size_t self, const glm::vec3& p, const glm::vec3& dir, float reach) {
        double best = reach;
        for (size_t j = 0; j < others.size(); ++j)
        {
            if (j == self || !others[j])
                continue;
            const glm::dvec3 o(others[j]->toLocal * glm::dvec4(glm::dvec3(p), 1.0));
            const glm::dvec3 d(others[j]->toLocal * glm::dvec4(glm::dvec3(dir), 0.0));
            geo::BvhHit h;
            if (others[j]->bvh.raycast(o, d, h, best))
                best = std::min(best, h.t);
        }
        return float(best);
    };

    addStageTiming(est.timings, "clearance", t0);
    t0 = Clock::now();
    std::vector<float> inward;
    {
        size_t allTris = 0;
        for (const LowState& s : states)
            allTris += s.input.soup.triangleCount();
        progress::Scope sampling("Sampling lows", double(allTris));
        for (LowState& s : states)
        {
            const LowInput& li = s.input;
            const uint32_t lowIndex = uint32_t(&s - states.data());
            const size_t tris = li.soup.triangleCount();
            // Each triangle's samples get a fixed slot range, so the constraints come out in triangle order.
            std::vector<uint8_t> steps(tris);
            std::vector<size_t> first(tris + 1, s.constraints.size());
            for (size_t t = 0; t < tris; ++t)
            {
                const glm::vec3* p = &li.soup.positions[t * 3];
                const float longest = std::max({glm::length(p[1] - p[0]), glm::length(p[2] - p[1]), glm::length(p[0] - p[2])});
                const int n = std::clamp(int(std::ceil(longest / s.spacing)), 2, 8);
                steps[t] = uint8_t(n);
                first[t + 1] = first[t] + size_t((n + 1) * (n + 2) / 2);
            }
            s.constraints.resize(first[tris]);
            // Inward distance per sample, negative where it doesn't count toward rear.
            std::vector<float> in(first[tris] - first[0], -1.0f);
            // Per corner: the most push before the cage reaches halfway to another low.
            std::vector<float> cornerClear(tris * 3, std::numeric_limits<float>::max());
            const bool clearance = options.clearance && others.size() > 1;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, tris, 64), [&](const tbb::blocked_range<size_t>& r) {
                sampling.checkCancel();
                for (size_t t = r.begin(); t < r.end(); ++t)
                {
                    const glm::vec3* p = &li.soup.positions[t * 3];
                    const int n = steps[t];
                    size_t slot = first[t];
                    for (int a = 0; a <= n; ++a)
                        for (int b = 0; a + b <= n; ++b, ++slot)
                        {
                            const glm::dvec3 w(double(n - a - b) / n, double(a) / n, double(b) / n);
                            const glm::vec3 P = mix3(p, w);
                            const glm::vec3 dir = cageDirection(li, t, w, skew);
                            const Need need = walk.at(lowIndex, P, dir, s.cap, s.eps, s.own(t));
                            s.constraints[slot] = {uint32_t(t), glm::vec3(w), (need.outward <= s.reach(t) ? need.outward : 0.0f) + s.margin};
                            if (clearance)
                            {
                                const int k = w.x >= w.y && w.x >= w.z ? 0 : w.y >= w.z ? 1 : 2;
                                // Meshes that touch or cross can't be kept apart; only a real gap holds the cage back.
                                const float gap = gapAhead(lowIndex, P, dir, s.cap);
                                if (gap < s.cap && kClearanceShare * gap > s.margin)
                                    cornerClear[t * 3 + size_t(k)] = std::min(cornerClear[t * 3 + size_t(k)], kClearanceShare * gap);
                            }
                            if (need.outward <= 0.0f && need.inward >= 0.0f)
                                in[slot - first[0]] = need.inward;
                        }
                }
                sampling.step(double(r.size()));
            });
            for (float d : in)
                if (d >= 0.0f)
                    inward.push_back(d);
            for (size_t c = 0; c < cornerClear.size(); ++c)
                s.clear[s.cornerPoint[c]] = std::min(s.clear[s.cornerPoint[c]], cornerClear[c]);
        }
    }

    addStageTiming(est.timings, "walk", t0);
    t0 = Clock::now();
    // Lone needs far above the rest (a stray high piece) are clamped; coverage passes put back any that were real.
    std::vector<float> all;
    for (const LowState& s : states)
        for (const Constraint& c : s.constraints)
            all.push_back(c.need);
    const float clampAt = 2.0f * quantile(all, 0.995);
    for (LowState& s : states)
    {
        for (Constraint& c : s.constraints)
            c.need = std::min(c.need, std::max(clampAt, s.margin * 2.0f));
        solve(s, s.margin);
        smoothUp(s);
        clampToClearance(s);
    }
    est.rear = std::max(2.0 * margin, double(quantile(inward, 0.999)) * 1.1 + margin);

    // Frontal puts the typical push at offset 1, and the largest within the attribute's 0-10.
    auto fit = [&] {
        std::vector<float> pushes;
        float most = 0.0f;
        for (const LowState& s : states)
            for (geo::Index p = 0; p < s.source->mesh->pointCapacity(); ++p)
                if (s.source->mesh->pointValid(p))
                {
                    pushes.push_back(s.push[p]);
                    most = std::max(most, s.push[p]);
                }
        double f = std::max(double(margin), double(quantile(pushes, 0.9)));
        f = std::max(f, double(most) / 10.0);
        est.frontal = f / est.cageOffset;
        est.offsets.clear();
        for (LowState& s : states)
        {
            std::vector<float> o(s.push.size(), 1.0f);
            for (size_t p = 0; p < o.size(); ++p)
                o[p] = std::clamp(float(double(s.push[p]) / f), 0.0f, 10.0f);
            for (size_t c = 0; c < s.cornerPoint.size(); ++c)
                s.input.cageScale[c] = o[s.cornerPoint[c]];
            est.offsets.push_back(std::move(o));
        }
    };

    // Coverage of every low; missed (when given) gets each low's misses in texel order, so refining is repeatable.
    auto cover = [&](std::vector<std::vector<TexelSample>>* missed) {
        const auto c0 = Clock::now();
        CageCoverage total;
        const CageParams cp{float(est.frontal), float(est.rear), float(est.cageOffset), skew};
        for (size_t i = 0; i < states.size(); ++i)
        {
            std::vector<TexelSample>* out = missed ? &(*missed)[i] : nullptr;
            const CageCoverage c = cageCoverage(states[i].input, uint32_t(i), cp, group, backend, options.resolution, out);
            if (out)
                std::ranges::sort(*out, [](const TexelSample& a, const TexelSample& b) {
                    return std::tie(a.tile, a.texel) < std::tie(b.tile, b.texel);
                });
            total.samples += c.samples;
            total.misses += c.misses;
            total.backfaces += c.backfaces;
        }
        addStageTiming(est.timings, "cover", c0);
        return total;
    };

    fit();
    addStageTiming(est.timings, "solve", t0);
    std::vector<std::vector<TexelSample>> missed(states.size());
    {
        progress::Scope pass("Checking coverage", double(states.size()));
        est.after = cover(&missed);
    }
    CageEstimate best = est;
    for (int it = 0; it < options.iterations && est.after.misses > 0; ++it)
    {
        progress::Scope pass("Refining, pass " + std::to_string(it + 1) + " of " + std::to_string(options.iterations), double(states.size()) + 1.0);
        t0 = Clock::now();
        uint64_t unreachable = 0;
        float rear = float(est.rear), inwardMost = -1.0f;
        bool added = false;
        for (size_t i = 0; i < states.size(); ++i)
        {
            LowState& s = states[i];
            const LowInput& li = s.input;
            const std::vector<TexelSample>& miss = missed[i];
            std::vector<Need> needs(miss.size());
            std::vector<glm::dvec3> bary(miss.size());
            tbb::parallel_for(tbb::blocked_range<size_t>(0, miss.size(), 64), [&](const tbb::blocked_range<size_t>& r) {
                pass.checkCancel();
                for (size_t k = r.begin(); k < r.end(); ++k)
                {
                    const size_t t = miss[k].lowTriangle;
                    bary[k] = sampleBary(li, miss[k]);
                    const glm::vec3 P = mix3(&li.soup.positions[t * 3], bary[k]);
                    needs[k] = walk.at(uint32_t(i), P, cageDirection(li, t, bary[k], skew), s.cap, s.eps, s.own(t));
                }
            });
            for (size_t k = 0; k < miss.size(); ++k)
            {
                const TexelSample& m = miss[k];
                const Need& need = needs[k];
                const glm::dvec3& w = bary[k];
                const size_t t = m.lowTriangle;
                const geo::Index* cp = &s.cornerPoint[t * 3];
                const float push = s.push[cp[0]] * float(w.x) + s.push[cp[1]] * float(w.y) + s.push[cp[2]] * float(w.z);
                if (!need.anyHit)
                {
                    ++unreachable;
                    continue;
                }
                // A ray that met a back face started inside the high: push past it. A plain miss fell short of the high below.
                float want = (need.outward <= s.reach(t) ? need.outward : 0.0f) + s.margin;
                if (m.sub)
                    want = std::max(want, push * 1.25f + s.margin);
                want = std::min({want, s.reach(t) + s.margin, std::max(clampAt, s.margin * 2.0f)});
                if (want > push)
                {
                    s.constraints.push_back({uint32_t(t), glm::vec3(w), want});
                    added = true;
                }
                if (!m.sub && need.inward > rear)
                    inwardMost = std::max(inwardMost, need.inward * 1.1f + s.margin);
            }
        }
        rear = std::max(rear, inwardMost);
        pass.step();
        est.unreachable = unreachable;
        if (!added && rear <= est.rear)
            break;
        est.rear = rear;
        for (LowState& s : states)
        {
            solve(s, s.margin);
            clampToClearance(s);
        }
        fit();
        addStageTiming(est.timings, "refine", t0);
        for (auto& m : missed)
            m.clear();
        est.after = cover(&missed);
        est.iterations = it + 1;
        if (est.after.misses < best.after.misses)
            best = est;
        else
            best.iterations = est.iterations;
        best.unreachable = est.unreachable;
    }
    best.timings = est.timings;
    return best;
}

CageCoverage groupCoverage(scene::Graph& g, scene::NodeId group, int resolution)
{
    LILA_ZONE();

    progress::Scope root("Measuring Cage Coverage", kReadSpan + kHighsSpan + kBeforeSpan, progress::Cancellable);
    const std::unique_ptr<Prepared> p = prepare(g, group);
    const GroupInputs& gi = p->inputs.groups[0];
    const CageParams cp{gi.frontal, gi.rear, float(gi.settings.cageOffset), float(gi.settings.skew)};
    CageCoverage total;
    progress::Scope stage("Tracing rays", double(gi.lows.size()), progress::None, kBeforeSpan);
    progress::runOnWorker([&] { total = sumCoverage(gi, cp, *p->backend, resolution); });
    return total;
}

namespace
{
constexpr const char* kProbeAttrs[3] = {"bakeCageProbeX", "bakeCageProbeY", "bakeCageProbeZ"};

/** The low as the bake exports it with stand in place of its base mesh, which is restored before returning. */
scene::MeshPtr exportWithBase(scene::Graph& g, scene::NodeId mesh, const scene::MeshPtr& base, scene::MeshPtr stand)
{
    const scene::PlugRef plug = scene::modifiers::editPlug(g, mesh);
    g.setValue(plug, std::move(stand));
    scene::MeshPtr out = scene::modifiers::exportMesh(g, mesh);
    g.setValue(plug, base);
    return out;
}

/**
 * For each exported point, the base point it comes from: base positions ride the stack as point attributes, and the
 * nearest corner of the base triangle closest to where a point started is its source (kInvalid where none is found).
 */
std::vector<geo::Index> exportSources(scene::Graph& g, scene::NodeId mesh, const scene::MeshPtr& base)
{
    auto probed = std::make_shared<geo::Mesh>(*base);
    for (int k = 0; k < 3; ++k)
    {
        std::span<float> v = probed->pointAttrs().add(kProbeAttrs[k], geo::AttrType::Float).values<float>();
        for (geo::Index p = 0; p < geo::Index(v.size()); ++p)
            v[p] = float(base->position(p)[k]);
    }
    const scene::MeshPtr exported = exportWithBase(g, mesh, base, std::move(probed));
    std::span<const float> probe[3];
    for (int k = 0; k < 3; ++k)
        probe[k] = exported->pointAttrs().get<float>(kProbeAttrs[k]);
    const size_t n = exported->pointCapacity();
    if (probe[0].size() != n || probe[1].size() != n || probe[2].size() != n)
        throw std::runtime_error("its modifiers drop point attributes, so the estimate can't be painted back onto its base points");
    const geo::MeshBvh bvh(*base);
    const geo::Triangulation& tris = bvh.triangles();
    std::vector<geo::Index> out(n, geo::kInvalid);
    for (size_t i = 0; i < n; ++i)
    {
        geo::BvhClosest hit;
        if (!exported->pointValid(geo::Index(i)) || !bvh.closestPoint({probe[0][i], probe[1][i], probe[2][i]}, hit))
            continue;
        const geo::Index* c = &tris.corners[size_t(hit.triangle) * 3];
        const int k = hit.bary.x >= hit.bary.y && hit.bary.x >= hit.bary.z ? 0 : hit.bary.y >= hit.bary.z ? 1 : 2;
        out[i] = base->cornerPoint(c[k]);
    }
    return out;
}

/**
 * Base point offsets that, carried through the low's modifiers, give each exported point at least the offset the
 * estimate solved for it. Starts from the largest need among each base point's exported points, then grows the base
 * points whose exported points still fall short, since modifiers like subdivision average offsets down.
 */
std::vector<float> fitBaseOffsets(scene::Graph& g, scene::NodeId mesh, const scene::MeshPtr& base, std::span<const float> need)
{
    const PaintAttr attr = *paintAttr(kOffsetAttr);
    const std::vector<geo::Index> source = exportSources(g, mesh, base);
    if (source.size() != need.size())
        throw std::runtime_error("its modifiers give a different mesh each time, so the estimate can't be painted back onto its base points");
    std::vector<float> values = pointValues(*base, attr);
    std::vector<float> best(values.size(), -1.0f);
    for (size_t i = 0; i < need.size(); ++i)
        if (source[i] != geo::kInvalid)
            best[source[i]] = std::max(best[source[i]], need[i]);
    for (size_t p = 0; p < values.size(); ++p)
        if (best[p] >= 0.0f)
            values[p] = best[p];

    // Growth stops at twice the most any nearby exported point needs: a point that barely feeds a short exported
    // point would otherwise climb to the attribute's max and tent the cage around it.
    std::vector<float> ceiling(values.size(), 0.0f);
    for (geo::Index p = 0; p < base->pointCapacity(); ++p)
    {
        if (!base->pointValid(p))
            continue;
        float most = best[p];
        base->forEachPointEdge(p, [&](geo::Index e) { most = std::max(most, best[base->edgeOtherPoint(e, p)]); });
        ceiling[p] = std::max(values[p], 2.0f * most);
    }
    constexpr int kPasses = 6;
    for (int pass = 0; pass < kPasses; ++pass)
    {
        auto trial = std::make_shared<geo::Mesh>(*base);
        geo::Attribute& a = trial->pointAttrs().add(attr.name, geo::AttrType::Float);
        a.setDefault(attr.unpainted);
        std::copy(values.begin(), values.end(), a.values<float>().begin());
        const scene::MeshPtr exported = exportWithBase(g, mesh, base, std::move(trial));
        const std::span<const float> got = exported->pointAttrs().get<float>(attr.name);
        if (got.size() != need.size())
            break;
        std::vector<float> grow(values.size(), 1.0f);
        bool shortfall = false;
        for (size_t i = 0; i < need.size(); ++i)
            if (source[i] != geo::kInvalid && got[i] > 0.0f && need[i] > got[i] * 1.01f)
            {
                grow[source[i]] = std::max(grow[source[i]], std::min(need[i] / got[i], 2.0f));
                shortfall = true;
            }
        if (!shortfall)
            break;
        for (size_t p = 0; p < values.size(); ++p)
            values[p] = std::min({values[p] * grow[p], ceiling[p], attr.max});
    }
    return values;
}

/** Coverage after the write, reusing p's highs and their BVHs: only the lows (and the group's distances) are read again. */
CageCoverage remeasure(scene::Graph& g, scene::NodeId group, Prepared& p, int resolution)
{
    progress::Scope stage("Measuring through modifiers", 0.0, progress::None, kRemeasureSpan);
    scene::Node* grp = g.node(group);
    scene::Node* project = groupProject(g, *grp);
    g.evaluate();
    BakeInputs fresh = buildInputs(g, project->id(), {group}, false);
    if (fresh.groups.size() != 1)
        throw std::runtime_error(grp->name() + ": nothing to bake");
    GroupInputs& gi = p.inputs.groups[0];
    GroupInputs& now = fresh.groups[0];
    gi.lows = std::move(now.lows);
    gi.settings = now.settings;
    gi.frontal = now.frontal;
    gi.rear = now.rear;
    stage.setTotal(1.0 + double(gi.lows.size()));
    stage.step();
    const CageParams cp{gi.frontal, gi.rear, float(gi.settings.cageOffset), float(gi.settings.skew)};
    CageCoverage out;
    progress::runOnWorker([&] { out = sumCoverage(gi, cp, *p.backend, resolution); });
    return out;
}
} // namespace

json estimateGroupCage(scene::Graph& g, scene::NodeId group, const CageEstimateOptions& options, bool apply)
{
    LILA_ZONE();

    // Graph work stays on this thread; the tracing runs on a worker while the UI keeps drawing the progress card.
    progress::Scope root("Auto Painting Cage", kTotalSpan, progress::Cancellable);
    json timings = json::object();
    auto t0 = Clock::now();
    const std::unique_ptr<Prepared> p = prepare(g, group, &timings);
    scene::Node* grp = g.node(group);
    GroupSettings s = groupSettings(*grp);
    GroupInputs& gi = p->inputs.groups[0];
    addStageTiming(timings, "prepare", t0);

    t0 = Clock::now();
    CageCoverage before;
    {
        progress::Scope stage("Tracing rays", double(gi.lows.size()), progress::None, kBeforeSpan);
        const CageParams cp{gi.frontal, gi.rear, float(s.cageOffset), float(s.skew)};
        progress::runOnWorker([&] { before = sumCoverage(gi, cp, *p->backend, options.resolution); });
    }
    addStageTiming(timings, "before", t0);
    root.checkCancel();

    t0 = Clock::now();
    std::vector<CageLow> lows;
    std::vector<scene::NodeId> lowIds;
    std::vector<scene::MeshPtr> bases;
    for (const std::string& name : s.lows)
    {
        scene::Node* n = meshByName(g, name);
        if (!n)
            throw std::runtime_error(grp->name() + ": low " + name + " is not a mesh in the scene");
        scene::MeshPtr m = scene::modifiers::exportMesh(g, n->id());
        if (!m)
            m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
        const scene::MeshPtr base = std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, n->id())));
        if (!m || !base)
            throw std::runtime_error(n->name() + " has no mesh");
        lows.push_back({m, worldOf(g, n->id()), n->name()});
        lowIds.push_back(n->id());
        bases.push_back(base);
    }
    addStageTiming(timings, "export", t0);
    t0 = Clock::now();
    CageEstimate est;
    {
        progress::Scope stage("", 1.0, progress::None, kEstimateSpan);
        progress::runOnWorker([&] { est = estimateCage(lows, s, p->inputs, 0, *p->backend, options); });
    }
    addStageTiming(timings, "estimate", t0);
    root.checkCancel();

    // Lows whose modifiers change their points get the estimate painted back onto their base points; bakeOffset rides
    // the stack from there, so their real coverage is measured again after the write.
    json mapped = json::array();
    t0 = Clock::now();
    if (apply)
    {
        UndoChunk chunk("Auto Paint Bake Cage");
        progress::Scope stage("Painting offsets", double(lowIds.size()), progress::None, kApplySpan);
        for (size_t i = 0; i < lowIds.size(); ++i, stage.step())
        {
            stage.checkCancel();
            std::vector<float> values = std::move(est.offsets[i]);
            if (lows[i].mesh->pointCapacity() != bases[i]->pointCapacity())
            {
                try
                {
                    values = fitBaseOffsets(g, lowIds[i], bases[i], values);
                }
                catch (const std::runtime_error& e)
                {
                    throw std::runtime_error(lows[i].name + ": " + e.what());
                }
                mapped.push_back(lows[i].name);
            }
            if (!setPointValues(g, lowIds[i], *paintAttr(kOffsetAttr), values, "Auto Paint Bake Cage"))
                throw std::runtime_error("couldn't write bakeOffset on " + g.node(lowIds[i])->name());
        }
        s.frontal = est.frontal;
        s.rear = est.rear;
        s.cageOffset = est.cageOffset;
        setGroupSettings(g, group, s);
        if (!s.cage.empty())
            cageFromPaint(g, group);
        addStageTiming(timings, "apply", t0);
        t0 = Clock::now();
        if (!mapped.empty())
            est.after = remeasure(g, group, *p, options.resolution);
        addStageTiming(timings, "remeasure", t0);
    }
    else
        for (size_t i = 0; i < lows.size(); ++i)
            if (lows[i].mesh->pointCapacity() != bases[i]->pointCapacity())
                mapped.push_back(lows[i].name);
    json out = est.toJson();
    for (auto& [k, v] : est.timings.items())
        timings[k] = v;
    out["timings"] = timings;
    out["group"] = grp->path();
    out["before"] = before.toJson();
    out["applied"] = apply;
    out["mapped"] = mapped;
    return out;
}

void registerCageCommands()
{
    if (ops::find("bakeAutoPaintCage"))
        return;
    using T = ArgType;
    auto groupOf = [](const Args& a, const char* cmd) -> scene::Node& {
        scene::Graph& g = scene::Graph::current();
        const std::vector<std::string> names = a.objects();
        auto find = [&](const std::string& n) -> scene::Node* {
            try
            {
                return g.find(n);
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        };
        for (const std::string& name : names)
            if (scene::Node* n = find(name); n && isGroup(*n))
                return *n;
        for (const std::string& name : names)
            if (scene::Node* n = find(name); n && n->type().isA("mesh"))
                for (scene::NodeId id : g.nodes())
                    if (scene::Node* grp = g.node(id); grp && isGroup(*grp))
                    {
                        const GroupSettings s = groupSettings(*grp);
                        if (std::ranges::find(s.lows, n->path()) != s.lows.end() || s.cage == n->path())
                            return *grp;
                    }
        throw CommandError(std::string(cmd) + ": name a bake group (or select one, or its low)");
    };

    ops::registerCommand({
        .name = "bakeAutoPaintCage",
        .doc = "Auto paint a bake group's cage in one step: paints bakeOffset on its lows so every texel's ray starts just outside "
               "the high and meets it, and sets the group's frontal and rear distances (an editable cage moves to the result). "
               "Checks itself by tracing the lows as a bake would, growing the cage where rays still miss. One undo step. "
               "A low whose modifiers change its points gets the result painted on its base points (listed in mapped) and its "
               "coverage measured again through the stack. Returns the miss rates before and after.",
        .positionalDoc = "group (or its low; default the selection)",
        .flags = {presetFlag({"resolution", "res", T::Int, 512, "Texels per side of the coverage check.", {}, {}, 16.0, 8192.0}, kTextureSizePresets),
                  {"iterations", "it", T::Int, 4, "Coverage passes that may grow the cage.", {}, {}, 0.0, 32.0},
                  {"margin", "mg", T::Float, 0.004, "Gap kept between cage and high, as a fraction of each low's bounds diagonal.", {}, {}, 0.0, 1.0},
                  {"apply", "ap", T::Bool, true, "Write the result; False only reports it."}},
        .run = [groupOf](const Args& a) -> json {
            scene::Node& grp = groupOf(a, "bakeAutoPaintCage");
            CageEstimateOptions o;
            o.resolution = std::clamp(int(a.get<int64_t>("resolution", 512)), 16, 8192);
            o.iterations = std::clamp(int(a.get<int64_t>("iterations", 4)), 0, 32);
            o.margin = std::clamp(a.get<double>("margin", 0.004), 0.0, 1.0);
            try
            {
                return estimateGroupCage(scene::Graph::current(), grp.id(), o, a.get<bool>("apply", true));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("bakeAutoPaintCage: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "bakeCageCoverage",
        .doc = "How many of a bake group's rays miss its highs as it would bake now (one sample per texel): samples, misses, "
               "backfaces and missRate.",
        .positionalDoc = "group (or its low; default the selection)",
        .flags = {presetFlag({"resolution", "res", T::Int, 512, "Texels per side.", {}, {}, 16.0, 8192.0}, kTextureSizePresets)},
        .run = [groupOf](const Args& a) -> json {
            scene::Node& grp = groupOf(a, "bakeCageCoverage");
            try
            {
                json j = groupCoverage(scene::Graph::current(), grp.id(), std::clamp(int(a.get<int64_t>("resolution", 512)), 16, 8192)).toJson();
                j["group"] = grp.path();
                return j;
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("bakeCageCoverage: ") + e.what());
            }
        },
    });
}
} // namespace lila::bake
