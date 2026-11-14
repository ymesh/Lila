#include "uv/FinalMesh.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"
#include "uv/FinalOperator.h"
#include "uv/Metrics.h"
#include "uv/OptimizeMath.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <numeric>
#include <optional>
#include <unordered_map>

namespace lila::uv
{
namespace
{
using namespace detail;

constexpr const char* kProbeFace = "lila:finalFace";
constexpr const char* kProbeCentre = "lila:finalCentre";
constexpr const char* kProbePrefix = "lila:finalProbe";
/** Added to a probed wedge's uv. Irrational-ish so probes never land exactly on another wedge's uv. */
constexpr double kDelta = 0.6180339887;
/** Weights under this are float noise. */
constexpr double kMinWeight = 2e-5;
/**
 * Whole-island probes keep weights down to this: a uv filled from its island's border reads thousands of wedges a little
 * each, and dropped together they miss the export. Still well above the float noise of a probe's response.
 */
constexpr double kMinIslandWeight = 1e-6;
/** Probe layers per stack run. */
constexpr int kLayersPerRun = 16;
/** Probe layers per run when whole islands are probed at once, which takes many more colours. */
constexpr int kIslandLayersPerRun = 64;
/** The solve opens triangles to this many times the collapsed fraction, so the written uvs keep them open. */
constexpr double kSolveMargin = 3.0;
constexpr const char* kNotABlend = "its uvs are not a blend of the cage's";

/** Wedge (chart vertex over the whole mesh, from uv agreement) of every corner, and a representative corner per wedge. */
struct Wedges
{
    std::vector<Index> ofCorner;
    std::vector<Index> rep;
    std::vector<Index> point;
    std::vector<std::vector<Index>> atPoint;
};

Wedges wedgesOf(const geo::Mesh& m, std::string_view uvSet)
{
    // Grouped by value at each point, as subdivision's uv islands compare them, so a probe never splits what it joins.
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvSet);
    auto same = [&](Index a, Index b) { return glm::all(glm::lessThanEqual(glm::abs(uv[a] - uv[b]), glm::vec2(1e-6f))); };
    Wedges w;
    w.ofCorner.assign(m.cornerCapacity(), kInvalid);
    w.atPoint.resize(m.pointCapacity());
    std::vector<Index> corners;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        corners.clear();
        m.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
        for (Index c : corners)
        {
            if (w.ofCorner[c] != kInvalid)
                continue;
            const Index id = Index(w.rep.size());
            w.rep.push_back(c);
            w.point.push_back(p);
            w.atPoint[p].push_back(id);
            for (Index o : corners)
                if (w.ofCorner[o] == kInvalid && same(c, o))
                    w.ofCorner[o] = id;
        }
    }
    return w;
}

/** Per face, the wedges at the points within rings edge steps of its points, sorted. */
std::vector<std::vector<Index>> neighbourhoods(const geo::Mesh& m, const Wedges& w, int rings)
{
    std::vector<std::vector<Index>> out(m.faceCapacity());
    tbb::parallel_for(tbb::blocked_range<Index>(0, m.faceCapacity(), 64), [&](const tbb::blocked_range<Index>& r) {
        std::vector<uint32_t> stamp(m.pointCapacity(), 0);
        uint32_t s = 0;
        std::vector<Index> front, next;
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            ++s;
            front.clear();
            m.forEachFaceCorner(f, [&](Index c) {
                const Index p = m.cornerPoint(c);
                if (stamp[p] != s)
                {
                    stamp[p] = s;
                    front.push_back(p);
                }
            });
            std::vector<Index> pts = front;
            for (int k = 0; k < rings; ++k)
            {
                next.clear();
                for (Index p : front)
                    m.forEachPointEdge(p, [&](Index e) {
                        const Index q = m.edgeOtherPoint(e, p);
                        if (stamp[q] != s)
                        {
                            stamp[q] = s;
                            next.push_back(q);
                        }
                    });
                pts.insert(pts.end(), next.begin(), next.end());
                front.swap(next);
            }
            std::vector<Index>& o = out[f];
            for (Index p : pts)
                o.insert(o.end(), w.atPoint[p].begin(), w.atPoint[p].end());
            std::sort(o.begin(), o.end());
        }
    });
    return out;
}

/**
 * Per face, its uv island: faces joined across manifold edges whose two ends are one wedge on both sides, as
 * subdivision's islands join them.
 */
std::vector<Index> faceIslands(const geo::Mesh& m, const Wedges& w, Index& count)
{
    std::vector<Index> parent(m.faceCapacity());
    for (Index f = 0; f < parent.size(); ++f)
        parent[f] = f;
    auto root = [&](Index f) {
        while (parent[f] != f)
            f = parent[f] = parent[parent[f]];
        return f;
    };
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e))
            continue;
        const Index c = m.edgeCorner(e), rc = m.cornerRadial(c);
        const Index p0 = m.edgePoint(e, 0), p1 = m.edgePoint(e, 1);
        if (w.ofCorner[cornerAt(c, p0)] == w.ofCorner[cornerAt(rc, p0)] && w.ofCorner[cornerAt(c, p1)] == w.ofCorner[cornerAt(rc, p1)])
            parent[root(m.cornerFace(c))] = root(m.cornerFace(rc));
    }
    std::vector<Index> id(m.faceCapacity(), kInvalid), out(m.faceCapacity(), kInvalid);
    count = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            Index& i = id[root(f)];
            if (i == kInvalid)
                i = count++;
            out[f] = i;
        }
    return out;
}

/** Greedy colouring so no two wedges of one face's neighbourhood share a colour. Returns the colour count. */
int colourWedges(const std::vector<std::vector<Index>>& hoods, size_t wedgeCount, std::vector<int>& colour)
{
    std::vector<std::vector<Index>> facesOf(wedgeCount);
    for (Index f = 0; f < hoods.size(); ++f)
        for (Index w : hoods[f])
            facesOf[w].push_back(f);
    colour.assign(wedgeCount, -1);
    std::vector<uint32_t> used;
    uint32_t stamp = 0;
    int count = 0;
    for (size_t w = 0; w < wedgeCount; ++w)
    {
        ++stamp;
        for (Index f : facesOf[w])
            for (Index o : hoods[f])
                if (colour[o] >= 0)
                {
                    if (size_t(colour[o]) >= used.size())
                        used.resize(size_t(colour[o]) + 1, 0);
                    used[size_t(colour[o])] = stamp;
                }
        int c = 0;
        while (size_t(c) < used.size() && used[size_t(c)] == stamp)
            ++c;
        colour[w] = c;
        count = std::max(count, c + 1);
    }
    return count;
}

bool sameTopology(const geo::Mesh& a, const geo::Mesh& b)
{
    return a.cornerCapacity() == b.cornerCapacity() && a.faceCapacity() == b.faceCapacity() && a.numFaces() == b.numFaces() &&
           a.numCorners() == b.numCorners();
}

/** Cage face centroids on a uniform grid, for the face nearest a point. */
class CentroidGrid
{
public:
    explicit CentroidGrid(const geo::Mesh& cage)
    {
        for (Index f = 0; f < cage.faceCapacity(); ++f)
            if (cage.faceValid(f))
            {
                mFaces.push_back(f);
                mPts.push_back(cage.faceCentroid(f));
            }
        glm::dvec3 lo(1e300), hi(-1e300);
        for (const glm::dvec3& p : mPts)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        mLo = lo;
        const double diag = mPts.empty() ? 1.0 : std::max(glm::length(hi - lo), 1e-9);
        mCell = diag / std::max(1.0, std::cbrt(double(mPts.size())));
        for (size_t i = 0; i < mPts.size(); ++i)
            mCells[key(cellOf(mPts[i]))].push_back(Index(i));
    }

    /** The nearest cage face, kInvalid when there are none. */
    Index nearest(const glm::dvec3& p) const
    {
        if (mPts.empty())
            return kInvalid;
        const glm::ivec3 c = cellOf(p);
        Index best = kInvalid;
        double bestD = 1e300;
        for (int r = 0; r < 64; ++r)
        {
            for (int x = -r; x <= r; ++x)
                for (int y = -r; y <= r; ++y)
                    for (int z = -r; z <= r; ++z)
                    {
                        if (std::max({std::abs(x), std::abs(y), std::abs(z)}) != r)
                            continue;
                        auto it = mCells.find(key(c + glm::ivec3(x, y, z)));
                        if (it == mCells.end())
                            continue;
                        for (Index i : it->second)
                            if (const double d = glm::length(mPts[i] - p); d < bestD)
                            {
                                bestD = d;
                                best = mFaces[i];
                            }
                    }
            // Anything in a farther shell is at least r cells away.
            if (best != kInvalid && bestD <= double(r) * mCell)
                break;
        }
        return best;
    }

private:
    glm::ivec3 cellOf(const glm::dvec3& p) const { return glm::ivec3(glm::floor((p - mLo) / mCell)); }
    static uint64_t key(const glm::ivec3& c)
    {
        return (uint64_t(uint32_t(c.x) & 0x1fffff) << 42) | (uint64_t(uint32_t(c.y) & 0x1fffff) << 21) | uint64_t(uint32_t(c.z) & 0x1fffff);
    }

    std::vector<Index> mFaces;
    std::vector<glm::dvec3> mPts;
    glm::dvec3 mLo{0.0};
    double mCell = 1.0;
    std::unordered_map<uint64_t, std::vector<Index>> mCells;
};

/**
 * Per result face, the cage face it came from. Each face carries its source's id (plus one, so 0 is a face made without
 * a source) and centroid; a modifier that blends face attributes garbles the id but leaves the centroid near where the
 * face came from, so a face whose id disagrees with its centroid takes the cage face nearest that centroid. Faces with
 * no source take a neighbour's.
 */
std::vector<int> sourceFaces(const geo::Mesh& cage, const geo::Mesh& res, std::span<const int> ids, std::span<const glm::vec3> centres)
{
    std::vector<int> src(res.faceCapacity(), -1);
    const glm::dvec3 size = cage.bounds().size();
    const double tol = 1e-5 * std::max(glm::length(size), 1e-9);
    std::unique_ptr<CentroidGrid> grid;
    for (Index f = 0; f < res.faceCapacity(); ++f)
    {
        if (!res.faceValid(f) || ids[f] <= 0)
            continue;
        const Index id = Index(ids[f] - 1);
        if (cage.faceValid(id) && glm::length(cage.faceCentroid(id) - glm::dvec3(centres[f])) <= tol)
        {
            src[f] = int(id);
            continue;
        }
        if (!grid)
            grid = std::make_unique<CentroidGrid>(cage);
        if (const Index n = grid->nearest(glm::dvec3(centres[f])); n != kInvalid)
            src[f] = int(n);
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Index f = 0; f < res.faceCapacity(); ++f)
        {
            if (!res.faceValid(f) || src[f] >= 0)
                continue;
            res.forEachFaceCorner(f, [&](Index c) {
                res.forEachEdgeCorner(res.cornerEdge(c), [&](Index r) {
                    if (src[f] < 0 && src[res.cornerFace(r)] >= 0)
                        src[f] = src[res.cornerFace(r)];
                });
            });
            changed |= src[f] >= 0;
        }
    }
    return src;
}

using Row = std::vector<std::pair<Index, FinalWeight>>;

bool byColumn(const std::pair<Index, FinalWeight>& a, const std::pair<Index, FinalWeight>& b)
{
    return a.first < b.first;
}

/** Sorts a row by column and sums repeated columns. */
void mergeRow(Row& r)
{
    std::sort(r.begin(), r.end(), byColumn);
    size_t n = 0;
    for (size_t k = 0; k < r.size(); ++k)
        if (n > 0 && r[n - 1].first == r[k].first)
            r[n - 1].second += r[k].second;
        else
            r[n++] = r[k];
    r.resize(n);
}

/** Rewrites interior uvs of y (indexed like the fills' corners) from its border uvs, fill by fill. */
void applyFills(std::span<const FinalFill> fills, std::span<glm::dvec2> y)
{
    std::vector<double> b, in;
    for (const FinalFill& f : fills)
    {
        const size_t nb = f.border.size(), ni = f.interiorStart.size() - 1;
        b.resize(2 * nb);
        in.resize(2 * ni);
        for (size_t k = 0; k < nb; ++k)
        {
            b[k] = y[f.border[k]].x;
            b[nb + k] = y[f.border[k]].y;
        }
        f.fill->solve(b, in, 2);
        for (size_t g = 0; g < ni; ++g)
            for (Index k = f.interiorStart[g]; k < f.interiorStart[g + 1]; ++k)
                y[f.interiorCorners[k]] = glm::dvec2(in[g], in[ni + g]);
    }
}

/**
 * The probe's rows made exported: each recorded island's border groups take their blends of the probed (Blend) rows,
 * and its interior groups leave the rows for a fill. Rows are per corner of the subdivided (exported) mesh.
 */
std::vector<FinalFill> composeRecord(const geo::FitRecord& record, std::vector<Row>& entries)
{
    std::vector<FinalFill> fills;
    std::vector<Row> rows;
    for (const geo::FitIslandRecord& isl : record.islands)
    {
        if (isl.groups.empty())
            continue;
        rows.assign(isl.border.size(), {});
        for (size_t k = 0; k < isl.border.size(); ++k)
        {
            for (auto [g, w] : isl.borderFrom[k])
                for (auto [col, x] : entries[isl.groups[g].front()])
                    rows[k].push_back({col, w * x});
            mergeRow(rows[k]);
        }
        for (size_t k = 0; k < isl.border.size(); ++k)
            for (Index c : isl.groups[isl.border[k]])
                entries[c] = rows[k];
        if (!isl.fill)
            continue;
        FinalFill& f = fills.emplace_back();
        f.fill = isl.fill;
        for (size_t k = 0; k < isl.fill->borderCount(); ++k)
            f.border.push_back(isl.groups[isl.border[k]].front());
        f.interiorStart.push_back(0);
        for (Index g : isl.interior)
        {
            for (Index c : isl.groups[g])
            {
                entries[c].clear();
                f.interiorCorners.push_back(c);
            }
            f.interiorStart.push_back(Index(f.interiorCorners.size()));
        }
    }
    return fills;
}

/**
 * One attempt at the map with the given ring count. Whole islands also probes every wedge of a face's uv island, for
 * stacks that spread a uv over its whole island (a Fit Surface subdivision fills each island's inside from its border).
 * composeFit runs the stack with Fit Surface fills recorded instead of applied and composes them into the map, failing
 * with "no fit" when the recording doesn't match the exported mesh. Empty error on success.
 */
std::string probe(const geo::Mesh& cage, std::string_view uvSet, const Stack& stack, const geo::Mesh& real, int rings, bool wholeIslands,
                  bool composeFit, FinalMap& out)
{
    LILA_ZONE();

    out.fills.clear();
    std::optional<geo::FitRecord> fit;
    const Wedges w = wedgesOf(cage, uvSet);
    std::vector<std::vector<Index>> hoods = neighbourhoods(cage, w, rings);
    // Per face, the hood its responses are looked up in: its own, or its island's: its faces' hoods and every wedge of
    // the islands those reach (a bevel strip of one island's face can carry its neighbour's uvs).
    std::vector<Index> hoodOf(cage.faceCapacity());
    for (Index f = 0; f < hoodOf.size(); ++f)
        hoodOf[f] = f;
    if (wholeIslands)
    {
        Index count = 0;
        const std::vector<Index> island = faceIslands(cage, w, count);
        std::vector<std::vector<Index>> ring(count), members(count);
        for (Index f = 0; f < cage.faceCapacity(); ++f)
            if (island[f] != kInvalid)
            {
                ring[island[f]].insert(ring[island[f]].end(), hoods[f].begin(), hoods[f].end());
                hoodOf[f] = island[f];
            }
        std::vector<Index> islandOfWedge(w.rep.size(), kInvalid);
        for (size_t wd = 0; wd < w.rep.size(); ++wd)
            if (const Index i = island[cage.cornerFace(w.rep[wd])]; i != kInvalid)
            {
                islandOfWedge[wd] = i;
                members[i].push_back(Index(wd));
            }
        std::vector<std::vector<Index>> merged(count);
        tbb::parallel_for(Index(0), count, [&](Index i) {
            std::vector<Index>& h = merged[i];
            std::sort(ring[i].begin(), ring[i].end());
            ring[i].erase(std::unique(ring[i].begin(), ring[i].end()), ring[i].end());
            std::vector<Index> reached;
            for (Index wd : ring[i])
                if (islandOfWedge[wd] != kInvalid)
                    reached.push_back(islandOfWedge[wd]);
            std::sort(reached.begin(), reached.end());
            reached.erase(std::unique(reached.begin(), reached.end()), reached.end());
            h = ring[i];
            for (Index j : reached)
                h.insert(h.end(), members[j].begin(), members[j].end());
            std::sort(h.begin(), h.end());
            h.erase(std::unique(h.begin(), h.end()), h.end());
        });
        hoods = std::move(merged);
    }
    std::vector<int> colour;
    const int colours = colourWedges(hoods, w.rep.size(), colour);
    // A composed fit's rotations come from its recording, so its ring-local probes only see blends that scale: a layer moves
    // one colour in u and another in v. Otherwise each colour gets a layer of its own, moved in u, and both channels answer,
    // so a weight that rotates shows in v.
    const int perLayer = composeFit ? 2 : 1;
    const int layers = (colours + perLayer - 1) / perLayer;
    const int perRun = wholeIslands ? kIslandLayersPerRun : kLayersPerRun;
    // Each hood's wedges by colour, which is unique within a hood.
    std::vector<std::vector<std::pair<int, Index>>> byColour(hoods.size());
    tbb::parallel_for(size_t(0), hoods.size(), [&](size_t h) {
        for (Index wd : hoods[h])
            byColour[h].push_back({colour[wd], wd});
        std::sort(byColour[h].begin(), byColour[h].end());
    });

    // The probe base: the cage with only the uv set among its uv layers, and each face's id.
    geo::Mesh base = cage;
    {
        std::vector<std::string> drop;
        for (size_t l = 0; l < base.cornerAttrs().layerCount(); ++l)
            if (const geo::Attribute& a = base.cornerAttrs().layer(l); a.type() == geo::AttrType::Float2 && a.name() != uvSet)
                drop.push_back(a.name());
        for (const std::string& n : drop)
            base.cornerAttrs().remove(n);
        std::span<int> ids = base.faceAttrs().getOrAdd<int>(kProbeFace);
        for (Index f = 0; f < base.faceCapacity(); ++f)
            ids[f] = int(f) + 1;
        std::span<glm::vec3> centres = base.faceAttrs().getOrAdd<glm::vec3>(kProbeCentre);
        for (Index f = 0; f < base.faceCapacity(); ++f)
            if (base.faceValid(f))
                centres[f] = glm::vec3(base.faceCentroid(f));
    }
    const std::span<const glm::vec2> cageUv = cage.cornerAttrs().get<glm::vec2>(uvSet);

    const Index rows = real.cornerCapacity();
    std::vector<Row> entries(rows);
    std::vector<double> unattributed(rows, 0.0);
    std::vector<int> sourceFace;
    out.runs = 0;
    for (int first = 0; first < std::max(layers, 1); first += perRun)
    {
        const int count = std::min(perRun, layers - first);
        geo::Mesh in = base;
        for (int k = 0; k < count; ++k)
        {
            std::span<glm::vec2> p = in.cornerAttrs().getOrAdd<glm::vec2>(std::string(kProbePrefix) + std::to_string(k) + "@" + std::string(uvSet));
            const int cu = 2 * (first + k), cv = cu + 1;
            for (Index c = 0; c < in.cornerCapacity(); ++c)
            {
                const Index wd = w.ofCorner[c];
                if (wd == kInvalid)
                    continue;
                glm::dvec2 v(cageUv[c]);
                if (perLayer == 1 ? colour[wd] == first + k : colour[wd] == cu)
                    v.x += kDelta;
                else if (perLayer == 2 && colour[wd] == cv)
                    v.y += kDelta;
                p[c] = glm::vec2(v);
            }
        }
        geo::Mesh res;
        {
            std::unique_ptr<geo::FitRecorder> recorder = composeFit ? std::make_unique<geo::FitRecorder>() : nullptr;
            res = runStack(in, stack);
            // The first run's recording stands for all: the island layer, and so the fills, are the same in each.
            if (recorder && first == 0)
            {
                if (recorder->records.size() > 1 ||
                    (recorder->records.size() == 1 &&
                     (recorder->records[0].islandLayer != uvSet || recorder->records[0].cornerCapacity != real.cornerCapacity())))
                    return "no fit";
                if (recorder->records.size() == 1)
                    fit = std::move(recorder->records[0]);
            }
        }
        ++out.runs;
        if (!sameTopology(res, real))
            return "its topology changes with the uvs";
        const std::span<const glm::vec2> baseUv = res.cornerAttrs().get<glm::vec2>(uvSet);
        const std::span<const int> faceIds = res.faceAttrs().get<int>(kProbeFace);
        const std::span<const glm::vec3> faceCentres = res.faceAttrs().get<glm::vec3>(kProbeCentre);
        if (baseUv.empty() || faceIds.empty() || faceCentres.empty())
            return "it drops corner or face attributes";
        if (sourceFace.empty())
            sourceFace = sourceFaces(cage, res, faceIds, faceCentres);
        std::vector<std::span<const glm::vec2>> probes(static_cast<size_t>(count));
        for (int k = 0; k < count; ++k)
        {
            probes[size_t(k)] = res.cornerAttrs().get<glm::vec2>(std::string(kProbePrefix) + std::to_string(k) + "@" + std::string(uvSet));
            if (probes[size_t(k)].empty())
                return "it drops extra uv layers";
        }
        tbb::parallel_for(tbb::blocked_range<Index>(0, res.faceCapacity(), 256), [&](const tbb::blocked_range<Index>& r) {
            for (Index f = r.begin(); f < r.end(); ++f)
            {
                if (!res.faceValid(f))
                    continue;
                const int src = sourceFace[f];
                const std::vector<std::pair<int, Index>>* hood = src >= 0 && Index(src) < hoodOf.size() ? &byColour[hoodOf[Index(src)]] : nullptr;
                res.forEachFaceCorner(f, [&](Index c) {
                    for (int k = 0; k < count; ++k)
                        for (int ch = 0; ch < perLayer; ++ch)
                        {
                            const glm::dvec2 r = (glm::dvec2(probes[size_t(k)][c]) - glm::dvec2(baseUv[c])) / kDelta;
                            const FinalWeight d = perLayer == 1 ? FinalWeight(r.x, r.y) : FinalWeight(r[ch], 0.0);
                            if (std::abs(d) < (wholeIslands ? kMinIslandWeight : kMinWeight))
                                continue;
                            const int col = perLayer * (first + k) + ch;
                            Index found = kInvalid;
                            if (hood)
                                if (auto it = std::lower_bound(hood->begin(), hood->end(), std::pair<int, Index>{col, 0}); it != hood->end() && it->first == col)
                                    found = it->second;
                            if (found == kInvalid)
                                unattributed[c] += std::abs(d);
                            else
                                entries[c].push_back({found, d});
                        }
                });
            }
        });
        // A response no wedge in reach owns stays unowned whatever the later runs find.
        if (std::ranges::any_of(unattributed, [](double u) { return u > 1e-3; }))
            return "reach";
    }

    if (fit)
        out.fills = composeRecord(*fit, entries);

    // Check: every response found its wedge, and the map reproduces the exported uvs.
    const std::span<const glm::vec2> realUv = real.cornerAttrs().get<glm::vec2>(uvSet);
    glm::dvec2 lo(1e300), hi(-1e300);
    for (Index c = 0; c < cage.cornerCapacity(); ++c)
        if (w.ofCorner[c] != kInvalid)
        {
            lo = glm::min(lo, glm::dvec2(cageUv[c]));
            hi = glm::max(hi, glm::dvec2(cageUv[c]));
        }
    const double tol = 1e-3 * std::max(1.0, glm::length(hi - lo));
    std::vector<glm::dvec2> mapped(rows, glm::dvec2(0.0));
    for (Index c = 0; c < rows; ++c)
        for (auto [wd, wt] : entries[c])
            mapped[c] += weigh(wt, glm::dvec2(cageUv[w.rep[wd]]));
    applyFills(out.fills, mapped);
    double worstMiss = 0.0, worstFit = 0.0;
    for (Index f = 0; f < real.faceCapacity(); ++f)
        if (real.faceValid(f))
            real.forEachFaceCorner(f, [&](Index c) {
                worstMiss = std::max(worstMiss, unattributed[c]);
                worstFit = std::max(worstFit, glm::length(mapped[c] - glm::dvec2(realUv[c])));
            });
    if (worstMiss > 1e-3)
        return "reach";
    if (worstFit > tol)
        return kNotABlend;

    out.rowStart.assign(size_t(rows) + 1, 0);
    for (Index c = 0; c < rows; ++c)
    {
        Row& e = entries[c];
        std::sort(e.begin(), e.end(), byColumn);
        out.rowStart[c + 1] = out.rowStart[c] + Index(e.size());
    }
    out.cols.resize(out.rowStart.back());
    out.weights.resize(out.rowStart.back());
    for (Index c = 0; c < rows; ++c)
        for (size_t k = 0; k < entries[c].size(); ++k)
        {
            out.cols[out.rowStart[c] + k] = w.rep[entries[c][k].first];
            out.weights[out.rowStart[c] + k] = entries[c][k].second;
        }
    out.sourceFace.assign(sourceFace.size(), kInvalid);
    for (size_t f = 0; f < sourceFace.size(); ++f)
        if (sourceFace[f] >= 0)
            out.sourceFace[f] = Index(sourceFace[f]);
    out.rings = rings;
    return {};
}
/** Moves an island that grew over its UDIM tile's border back inside, shrinking it only when it is bigger than the tile. */
void keepInTile(Chart& ch, const glm::ivec2& tile)
{
    // Just inside, so the island's uvs never round onto the next tile.
    const glm::dvec2 lo = glm::dvec2(tile) + glm::dvec2(1e-5), hi = glm::dvec2(tile) + glm::dvec2(1.0 - 1e-5);
    glm::dvec2 mn = ch.uvMin(), mx = ch.uvMax();
    const glm::dvec2 size = mx - mn;
    const double fit = std::min(1.0, (hi.x - lo.x) / std::max(size.x, size.y));
    if (fit < 1.0)
    {
        const glm::dvec2 c = (mn + mx) * 0.5;
        for (glm::dvec2& p : ch.uv)
            p = c + (p - c) * fit;
        mn = ch.uvMin();
        mx = ch.uvMax();
    }
    glm::dvec2 shift(0.0);
    for (int k = 0; k < 2; ++k)
        if (mn[k] < lo[k])
            shift[k] = lo[k] - mn[k];
        else if (mx[k] > hi[k])
            shift[k] = hi[k] - mx[k];
    if (shift != glm::dvec2(0.0))
        for (glm::dvec2& p : ch.uv)
            p += shift;
}
} // namespace

FinalMap traceFaces(const geo::Mesh& cage, const Stack& stack)
{
    LILA_ZONE();

    FinalMap map;
    if (stack.empty())
        return map;
    geo::Mesh in = cage;
    std::span<int> ids = in.faceAttrs().getOrAdd<int>(kProbeFace);
    std::span<glm::vec3> centres = in.faceAttrs().getOrAdd<glm::vec3>(kProbeCentre);
    for (Index f = 0; f < in.faceCapacity(); ++f)
        if (in.faceValid(f))
        {
            ids[f] = int(f) + 1;
            centres[f] = glm::vec3(in.faceCentroid(f));
        }
    geo::Mesh out;
    try
    {
        out = runStack(in, stack);
    }
    catch (const std::exception& e)
    {
        map.warning = std::string("the modifier stack failed (") + e.what() + "); measured on the cage";
        return map;
    }
    if (!changesSurface(cage, out))
        return map;
    const std::span<const int> outIds = out.faceAttrs().get<int>(kProbeFace);
    const std::span<const glm::vec3> outCentres = out.faceAttrs().get<glm::vec3>(kProbeCentre);
    if (outIds.empty() || outCentres.empty())
    {
        map.warning = "the modifier stack drops face attributes; measured on the cage";
        return map;
    }
    const std::vector<int> src = sourceFaces(cage, out, outIds, outCentres);
    map.sourceFace.assign(src.size(), kInvalid);
    for (size_t f = 0; f < src.size(); ++f)
        if (src[f] >= 0)
            map.sourceFace[f] = Index(src[f]);
    out.faceAttrs().remove(kProbeFace);
    out.faceAttrs().remove(kProbeCentre);
    map.exported = std::make_shared<geo::Mesh>(std::move(out));
    return map;
}

void keepInUdimTile(Chart& chart, const glm::ivec2& tile)
{
    keepInTile(chart, tile);
}

std::vector<Index> exportedFaces(const FinalMap& map, std::span<const Index> cageFaces)
{
    std::vector<Index> out;
    if (!map.exported)
        return out;
    const geo::Mesh& ex = *map.exported;
    std::vector<uint8_t> want;
    if (!cageFaces.empty())
    {
        Index cap = 0;
        for (Index f : cageFaces)
            cap = std::max(cap, f + 1);
        want.assign(cap, 0);
        for (Index f : cageFaces)
            want[f] = 1;
    }
    for (Index f = 0; f < ex.faceCapacity(); ++f)
    {
        if (!ex.faceValid(f))
            continue;
        const Index src = f < map.sourceFace.size() ? map.sourceFace[f] : kInvalid;
        if (want.empty() || (src < want.size() && want[src]))
            out.push_back(f);
    }
    return out;
}

bool changesSurface(const geo::Mesh& cage, const geo::Mesh& exported)
{
    if (cage.pointCapacity() != exported.pointCapacity() || cage.numPoints() != exported.numPoints() || !sameTopology(cage, exported))
        return true;
    for (Index f = 0; f < cage.faceCapacity(); ++f)
        if (cage.faceValid(f) != exported.faceValid(f) || (cage.faceValid(f) && cage.faceSize(f) != exported.faceSize(f)))
            return true;
    for (Index p = 0; p < cage.pointCapacity(); ++p)
        if (cage.pointValid(p) && glm::length(cage.position(p) - exported.position(p)) > 1e-9 * std::max(1.0, glm::length(cage.position(p))))
            return true;
    return false;
}

geo::Mesh runStack(const geo::Mesh& mesh, const Stack& stack)
{
    geo::Mesh cur = mesh;
    const bool spacing = std::ranges::any_of(stack, &StackStep::subdivides);
    if (spacing)
        geo::addFitSpacing(cur);
    for (const StackStep& s : stack)
        cur = s.apply(cur);
    if (spacing)
        geo::removeFitSpacing(cur);
    return cur;
}

FinalMap buildFinalMap(const geo::Mesh& cage, std::string_view uvSet, const Stack& stack)
{
    LILA_ZONE();

    FinalMap map;
    if (stack.empty())
        return map;
    geo::Mesh real;
    try
    {
        real = runStack(cage, stack);
    }
    catch (const std::exception& e)
    {
        map.warning = std::string("the modifier stack failed (") + e.what() + "); solved on the cage";
        return map;
    }
    auto exported = std::make_shared<geo::Mesh>(std::move(real));
    map.exported = exported;
    if (!changesSurface(cage, *exported) || cage.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return map;
    if (exported->cornerAttrs().get<glm::vec2>(uvSet).empty())
    {
        map.warning = "the modifier stack drops " + std::string(uvSet) + "; solved on the cage";
        return map;
    }
    std::string why;
    // Ring-local probes with Fit Surface fills composed in first; whole islands for stacks that spread uvs over them
    // some other way.
    struct Attempt
    {
        int rings;
        bool wholeIslands, composeFit;
    };
    for (const Attempt a : {Attempt{3, false, true}, Attempt{6, false, true}, Attempt{12, false, true}, Attempt{3, true, false}, Attempt{6, true, false}})
    {
        try
        {
            why = probe(cage, uvSet, stack, *exported, a.rings, a.wholeIslands, a.composeFit, map);
        }
        catch (const std::exception& e)
        {
            why = e.what();
        }
        if (why.empty())
        {
            map.method = FinalMethod::Probed;
            return map;
        }
        // A composed map that misses falls back to probing whole islands.
        if (why != "reach" && !(a.composeFit && (why == "no fit" || why == kNotABlend)))
            break;
    }
    if (why == "reach")
        why = "its uvs depend on cage uvs too far away";
    // Name the first modifier the map breaks at.
    std::string culprit;
    for (size_t n = 1; n <= stack.size() && culprit.empty() && stack.size() > 1; ++n)
    {
        Stack prefix(stack.begin(), stack.begin() + std::ptrdiff_t(n));
        FinalMap trial;
        try
        {
            geo::Mesh r = runStack(cage, prefix);
            if (!changesSurface(cage, r))
                continue;
            if (r.cornerAttrs().get<glm::vec2>(uvSet).empty() || (!probe(cage, uvSet, prefix, r, 12, false, false, trial).empty() && !probe(cage, uvSet, prefix, r, 6, true, false, trial).empty()))
                culprit = stack[n - 1].name;
        }
        catch (const std::exception&)
        {
            culprit = stack[n - 1].name;
        }
    }
    if (culprit.empty())
        culprit = stack.size() == 1 ? stack.front().name : "the modifier stack";
    map.warning = culprit + " changes uvs in a way uv tools can't follow (" + why + "); solved on the cage";
    map.rowStart.clear();
    map.fills.clear();
    map.sourceFace.clear();
    map.cols.clear();
    map.weights.clear();
    return map;
}

std::vector<glm::dvec2> exportedUvs(const FinalMap& map, const geo::Mesh& cage, std::string_view uvSet)
{
    return exportedUvs(map, cage.cornerAttrs().get<glm::vec2>(uvSet));
}

std::vector<glm::dvec2> exportedUvs(const FinalMap& map, std::span<const glm::vec2> cageUv)
{
    std::vector<glm::dvec2> out;
    if (!map.active() || !map.exported || cageUv.empty())
        return out;
    out.assign(map.exported->cornerCapacity(), glm::dvec2(0.0));
    for (size_t c = 0; c + 1 < map.rowStart.size(); ++c)
        for (Index k = map.rowStart[c]; k < map.rowStart[c + 1]; ++k)
            out[c] += weigh(map.weights[k], glm::dvec2(cageUv[map.cols[k]]));
    applyFills(map.fills, out);
    return out;
}

double FinalIsland::area3d() const
{
    double a = 0.0;
    for (const auto& t : tris)
        a += 0.5 * glm::length(glm::cross(pos[t[1]] - pos[t[0]], pos[t[2]] - pos[t[0]]));
    return a;
}

std::vector<glm::dvec2> FinalIsland::uvs(const Chart& chart) const
{
    std::vector<glm::dvec2> out(pos.size());
    for (size_t c = 0; c < pos.size(); ++c)
    {
        glm::dvec2 s = offset[c];
        for (Index k = rowStart[c]; k < rowStart[c + 1]; ++k)
            s += weigh(weights[k], chart.uv[cols[k]]);
        out[c] = s;
    }
    applyFills(fills, out);
    return out;
}

double FinalIsland::areaUv(const Chart& chart) const
{
    const std::vector<glm::dvec2> y = uvs(chart);
    double a = 0.0;
    for (const auto& t : tris)
        a += signedArea2(y[t[0]], y[t[1]], y[t[2]]);
    return a;
}

std::vector<FinalIsland> finalIslands(const FinalMap& map, const geo::Mesh& cage, std::span<const Chart> charts, std::string_view uvSet)
{
    LILA_ZONE();

    std::vector<FinalIsland> out(charts.size());
    if (!map.active() || !map.exported)
        return out;
    const geo::Mesh& ex = *map.exported;
    std::vector<Index> cornerChart(cage.cornerCapacity(), kInvalid), cornerVertex(cage.cornerCapacity(), kInvalid);
    for (size_t i = 0; i < charts.size(); ++i)
        for (size_t k = 0; k < charts[i].corners.size(); ++k)
        {
            cornerChart[charts[i].corners[k]] = Index(i);
            cornerVertex[charts[i].corners[k]] = charts[i].cornerVertex[k];
        }
    const std::span<const glm::vec2> cageUv = cage.cornerAttrs().get<glm::vec2>(uvSet);
    auto tallyRow = [&](Index c, std::vector<std::pair<Index, double>>& tally, double scale) {
        for (Index k = map.rowStart[c]; k < map.rowStart[c + 1]; ++k)
        {
            const Index ch = cornerChart[map.cols[k]];
            if (ch == kInvalid)
                continue;
            auto it = std::find_if(tally.begin(), tally.end(), [&](const auto& t) { return t.first == ch; });
            if (it == tally.end())
                tally.push_back({ch, scale * std::abs(map.weights[k])});
            else
                it->second += scale * std::abs(map.weights[k]);
        }
    };
    auto most = [](const std::vector<std::pair<Index, double>>& tally) {
        return tally.empty() ? kInvalid : std::max_element(tally.begin(), tally.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    };
    // A fill's interior corners have no rows; they count for the chart its border reads most.
    std::vector<Index> cornerFill(ex.cornerCapacity(), kInvalid), fillChart(map.fills.size(), kInvalid);
    for (size_t f = 0; f < map.fills.size(); ++f)
    {
        for (Index c : map.fills[f].interiorCorners)
            cornerFill[c] = Index(f);
        std::vector<std::pair<Index, double>> tally;
        for (Index c : map.fills[f].border)
            tallyRow(c, tally, 1.0);
        fillChart[f] = most(tally);
    }
    // Each exported face goes to the chart with most of its weight.
    std::vector<Index> faceChart(ex.faceCapacity(), kInvalid);
    tbb::parallel_for(tbb::blocked_range<Index>(0, ex.faceCapacity(), 512), [&](const tbb::blocked_range<Index>& r) {
        std::vector<std::pair<Index, double>> tally;
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (!ex.faceValid(f))
                continue;
            tally.clear();
            ex.forEachFaceCorner(f, [&](Index c) {
                tallyRow(c, tally, 1.0);
                if (const Index fl = cornerFill[c]; fl != kInvalid && fillChart[fl] != kInvalid)
                {
                    auto it = std::find_if(tally.begin(), tally.end(), [&](const auto& t) { return t.first == fillChart[fl]; });
                    if (it == tally.end())
                        tally.push_back({fillChart[fl], 1.0});
                    else
                        it->second += 1.0;
                }
            });
            faceChart[f] = most(tally);
        }
    });
    std::vector<Index> local(ex.cornerCapacity(), kInvalid), cornerIsland(ex.cornerCapacity(), kInvalid), tri;
    Row row;
    // A corner of the island's faces, or (not own) one only a fill of the island reads.
    auto addCorner = [&](Index ci, Index c, bool own) {
        FinalIsland& isl = out[ci];
        if (isl.rowStart.empty())
            isl.rowStart.push_back(0);
        const Index at = Index(isl.pos.size());
        if (own)
        {
            local[c] = at;
            cornerIsland[c] = ci;
        }
        isl.pos.push_back(ex.position(ex.cornerPoint(c)));
        isl.corner.push_back(c);
        glm::dvec2 off(0.0);
        row.clear();
        for (Index k = map.rowStart[c]; k < map.rowStart[c + 1]; ++k)
        {
            const Index col = map.cols[k];
            if (cornerChart[col] == ci)
                row.push_back({cornerVertex[col], map.weights[k]});
            else if (col < cageUv.size())
                off += weigh(map.weights[k], glm::dvec2(cageUv[col]));
        }
        std::sort(row.begin(), row.end(), byColumn);
        for (size_t k = 0; k < row.size(); ++k)
            if (!isl.cols.empty() && isl.cols.size() > isl.rowStart.back() && isl.cols.back() == row[k].first)
                isl.weights.back() += row[k].second;
            else
            {
                isl.cols.push_back(row[k].first);
                isl.weights.push_back(row[k].second);
            }
        isl.rowStart.push_back(Index(isl.cols.size()));
        isl.offset.push_back(off);
        return at;
    };
    for (Index f = 0; f < ex.faceCapacity(); ++f)
    {
        const Index ci = faceChart[f];
        if (ci == kInvalid)
            continue;
        ex.forEachFaceCorner(f, [&](Index c) { addCorner(ci, c, true); });
        FinalIsland& isl = out[ci];
        tri.clear();
        geo::triangulateFace(ex, f, tri);
        for (size_t i = 0; i + 2 < tri.size(); i += 3)
        {
            isl.tris.push_back({local[tri[i]], local[tri[i + 1]], local[tri[i + 2]]});
            isl.triFace.push_back(f);
        }
    }
    // Each island holding a fill's interior corners gets the fill, reading local corners; border groups whose corners
    // went to another island get one of their own.
    std::vector<Index> islandsOfFill;
    for (const FinalFill& mf : map.fills)
    {
        islandsOfFill.clear();
        for (Index c : mf.interiorCorners)
            if (cornerIsland[c] != kInvalid && std::find(islandsOfFill.begin(), islandsOfFill.end(), cornerIsland[c]) == islandsOfFill.end())
                islandsOfFill.push_back(cornerIsland[c]);
        for (Index ci : islandsOfFill)
        {
            FinalFill lf;
            lf.fill = mf.fill;
            for (Index c : mf.border)
                lf.border.push_back(cornerIsland[c] == ci ? local[c] : addCorner(ci, c, false));
            lf.interiorStart.push_back(0);
            for (size_t g = 0; g + 1 < mf.interiorStart.size(); ++g)
            {
                for (Index k = mf.interiorStart[g]; k < mf.interiorStart[g + 1]; ++k)
                    if (const Index c = mf.interiorCorners[k]; cornerIsland[c] == ci)
                        lf.interiorCorners.push_back(local[c]);
                lf.interiorStart.push_back(Index(lf.interiorCorners.size()));
            }
            out[ci].fills.push_back(std::move(lf));
        }
    }
    return out;
}

detail::FinalOperator::FinalOperator(const FinalIsland& isl, size_t vertexCount, bool conjugate)
    : mIsl(isl), mCorners(isl.pos.size()), mVertices(vertexCount), mConjugate(conjugate)
{
    LILA_ZONE();

    mRotates = std::ranges::any_of(isl.weights, [](const FinalWeight& w) { return w.imag() != 0.0; });
    const auto C = Eigen::Index(mCorners), V = Eigen::Index(mVertices);
    std::vector<Triplet> re, im;
    re.reserve(isl.cols.size());
    for (size_t c = 0; c < mCorners; ++c)
        for (Index k = isl.rowStart[c]; k < isl.rowStart[c + 1]; ++k)
        {
            const FinalWeight w = weight(k);
            re.emplace_back(int(c), int(isl.cols[k]), w.real());
            if (w.imag() != 0.0)
                im.emplace_back(int(c), int(isl.cols[k]), w.imag());
        }
    mR.resize(C, V);
    mR.setFromTriplets(re.begin(), re.end());
    mRi.resize(C, V);
    mRi.setFromTriplets(im.begin(), im.end());
    std::vector<int> at(mVertices, -1);
    for (const FinalFill& f : isl.fills)
    {
        Block b;
        for (Index c : f.border)
            for (Index k = isl.rowStart[c]; k < isl.rowStart[c + 1]; ++k)
            {
                b.cols.push_back(isl.cols[k]);
                if (isl.weights[k].imag() != 0.0)
                    b.imCols.push_back(isl.cols[k]);
            }
        for (std::vector<Index>* v : {&b.cols, &b.imCols})
        {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        for (size_t j = 0; j < b.cols.size(); ++j)
            at[b.cols[j]] = int(j);
        const auto nb = Eigen::Index(f.border.size()), m = Eigen::Index(b.cols.size()), q = Eigen::Index(b.imCols.size());
        const auto ni = Eigen::Index(f.fill->interiorCount());
        // The border's rows over the fill's columns (real parts), then over the columns that rotate (imaginary parts): the
        // fill solves both alike.
        Eigen::MatrixXd rows = Eigen::MatrixXd::Zero(nb, m + q);
        for (Eigen::Index k = 0; k < nb; ++k)
        {
            const Index c = f.border[size_t(k)];
            for (Index e = isl.rowStart[c]; e < isl.rowStart[c + 1]; ++e)
            {
                const FinalWeight w = weight(e);
                rows(k, at[isl.cols[e]]) += w.real();
                if (w.imag() != 0.0)
                {
                    const auto j = std::lower_bound(b.imCols.begin(), b.imCols.end(), isl.cols[e]) - b.imCols.begin();
                    rows(k, m + j) += w.imag();
                }
            }
        }
        for (Index v : b.cols)
            at[v] = -1;
        Eigen::MatrixXd d(ni, m + q);
        f.fill->solve(std::span<const double>(rows.data(), size_t(rows.size())), std::span<double>(d.data(), size_t(d.size())), size_t(m + q));
        b.d = d.leftCols(m);
        b.di = d.rightCols(q);
        Eigen::MatrixXd ob(nb, 2);
        for (Eigen::Index k = 0; k < nb; ++k)
        {
            ob(k, 0) = isl.offset[f.border[size_t(k)]].x;
            ob(k, 1) = isl.offset[f.border[size_t(k)]].y;
        }
        b.off.resize(ni, 2);
        f.fill->solve(std::span<const double>(ob.data(), size_t(ob.size())), std::span<double>(b.off.data(), size_t(b.off.size())), 2);
        b.start = f.interiorStart;
        b.corners = f.interiorCorners;
        mBlocks.push_back(std::move(b));
    }
}

namespace
{
/** The u and v of the given chart vertices. */
void gatherColumns(std::span<const glm::dvec2> x, std::span<const Index> cols, Eigen::VectorXd& u, Eigen::VectorXd& v)
{
    u.resize(Eigen::Index(cols.size()));
    v.resize(Eigen::Index(cols.size()));
    for (size_t j = 0; j < cols.size(); ++j)
    {
        u[Eigen::Index(j)] = x[cols[j]].x;
        v[Eigen::Index(j)] = x[cols[j]].y;
    }
}
} // namespace

void detail::FinalOperator::apply(std::span<const glm::dvec2> x, std::span<glm::dvec2> y, glm::dvec2 offsetScale) const
{
    for (size_t c = 0; c < mCorners; ++c)
    {
        glm::dvec2 s = mIsl.offset[c] * offsetScale;
        for (Index k = mIsl.rowStart[c]; k < mIsl.rowStart[c + 1]; ++k)
            s += weigh(weight(k), x[mIsl.cols[k]]);
        y[c] = s;
    }
    Eigen::VectorXd xu, xv, iu, iv, fu, fv;
    for (const Block& b : mBlocks)
    {
        gatherColumns(x, b.cols, xu, xv);
        gatherColumns(x, b.imCols, iu, iv);
        fu = b.d * xu + b.off.col(0) * offsetScale.x;
        fv = b.d * xv + b.off.col(1) * offsetScale.y;
        if (b.di.cols() > 0)
        {
            fu.noalias() -= b.di * iv;
            fv.noalias() += b.di * iu;
        }
        for (size_t g = 0; g + 1 < b.start.size(); ++g)
            for (Index k = b.start[g]; k < b.start[g + 1]; ++k)
                y[b.corners[k]] = glm::dvec2(fu[Eigen::Index(g)], fv[Eigen::Index(g)]);
    }
}

void detail::FinalOperator::applyTransposed(std::span<const glm::dvec2> g, std::span<glm::dvec2> out) const
{
    std::fill(out.begin(), out.end(), glm::dvec2(0.0));
    for (size_t c = 0; c < mCorners; ++c)
        for (Index k = mIsl.rowStart[c]; k < mIsl.rowStart[c + 1]; ++k)
            out[mIsl.cols[k]] += weigh(std::conj(weight(k)), g[c]);
    Eigen::VectorXd gu, gv, ru, rv;
    for (const Block& b : mBlocks)
    {
        const auto ni = Eigen::Index(b.start.size() - 1);
        gu.setZero(ni);
        gv.setZero(ni);
        for (Eigen::Index q = 0; q < ni; ++q)
            for (Index k = b.start[size_t(q)]; k < b.start[size_t(q) + 1]; ++k)
            {
                gu[q] += g[b.corners[k]].x;
                gv[q] += g[b.corners[k]].y;
            }
        ru.noalias() = b.d.transpose() * gu;
        rv.noalias() = b.d.transpose() * gv;
        for (size_t j = 0; j < b.cols.size(); ++j)
            out[b.cols[j]] += glm::dvec2(ru[Eigen::Index(j)], rv[Eigen::Index(j)]);
        if (b.di.cols() > 0)
        {
            ru.noalias() = b.di.transpose() * gv;
            rv.noalias() = b.di.transpose() * gu;
            for (size_t j = 0; j < b.imCols.size(); ++j)
                out[b.imCols[j]] += glm::dvec2(ru[Eigen::Index(j)], -rv[Eigen::Index(j)]);
        }
    }
}

namespace
{
/** One part (real or imaginary) of the operator: its sparse rows, and per fill block its dense rows over some vertices. */
struct OperatorPart
{
    const SpMat* rows;
    std::vector<const Eigen::MatrixXd*> d;
    std::vector<const std::vector<Index>*> cols;
};

/** a^T g b for dense a, b and sparse g, split over column blocks of b so one big fill uses every core. */
Eigen::MatrixXd denseProduct(const Eigen::MatrixXd& a, const SpMat& g, const Eigen::MatrixXd& b)
{
    Eigen::MatrixXd out(a.cols(), b.cols());
    constexpr Eigen::Index kBlock = 16;
    tbb::parallel_for(Eigen::Index(0), (b.cols() + kBlock - 1) / kBlock, [&](Eigen::Index blk) {
        const Eigen::Index c0 = blk * kBlock, n = std::min(kBlock, b.cols() - c0);
        out.middleCols(c0, n) = a.transpose() * (g * b.middleCols(c0, n));
    });
    return out;
}

/** A^T S B for two parts of the operator, as triplets; fills of different islands share no triangles. */
void crossTerms(const OperatorPart& a, const OperatorPart& b, const SpMat& s, std::span<const SpMat> zt, std::span<const SpMat> g,
                std::vector<Triplet>& out, SpMat& sparse)
{
    sparse = SpMat(SpMat(a.rows->transpose()) * (s * *b.rows)).pruned();
    // A^T S A is symmetric: one cross product gives both of its sides.
    const bool same = &a == &b;
    for (size_t k = 0; k < zt.size(); ++k)
    {
        const Eigen::MatrixXd& da = *a.d[k];
        const Eigen::MatrixXd& db = *b.d[k];
        const std::vector<Index>& ca = *a.cols[k];
        const std::vector<Index>& cb = *b.cols[k];
        if (da.cols() > 0 && db.cols() > 0)
        {
            const Eigen::MatrixXd dgd = denseProduct(da, g[k], db);
            for (Eigen::Index i = 0; i < dgd.rows(); ++i)
                for (Eigen::Index j = 0; j < dgd.cols(); ++j)
                    out.emplace_back(int(ca[size_t(i)]), int(cb[size_t(j)]), dgd(i, j));
        }
        // Chart vertices are few, so each cross term is a dense vertices x columns product.
        if (db.cols() > 0 && a.rows->nonZeros() > 0)
        {
            const SpMat xa = zt[k] * *a.rows;
            const Eigen::MatrixXd cross = SpMat(xa.transpose()) * db;
            for (Eigen::Index v = 0; v < cross.rows(); ++v)
                for (Eigen::Index j = 0; j < cross.cols(); ++j)
                    if (const double x = cross(v, j); x != 0.0)
                    {
                        out.emplace_back(int(v), int(cb[size_t(j)]), x);
                        if (same)
                            out.emplace_back(int(cb[size_t(j)]), int(v), x);
                    }
        }
        if (!same && da.cols() > 0 && b.rows->nonZeros() > 0)
        {
            const SpMat xb = zt[k] * *b.rows;
            const Eigen::MatrixXd cross = SpMat(xb.transpose()) * da;
            for (Eigen::Index v = 0; v < cross.rows(); ++v)
                for (Eigen::Index i = 0; i < cross.cols(); ++i)
                    if (const double x = cross(v, i); x != 0.0)
                        out.emplace_back(int(ca[size_t(i)]), int(v), x);
        }
    }
}
} // namespace

std::pair<SpMat, SpMat> detail::FinalOperator::gram(const SpMat& s) const
{
    LILA_ZONE();

    std::vector<SpMat> zt(mBlocks.size()), g(mBlocks.size());
    OperatorPart re{&mR, {}, {}}, im{&mRi, {}, {}};
    for (size_t k = 0; k < mBlocks.size(); ++k)
    {
        const Block& b = mBlocks[k];
        const auto ni = Eigen::Index(b.start.size() - 1);
        std::vector<Triplet> e;
        for (Eigen::Index q = 0; q < ni; ++q)
            for (Index c = b.start[size_t(q)]; c < b.start[size_t(q) + 1]; ++c)
                e.emplace_back(int(b.corners[c]), int(q), 1.0);
        SpMat E(Eigen::Index(mCorners), ni);
        E.setFromTriplets(e.begin(), e.end());
        // Interior groups against themselves through the block, and against the rows they share triangles with.
        zt[k] = SpMat(E.transpose()) * s;
        g[k] = zt[k] * E;
        re.d.push_back(&b.d);
        re.cols.push_back(&b.cols);
        im.d.push_back(&b.di);
        im.cols.push_back(&b.imCols);
    }
    const auto V = Eigen::Index(mVertices);
    auto build = [&](const std::vector<Triplet>& t, const SpMat& sparse) {
        SpMat add(V, V);
        add.setFromTriplets(t.begin(), t.end());
        return SpMat(sparse + add);
    };
    // R^H S R with R = Re + i Im: the real part is Re^T S Re + Im^T S Im, the imaginary part Re^T S Im - (Re^T S Im)^T.
    std::vector<Triplet> t;
    SpMat sparse;
    crossTerms(re, re, s, zt, g, t, sparse);
    SpMat k = build(t, sparse);
    if (!mRotates)
        return {k, SpMat()};
    t.clear();
    crossTerms(im, im, s, zt, g, t, sparse);
    k += build(t, sparse);
    t.clear();
    crossTerms(re, im, s, zt, g, t, sparse);
    const SpMat ri = build(t, sparse);
    return {k, SpMat(ri - SpMat(ri.transpose())).pruned()};
}

std::vector<double> detail::FinalOperator::columnWeight() const
{
    std::vector<double> w(mVertices, 0.0);
    for (size_t k = 0; k < mIsl.cols.size(); ++k)
        w[mIsl.cols[k]] += std::abs(mIsl.weights[k]);
    for (const Block& b : mBlocks)
        for (size_t q = 0; q + 1 < b.start.size(); ++q)
        {
            const double n = double(b.start[q + 1] - b.start[q]);
            for (size_t j = 0; j < b.cols.size(); ++j)
                w[b.cols[j]] += n * std::abs(b.d(Eigen::Index(q), Eigen::Index(j)));
            for (size_t j = 0; j < b.imCols.size(); ++j)
                w[b.imCols[j]] += n * std::abs(b.di(Eigen::Index(q), Eigen::Index(j)));
        }
    return w;
}

bool detail::UvFactor::compute(const SpMat& re, const SpMat& im)
{
    LILA_ZONE();

    mComplex = im.nonZeros() > 0;
    mCxReady = false;
    mRe = mComplex ? re : SpMat();
    mIm = mComplex ? im : SpMat();
    mReal.compute(re);
    mOk = mReal.info() == Eigen::Success;
    return mOk;
}

bool detail::UvFactor::solve(const Eigen::VectorXd& bu, const Eigen::VectorXd& bv, Eigen::VectorXd& su, Eigen::VectorXd& sv) const
{
    if (!mOk)
        return false;
    su = mReal.solve(bu);
    sv = mReal.solve(bv);
    if (!mComplex)
        return mReal.info() == Eigen::Success && su.allFinite() && sv.allFinite();
    // Re u - Im v = bu, Re v + Im u = bv. The imaginary part is a few rotated corners, so refining against it with the real
    // factor converges in a handful of steps; the complex factor is built only when it doesn't.
    const double scale = std::max(bu.norm() + bv.norm(), 1e-300);
    for (int it = 0; it < kRefineSteps; ++it)
    {
        const Eigen::VectorXd ru = bu - (mRe * su - mIm * sv), rv = bv - (mRe * sv + mIm * su);
        if (!ru.allFinite() || !rv.allFinite())
            break;
        if (ru.norm() + rv.norm() <= 1e-12 * scale)
            return true;
        su += mReal.solve(ru);
        sv += mReal.solve(rv);
    }
    if (!mCxReady)
    {
        const CSpMat k = mRe.cast<std::complex<double>>() + std::complex<double>(0.0, 1.0) * mIm.cast<std::complex<double>>();
        mCx.compute(k);
        mCxReady = true;
    }
    if (mCx.info() != Eigen::Success)
        return false;
    Eigen::VectorXcd b(bu.size());
    b.real() = bu;
    b.imag() = bv;
    const Eigen::VectorXcd z = mCx.solve(b);
    su = z.real();
    sv = z.imag();
    return su.allFinite() && sv.allFinite();
}

struct UninfluencedFill::Solver
{
    /** Per filled vertex (in mUnknown order), its uv as weights of the solved vertices and pins. */
    std::vector<std::vector<std::pair<Index, double>>> rows;
};

UninfluencedFill::UninfluencedFill(const Chart& ch, const FinalIsland& isl)
{
    const size_t V = ch.pos.size();
    std::vector<double> reach(V, 0.0);
    for (size_t k = 0; k < isl.cols.size(); ++k)
        if (isl.cols[k] < V)
            reach[isl.cols[k]] = std::max(reach[isl.cols[k]], std::abs(isl.weights[k]));
    mSolved.assign(V, 1);
    std::vector<int> index(V, -1);
    bool anyKnown = false;
    for (size_t v = 0; v < V; ++v)
        if (reach[v] < kWeakInfluence && !(v < ch.pinned.size() && ch.pinned[v]))
        {
            index[v] = int(mUnknown.size());
            mUnknown.push_back(Index(v));
        }
        else
            anyKnown = true;
    if (mUnknown.empty() || !anyKnown || isl.tris.empty())
    {
        mUnknown.clear();
        return;
    }
    // Clamped cotan weights: the cage's own triangles can be poor, and these uvs only have to read sensibly.
    std::vector<Triplet> ta, tb;
    std::vector<int> knownIndex(V, -1);
    std::vector<Index> knownVertex;
    for (const auto& t : ch.tris)
        for (int k = 0; k < 3; ++k)
        {
            const Index a = t[(k + 1) % 3], b = t[(k + 2) % 3];
            const glm::dvec3 u = ch.pos[a] - ch.pos[t[k]], v = ch.pos[b] - ch.pos[t[k]];
            const double sn = glm::length(glm::cross(u, v));
            const double w = 0.5 * std::max(sn > 1e-300 ? glm::dot(u, v) / sn : 0.0, 0.0) + 1e-3;
            for (auto [p, q] : {std::pair{a, b}, std::pair{b, a}})
            {
                if (index[p] < 0)
                    continue;
                ta.emplace_back(index[p], index[p], w);
                if (index[q] >= 0)
                    ta.emplace_back(index[p], index[q], -w);
                else
                {
                    if (knownIndex[q] < 0)
                    {
                        knownIndex[q] = int(knownVertex.size());
                        knownVertex.push_back(q);
                    }
                    tb.emplace_back(index[p], knownIndex[q], w);
                }
            }
        }
    const auto n = Eigen::Index(mUnknown.size()), nk = Eigen::Index(knownVertex.size());
    SpMat a(n, n);
    a.setFromTriplets(ta.begin(), ta.end());
    double trace = 0.0;
    for (Eigen::Index i = 0; i < n; ++i)
        trace += a.coeff(i, i);
    // Parts of the chart no solved vertex reaches settle at the origin of the frame instead of failing the solve.
    for (Eigen::Index i = 0; i < n; ++i)
        a.coeffRef(i, i) += 1e-9 * std::max(trace / double(n), 1e-30);
    Eigen::SimplicialLDLT<SpMat> ldlt(a);
    if (ldlt.info() != Eigen::Success || nk == 0)
    {
        mUnknown.clear();
        return;
    }
    SpMat bs(n, nk);
    bs.setFromTriplets(tb.begin(), tb.end());
    const Eigen::MatrixXd h = ldlt.solve(Eigen::MatrixXd(bs));
    if (!h.allFinite())
    {
        mUnknown.clear();
        return;
    }
    mSolver = std::make_unique<Solver>();
    mSolver->rows.resize(size_t(n));
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index k = 0; k < nk; ++k)
            if (std::abs(h(i, k)) > 1e-9)
                mSolver->rows[size_t(i)].push_back({knownVertex[size_t(k)], h(i, k)});
    for (Index v : mUnknown)
        mSolved[v] = 0;
}

UninfluencedFill::~UninfluencedFill() = default;

FinalIsland UninfluencedFill::fold(const FinalIsland& isl) const
{
    if (mUnknown.empty())
        return isl;
    std::vector<int> index(mSolved.size(), -1);
    for (size_t i = 0; i < mUnknown.size(); ++i)
        index[mUnknown[i]] = int(i);
    FinalIsland out = isl;
    out.rowStart.assign(1, 0);
    out.cols.clear();
    out.weights.clear();
    Row row;
    for (size_t c = 0; c + 1 < isl.rowStart.size(); ++c)
    {
        row.clear();
        for (Index k = isl.rowStart[c]; k < isl.rowStart[c + 1]; ++k)
        {
            const Index v = isl.cols[k];
            if (v < index.size() && index[v] >= 0)
                for (auto [q, h] : mSolver->rows[size_t(index[v])])
                    row.push_back({q, isl.weights[k] * h});
            else
                row.push_back({v, isl.weights[k]});
        }
        std::sort(row.begin(), row.end(), byColumn);
        for (size_t k = 0; k < row.size(); ++k)
            if (k > 0 && row[k].first == out.cols.back() && out.cols.size() > out.rowStart.back())
                out.weights.back() += row[k].second;
            else
            {
                out.cols.push_back(row[k].first);
                out.weights.push_back(row[k].second);
            }
        out.rowStart.push_back(Index(out.cols.size()));
    }
    return out;
}

void UninfluencedFill::apply(std::span<glm::dvec2> uv) const
{
    if (mUnknown.empty())
        return;
    // All rows read only solved vertices, so the order of the writes doesn't matter.
    for (size_t i = 0; i < mUnknown.size(); ++i)
    {
        glm::dvec2 s(0.0);
        for (auto [q, h] : mSolver->rows[i])
            s += h * uv[q];
        uv[mUnknown[i]] = s;
    }
}

ChartStats finalStats(const FinalIsland& isl, const Chart& chart)
{
    ChartStats s;
    const std::vector<glm::dvec2> y = isl.uvs(chart);
    std::vector<TriFrame> fr(isl.tris.size());
    double a3 = 0.0, au = 0.0;
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        const auto& t = isl.tris[i];
        fr[i] = triFrame(isl.pos[t[0]], isl.pos[t[1]], isl.pos[t[2]]);
        a3 += fr[i].area;
        au += signedArea2(y[t[0]], y[t[1]], y[t[2]]);
    }
    if (a3 <= 0.0 || au == 0.0)
        return s;
    const double sign = au < 0.0 ? -1.0 : 1.0;
    const double ratio = std::abs(au) / a3;
    double wsum = 0.0;
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        if (fr[i].area <= 0.0)
            continue;
        const auto& t = isl.tris[i];
        const double a = sign * signedArea2(y[t[0]], y[t[1]], y[t[2]]);
        if (a <= 0.0)
        {
            ++s.folded;
            continue;
        }
        const double la = std::abs(std::log2(a / fr[i].area / ratio));
        const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
        const double an = d.s1 / std::max(std::abs(d.s2), 1e-12) - 1.0;
        s.area += fr[i].area * la;
        s.angle += fr[i].area * an;
        s.maxArea = std::max(s.maxArea, la);
        s.maxAngle = std::max(s.maxAngle, an);
        wsum += fr[i].area;
    }
    if (wsum > 0.0)
    {
        s.area /= wsum;
        s.angle /= wsum;
    }
    return s;
}

namespace
{
size_t collapsedOf(const FinalIsland& isl, std::span<const glm::dvec2> y, double fraction = kCollapsedFraction)
{
    double a3 = 0.0, au = 0.0;
    std::vector<double> tri3(isl.tris.size()), triUv(isl.tris.size());
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        const auto& t = isl.tris[i];
        tri3[i] = 0.5 * glm::length(glm::cross(isl.pos[t[1]] - isl.pos[t[0]], isl.pos[t[2]] - isl.pos[t[0]]));
        triUv[i] = std::abs(signedArea2(y[t[0]], y[t[1]], y[t[2]]));
        a3 += tri3[i];
        au += triUv[i];
    }
    if (a3 <= 0.0)
        return 0;
    const double density = au / a3;
    size_t n = 0;
    for (size_t i = 0; i < isl.tris.size(); ++i)
        if (tri3[i] > 1e-12 * a3 && triUv[i] < tri3[i] * density * fraction)
            ++n;
    return n;
}
} // namespace

size_t collapsedTriangles(const FinalIsland& isl, const Chart& chart)
{
    return collapsedOf(isl, isl.uvs(chart));
}

size_t collapsedTriangles(const FinalIsland& isl, std::span<const glm::vec2> exportedUv)
{
    std::vector<glm::dvec2> y(isl.corner.size());
    for (size_t k = 0; k < y.size(); ++k)
        y[k] = isl.corner[k] < exportedUv.size() ? glm::dvec2(exportedUv[isl.corner[k]]) : glm::dvec2(0.0);
    return collapsedOf(isl, y);
}

void setDensityArea(Chart& chart, const FinalIsland& isl)
{
    chart.densityArea = 0.0;
    if (isl.tris.empty())
        return;
    const double cageUv = std::abs(chart.areaUv()), exUv = std::abs(isl.areaUv(chart)), ex3 = isl.area3d();
    if (cageUv <= 0.0 || exUv <= 0.0 || ex3 <= 0.0)
        return;
    // A collapsed island would read as nearly no density; bound how far the export can move it from the cage's.
    const double a3 = chart.area3d();
    chart.densityArea = std::clamp(ex3 * cageUv / exUv, a3 / 16.0, a3 * 16.0);
}

namespace detail
{
CageHold::CageHold(const Chart& ch, const FinalIsland& isl, std::span<const glm::dvec2> x0, bool whole)
{
    const size_t V = ch.pos.size();
    std::vector<double> reach(V, 0.0);
    for (size_t k = 0; k < isl.cols.size(); ++k)
        if (isl.cols[k] < V)
            reach[isl.cols[k]] = std::max(reach[isl.cols[k]], std::abs(isl.weights[k]));
    std::vector<double> weak(V);
    for (size_t v = 0; v < V; ++v)
        weak[v] = whole ? 1.0 : std::clamp((kHoldReach - reach[v]) / (kHoldReach - kWeakInfluence), 0.0, 1.0);
    std::map<std::pair<Index, Index>, double> w;
    for (const auto& t : ch.tris)
        for (int k = 0; k < 3; ++k)
        {
            Index a = t[(k + 1) % 3], b = t[(k + 2) % 3];
            const double ww = 0.5 * (weak[a] + weak[b]);
            if (ww <= 0.0)
                continue;
            const glm::dvec3 u = ch.pos[a] - ch.pos[t[k]], v = ch.pos[b] - ch.pos[t[k]];
            const double sn = glm::length(glm::cross(u, v));
            // Clamped cotan weights, as the uninfluenced fill uses: the cage's own triangles can be poor.
            const double c = 0.5 * std::max(sn > 1e-300 ? glm::dot(u, v) / sn : 0.0, 0.0) + 1e-3;
            if (a > b)
                std::swap(a, b);
            w[{a, b}] += ww * c;
        }
    rest.assign(V, glm::dvec2(0.0));
    std::vector<double> diag(V, 0.0);
    for (const auto& [e, we] : w)
    {
        const auto [a, b] = e;
        edges.push_back({a, b});
        weight.push_back(we);
        start.push_back(x0[b] - x0[a]);
        laplacian.emplace_back(int(a), int(a), we);
        laplacian.emplace_back(int(b), int(b), we);
        laplacian.emplace_back(int(a), int(b), -we);
        laplacian.emplace_back(int(b), int(a), -we);
        rest[a] -= we * (x0[b] - x0[a]);
        rest[b] += we * (x0[b] - x0[a]);
        diag[a] += we;
        diag[b] += we;
    }
    size_t held = 0;
    for (size_t v = 0; v < V; ++v)
        if (diag[v] > 0.0)
        {
            diagMean += diag[v];
            ++held;
        }
    diagMean = held ? diagMean / double(held) : 0.0;
}


double CageHold::energy(std::span<const glm::dvec2> x) const
{
    double e = 0.0;
    for (size_t k = 0; k < edges.size(); ++k)
    {
        const glm::dvec2 d = x[edges[k][1]] - x[edges[k][0]] - start[k];
        e += weight[k] * glm::dot(d, d);
    }
    return e;
}
} // namespace detail

namespace
{
OptimizeResult solveHeld(Chart& ch, const FinalIsland& exact, const FinalSolveOptions& o, double holdShare, bool wholeCage = false);
} // namespace

OptimizeResult solveFinal(Chart& ch, const FinalIsland& exact, const FinalSolveOptions& o)
{
    return solveHeld(ch, exact, o, detail::kCageHold);
}

namespace
{
/** solveFinal with the cage hold at the given share of the export's stiffness (0: none), on the barely read vertices or the whole cage. */
OptimizeResult solveHeld(Chart& ch, const FinalIsland& exact, const FinalSolveOptions& o, double holdShare, bool wholeCage)
{
    LILA_ZONE();

    OptimizeResult r;
    if (ch.pos.size() < 3 || exact.tris.empty())
        return r;
    r.before = finalStats(exact, ch);
    // Vertices the export barely reads are placed from the others; the solve sees them through the fill.
    const UninfluencedFill fill(ch, exact);
    const FinalIsland isl = fill.fold(exact);
    const std::vector<glm::dvec2> original = ch.uv;
    const size_t collapsedOriginal = collapsedTriangles(exact, ch);
    fill.apply(ch.uv);
    const size_t V = ch.pos.size(), C = isl.pos.size(), T = isl.tris.size();
    size_t pins = 0;
    if (o.usePins)
        for (uint8_t p : ch.pinned)
            pins += p ? 1 : 0;

    const double cageUv = ch.areaUv(), cage3 = ch.area3d();
    if (cage3 <= 0.0 || std::abs(cageUv) <= 1e-300)
        return r;
    const bool mirror = cageUv < 0.0 && pins < 2;
    const double sx = mirror ? -1.0 : 1.0;
    std::vector<glm::dvec2> x(ch.uv.begin(), ch.uv.end());
    for (glm::dvec2& p : x)
        p.x *= sx;

    // Exported triangles at the island's density on the cage, so 1 means "as dense as the island is now".
    const double scale = std::sqrt(std::abs(cageUv) / cage3);
    std::vector<TriFrame> fr(T);
    for (size_t i = 0; i < T; ++i)
    {
        const auto& t = isl.tris[i];
        fr[i] = triFrame(isl.pos[t[0]] * scale, isl.pos[t[1]] * scale, isl.pos[t[2]] * scale);
    }

    // y = R x + off: exported corner uvs from chart vertex uvs.
    const FinalOperator op(isl, V, mirror);
    const glm::dvec2 offScale(sx, 1.0);
    std::vector<glm::dvec2> off(C);
    op.apply(std::vector<glm::dvec2>(V, glm::dvec2(0.0)), off, offScale);

    std::vector<int> freeIdx(V, -1);
    int nFree = 0;
    std::vector<uint8_t> fixed(V, 0);
    if (pins > 0)
        for (size_t v = 0; v < V; ++v)
            fixed[v] = ch.pinned[v];
    else
    {
        // Pin the vertex with the most weight so the system has a fixed point.
        const std::vector<double> colWeight = op.columnWeight();
        fixed[size_t(std::max_element(colWeight.begin(), colWeight.end()) - colWeight.begin())] = 1;
    }
    for (size_t v = 0; v < V; ++v)
        fixed[v] |= !fill.solved()[v];
    for (size_t v = 0; v < V; ++v)
        if (!fixed[v])
            freeIdx[v] = nFree++;
    if (nFree == 0)
        return r;

    // The weighted stiffness of the exported triangles and its factorisation over the free chart vertices.
    struct Coupling
    {
        int row, col;
        FinalWeight w;
    };
    SpMat S(static_cast<Eigen::Index>(C), static_cast<Eigen::Index>(C));
    Eigen::VectorXd offU(static_cast<Eigen::Index>(C)), offV(static_cast<Eigen::Index>(C)), soU, soV;
    for (size_t c = 0; c < C; ++c)
    {
        offU[Eigen::Index(c)] = off[c].x;
        offV[Eigen::Index(c)] = off[c].y;
    }
    std::vector<Coupling> kfc;
    const detail::CageHold hold(ch, isl, x, wholeCage);
    double holdScale = 0.0;
    auto solver = std::make_unique<UvFactor>();
    auto factor = [&](const std::vector<double>& wt) {
        std::vector<Triplet> trips;
        trips.reserve(T * 9);
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const double a = fr[i].area * wt[i];
            for (int p = 0; p < 3; ++p)
                for (int q = 0; q < 3; ++q)
                    trips.push_back(Triplet(int(isl.tris[i][p]), int(isl.tris[i][q]), a * glm::dot(fr[i].g[p], fr[i].g[q])));
        }
        S.setFromTriplets(trips.begin(), trips.end());
        soU = S * offU;
        soV = S * offV;
        const auto [K, Ki] = op.gram(S);
        std::vector<Triplet> kff, kffi;
        kfc.clear();
        double diag = 0.0;
        for (int part = 0; part < 2; ++part)
        {
            const SpMat& M = part == 0 ? K : Ki;
            for (int k = 0; k < M.outerSize(); ++k)
                for (SpMat::InnerIterator it(M, k); it; ++it)
                {
                    const int fr0 = freeIdx[size_t(it.row())], fc0 = freeIdx[size_t(it.col())];
                    if (fr0 < 0)
                        continue;
                    if (fc0 >= 0)
                    {
                        (part == 0 ? kff : kffi).push_back(Triplet(fr0, fc0, it.value()));
                        if (fr0 == fc0 && part == 0)
                            diag += it.value();
                    }
                    else
                        kfc.push_back({fr0, int(it.col()), part == 0 ? FinalWeight(it.value(), 0.0) : FinalWeight(0.0, it.value())});
                }
        }
        // The hold is scaled to the export's stiffness once, so it weighs the same against it whatever the island's size.
        if (holdScale == 0.0)
            holdScale = hold.scaleFor(holdShare, diag / nFree);
        for (const Triplet& t : hold.laplacian)
        {
            const int fr0 = freeIdx[size_t(t.row())], fc0 = freeIdx[size_t(t.col())];
            if (fr0 < 0)
                continue;
            if (fc0 >= 0)
                kff.push_back(Triplet(fr0, fc0, holdScale * t.value()));
            else
                kfc.push_back({fr0, int(t.col()), FinalWeight(holdScale * t.value(), 0.0)});
        }
        // Chart vertices no exported triangle reaches keep a tiny diagonal so the system stays solvable.
        const double eps = 1e-9 * std::max(diag / nFree, 1e-30);
        for (int i = 0; i < nFree; ++i)
            kff.push_back(Triplet(i, i, eps));
        SpMat Kff(nFree, nFree), Kffi;
        Kff.setFromTriplets(kff.begin(), kff.end());
        if (!kffi.empty())
        {
            Kffi.resize(nFree, nFree);
            Kffi.setFromTriplets(kffi.begin(), kffi.end());
        }
        solver = std::make_unique<UvFactor>();
        return solver->compute(Kff, Kffi);
    };

    auto exportUv = [&](const std::vector<glm::dvec2>& xs, std::vector<glm::dvec2>& y) {
        y.resize(C);
        op.apply(xs, y, offScale);
    };
    // The reweighted rounds pull towards each triangle's own shape whatever the mode: the other modes let slivers stay small.
    bool isometric = o.mode == OptimizeMode::Isometric;
    auto targetOf = [&](const Svd2& d) {
        if (isometric)
            return std::pair<double, double>(1.0, 1.0);
        return targetStretch(o.mode, o.mix, d.s1, d.s2);
    };
    auto energy = [&](const std::vector<glm::dvec2>& y, const std::vector<double>& wt) {
        double e = 0.0;
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = isl.tris[i];
            const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
            auto [t1, t2] = targetOf(d);
            e += wt[i] * fr[i].area * ((d.s1 - t1) * (d.s1 - t1) + (d.s2 - t2) * (d.s2 - t2));
        }
        return e;
    };
    auto exportArea = [&](const std::vector<glm::dvec2>& y) {
        double a = 0.0;
        for (const auto& t : isl.tris)
            a += signedArea2(y[t[0]], y[t[1]], y[t[2]]);
        return a;
    };
    auto centroid = [&](const std::vector<glm::dvec2>& xs) {
        glm::dvec2 s(0.0);
        for (const glm::dvec2& p : xs)
            s += p;
        return s / double(xs.size());
    };
    // Isometric targets carry their own scale; the others would drift without holding the exported area.
    const bool keepArea = pins < 2 && o.mode != OptimizeMode::Isometric && o.mode != OptimizeMode::Authalic;
    double keptArea = 0.0;
    auto restoreArea = [&](std::vector<glm::dvec2>& xs) {
        std::vector<glm::dvec2> y;
        exportUv(xs, y);
        const double a = std::abs(exportArea(y));
        if (a <= 0.0)
            return;
        const glm::dvec2 c = centroid(xs);
        const double k = std::sqrt(keptArea / a);
        for (glm::dvec2& p : xs)
            p = c + (p - c) * k;
    };
    auto folded = [&](const std::vector<glm::dvec2>& y) {
        size_t n = 0;
        for (size_t i = 0; i < T; ++i)
            if (fr[i].area > 0.0 && signedArea2(y[isl.tris[i][0]], y[isl.tris[i][1]], y[isl.tris[i][2]]) <= 0.0)
                ++n;
        return n;
    };
    // Triangles under kCollapsedFraction of the uv area the island's density gives them, as bake lint counts them, with a
    // margin: the export the written uvs give can differ a little (a fit's rebuilt island corners follow the new uvs).
    auto collapsed = [&](const std::vector<glm::dvec2>& y) {
        double a3 = 0.0, au = 0.0;
        for (size_t i = 0; i < T; ++i)
        {
            a3 += fr[i].area;
            au += std::abs(signedArea2(y[isl.tris[i][0]], y[isl.tris[i][1]], y[isl.tris[i][2]]));
        }
        size_t n = 0;
        if (a3 <= 0.0)
            return n;
        const double density = au / a3;
        for (size_t i = 0; i < T; ++i)
            if (fr[i].area > 1e-12 * a3 &&
                std::abs(signedArea2(y[isl.tris[i][0]], y[isl.tris[i][1]], y[isl.tris[i][2]])) < fr[i].area * density * kCollapsedFraction * kSolveMargin)
                ++n;
        return n;
    };

    const glm::dvec2 centre0 = ch.uvCentroid();
    std::vector<glm::dvec2> y;
    exportUv(x, y);
    keptArea = std::abs(exportArea(y));
    const size_t folded0 = folded(y);
    std::vector<double> wt(T, 1.0);
    if (!factor(wt))
        return r;
    // Steps are free to fold on the way (a flip guard stalls on the slivers this solve exists to open). The result is the
    // iterate with the fewest collapsed triangles, then the lowest plain energy, that folds no more than the start.
    const std::vector<double> plain(T, 1.0);
    std::vector<glm::dvec2> best = x;
    size_t bestCollapsed = collapsed(y);
    double bestE = energy(y, plain);
    auto consider = [&]() {
        if (folded(y) > folded0)
            return;
        const size_t c = collapsed(y);
        const double e = energy(y, plain);
        if (c < bestCollapsed || (c == bestCollapsed && e < bestE))
        {
            best = x;
            bestCollapsed = c;
            bestE = e;
        }
    };

    Eigen::VectorXd bu(nFree), bv(nFree);
    std::vector<glm::dvec2> be(C), trial(V), dir(V), ty, gc(C), fuv(V);
    std::vector<Jac2> target(T);
    // Local/global iterations with the current weights; true when they ran to a settled energy.
    auto iterate = [&](int maxIt, int& ran) {
        double e0 = energy(y, wt) + holdScale * hold.energy(x);
        for (int it = 0; it < maxIt; ++it)
        {
            ++ran;
            tbb::parallel_for(size_t(0), T, [&](size_t i) {
                if (fr[i].area <= 0.0)
                    return;
                const auto& t = isl.tris[i];
                const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
                auto [t1, t2] = targetOf(d);
                target[i] = compose(d, t1, t2);
            });
            std::fill(be.begin(), be.end(), glm::dvec2(0.0));
            for (size_t i = 0; i < T; ++i)
            {
                if (fr[i].area <= 0.0)
                    continue;
                const auto& t = isl.tris[i];
                const double a = fr[i].area * wt[i];
                for (int p = 0; p < 3; ++p)
                    be[t[p]] += a * glm::dvec2(fr[i].g[p].x * target[i].a + fr[i].g[p].y * target[i].b,
                                               fr[i].g[p].x * target[i].c + fr[i].g[p].y * target[i].d);
            }
            // rhs = R^T (be - S off), then moved over the fixed vertices.
            for (size_t c = 0; c < C; ++c)
                gc[c] = be[c] - glm::dvec2(soU[Eigen::Index(c)], soV[Eigen::Index(c)]);
            op.applyTransposed(gc, fuv);
            for (size_t v = 0; v < V; ++v)
                if (freeIdx[v] >= 0)
                {
                    bu[freeIdx[v]] = fuv[v].x + holdScale * hold.rest[v].x;
                    bv[freeIdx[v]] = fuv[v].y + holdScale * hold.rest[v].y;
                }
            for (const Coupling& cp : kfc)
            {
                const glm::dvec2 k = weigh(cp.w, x[size_t(cp.col)]);
                bu[cp.row] -= k.x;
                bv[cp.row] -= k.y;
            }
            Eigen::VectorXd su, sv;
            if (!solver->solve(bu, bv, su, sv))
                return false;
            for (size_t v = 0; v < V; ++v)
                dir[v] = freeIdx[v] >= 0 ? glm::dvec2(su[freeIdx[v]], sv[freeIdx[v]]) - x[v] : glm::dvec2(0.0);
            double step = 1.0, e1 = e0;
            bool accepted = false;
            for (int back = 0; back < 12; ++back)
            {
                for (size_t v = 0; v < V; ++v)
                    trial[v] = x[v] + step * dir[v];
                if (keepArea)
                    restoreArea(trial);
                exportUv(trial, ty);
                e1 = energy(ty, wt) + holdScale * hold.energy(trial);
                if (e1 <= e0)
                {
                    accepted = true;
                    break;
                }
                step *= 0.5;
            }
            if (!accepted)
                return true;
            x.swap(trial);
            y.swap(ty);
            consider();
            const bool settled = e0 - e1 <= 1e-7 * std::max(e0, 1e-30);
            e0 = e1;
            if (settled)
                return true;
        }
        return false;
    };

    int it = 0;
    iterate(o.iterations, it);
    // Then reweighted rounds that weigh squeezed triangles up (towards symmetric Dirichlet's barrier), so slivers open
    // even where they are a small share of the island's area.
    for (int round = 0; round < 6 && collapsed(y) > 0; ++round)
    {
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = isl.tris[i];
            const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
            const double s2 = std::max(std::abs(d.s2), 1e-3);
            wt[i] = std::min(1.0 + 0.05 / (s2 * s2), 1e4);
        }
        if (!factor(wt))
            break;
        isometric = true;
        iterate(std::max(10, o.iterations / 4), it);
    }
    for (size_t v = 0; v < V; ++v)
        ch.uv[v] = glm::dvec2(best[v].x * sx, best[v].y);
    fill.apply(ch.uv);
    if (pins == 0)
    {
        const glm::dvec2 shift = centre0 - ch.uvCentroid();
        for (glm::dvec2& p : ch.uv)
            p += shift;
    }
    // The fill moves vertices the export reads a little, so the result can lose to where the island started.
    if (collapsedTriangles(exact, ch) > collapsedOriginal)
        ch.uv = original;
    r.ok = true;
    r.iterations = it;
    r.after = finalStats(exact, ch);
    return r;
}
} // namespace

namespace
{
/** Re-solves against neighbours' new uvs after the first solve, for islands still left with slivers. */
constexpr int kNeighbourRounds = 2;

/** How tangled the island's own layout is: the share of its uv area in triangles that overlap another or are wound against it. */
double tangled(const Chart& c)
{
    std::vector<std::array<glm::dvec2, 3>> tris(c.tris.size());
    for (size_t i = 0; i < c.tris.size(); ++i)
        tris[i] = {c.uv[c.tris[i][0]], c.uv[c.tris[i][1]], c.uv[c.tris[i][2]]};
    std::vector<uint8_t> over;
    triangleOverlaps(tris, over, c.triFace, true);
    const double side = c.areaUv() < 0.0 ? -1.0 : 1.0;
    double bad = 0.0, all = 0.0;
    for (size_t i = 0; i < tris.size(); ++i)
    {
        const double a = side * signedArea2(tris[i][0], tris[i][1], tris[i][2]);
        all += std::abs(a);
        if (a < 0.0 || over[i])
            bad += std::abs(a);
    }
    return all > 0.0 ? bad / all : 0.0;
}

/**
 * The solved island moved back towards where it started until its own layout is no more tangled than it was there: the
 * export is solved for, but the cage island is what the UV Editor shows and edits. False when only the start is left.
 */
bool untangle(Chart& solved, const Chart& start, const FinalIsland& isl, size_t limit)
{
    const double allowed = tangled(start);
    if (tangled(solved) <= allowed)
        return true;
    Chart mixed = solved;
    for (const double t : {0.75, 0.5, 0.25})
    {
        for (size_t v = 0; v < mixed.uv.size(); ++v)
            mixed.uv[v] = glm::mix(start.uv[v], solved.uv[v], t);
        if (tangled(mixed) <= allowed && collapsedTriangles(isl, mixed) <= limit)
        {
            solved = std::move(mixed);
            return true;
        }
    }
    return false;
}

/**
 * Solves one island against its export and keeps the result when it collapses no more than limit and leaves the island's
 * own layout no more tangled; a move back into its tile that would cost it slivers is dropped on its own. False when the
 * solve failed.
 */
bool solveIsland(Chart& chart, const FinalIsland& isl, const FinalSolveOptions& opts, size_t limit)
{
    Chart trial = chart, loose = chart;
    const glm::ivec2 tile(glm::floor(trial.uvCentroid()));
    // The hold keeps barely read vertices home; where slivers are left (with the solve's margin, as the written export can
    // differ a little), the solve without it may open them. Both run at once; the second is skipped if it starts after the
    // first came out clean.
    auto slivers = [&](const Chart& c) {
        const std::vector<glm::dvec2> y = isl.uvs(c);
        return std::pair{collapsedOf(isl, y), collapsedOf(isl, y, kCollapsedFraction * kSolveMargin)};
    };
    enum : int { Solving, Clean, Slivers, Failed };
    std::atomic<int> held{Solving};
    std::pair<size_t, size_t> heldSlivers{0, 0};
    bool looseOk = false;
    tbb::parallel_invoke(
        [&] {
            if (!solveFinal(trial, isl, opts).ok)
            {
                held = Failed;
                return;
            }
            heldSlivers = slivers(trial);
            held = heldSlivers.second > 0 ? Slivers : Clean;
        },
        [&] {
            if (const int h = held.load(); h == Solving || h == Slivers)
                looseOk = solveHeld(loose, isl, opts, 0.0).ok;
        });
    if (held == Failed)
        return false;
    if (held == Slivers && looseOk && slivers(loose) < heldSlivers && tangled(loose) <= tangled(chart))
        trial = std::move(loose);
    // An island the solve tangled to open slivers solves again with its whole cage shape held, harder each time.
    const double allowed = tangled(chart);
    for (const double share : {0.25, 1.0, 4.0, 16.0})
    {
        if (tangled(trial) <= allowed)
            break;
        Chart firm = chart;
        if (solveHeld(firm, isl, opts, share, true).ok)
            trial = std::move(firm);
    }
    if (!untangle(trial, chart, isl, limit))
        return true;
    // Moving the island alone shifts it against the uvs its neighbours lend its export: that move goes, not the solve.
    if (!opts.usePins || std::ranges::none_of(trial.pinned, [](uint8_t p) { return p != 0; }))
    {
        Chart moved = trial;
        keepInTile(moved, tile);
        if (collapsedTriangles(isl, moved) <= collapsedTriangles(isl, trial))
            trial = std::move(moved);
    }
    if (collapsedTriangles(isl, trial) <= limit)
        chart = std::move(trial);
    return true;
}

/**
 * Solves the islands, all of them or only those whose export collapses now, against the export as the cage's current
 * uvs give it, and writes them back (see solveIsland). The written uvs' export is then measured with one run of the stack:
 * islands that came out with more slivers than they started with go back, and those still left with slivers solve again
 * against where their neighbours went.
 */
FinalRefineResult refineRounds(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack,
                               const FinalSolveOptions& opts, bool repairOnly)
{
    FinalRefineResult out;
    if (stack.empty() || cage.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return out;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    const std::vector<Index> live = faces.empty() ? std::vector<Index>{} : islandsOfFaces(cage, liveFaces(cage, faces), co);
    auto distortion = [](std::span<const FinalIsland> islands, std::span<const Chart> charts) {
        double d = 0.0, w = 0.0;
        for (size_t i = 0; i < charts.size(); ++i)
            if (!islands[i].tris.empty())
            {
                const double a = islands[i].area3d();
                d += a * finalStats(islands[i], charts[i]).angle;
                w += a;
            }
        return w > 0.0 ? d / w : 0.0;
    };
    const auto shared = std::make_shared<const FinalMap>(buildFinalMap(cage, uvSet, stack));
    const FinalMap& map = *shared;
    out.map = shared;
    out.method = map.method;
    out.warning = map.warning;
    if (!map.active())
        return out;
    std::vector<Chart> charts = buildCharts(cage, live, co);
    const std::vector<FinalIsland> islands = finalIslands(map, cage, charts, uvSet);
    const size_t n = charts.size();
    std::vector<size_t> now(n, 0);
    tbb::parallel_for(size_t(0), n, size_t(1), [&](size_t i) {
        if (!islands[i].tris.empty())
            now[i] = collapsedTriangles(islands[i], charts[i]);
    });
    out.collapsedBefore = std::accumulate(now.begin(), now.end(), size_t(0));
    out.distortionBefore = distortion(islands, charts);
    out.islands = size_t(std::ranges::count_if(islands, [](const FinalIsland& isl) { return !isl.tris.empty(); }));
    std::vector<uint8_t> pick(n, 0);
    for (size_t i = 0; i < n; ++i)
        pick[i] = !islands[i].tris.empty() && (!repairOnly || now[i] > 0);
    if (std::ranges::none_of(pick, [](uint8_t p) { return p != 0; }))
    {
        out.collapsedAfter = out.collapsedBefore;
        out.distortionAfter = out.distortionBefore;
        return out;
    }
    std::vector<std::vector<glm::dvec2>> prevUv(n);
    std::vector<uint8_t> ok(n, 1);
    tbb::parallel_for(size_t(0), n, size_t(1), [&](size_t i) {
        if (!pick[i])
            return;
        prevUv[i] = charts[i].uv;
        ok[i] = solveIsland(charts[i], islands[i], opts, now[i]);
    });
    out.failed = size_t(std::ranges::count(ok, uint8_t(0)));
    writeCharts(cage, charts, uvSet);
    // Measured on the export itself, so the count is what bake lint finds; the map's prediction stands in if the stack's
    // output no longer matches it.
    auto measure = [&](std::vector<size_t>& count) {
        const geo::Mesh ex = runStack(cage, stack);
        const std::span<const glm::vec2> uv = ex.cornerAttrs().get<glm::vec2>(uvSet);
        const bool same = !uv.empty() && ex.cornerCapacity() == map.exported->cornerCapacity() && ex.faceCapacity() == map.exported->faceCapacity();
        tbb::parallel_for(size_t(0), n, size_t(1), [&](size_t i) {
            if (!islands[i].tris.empty())
                count[i] = same ? collapsedTriangles(islands[i], uv) : collapsedTriangles(islands[i], charts[i]);
        });
    };
    std::vector<size_t> after(n, 0);
    measure(after);
    bool restored = false;
    for (size_t i = 0; i < n; ++i)
        if (pick[i] && after[i] > now[i])
        {
            charts[i].uv = prevUv[i];
            restored = true;
        }
    if (restored)
    {
        writeCharts(cage, charts, uvSet);
        measure(after);
    }
    // An island's export also reads its neighbours' uvs where a bevel strip crosses their seam, and each island was solved
    // with its neighbours where they started. Islands that came out with more slivers than their solve expected solve
    // again against where their neighbours went.
    std::vector<FinalIsland> current;
    for (int round = 0; round < kNeighbourRounds; ++round)
    {
        const std::vector<FinalIsland>& solvedOn = round == 0 ? islands : current;
        std::vector<uint8_t> again(n, 0);
        for (size_t i = 0; i < n; ++i)
            again[i] = pick[i] && after[i] > collapsedTriangles(solvedOn[i], charts[i]);
        if (std::ranges::none_of(again, [](uint8_t a) { return a != 0; }))
            break;
        std::vector<FinalIsland> moved = finalIslands(map, cage, charts, uvSet);
        std::vector<std::vector<glm::dvec2>> kept(n);
        tbb::parallel_for(size_t(0), n, size_t(1), [&](size_t i) {
            if (!again[i])
                return;
            kept[i] = charts[i].uv;
            solveIsland(charts[i], moved[i], opts, collapsedTriangles(moved[i], charts[i]));
        });
        writeCharts(cage, charts, uvSet);
        std::vector<size_t> next(n, 0);
        measure(next);
        bool back = false;
        for (size_t i = 0; i < n; ++i)
            if (again[i] && next[i] > after[i])
            {
                charts[i].uv = kept[i];
                back = true;
            }
        if (back)
        {
            writeCharts(cage, charts, uvSet);
            measure(next);
        }
        after = std::move(next);
        current = std::move(moved);
    }
    out.collapsedAfter = std::accumulate(after.begin(), after.end(), size_t(0));
    out.distortionAfter = distortion(islands, charts);
    return out;
}
} // namespace

FinalRefineResult refineFinal(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack, const FinalSolveOptions& opts)
{
    LILA_ZONE();
    return refineRounds(cage, faces, uvSet, stack, opts, false);
}

FinalRefineResult repairFinal(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack, const FinalSolveOptions& opts)
{
    LILA_ZONE();
    return refineRounds(cage, faces, uvSet, stack, opts, true);
}

FinalMap applyDensityAreas(const geo::Mesh& cage, std::span<Chart> charts, std::string_view uvSet, const Stack& stack)
{
    FinalMap map = buildFinalMap(cage, uvSet, stack);
    if (!map.active())
        return map;
    std::vector<FinalIsland> islands = finalIslands(map, cage, charts, uvSet);
    for (size_t i = 0; i < charts.size(); ++i)
        setDensityArea(charts[i], islands[i]);
    return map;
}
} // namespace lila::uv
