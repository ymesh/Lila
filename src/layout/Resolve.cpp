#include "layout/Resolve.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"
#include "layout/Surface.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace lila::layout
{
namespace
{
uint64_t cellKey(const glm::ivec3& c)
{
    auto part = [](int v) { return uint64_t(uint32_t(v + (1 << 20)) & 0x1fffffu); };
    return part(c.x) | (part(c.y) << 21) | (part(c.z) << 42);
}

glm::ivec3 cellOf(const glm::dvec3& p, double cell) { return glm::ivec3(glm::floor(p / cell)); }

uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/** Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5). */
glm::dvec3 closestOnTriangle(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return a;
    const glm::dvec3 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return a + ab * (d1 / (d1 - d3));
    const glm::dvec3 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

glm::dvec3 reflected(glm::dvec3 p, int axis)
{
    p[axis] = -p[axis];
    return p;
}
} // namespace

struct Resolved::Grid
{
    double cell = 1.0;
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> cells;
    size_t segments = 0;
};

struct Resolved::Labels
{
    std::vector<glm::dvec3> pos;
    std::vector<glm::uvec3> tris;
    /** The mesh triangle each label triangle came from. */
    std::vector<uint32_t> origin;
    std::vector<int> label;
    double cell = 1.0;
    double eps = 0.1;
    double probe = 0.1;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    std::vector<PanelInfo> panels;
    size_t meshTriangles = 0;

    double distanceTo(uint32_t t, const glm::dvec3& p) const
    {
        const glm::uvec3 i = tris[t];
        return glm::length(p - closestOnTriangle(p, pos[i.x], pos[i.y], pos[i.z]));
    }

    /** Nearest label triangle to p (uncut only when asked); -1 when there are none. */
    int nearest(const glm::dvec3& p, bool uncutOnly) const
    {
        const glm::ivec3 c = cellOf(p, cell);
        int best = -1;
        double bestD = std::numeric_limits<double>::max();
        for (int ring = 0; ring <= 4; ++ring)
        {
            for (int dx = -ring; dx <= ring; ++dx)
                for (int dy = -ring; dy <= ring; ++dy)
                    for (int dz = -ring; dz <= ring; ++dz)
                    {
                        if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != ring)
                            continue;
                        auto it = cells.find(cellKey(c + glm::ivec3(dx, dy, dz)));
                        if (it == cells.end())
                            continue;
                        for (uint32_t t : it->second)
                        {
                            if (uncutOnly && label[t] < 0)
                                continue;
                            const double d = distanceTo(t, p);
                            if (d < bestD)
                            {
                                bestD = d;
                                best = int(t);
                            }
                        }
                    }
            // Anything found within this ring's reach can't be beaten further out.
            if (best >= 0 && bestD <= ring * cell)
                return best;
        }
        if (best >= 0)
            return best;
        for (uint32_t t = 0; t < tris.size(); ++t)
            if (!uncutOnly || label[t] >= 0)
                if (const double d = distanceTo(t, p); d < bestD)
                {
                    bestD = d;
                    best = int(t);
                }
        return best;
    }
};

std::shared_ptr<const Resolved> Resolved::build(const PanelLayout& layout, scene::MeshPtr mesh, const ResolveOptions& opts)
{
    LILA_ZONE();
    auto r = std::shared_ptr<Resolved>(new Resolved());
    r->mLayout = layout;
    r->mMesh = mesh;
    if (!mesh || mesh->numFaces() == 0)
    {
        r->buildGrid();
        return r;
    }
    const Surface surf(*mesh);
    r->mSpacing = opts.spacing > 0.0 ? opts.spacing : defaultSpacing(layout, surf);

    auto countRebound = [&](const Anchor& a) {
        bool lost = false;
        surf.resolve(a, &lost);
        r->mRebound += int(lost);
    };
    for (const Junction& j : layout.junctions)
        countRebound(j.at);
    for (const Run& run : layout.runs)
        for (const Anchor& a : run.controls)
            countRebound(a);
    if (r->mRebound > 0)
        r->mWarnings.push_back(std::to_string(r->mRebound) + " layout points moved to the closest surface: the mesh topology changed");

    for (const Junction& j : layout.junctions)
        r->mJunctions.push_back({j.id, surf.resolve(j.at), 0, j.corner});
    for (const Run& run : layout.runs)
    {
        ResolvedRun rr;
        rr.id = run.id;
        rr.a = run.a;
        rr.b = run.b;
        rr.set = run.set;
        rr.path = runPath(layout, run, surf, r->mSpacing);
        if (rr.path.empty())
            continue;
        const LineSet& set = layout.setOf(run);
        rr.width = layout.widthOf(run);
        rr.depth = layout.depthOf(run);
        rr.profile = layout.profileOf(run);
        rr.output = set.output;
        if (const Junction* a = layout.junction(run.a))
            rr.endA = a->end;
        if (const Junction* b = layout.junction(run.b))
            rr.endB = b->end;
        r->mRuns.push_back(std::move(rr));
    }

    const int axis = layout.mirrorAxis;
    if (axis >= 0)
    {
        auto onPlane = [&](const glm::dvec3& p) { return std::abs(p[axis]) < r->mSpacing; };
        const size_t junctionCount = r->mJunctions.size();
        for (size_t k = 0; k < junctionCount; ++k)
        {
            const ResolvedJunction j = r->mJunctions[k];
            if (!onPlane(j.position))
                r->mJunctions.push_back({j.id | kMirrored, surf.project(reflected(j.position, axis)).point, 0, j.corner});
        }
        auto mirroredJunction = [&](uint32_t id) {
            for (const ResolvedJunction& j : r->mJunctions)
                if (j.id == id)
                    return onPlane(j.position) ? id : (id | kMirrored);
            return id | kMirrored;
        };
        const size_t runCount = r->mRuns.size();
        for (size_t k = 0; k < runCount; ++k)
        {
            const ResolvedRun& src = r->mRuns[k];
            if (std::all_of(src.path.points.begin(), src.path.points.end(), onPlane))
                continue;
            ResolvedRun m = src;
            m.id = src.id | kMirrored;
            m.a = mirroredJunction(src.a);
            m.b = mirroredJunction(src.b);
            for (size_t i = 0; i < m.path.points.size(); ++i)
            {
                const SurfaceHit h = surf.project(reflected(src.path.points[i], axis));
                m.path.points[i] = h.point;
                m.path.normals[i] = h.normal;
            }
            m.path.measure();
            r->mRuns.push_back(std::move(m));
        }
    }

    for (ResolvedJunction& j : r->mJunctions)
        for (const ResolvedRun& run : r->mRuns)
            j.degree += int(run.a == j.id) + int(run.b == j.id);
    for (ResolvedRun& run : r->mRuns)
    {
        for (const ResolvedJunction& j : r->mJunctions)
        {
            if (j.id == run.a)
                run.freeA = j.degree == 1;
            if (j.id == run.b)
                run.freeB = j.degree == 1;
        }
        r->mMaxWidth = std::max(r->mMaxWidth, run.width);
    }
    r->buildGrid();
    return r;
}

void Resolved::buildGrid()
{
    mGrid = std::make_shared<Grid>();
    mGrid->cell = std::max({mMaxWidth * 0.5, mSpacing * 4.0, 1e-6});
    for (uint32_t ri = 0; ri < mRuns.size(); ++ri)
    {
        const Path& p = mRuns[ri].path;
        for (uint32_t s = 0; s + 1 < p.size(); ++s)
        {
            const glm::ivec3 lo = cellOf(glm::min(p.points[s], p.points[s + 1]), mGrid->cell);
            const glm::ivec3 hi = cellOf(glm::max(p.points[s], p.points[s + 1]), mGrid->cell);
            for (int x = lo.x; x <= hi.x; ++x)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int z = lo.z; z <= hi.z; ++z)
                        mGrid->cells[cellKey({x, y, z})].push_back({ri, s});
            ++mGrid->segments;
        }
    }
}

template <class F> void Resolved::forSegments(const glm::dvec3& p, double reach, F&& test) const
{
    if (mRuns.empty())
        return;
    const double r = std::isfinite(reach) ? reach : std::numeric_limits<double>::max();
    const glm::dvec3 lo = (p - r) / mGrid->cell, hi = (p + r) / mGrid->cell;
    const double cellsAcross = (hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
    if (!std::isfinite(cellsAcross) || cellsAcross > 4096.0)
    {
        for (uint32_t ri = 0; ri < mRuns.size(); ++ri)
            for (uint32_t s = 0; s + 1 < mRuns[ri].path.size(); ++s)
                test(ri, s);
        return;
    }
    const glm::ivec3 a = glm::ivec3(glm::floor(lo)), b = glm::ivec3(glm::floor(hi));
    for (int x = a.x; x <= b.x; ++x)
        for (int y = a.y; y <= b.y; ++y)
            for (int z = a.z; z <= b.z; ++z)
                if (auto it = mGrid->cells.find(cellKey({x, y, z})); it != mGrid->cells.end())
                    for (const auto& [ri, s] : it->second)
                        test(ri, s);
}

namespace
{
/** The closest spot on one segment of a run, as a sample. */
LineSample segmentSample(const ResolvedRun& run, uint32_t ri, uint32_t s, const glm::dvec3& p)
{
    LineSample out;
    const glm::dvec3 a = run.path.points[s], b = run.path.points[s + 1], d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
    const glm::dvec3 q = a + d * t;
    out.distance = glm::length(p - q);
    out.run = int(ri);
    out.point = q;
    out.s = run.path.lengths[s] + (run.path.lengths[s + 1] - run.path.lengths[s]) * t;
    out.tangent = len2 > 0.0 ? d / std::sqrt(len2) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 n = glm::mix(run.path.normals[s], run.path.normals[s + 1], t);
    out.normal = glm::length(n) > 1e-12 ? glm::normalize(n) : glm::dvec3(0.0, 1.0, 0.0);
    out.side = glm::dot(glm::cross(out.normal, out.tangent), p - q);
    if (s == 0 && t <= 0.0)
        out.beyond = std::max(0.0, glm::dot(p - a, -out.tangent));
    else if (s + 2 == run.path.size() && t >= 1.0)
        out.beyond = std::max(0.0, glm::dot(p - b, out.tangent));
    return out;
}
} // namespace

LineSample Resolved::lineAt(const glm::dvec3& p, double maxDistance, uint32_t mask) const
{
    LineSample best;
    forSegments(p, maxDistance, [&](uint32_t ri, uint32_t s) {
        const ResolvedRun& run = mRuns[ri];
        if (!(mask & outputBit(run.output)))
            return;
        LineSample c = segmentSample(run, ri, s, p);
        if (c.distance <= maxDistance && c.distance < best.distance)
            best = c;
    });
    return best;
}

std::vector<LineSample> Resolved::linesNear(const glm::dvec3& p, double reach, uint32_t mask) const
{
    std::vector<LineSample> out;
    forSegments(p, reach, [&](uint32_t ri, uint32_t s) {
        const ResolvedRun& run = mRuns[ri];
        if (!(mask & outputBit(run.output)))
            return;
        LineSample c = segmentSample(run, ri, s, p);
        if (c.distance > reach)
            return;
        auto it = std::find_if(out.begin(), out.end(), [ri](const LineSample& x) { return x.run == int(ri); });
        if (it == out.end())
            out.push_back(c);
        else if (c.distance < it->distance)
            *it = c;
    });
    return out;
}

void Resolved::buildLabels() const
{
    LILA_ZONE();
    auto L = std::make_shared<Labels>();
    mLabels = L;
    if (!mMesh || mMesh->numFaces() == 0)
        return;
    const geo::Mesh& m = *mMesh;
    const geo::Triangulation tris = geo::triangulate(m);
    L->meshTriangles = tris.triangleCount();
    L->pos.assign(m.positions().begin(), m.positions().end());
    L->tris.reserve(tris.triangleCount());
    for (Index t = 0; t < tris.triangleCount(); ++t)
    {
        L->tris.push_back({m.cornerPoint(tris.corners[size_t(t) * 3]), m.cornerPoint(tris.corners[size_t(t) * 3 + 1]),
                           m.cornerPoint(tris.corners[size_t(t) * 3 + 2])});
        L->origin.push_back(t);
    }

    const double size = std::max(1e-6, glm::length(m.bounds().size()));
    const double target = std::max(mSpacing * 2.0, size / 250.0);
    auto longestEdge = [&] {
        double e = 0.0;
        for (const glm::uvec3& t : L->tris)
            e = std::max({e, glm::length(L->pos[t.x] - L->pos[t.y]), glm::length(L->pos[t.y] - L->pos[t.z]), glm::length(L->pos[t.z] - L->pos[t.x])});
        return e;
    };
    // Midpoint subdivision until no triangle is far bigger than a line is wide, so every panel holds whole triangles.
    for (int level = 0; level < 6 && longestEdge() > target && L->tris.size() * 4 <= 3'000'000; ++level)
    {
        std::unordered_map<uint64_t, uint32_t> mid;
        auto midpoint = [&](uint32_t a, uint32_t b) {
            const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            auto [it, fresh] = mid.try_emplace(key, uint32_t(L->pos.size()));
            if (fresh)
                L->pos.push_back((L->pos[a] + L->pos[b]) * 0.5);
            return it->second;
        };
        std::vector<glm::uvec3> next;
        std::vector<uint32_t> origin;
        next.reserve(L->tris.size() * 4);
        origin.reserve(L->tris.size() * 4);
        for (size_t k = 0; k < L->tris.size(); ++k)
        {
            const glm::uvec3 t = L->tris[k];
            const uint32_t ab = midpoint(t.x, t.y), bc = midpoint(t.y, t.z), ca = midpoint(t.z, t.x);
            for (glm::uvec3 n : {glm::uvec3(t.x, ab, ca), glm::uvec3(ab, t.y, bc), glm::uvec3(ca, bc, t.z), glm::uvec3(ab, bc, ca)})
            {
                next.push_back(n);
                origin.push_back(L->origin[k]);
            }
        }
        L->tris = std::move(next);
        L->origin = std::move(origin);
    }

    L->cell = std::max(target * 1.5, 1e-6);
    for (uint32_t t = 0; t < L->tris.size(); ++t)
    {
        const glm::uvec3 i = L->tris[t];
        const glm::dvec3 lo = glm::min(glm::min(L->pos[i.x], L->pos[i.y]), L->pos[i.z]);
        const glm::dvec3 hi = glm::max(glm::max(L->pos[i.x], L->pos[i.y]), L->pos[i.z]);
        const glm::ivec3 a = cellOf(lo, L->cell), b = cellOf(hi, L->cell);
        for (int x = a.x; x <= b.x; ++x)
            for (int y = a.y; y <= b.y; ++y)
                for (int z = a.z; z <= b.z; ++z)
                    L->cells[cellKey({x, y, z})].push_back(t);
    }

    // Triangles a line passes over divide panels: mark them, then flood the rest.
    L->eps = mSpacing * 0.75 + size * 1e-7;
    L->probe = L->eps * 2.0 + target;
    L->label.assign(L->tris.size(), 0);
    auto markNear = [&](const glm::dvec3& q) {
        const glm::ivec3 lo = cellOf(q - L->eps, L->cell), hi = cellOf(q + L->eps, L->cell);
        for (int x = lo.x; x <= hi.x; ++x)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int z = lo.z; z <= hi.z; ++z)
                    if (auto it = L->cells.find(cellKey({x, y, z})); it != L->cells.end())
                        for (uint32_t t : it->second)
                            if (L->label[t] == 0 && L->distanceTo(t, q) <= L->eps)
                                L->label[t] = -1;
    };
    for (const ResolvedRun& run : mRuns)
        for (size_t k = 0; k < run.path.size(); ++k)
        {
            markNear(run.path.points[k]);
            if (k + 1 < run.path.size())
                markNear((run.path.points[k] + run.path.points[k + 1]) * 0.5);
        }

    std::unordered_map<uint64_t, std::array<uint32_t, 2>> edgeTris;
    edgeTris.reserve(L->tris.size() * 2);
    constexpr uint32_t kNone = ~0u;
    for (uint32_t t = 0; t < L->tris.size(); ++t)
    {
        const glm::uvec3 i = L->tris[t];
        for (auto [a, b] : {std::pair{i.x, i.y}, std::pair{i.y, i.z}, std::pair{i.z, i.x}})
        {
            const uint64_t key = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
            auto [it, fresh] = edgeTris.try_emplace(key, std::array<uint32_t, 2>{t, kNone});
            if (!fresh && it->second[1] == kNone)
                it->second[1] = t;
        }
    }
    std::vector<std::vector<uint32_t>> nbr(L->tris.size());
    for (const auto& [key, ts] : edgeTris)
        if (ts[1] != kNone)
        {
            nbr[ts[0]].push_back(ts[1]);
            nbr[ts[1]].push_back(ts[0]);
        }
    int panels = 0;
    std::deque<uint32_t> queue;
    for (uint32_t t = 0; t < L->tris.size(); ++t)
    {
        if (L->label[t] != 0)
            continue;
        const int id = ++panels;
        L->label[t] = id;
        queue.push_back(t);
        while (!queue.empty())
        {
            const uint32_t u = queue.front();
            queue.pop_front();
            for (uint32_t v : nbr[u])
                if (L->label[v] == 0)
                {
                    L->label[v] = id;
                    queue.push_back(v);
                }
        }
    }
    // Labels become 0-based panel indices; cut triangles stay -1.
    for (int& l : L->label)
        if (l > 0)
            --l;
    L->panels.assign(size_t(panels), PanelInfo{});
    for (uint32_t t = 0; t < L->tris.size(); ++t)
        if (L->label[t] >= 0)
        {
            const glm::uvec3 i = L->tris[t];
            L->panels[size_t(L->label[t])].area += 0.5 * glm::length(glm::cross(L->pos[i.y] - L->pos[i.x], L->pos[i.z] - L->pos[i.x]));
        }

    constexpr uint64_t kUnset = ~0ull;
    for (PanelInfo& p : L->panels)
        p.key = kUnset;
    for (const ResolvedRun& run : mRuns)
    {
        const size_t step = std::max<size_t>(1, run.path.size() / 8);
        for (size_t k = step / 2; k < run.path.size(); k += step)
        {
            const glm::dvec3 q = run.path.points[k], n = run.path.normals[k];
            const glm::dvec3 t = run.path.tangentAt(run.path.lengths[k]);
            glm::dvec3 sideDir = glm::cross(n, t);
            if (glm::length(sideDir) < 1e-12)
                continue;
            sideDir = glm::normalize(sideDir);
            for (int sgn : {1, -1})
            {
                const int tri = L->nearest(q + sideDir * (L->probe * sgn), true);
                if (tri < 0)
                    continue;
                PanelInfo& p = L->panels[size_t(L->label[size_t(tri)])];
                const uint64_t key = (uint64_t(run.source()) << 2) | (run.mirrored() ? 2u : 0u) | (sgn > 0 ? 1u : 0u);
                p.key = std::min(p.key, key);
            }
        }
    }
    for (size_t k = 0; k < L->panels.size(); ++k)
    {
        PanelInfo& p = L->panels[k];
        if (p.key == kUnset)
            p.key = (1ull << 62) | k;
        p.random = double(mix64(p.key) >> 11) * (1.0 / 9007199254740992.0);
    }
    const Surface surf(*mMesh);
    for (size_t k = 0; k < mLayout.panels.size(); ++k)
    {
        const int tri = L->nearest(surf.resolve(mLayout.panels[k].seed), true);
        if (tri >= 0)
            L->panels[size_t(L->label[size_t(tri)])].override = int(k);
    }
}

int Resolved::labelAt(const glm::dvec3& p, int depth) const
{
    const Labels& L = *mLabels;
    const int tri = L.nearest(p, false);
    if (tri < 0)
        return -1;
    if (L.label[size_t(tri)] >= 0)
        return L.label[size_t(tri)];
    // On a line: step off it to the side the point lies on.
    const LineSample line = lineAt(p, L.probe * 2.0);
    if (line.run >= 0 && depth < 2)
    {
        glm::dvec3 sideDir = glm::cross(line.normal, line.tangent);
        if (glm::length(sideDir) > 1e-12)
        {
            sideDir = glm::normalize(sideDir) * (line.side >= 0.0 ? 1.0 : -1.0);
            return labelAt(line.point + sideDir * L.probe, depth + 1);
        }
    }
    const int near = L.nearest(p, true);
    return near < 0 ? -1 : L.label[size_t(near)];
}

const std::vector<PanelInfo>& Resolved::panels() const
{
    std::call_once(mLabelsOnce, [this] { buildLabels(); });
    return mLabels->panels;
}

PanelSample Resolved::panelAt(const glm::dvec3& p) const
{
    std::call_once(mLabelsOnce, [this] { buildLabels(); });
    PanelSample s;
    if (mLabels->tris.empty())
        return s;
    s.panel = labelAt(p, 0);
    if (s.panel >= 0)
    {
        const PanelInfo& info = mLabels->panels[size_t(s.panel)];
        s.key = info.key;
        s.random = info.random;
        s.override = info.override;
    }
    return s;
}

std::vector<int> Resolved::trianglePanels() const
{
    std::call_once(mLabelsOnce, [this] { buildLabels(); });
    const Labels& L = *mLabels;
    std::vector<int> out(L.meshTriangles, -1);
    // The label of each mesh triangle's first piece that lies off the lines.
    for (size_t t = 0; t < L.tris.size(); ++t)
        if (L.label[t] >= 0 && out[L.origin[t]] < 0)
            out[L.origin[t]] = L.label[t];
    return out;
}

json ResolvedData::toJson() const
{
    json j = {{"kind", "panelLayoutResolved"}};
    if (resolved)
        j["runs"] = resolved->runs().size();
    return j;
}

std::shared_ptr<const Resolved> resolvedOf(const scene::DataPtr& d)
{
    if (!d || std::string_view(d->kind()) != "panelLayoutResolved")
        return nullptr;
    return static_cast<const ResolvedData&>(*d).resolved;
}

bool hasOutput(uint32_t mask, Output o) { return (mask & outputBit(o)) != 0; }
} // namespace lila::layout
