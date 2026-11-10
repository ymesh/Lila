#include "scene/SkinPaint.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Anim.h"
#include "scene/Components.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/RigUtil.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene::skinpaint
{
using skin::PaintOutcome;

// ------------------------------------------------------------------------------------------------ painting

void PaintReport::add(uint32_t p, PaintOutcome o)
{
    ++counts[size_t(o)];
    if (skin::refused(o))
        refused.push_back(p);
}

void PaintReport::merge(const PaintReport& other)
{
    for (size_t k = 0; k < counts.size(); ++k)
        counts[k] += other.counts[k];
    refused.insert(refused.end(), other.refused.begin(), other.refused.end());
}

PaintOutcome PaintReport::mainRefusal() const
{
    auto best = PaintOutcome::Painted;
    size_t most = 0;
    for (size_t k = size_t(PaintOutcome::Ambiguous); k < counts.size(); ++k)
        if (counts[k] > most)
        {
            most = counts[k];
            best = PaintOutcome(k);
        }
    return best;
}

json PaintReport::toJson() const
{
    json refusedBy = json::object();
    for (size_t k = size_t(PaintOutcome::Ambiguous); k < counts.size(); ++k)
        if (counts[k])
            refusedBy[skin::outcomeName(PaintOutcome(k))] = counts[k];
    json j = {{"painted", counts[size_t(PaintOutcome::Painted)]}, {"unchanged", counts[size_t(PaintOutcome::Unchanged)]}, {"refused", refusedBy}};
    if (!refused.empty())
    {
        j["refusedPoints"] = refused;
        j["hint"] = refusalHint(mainRefusal());
    }
    return j;
}

const char* refusalHint(PaintOutcome o)
{
    switch (o)
    {
    case PaintOutcome::Ambiguous: return "more than one unlocked joint could take the weight: set a receiver or lock the others";
    case PaintOutcome::NowhereToGo: return "no other unlocked joint has weight here: unlock one or set a receiver";
    case PaintOutcome::InfluenceLimit: return "the point is at its influence limit: lock or remove a joint there, or raise maxInfluences";
    case PaintOutcome::Locked: return "the joint is locked";
    default: return "";
    }
}

PaintReport paintPoints(WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, skin::PaintMode mode, bool invert, float value, float strength,
                        const std::vector<char>& locked, const skin::PaintOptions& o, bool dryRun)
{
    PaintReport r;
    for (auto [p, f] : pts)
        r.add(p, skin::paintPoint(t, p, mode, invert, value, f * strength, locked, o, dryRun));
    return r;
}

bool strictDefault() { return PrefStore::get().value<bool>("general", kStrictPref, true); }

// ------------------------------------------------------------------------------------------------ settle

std::vector<glm::dvec3> skinPoints(const geo::Mesh& rest, const WeightTable& t, const std::vector<glm::dmat4>& mats, const std::vector<uint32_t>& pts)
{
    std::vector<glm::dvec3> out(rest.pointCapacity(), glm::dvec3(0.0));
    for (uint32_t p : pts)
    {
        if (p >= rest.pointCapacity() || !rest.pointValid(p))
            continue;
        const glm::dvec4 r(rest.position(p), 1.0);
        glm::dvec3 acc(0.0);
        double sum = 0.0;
        if (p < t.points.size())
            for (auto& [i, w] : t.points[p])
                if (i < mats.size())
                {
                    acc += double(w) * glm::dvec3(mats[i] * r);
                    sum += w;
                }
        out[p] = sum > 1e-9 ? acc / sum : rest.position(p);
    }
    return out;
}

std::vector<uint32_t> grow(const geo::Mesh& m, const std::vector<uint32_t>& seeds, int rings)
{
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    for (uint32_t p : seeds)
        if (p < m.pointCapacity() && m.pointValid(p) && seen.insert(p).second)
            out.push_back(p);
    size_t begin = 0;
    for (int r = 0; r < rings; ++r)
    {
        const size_t end = out.size();
        for (size_t k = begin; k < end; ++k)
            m.forEachPointEdge(out[k], [&](geo::Index e) {
                geo::Index q = m.edgeOtherPoint(e, out[k]);
                if (seen.insert(q).second)
                    out.push_back(q);
            });
        begin = end;
    }
    return out;
}

int settleRings(const SettleOptions& o) { return o.target == SettleTarget::Pose ? std::max(1, o.smoothIterations) + 1 : 2; }

namespace
{
/** Unit normal of point p over positions x: the sum of its faces' Newell vectors. */
glm::dvec3 pointNormal(const geo::Mesh& m, const std::vector<glm::dvec3>& x, uint32_t p)
{
    glm::dvec3 n(0.0);
    for (geo::Index f : m.pointFaces(p))
    {
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec3& a = x[pts[k]];
            const glm::dvec3& b = x[pts[(k + 1) % pts.size()]];
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
    }
    const double len = glm::length(n);
    return len > 1e-18 ? n / len : glm::dvec3(0, 1, 0);
}

/** A frame at p: normal, tangent toward its first edge neighbour, bitangent. Columns of the matrix. */
glm::dmat3 pointFrame(const geo::Mesh& m, const std::vector<glm::dvec3>& x, uint32_t p)
{
    const glm::dvec3 n = pointNormal(m, x, p);
    glm::dvec3 t(0.0);
    bool found = false;
    m.forEachPointEdge(p, [&](geo::Index e) {
        if (found)
            return;
        glm::dvec3 d = x[m.edgeOtherPoint(e, p)] - x[p];
        d -= n * glm::dot(d, n);
        if (glm::length(d) > 1e-12)
        {
            t = glm::normalize(d);
            found = true;
        }
    });
    if (!found)
        t = glm::normalize(glm::abs(n.x) < 0.9 ? glm::cross(n, glm::dvec3(1, 0, 0)) : glm::cross(n, glm::dvec3(0, 1, 0)));
    return glm::dmat3(t, glm::cross(n, t), n);
}

/** Jacobi Laplacian smoothing of the listed points; the others hold still. */
void smoothPositions(const geo::Mesh& m, std::vector<glm::dvec3>& x, const std::vector<uint32_t>& pts, int iterations, double step)
{
    std::vector<glm::dvec3> next(pts.size());
    for (int it = 0; it < iterations; ++it)
    {
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const uint32_t p = pts[k];
            glm::dvec3 avg(0.0);
            int n = 0;
            m.forEachPointEdge(p, [&](geo::Index e) {
                avg += x[m.edgeOtherPoint(e, p)];
                ++n;
            });
            next[k] = n ? glm::mix(x[p], avg / double(n), step) : x[p];
        }
        for (size_t k = 0; k < pts.size(); ++k)
            x[pts[k]] = next[k];
    }
}

std::vector<glm::dvec3> deltaMushTargets(const geo::Mesh& rest, const std::vector<glm::dvec3>& posed, const std::vector<uint32_t>& region, const SettleOptions& o)
{
    const int k = std::max(1, o.smoothIterations);
    // Smoothing reaches k rings out; the ring beyond holds still as the boundary.
    const std::vector<uint32_t> moving = grow(rest, region, k);
    std::vector<glm::dvec3> restX(rest.pointCapacity()), restS, posedS = posed;
    for (geo::Index p = 0; p < rest.pointCapacity(); ++p)
        restX[p] = rest.position(p);
    restS = restX;
    smoothPositions(rest, restS, moving, k, o.smoothStep);
    smoothPositions(rest, posedS, moving, k, o.smoothStep);
    std::vector<glm::dvec3> out = posed;
    for (uint32_t p : region)
    {
        const glm::dmat3 fr = pointFrame(rest, restS, p), fp = pointFrame(rest, posedS, p);
        const glm::dvec3 local = glm::transpose(fr) * (restX[p] - restS[p]);
        out[p] = posedS[p] + fp * local;
    }
    return out;
}

std::vector<glm::dvec3> simulateTargets(const geo::Mesh& rest, const std::vector<glm::dvec3>& posed, const std::vector<uint32_t>& region, const SettleOptions& o)
{
    std::vector<glm::dvec3> x = posed;
    std::unordered_set<uint32_t> free(region.begin(), region.end());
    struct Link
    {
        uint32_t a, b;
        double length, stiffness;
    };
    std::vector<Link> links;
    double edgeSum = 0.0;
    size_t edgeCount = 0;
    for (uint32_t p : region)
    {
        std::vector<uint32_t> ring;
        rest.forEachPointEdge(p, [&](geo::Index e) {
            const geo::Index q = rest.edgeOtherPoint(e, p);
            ring.push_back(q);
            const double len = glm::length(rest.position(q) - rest.position(p));
            if (p < q || !free.contains(q))
                links.push_back({p, q, len, 1.0});
            edgeSum += len;
            ++edgeCount;
        });
        // The one ring held to its rest spacing too, so the surface keeps its bend and area, not just its edge lengths.
        for (size_t i = 0; i < ring.size(); ++i)
            for (size_t j = i + 1; j < ring.size(); ++j)
                links.push_back({ring[i], ring[j], glm::length(rest.position(ring[i]) - rest.position(ring[j])), 0.3});
    }
    const double radius = edgeCount ? 0.5 * edgeSum / double(edgeCount) : 0.0;
    const std::vector<uint32_t> nearby = grow(rest, region, 2);
    auto isNeighbour = [&](uint32_t p, uint32_t q) {
        bool hit = false;
        rest.forEachPointEdge(p, [&](geo::Index e) { hit |= rest.edgeOtherPoint(e, p) == q; });
        return hit;
    };
    for (int it = 0; it < std::max(1, o.simIterations); ++it)
    {
        for (const Link& l : links)
        {
            const bool fa = free.contains(l.a), fb = free.contains(l.b);
            if (!fa && !fb)
                continue;
            glm::dvec3 d = x[l.b] - x[l.a];
            const double len = glm::length(d);
            if (len < 1e-12)
                continue;
            const glm::dvec3 corr = d * ((len - l.length) / len) * l.stiffness;
            if (fa && fb)
            {
                x[l.a] += corr * 0.5;
                x[l.b] -= corr * 0.5;
            }
            else if (fa)
                x[l.a] += corr;
            else
                x[l.b] -= corr;
        }
        if (!o.contact || radius <= 0.0)
            continue;
        // Contact: points closer than half an edge that aren't neighbours push apart, so a fold doesn't pass through itself.
        std::unordered_map<int64_t, std::vector<uint32_t>> cells;
        auto key = [&](const glm::dvec3& p) {
            const auto c = glm::ivec3(glm::floor(p / radius));
            return (int64_t(c.x) * 73856093) ^ (int64_t(c.y) * 19349663) ^ (int64_t(c.z) * 83492791);
        };
        for (uint32_t p : nearby)
            cells[key(x[p])].push_back(p);
        for (uint32_t p : region)
        {
            const auto c = glm::ivec3(glm::floor(x[p] / radius));
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        const glm::ivec3 cc = c + glm::ivec3(dx, dy, dz);
                        auto found = cells.find((int64_t(cc.x) * 73856093) ^ (int64_t(cc.y) * 19349663) ^ (int64_t(cc.z) * 83492791));
                        if (found == cells.end())
                            continue;
                        for (uint32_t q : found->second)
                        {
                            if (q == p || isNeighbour(p, q))
                                continue;
                            glm::dvec3 d = x[p] - x[q];
                            const double len = glm::length(d);
                            if (len >= radius || len < 1e-12)
                                continue;
                            const glm::dvec3 push = d / len * (radius - len);
                            if (free.contains(q))
                            {
                                x[p] += push * 0.5;
                                x[q] -= push * 0.5;
                            }
                            else
                                x[p] += push;
                        }
                    }
        }
    }
    return x;
}

/** Euclidean projection onto {w >= 0, sum w = total}. */
void projectSimplex(std::vector<double>& v, double total)
{
    if (v.empty())
        return;
    std::vector<double> u = v;
    std::sort(u.begin(), u.end(), std::greater<>());
    double css = 0.0, theta = 0.0;
    for (size_t j = 0; j < u.size(); ++j)
    {
        css += u[j];
        const double t = (css - total) / double(j + 1);
        if (u[j] - t > 0.0)
            theta = t;
    }
    for (double& x : v)
        x = std::max(0.0, x - theta);
}

/** min x'Hx - 2c'x over the scaled simplex, by projected gradient, for the entries where active is set. */
void solveSimplex(const std::vector<double>& h, const std::vector<double>& c, std::vector<double>& x, const std::vector<char>& active, double total)
{
    const size_t n = x.size();
    double lip = 1e-12;
    for (size_t i = 0; i < n; ++i)
    {
        if (!active[i])
            continue;
        double row = 0.0;
        for (size_t j = 0; j < n; ++j)
            if (active[j])
                row += std::abs(h[i * n + j]);
        lip = std::max(lip, row);
    }
    std::vector<double> sub, grad(n);
    for (int it = 0; it < 300; ++it)
    {
        for (size_t i = 0; i < n; ++i)
        {
            grad[i] = 0.0;
            if (!active[i])
                continue;
            for (size_t j = 0; j < n; ++j)
                if (active[j])
                    grad[i] += h[i * n + j] * x[j];
            grad[i] -= c[i];
        }
        sub.clear();
        for (size_t i = 0; i < n; ++i)
            if (active[i])
                sub.push_back(x[i] - grad[i] / lip);
        projectSimplex(sub, total);
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            x[i] = active[i] ? sub[k++] : 0.0;
    }
}
} // namespace

std::vector<glm::dvec3> settleTargets(const geo::Mesh& rest, const std::vector<glm::dvec3>& posed, const std::vector<uint32_t>& region, const SettleOptions& o)
{
    LILA_ZONE();
    return o.target == SettleTarget::Pose ? deltaMushTargets(rest, posed, region, o) : simulateTargets(rest, posed, region, o);
}

WeightFit::WeightFit(const geo::Mesh& rest, const WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, const std::vector<char>& locked, int maxInf)
    : mRest(rest), mMaxInf(maxInf)
{
    auto isLocked = [&](uint32_t i) { return i < locked.size() && locked[i]; };
    for (auto [p, amount] : pts)
    {
        if (p >= t.points.size() || p >= rest.pointCapacity() || !rest.pointValid(p) || amount <= 0.0f)
            continue;
        Point pt;
        pt.id = p;
        pt.amount = std::min(1.0f, amount);
        std::unordered_map<uint32_t, float> reach;
        double lockedSum = 0.0;
        for (auto& [i, w] : t.points[p])
        {
            if (isLocked(i))
            {
                pt.lockedWeights.push_back({i, w});
                lockedSum += w;
            }
            else
                reach[i] += w + 1.0f;
        }
        rest.forEachPointEdge(p, [&](geo::Index e) {
            const geo::Index q = rest.edgeOtherPoint(e, p);
            if (q < t.points.size())
                for (auto& [i, w] : t.points[q])
                    if (!isLocked(i))
                        reach[i] += w;
        });
        std::vector<std::pair<float, uint32_t>> order;
        for (auto& [i, w] : reach)
            order.push_back({w, i});
        std::sort(order.begin(), order.end(), std::greater<>());
        // A few more candidates than the point may keep, so the fit can trade one joint for another.
        order.resize(std::min<size_t>(order.size(), size_t(std::max(4, maxInf + 4))));
        for (auto& [w, i] : order)
        {
            pt.candidates.push_back(i);
            pt.current.push_back(t.get(p, i));
        }
        pt.room = std::max(0.0, 1.0 - lockedSum);
        if (pt.candidates.empty() || pt.room <= 0.0)
            continue;
        const size_t n = pt.candidates.size();
        pt.a.assign(n * n, 0.0);
        pt.b.assign(n, 0.0);
        mPoints.push_back(std::move(pt));
    }
}

void WeightFit::addSample(const std::vector<glm::dmat4>& mats, const std::vector<glm::dvec3>& targets)
{
    tbb::parallel_for(size_t(0), mPoints.size(), [&](size_t k) {
        Point& pt = mPoints[k];
        const glm::dvec4 r(mRest.position(pt.id), 1.0);
        auto moved = [&](uint32_t i) { return i < mats.size() ? glm::dvec3(mats[i] * r) : glm::dvec3(r); };
        glm::dvec3 fixedPart(0.0);
        for (auto [i, w] : pt.lockedWeights)
            fixedPart += double(w) * moved(i);
        const glm::dvec3 goal = (pt.id < targets.size() ? targets[pt.id] : glm::dvec3(r)) - fixedPart;
        const size_t n = pt.candidates.size();
        std::vector<glm::dvec3> y(n);
        for (size_t i = 0; i < n; ++i)
            y[i] = moved(pt.candidates[i]);
        for (size_t i = 0; i < n; ++i)
        {
            pt.b[i] += glm::dot(y[i], goal);
            for (size_t j = 0; j < n; ++j)
                pt.a[i * n + j] += glm::dot(y[i], y[j]);
        }
    });
}

void WeightFit::solve(WeightTable& t, double regularize) const
{
    LILA_ZONE();

    std::vector<std::vector<WeightTable::Entry>> results(mPoints.size());
    tbb::parallel_for(size_t(0), mPoints.size(), [&](size_t k) {
        const Point& pt = mPoints[k];
        const size_t n = pt.candidates.size();
        double trace = 0.0;
        for (size_t i = 0; i < n; ++i)
            trace += pt.a[i * n + i];
        // Scaled by the data so the pull toward the current weights means the same on any mesh size.
        const double lambda = std::max(1e-9, regularize) * std::max(1e-9, trace / double(n));
        std::vector<double> h = pt.a, c = pt.b, x = pt.current;
        for (size_t i = 0; i < n; ++i)
        {
            h[i * n + i] += lambda;
            c[i] += lambda * pt.current[i];
        }
        std::vector<char> active(n, 1);
        projectSimplex(x, pt.room);
        solveSimplex(h, c, x, active, pt.room);
        const int slots = std::max(1, mMaxInf - int(pt.lockedWeights.size()));
        if (int(std::count_if(x.begin(), x.end(), [](double v) { return v > 1e-6; })) > slots)
        {
            std::vector<size_t> idx(n);
            std::iota(idx.begin(), idx.end(), 0);
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return x[a] > x[b]; });
            std::fill(active.begin(), active.end(), 0);
            for (int s = 0; s < slots && s < int(n); ++s)
                active[idx[size_t(s)]] = 1;
            for (size_t i = 0; i < n; ++i)
                if (!active[i])
                    x[i] = 0.0;
            solveSimplex(h, c, x, active, pt.room);
        }
        auto& out = results[k];
        for (auto [i, w] : pt.lockedWeights)
            out.push_back({i, w});
        for (size_t i = 0; i < n; ++i)
        {
            const double v = (1.0 - pt.amount) * pt.current[i] + pt.amount * x[i];
            if (v > 1e-6)
                out.push_back({pt.candidates[i], float(v)});
        }
        // Weights of the point that were not candidates (dropped by the candidate cap) fade out with the blend.
        std::sort(out.begin(), out.end());
    });
    for (size_t k = 0; k < mPoints.size(); ++k)
    {
        const uint32_t p = mPoints[k].id;
        t.points[p] = std::move(results[k]);
        std::vector<char> keep;
        for (auto [i, w] : mPoints[k].lockedWeights)
        {
            keep.resize(std::max<size_t>(keep.size(), i + 1), 0);
            keep[i] = 1;
        }
        t.prune(p, mMaxInf, 1e-4f, keep);
        t.markPointAuthored(p);
    }
}

void settlePoints(Graph& g, NodeId skin, WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, const std::vector<char>& locked, int maxInf,
                  const SettleOptions& o)
{
    LILA_ZONE();

    MeshPtr rest = skin::inputMesh(g, skin);
    if (!rest || pts.empty())
        return;
    std::vector<uint32_t> region;
    for (auto [p, a] : pts)
        if (a > 0.0f)
            region.push_back(p);
    const std::vector<glm::dmat4> mats = skin::objectMatrices(g, skin);
    const std::vector<uint32_t> reach = grow(*rest, region, settleRings(o));
    const std::vector<glm::dvec3> posed = skinPoints(*rest, t, mats, reach);
    WeightFit fit(*rest, t, pts, locked, maxInf);
    fit.addSample(mats, settleTargets(*rest, posed, region, o));
    fit.solve(t, o.regularize);
}

void setLayerMask(Graph& g, NodeId skin, int layer, const std::vector<float>& mask)
{
    auto l = skin::layers(g, skin);
    if (!l || layer <= 0 || layer >= int(l->layers.size()))
        throw CommandError("the base layer has no mask; pick a layer above it");
    auto next = std::make_shared<SkinLayers>(*l);
    SkinLayer& target = next->layers[size_t(layer)];
    const int maxInf = int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences));
    target.weights.points.resize(std::max(target.weights.points.size(), mask.size()));
    target.weights.automatic.resize(target.weights.points.size());
    // A point the mask uncovers shows what is below it until it is painted, not weights from when the layer was made.
    for (size_t p = 0; p < mask.size(); ++p)
    {
        const float before = p < target.mask.size() ? target.mask[p] : 0.0f;
        if (before <= 0.0f && mask[p] > 0.0f)
            skin::compositePoint(*l, layer, p, target.weights, maxInf);
    }
    target.mask = mask;
    const size_t n = l->layers[0].weights.points.size();
    skin::setLayers(g, skin, next);
    skin::setWeights(g, skin, skin::composite(*next, n, maxInf));
}

// ------------------------------------------------------------------------------------------------ commands

namespace
{
using T = ArgType;
using rigutil::graph;

NodeId skinOf(const Args& a, const char* cmd)
{
    auto objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": give a skinned mesh");
    return skin::resolve(graph(), objs.back(), cmd);
}

/** The vertices flag, else the mesh's selected vertices, else every point. */
std::vector<uint32_t> pointsOf(const Args& a, Graph& g, NodeId skin)
{
    std::vector<uint32_t> out;
    if (a.has("vertices"))
    {
        for (const json& v : a.flags["vertices"])
            out.push_back(v.get<uint32_t>());
        return out;
    }
    const NodeId mesh = skin::meshOf(g, skin);
    if (mesh != kNoNode && components::mode() != ComponentMode::Object)
        out = components::pointsOf(mesh);
    if (out.empty())
        if (MeshPtr m = skin::inputMesh(g, skin))
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                    out.push_back(p);
    return out;
}

int64_t influenceArg(const std::vector<skin::Influence>& infs, const std::string& name, const char* cmd)
{
    const int64_t i = skin::indexOf(infs, name);
    if (i < 0)
        throw CommandError(std::string(cmd) + ": " + name + " is not an influence of the skin");
    return i;
}

int layerArg(const SkinLayers& l, const json& v, const char* cmd)
{
    if (v.is_number_integer())
    {
        const int i = v.get<int>();
        if (i < 0 || i >= int(l.layers.size()))
            throw CommandError(std::string(cmd) + ": no layer " + std::to_string(i));
        return i;
    }
    const std::string name = v.get<std::string>();
    for (size_t i = 0; i < l.layers.size(); ++i)
        if (l.layers[i].name == name)
            return int(i);
    throw CommandError(std::string(cmd) + ": no layer named " + name);
}

std::shared_ptr<const SkinLayers> layersOrThrow(Graph& g, NodeId skin, const char* cmd)
{
    auto l = skin::layers(g, skin);
    if (!l)
        throw CommandError(std::string(cmd) + ": the skin has no weight layers; add one with addSkinLayer");
    return l;
}

json layersJson(const SkinLayers& l)
{
    json arr = json::array();
    for (size_t i = 0; i < l.layers.size(); ++i)
    {
        const SkinLayer& layer = l.layers[i];
        size_t masked = 0;
        for (float m : layer.mask)
            masked += m > 0.0f;
        arr.push_back({{"index", i}, {"name", layer.name}, {"enabled", layer.enabled}, {"opacity", layer.opacity}, {"active", int(i) == l.active},
                       {"maskedPoints", i == 0 ? json(nullptr) : json(masked)}});
    }
    return arr;
}

std::string uniqueLayerName(const SkinLayers& l, std::string base)
{
    auto taken = [&](const std::string& n) { return std::any_of(l.layers.begin(), l.layers.end(), [&](const SkinLayer& x) { return x.name == n; }); };
    if (!taken(base))
        return base;
    for (int k = 2;; ++k)
        if (std::string n = base + std::to_string(k); !taken(n))
            return n;
}

SettleOptions settleOptions(const Args& a)
{
    SettleOptions o;
    const std::string target = a.get<std::string>("target", "pose");
    if (target != "pose" && target != "simulate")
        throw CommandError("settleSkinWeights: target is pose or simulate");
    o.target = target == "simulate" ? SettleTarget::Simulate : SettleTarget::Pose;
    o.smoothIterations = a.get<int>("smoothIterations", 10);
    o.simIterations = a.get<int>("simIterations", 40);
    o.regularize = a.get<double>("regularize", 0.05);
    o.contact = a.get<bool>("contact", true);
    return o;
}

const std::vector<std::string> kModes = {"add", "replace", "scale"};
} // namespace

void registerCommands()
{
    diagnostics::validators().add("skinWeights", {.doc = "Skinned meshes with automatic weights: weight Lila split between joints rather than one a person set.",
                                                  .run = [] { skin::reportAutomatic(Graph::current()); }},
                                  "lila");

    ops::registerCommand({
        .name = "floodSkinWeights",
        .doc = "Paint one influence's weight over whole vertices, as the Paint Weights tool's Flood does.\n\n"
               "The weight moved goes to (or comes from) the receiver, else the one other unlocked influence with weight at the "
               "vertex. With strict on, a vertex where more than one could take it is refused; off, it is split in proportion "
               "and marked automatic. Returns counts of painted, unchanged and refused vertices (by reason) and the refused ids.",
        .positionalDoc = "mesh",
        .flags = {
            {"influence", "inf", T::String, nullptr, "The joint to paint."},
            {"receiver", "rcv", T::String, nullptr, "The joint that takes removed weight and gives added weight."},
            {"mode", "m", T::String, "replace", "add | replace | scale", kModes},
            {"value", "val", T::Float, 1.0, "The weight Replace paints toward, Add adds, Scale multiplies by.", {}, {}, 0.0, 1.0},
            {"strength", "st", T::Float, 1.0, "How far toward the value each vertex goes.", {}, {}, 0.0, 1.0},
            {"invert", "inv", T::Bool, false, "Add subtracts instead."},
            {"strict", "sc", T::Bool, nullptr, "Refuse vertices whose weight would be split; the Strict preference when left out."},
            {"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            NodeId skin = skinOf(a, "floodSkinWeights");
            if (!a.has("influence"))
                throw CommandError("floodSkinWeights: give influence");
            skin::WeightEdit edit(g, skin);
            skin::PaintOptions o;
            o.influence = uint32_t(influenceArg(edit.influences(), a.get<std::string>("influence"), "floodSkinWeights"));
            if (a.has("receiver"))
                o.receiver = influenceArg(edit.influences(), a.get<std::string>("receiver"), "floodSkinWeights");
            o.strict = a.has("strict") ? a.get<bool>("strict") : strictDefault();
            o.maxInfluences = edit.maxInfluences();
            const std::string mode = a.get<std::string>("mode", "replace");
            const auto m = mode == "add" ? skin::PaintMode::Add : mode == "scale" ? skin::PaintMode::Scale : skin::PaintMode::Replace;
            if (std::find(kModes.begin(), kModes.end(), mode) == kModes.end())
                throw CommandError("floodSkinWeights: mode is add, replace or scale");
            std::vector<std::pair<uint32_t, float>> pts;
            for (uint32_t p : pointsOf(a, g, skin))
                pts.push_back({p, 1.0f});
            PaintReport r = paintPoints(edit.table(), pts, m, a.get<bool>("invert"), a.get<float>("value", 1.0f), a.get<float>("strength", 1.0f), edit.locked(), o);
            if (r.counts[size_t(PaintOutcome::Painted)])
            {
                edit.commit();
                skin::reportAutomatic(g);
            }
            if (r.refusedCount())
                log::warn("floodSkinWeights: {} vertices refused: {}", r.refusedCount(), refusalHint(r.mainRefusal()));
            return r.toJson();
        },
    });

    ops::registerCommand({
        .name = "acceptSkinWeights",
        .doc = "Mark automatic weights as set by you, so they no longer warn. Returns how many vertices changed.",
        .positionalDoc = "mesh",
        .flags = {{"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "acceptSkinWeights");
            const std::vector<uint32_t> pts = pointsOf(a, g, skin);
            WeightTable t = skin::table(g, skin);
            int accepted = 0;
            for (uint32_t p : pts)
                if (t.pointAutomatic(p))
                {
                    t.markPointAuthored(p);
                    ++accepted;
                }
            if (!accepted)
                return 0;
            // A layered skin's flags are the union of its layers', so every layer lets go of them at these points.
            if (auto l = skin::layers(g, skin))
            {
                auto next = std::make_shared<SkinLayers>(*l);
                for (SkinLayer& layer : next->layers)
                    for (uint32_t p : pts)
                        layer.weights.markPointAuthored(p);
                skin::setLayers(g, skin, next);
            }
            skin::setWeights(g, skin, t);
            skin::reportAutomatic(g);
            return accepted;
        },
    });

    ops::registerCommand({
        .name = "selectAutomaticWeights",
        .doc = "Select the vertices of a skinned mesh whose weights are automatic. Returns how many.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "selectAutomaticWeights");
            const NodeId mesh = skin::meshOf(g, skin);
            if (mesh == kNoNode)
                throw CommandError("selectAutomaticWeights: the skin feeds no mesh");
            std::vector<uint32_t> pts = skin::table(g, skin).automaticPoints();
            components::setMode(ComponentMode::Vertex);
            components::select(mesh, ComponentMode::Vertex, pts);
            return pts.size();
        },
    });

    ops::registerCommand({
        .name = "relaxSkinWeights",
        .doc = "Relax weights over the surface, edge-length weighted so dense and sparse areas spread alike.\n\n"
               "With keepAuthored, vertices whose weights are all set by you hold still and the automatic ones settle between "
               "them: paint the key areas, then relax the rest into place. Locked joints keep their weights.",
        .positionalDoc = "mesh",
        .flags = {
            {"iterations", "i", T::Int, 30, "Passes; more spreads further.", {}, {}, 1.0, 1000.0},
            {"strength", "st", T::Float, 1.0, "How far each pass moves toward the neighbours.", {}, {}, 0.0, 1.0},
            {"keepAuthored", "ka", T::Bool, true, "Hold vertices with no automatic weights still."},
            {"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            NodeId skin = skinOf(a, "relaxSkinWeights");
            MeshPtr rest = skin::inputMesh(g, skin);
            if (!rest)
                throw CommandError("relaxSkinWeights: the mesh has no geometry");
            skin::WeightEdit edit(g, skin);
            WeightTable& t = edit.table();
            std::vector<std::pair<uint32_t, float>> pts;
            for (uint32_t p : pointsOf(a, g, skin))
                pts.push_back({p, a.get<float>("strength", 1.0f)});
            std::vector<char> fixed;
            if (a.get<bool>("keepAuthored", true))
            {
                fixed.assign(t.points.size(), 0);
                for (size_t p = 0; p < t.points.size(); ++p)
                    fixed[p] = !t.pointAutomatic(p);
            }
            skin::relaxPoints(*rest, t, pts, edit.locked(), edit.maxInfluences(), a.get<int>("iterations", 30), fixed.empty() ? nullptr : &fixed);
            edit.commit();
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "settleSkinWeights",
        .doc = "Fit weights so the skin bends into a settled shape: pinches, collapses and candy wrappers ease out.\n\n"
               "target=pose aims each vertex at a delta mush of the posed mesh (smoothed, with the rest detail put back). "
               "target=simulate relaxes the posed mesh toward its rest edge lengths, keeping folds from passing through "
               "themselves. Give start and end to fit over that frame range instead of the current pose. Locked joints keep "
               "their weights; the fit stays within maxInfluences.",
        .positionalDoc = "mesh",
        .flags = {
            {"target", "t", T::String, "pose", "pose | simulate", {"pose", "simulate"}},
            {"start", "s", T::Int, nullptr, "First frame to fit over."},
            {"end", "e", T::Int, nullptr, "Last frame to fit over."},
            {"step", "by", T::Int, 1, "Frames between samples.", {}, {}, 1.0},
            {"strength", "st", T::Float, 1.0, "How far toward the fit.", {}, {}, 0.0, 1.0},
            {"regularize", "rg", T::Float, 0.05, "Pull toward the current weights; higher keeps more of what is painted.", {}, {}, 0.0, 10.0},
            {"smoothIterations", "si", T::Int, 10, "pose: smoothing passes of the delta mush.", {}, {}, 1.0, 200.0},
            {"simIterations", "ni", T::Int, 40, "simulate: solver passes.", {}, {}, 1.0, 1000.0},
            {"contact", "ct", T::Bool, true, "simulate: keep folds from passing through themselves."},
            {"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            NodeId skin = skinOf(a, "settleSkinWeights");
            MeshPtr rest = skin::inputMesh(g, skin);
            if (!rest)
                throw CommandError("settleSkinWeights: the mesh has no geometry");
            const SettleOptions o = settleOptions(a);
            skin::WeightEdit edit(g, skin);
            WeightTable& t = edit.table();
            std::vector<std::pair<uint32_t, float>> pts;
            std::vector<uint32_t> region;
            for (uint32_t p : pointsOf(a, g, skin))
            {
                pts.push_back({p, a.get<float>("strength", 1.0f)});
                region.push_back(p);
            }
            const std::vector<uint32_t> reach = grow(*rest, region, settleRings(o));
            WeightFit fit(*rest, t, pts, edit.locked(), edit.maxInfluences());
            int samples = 0;
            auto sample = [&] {
                const std::vector<glm::dmat4> mats = skin::objectMatrices(g, skin);
                fit.addSample(mats, settleTargets(*rest, skinPoints(*rest, t, mats, reach), region, o));
                ++samples;
            };
            if (a.has("start") || a.has("end"))
            {
                const double fps = std::max(anim::playback().fps, 1e-6);
                const int64_t start = a.has("start") ? a.get<int64_t>("start") : int64_t(std::llround(anim::playback().minTime * fps));
                const int64_t end = a.has("end") ? a.get<int64_t>("end") : int64_t(std::llround(anim::playback().maxTime * fps));
                if (end < start)
                    throw CommandError("settleSkinWeights: end is before start");
                const double saved = g.time();
                {
                    UndoStack& u = UndoStack::get();
                    const bool was = u.enabled();
                    u.setEnabled(false);
                    for (int64_t f = start; f <= end; f += std::max(1, a.get<int>("step", 1)))
                    {
                        g.setTime(double(f) / fps);
                        sample();
                    }
                    g.setTime(saved);
                    g.evaluate();
                    u.setEnabled(was);
                }
            }
            else
                sample();
            fit.solve(t, o.regularize);
            edit.commit();
            skin::reportAutomatic(g);
            return {{"samples", samples}, {"points", pts.size()}};
        },
    });

    ops::registerCommand({
        .name = "skinLayers",
        .doc = "A skinned mesh's weight layers, bottom (the base) first: name, enabled, opacity, active and how many points each mask uncovers.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto l = skin::layers(g, skinOf(a, "skinLayers"));
            return l ? layersJson(*l) : json::array();
        },
    });

    ops::registerCommand({
        .name = "addSkinLayer",
        .doc = "Add a weight layer above the others and make it the one painted. The first layer added also makes the "
               "base from the current weights. A new layer shows nothing until painted or its mask is.",
        .positionalDoc = "mesh",
        .flags = {{"name", "n", T::String, "Layer", "Name of the layer."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "addSkinLayer");
            MeshPtr m = skin::inputMesh(g, skin);
            const size_t n = m ? m->pointCapacity() : 0;
            auto l = std::make_shared<SkinLayers>();
            if (auto old = skin::layers(g, skin))
                *l = *old;
            else
                l->layers.push_back({.name = "Base", .enabled = true, .opacity = 1.0f, .weights = skin::table(g, skin), .mask = {}});
            const int maxInf = int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences));
            SkinLayer layer{.name = uniqueLayerName(*l, a.get<std::string>("name", "Layer")), .enabled = true, .opacity = 1.0f,
                            .weights = skin::composite(*l, n, maxInf), .mask = std::vector<float>(n, 0.0f)};
            const std::string name = layer.name;
            l->layers.push_back(std::move(layer));
            l->active = int(l->layers.size()) - 1;
            skin::setLayers(g, skin, l);
            return name;
        },
    });

    ops::registerCommand({
        .name = "removeSkinLayer",
        .doc = "Remove a weight layer; the weights become the composite of the rest. Removing the last one above the base "
               "leaves plain weights.",
        .positionalDoc = "mesh",
        .flags = {{"layer", "l", T::Any, nullptr, "Layer name or index (not the base, 0)."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "removeSkinLayer");
            auto old = layersOrThrow(g, skin, "removeSkinLayer");
            if (!a.has("layer"))
                throw CommandError("removeSkinLayer: give layer");
            const int i = layerArg(*old, a.flags["layer"], "removeSkinLayer");
            if (i == 0)
                throw CommandError("removeSkinLayer: the base layer stays; use flattenSkinLayers to drop the layers");
            auto l = std::make_shared<SkinLayers>(*old);
            l->layers.erase(l->layers.begin() + i);
            l->active = std::clamp(l->active >= i ? l->active - 1 : l->active, 0, int(l->layers.size()) - 1);
            const int maxInf = int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences));
            const size_t n = l->layers[0].weights.points.size();
            skin::setWeights(g, skin, skin::composite(*l, n, maxInf));
            skin::setLayers(g, skin, l->layers.size() > 1 ? l : nullptr);
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "editSkinLayer",
        .doc = "Rename, enable, fade, reorder or make active a weight layer.",
        .positionalDoc = "mesh",
        .flags = {
            {"layer", "l", T::Any, nullptr, "Layer name or index."},
            {"name", "n", T::String, nullptr, "New name."},
            {"enabled", "en", T::Bool, nullptr, "Whether it shows in the weights."},
            {"opacity", "op", T::Float, nullptr, "How much it shows, 0 to 1.", {}, {}, 0.0, 1.0},
            {"active", "ac", T::Bool, nullptr, "Make it the layer painting edits."},
            {"index", "ix", T::Int, nullptr, "Move it to this place, 1 or above (the base stays at 0)."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "editSkinLayer");
            auto old = layersOrThrow(g, skin, "editSkinLayer");
            if (!a.has("layer"))
                throw CommandError("editSkinLayer: give layer");
            int i = layerArg(*old, a.flags["layer"], "editSkinLayer");
            auto l = std::make_shared<SkinLayers>(*old);
            SkinLayer& layer = l->layers[size_t(i)];
            if (a.has("name"))
                layer.name = uniqueLayerName(*old, a.get<std::string>("name"));
            if (a.has("enabled"))
                layer.enabled = i == 0 || a.get<bool>("enabled");
            if (a.has("opacity"))
                layer.opacity = i == 0 ? 1.0f : std::clamp(a.get<float>("opacity"), 0.0f, 1.0f);
            if (a.get<bool>("active", false))
                l->active = i;
            if (a.has("index") && i > 0)
            {
                const int to = std::clamp(a.get<int>("index"), 1, int(l->layers.size()) - 1);
                const bool wasActive = l->active == i;
                SkinLayer moved = std::move(l->layers[size_t(i)]);
                l->layers.erase(l->layers.begin() + i);
                l->layers.insert(l->layers.begin() + to, std::move(moved));
                if (wasActive)
                    l->active = to;
                else if (l->active > i && l->active <= to)
                    --l->active;
                else if (l->active < i && l->active >= to)
                    ++l->active;
                i = to;
            }
            const int maxInf = int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences));
            skin::setLayers(g, skin, l);
            skin::setWeights(g, skin, skin::composite(*l, l->layers[0].weights.points.size(), maxInf));
            skin::reportAutomatic(g);
            return layersJson(*l)[size_t(i)];
        },
    });

    ops::registerCommand({
        .name = "setSkinLayerMask",
        .doc = "Set where a weight layer shows: values per point (0 hides it, 1 shows it), or one value on the given or "
               "selected vertices.",
        .positionalDoc = "mesh",
        .flags = {
            {"layer", "l", T::Any, nullptr, "Layer name or index (not the base, 0); the active layer when left out."},
            {"values", "vals", T::List, nullptr, "A value for every point."},
            {"value", "val", T::Float, 1.0, "The value set on the vertices, without values.", {}, {}, 0.0, 1.0},
            {"vertices", "v", T::List, nullptr, "Point ids; defaults to the selected vertices, else all."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "setSkinLayerMask");
            auto l = layersOrThrow(g, skin, "setSkinLayerMask");
            const int i = a.has("layer") ? layerArg(*l, a.flags["layer"], "setSkinLayerMask") : l->active;
            if (i == 0)
                throw CommandError("setSkinLayerMask: the base layer has no mask");
            std::vector<float> mask = l->layers[size_t(i)].mask;
            mask.resize(l->layers[0].weights.points.size(), 0.0f);
            if (a.has("values"))
            {
                const json& vals = a.flags["values"];
                for (size_t p = 0; p < std::min(mask.size(), vals.size()); ++p)
                    mask[p] = std::clamp(vals[p].get<float>(), 0.0f, 1.0f);
            }
            else
                for (uint32_t p : pointsOf(a, g, skin))
                    if (p < mask.size())
                        mask[p] = a.get<float>("value", 1.0f);
            setLayerMask(g, skin, i, mask);
            skin::reportAutomatic(g);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "flattenSkinLayers",
        .doc = "Merge the weight layers into plain weights and drop them.",
        .positionalDoc = "mesh",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId skin = skinOf(a, "flattenSkinLayers");
            auto l = layersOrThrow(g, skin, "flattenSkinLayers");
            const int maxInf = int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences));
            skin::setWeights(g, skin, skin::composite(*l, l->layers[0].weights.points.size(), maxInf));
            skin::setLayers(g, skin, nullptr);
            return nullptr;
        },
    });
}
} // namespace lila::scene::skinpaint
