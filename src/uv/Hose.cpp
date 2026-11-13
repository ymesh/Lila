#include "uv/Hose.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "uv/Layout.h"
#include "uv/Project.h"
#include "uv/Unwrap.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace lila::uv
{
namespace
{
/** A closed strip of quads going once around a tube, one face per column. Side 0 and side 1 are its two edge loops. */
struct Ring
{
    std::vector<Index> faces;
    std::vector<Index> cross;                // per column: the edge shared with the previous column, running along the tube
    std::array<std::vector<Index>, 2> side;  // per column: its edge on each loop
    std::array<std::vector<Index>, 2> point; // per column: where its cross edge meets each loop
    double length = 0.0;
    std::array<glm::dvec3, 2> center{};
    std::array<double, 2> radius{};
    std::array<double, 2> loopLength{};
    glm::dvec3 axis{0.0};
    bool flat = false;
    std::array<int, 2> next{-1, -1}; // the ring stacked across each loop
};

/** One ring of a run and the side its predecessor is on. */
struct Link
{
    int ring;
    int in;
};

enum class Kind : uint8_t
{
    Strip,
    Cap,
    Other,
};

struct Plan
{
    std::vector<int> island; // per face, -1 outside the faces
    std::vector<Kind> kind;  // per island
    std::vector<uint8_t> rail; // per edge: cut along a strip
    HoseResult result;
};

class Analysis
{
public:
    Analysis(const geo::Mesh& m, std::span<const Index> faces, const HoseOptions& o) : mMesh(m), mFaces(faces), mOpts(o)
    {
        mInSet.assign(m.faceCapacity(), 0);
        for (Index f : faces)
            mInSet[f] = 1;
    }

    Plan run()
    {
        LILA_ZONE();
        mPlan.island.assign(mMesh.faceCapacity(), -1);
        mPlan.rail.assign(mMesh.edgeCapacity(), 0);
        findRings();
        linkRings();
        mCost = seamCosts(mMesh, mFaces, mOpts.visibility, mOpts.views);
        for (std::vector<Link>& chain : chains())
            layRun(std::move(chain));
        groupCaps();
        return std::move(mPlan);
    }

private:
    const geo::Mesh& mMesh;
    std::span<const Index> mFaces;
    const HoseOptions& mOpts;
    std::vector<uint8_t> mInSet;
    std::vector<Ring> mRings;
    std::vector<int> mRingOf; // per face
    std::vector<double> mCost;
    std::vector<uint8_t> mCycle; // per chain from chains()
    Plan mPlan;

    bool quad(Index f) const { return f != kInvalid && mInSet[f] && mMesh.faceSize(f) == 4; }

    /** The corner entering the next quad across the face's far edge, or kInvalid where the strip ends. */
    Index across(Index c) const
    {
        const Index exit = mMesh.cornerNext(mMesh.cornerNext(c));
        const Index r = mMesh.cornerRadial(exit);
        if (r == exit || mMesh.cornerRadial(r) != exit || !quad(mMesh.cornerFace(r)))
            return kInvalid;
        // Flipped winding between the two faces would swap the loops.
        return mMesh.cornerPoint(r) == mMesh.cornerNextPoint(exit) ? r : kInvalid;
    }

    int direction(Index c) const
    {
        int i = 0;
        for (Index k = mMesh.faceCorner(mMesh.cornerFace(c)); k != c; k = mMesh.cornerNext(k))
            ++i;
        return i & 1;
    }

    /** Closed quad strips, shortest first, each face kept by the first (shortest) strip through it. */
    void findRings()
    {
        LILA_ZONE();
        std::vector<uint8_t> done(size_t(mMesh.faceCapacity()) * 2, 0);
        std::vector<std::vector<Index>> strips;
        std::vector<double> lengths;
        for (Index f : mFaces)
        {
            if (!quad(f))
                continue;
            Index c0 = mMesh.faceCorner(f);
            for (int dir = 0; dir < 2; ++dir, c0 = mMesh.cornerNext(c0))
            {
                if (done[size_t(f) * 2 + dir])
                    continue;
                std::vector<Index> walk{c0};
                bool closed = false;
                for (Index c = c0;;)
                {
                    const Index n = across(c);
                    if (n == kInvalid || done[size_t(mMesh.cornerFace(n)) * 2 + direction(n)] || walk.size() > mFaces.size())
                        break;
                    if (n == c0)
                    {
                        closed = true;
                        break;
                    }
                    walk.push_back(n);
                    c = n;
                }
                for (Index c : walk)
                    done[size_t(mMesh.cornerFace(c)) * 2 + direction(c)] = 1;
                if (!closed || walk.size() < 3)
                    continue;
                double len = 0.0;
                for (size_t i = 0; i < walk.size(); ++i)
                    len += glm::distance(mMesh.faceCentroid(mMesh.cornerFace(walk[i])), mMesh.faceCentroid(mMesh.cornerFace(walk[(i + 1) % walk.size()])));
                strips.push_back(std::move(walk));
                lengths.push_back(len);
            }
        }
        std::vector<size_t> order(strips.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lengths[a] < lengths[b]; });
        mRingOf.assign(mMesh.faceCapacity(), -1);
        for (size_t s : order)
        {
            const std::vector<Index>& walk = strips[s];
            bool free = true;
            for (Index c : walk)
                free &= mRingOf[mMesh.cornerFace(c)] == -1;
            if (!free)
                continue;
            const int id = int(mRings.size());
            for (Index c : walk)
                mRingOf[mMesh.cornerFace(c)] = id;
            mRings.push_back(makeRing(walk, lengths[s]));
        }
    }

    Ring makeRing(const std::vector<Index>& walk, double length) const
    {
        Ring r;
        r.length = length;
        const size_t n = walk.size();
        for (Index c : walk)
        {
            r.faces.push_back(mMesh.cornerFace(c));
            r.cross.push_back(mMesh.cornerEdge(c));
            r.side[0].push_back(mMesh.cornerEdge(mMesh.cornerNext(c)));
            r.side[1].push_back(mMesh.cornerEdge(mMesh.cornerPrev(c)));
            r.point[0].push_back(mMesh.cornerNextPoint(c));
            r.point[1].push_back(mMesh.cornerPoint(c));
        }
        for (int s = 0; s < 2; ++s)
        {
            glm::dvec3 c(0.0);
            for (Index p : r.point[s])
                c += mMesh.position(p);
            c /= double(n);
            double rad = 0.0;
            for (Index p : r.point[s])
                rad += glm::distance(mMesh.position(p), c);
            r.center[s] = c;
            r.radius[s] = rad / double(n);
            for (Index e : r.side[s])
                r.loopLength[s] += mMesh.edgeLength(e);
        }
        glm::dvec3 mid = 0.5 * (r.center[0] + r.center[1]);
        for (size_t i = 0; i < n; ++i)
            r.axis += glm::cross(mMesh.faceCentroid(r.faces[i]) - mid, mMesh.faceCentroid(r.faces[(i + 1) % n]) - mid);
        if (glm::length(r.axis) < 1e-12)
            r.axis = r.center[1] - r.center[0];
        if (glm::length(r.axis) > 1e-12)
            r.axis = glm::normalize(r.axis);
        // An annulus (a rim, an end ring of a grid cap) widens without moving along the axis.
        const double along = std::abs(glm::dot(r.center[1] - r.center[0], r.axis));
        r.flat = along < 0.2 * std::abs(r.radius[1] - r.radius[0]);
        return r;
    }

    /** The ring every face across the loop belongs to, or -1 when the loop borders anything else. */
    int ringAcross(const Ring& r, int s) const
    {
        int found = -1;
        for (size_t k = 0; k < r.faces.size(); ++k)
        {
            Index c = mMesh.faceCorner(r.faces[k]);
            while (mMesh.cornerEdge(c) != r.side[s][k])
                c = mMesh.cornerNext(c);
            const Index o = mMesh.cornerRadial(c);
            if (o == c || mMesh.cornerRadial(o) != c || !mInSet[mMesh.cornerFace(o)])
                return -1;
            const int other = mRingOf[mMesh.cornerFace(o)];
            if (other < 0 || (found >= 0 && other != found))
                return -1;
            found = other;
        }
        return found;
    }

    void linkRings()
    {
        std::vector<std::array<int, 2>> across(mRings.size());
        for (size_t i = 0; i < mRings.size(); ++i)
            for (int s = 0; s < 2; ++s)
                across[i][s] = ringAcross(mRings[i], s);
        for (size_t i = 0; i < mRings.size(); ++i)
            for (int s = 0; s < 2; ++s)
            {
                const int j = across[i][s];
                if (j < 0 || j == int(i) || mRings[i].flat || mRings[size_t(j)].flat || mRings[i].faces.size() != mRings[size_t(j)].faces.size())
                    continue;
                // Exactly one of its loops is this one.
                if ((across[size_t(j)][0] == int(i)) != (across[size_t(j)][1] == int(i)))
                    mRings[i].next[s] = j;
            }
    }

    /** Runs of stacked rings, open ones first walked from an end; cycles (rings, closed pipes) after. */
    std::vector<std::vector<Link>> chains()
    {
        std::vector<std::vector<Link>> out;
        std::vector<uint8_t> seen(mRings.size(), 0);
        auto walk = [&](int start, int in) {
            std::vector<Link> chain;
            int cur = start;
            while (cur >= 0 && !seen[size_t(cur)])
            {
                seen[size_t(cur)] = 1;
                chain.push_back({cur, in});
                const int nxt = mRings[size_t(cur)].next[1 - in];
                if (nxt < 0)
                    break;
                in = mRings[size_t(nxt)].next[0] == cur ? 0 : 1;
                cur = nxt;
            }
            return chain;
        };
        for (size_t i = 0; i < mRings.size(); ++i)
        {
            const Ring& r = mRings[i];
            if (seen[i] || r.flat || (r.next[0] >= 0 && r.next[1] >= 0))
                continue;
            out.push_back(walk(int(i), r.next[0] >= 0 ? 1 : 0));
            mCycle.push_back(0);
        }
        for (size_t i = 0; i < mRings.size(); ++i)
            if (!seen[i] && !mRings[i].flat)
            {
                out.push_back(walk(int(i), 0));
                mCycle.push_back(1);
            }
        return out;
    }

    double loopCost(const Ring& r, int s) const
    {
        double c = 0.0;
        for (Index e : r.side[s])
            c += mCost[e];
        return c;
    }

    /** Column map from ring a (leaving through side out) to ring b (entering through side in). */
    std::vector<int> columnMap(const Ring& a, int out, const Ring& b, int in) const
    {
        std::unordered_map<Index, int> at;
        for (size_t k = 0; k < b.point[size_t(in)].size(); ++k)
            at[b.point[size_t(in)][k]] = int(k);
        std::vector<int> map(a.faces.size(), -1);
        for (size_t k = 0; k < a.faces.size(); ++k)
            if (auto it = at.find(a.point[size_t(out)][k]); it != at.end())
                map[k] = it->second;
        return map;
    }

    void layRun(std::vector<Link> chain)
    {
        if (chain.empty())
            return;
        const bool cycle = mCycle[mChainIndex++] != 0;
        if (!cycle)
            trimEnds(chain);
        if (chain.empty())
            return;
        const size_t n = chain.size();
        if (cycle)
        {
            // The cheapest loop opens the cycle.
            size_t best = 0;
            double bestCost = std::numeric_limits<double>::max();
            for (size_t i = 0; i < n; ++i)
                if (double c = loopCost(mRings[size_t(chain[i].ring)], chain[i].in); c < bestCost)
                    bestCost = c, best = i;
            std::rotate(chain.begin(), chain.begin() + std::ptrdiff_t(best), chain.end());
        }

        // Loops 0..n: loop i is ring i's entry side, loop n the last ring's exit side.
        std::vector<double> pos(n + 1, 0.0), cost(n + 1, 0.0);
        double circumference = 0.0;
        for (size_t i = 0; i <= n; ++i)
        {
            const Ring& r = mRings[size_t(chain[std::min(i, n - 1)].ring)];
            const int s = i < n ? chain[i].in : 1 - chain[n - 1].in;
            cost[i] = loopCost(r, s);
            circumference += r.loopLength[size_t(s)];
            if (i > 0)
            {
                const Ring& prev = mRings[size_t(chain[i - 1].ring)];
                pos[i] = pos[i - 1] + glm::distance(prev.center[size_t(chain[i - 1].in)], prev.center[size_t(1 - chain[i - 1].in)]);
            }
        }
        circumference /= double(n + 1);
        const double limit = mOpts.maxLength > 0.0 ? mOpts.maxLength
                           : mOpts.maxAspect > 0.0 ? mOpts.maxAspect * circumference
                                                   : std::numeric_limits<double>::infinity();
        std::vector<size_t> cuts = placeCuts(pos, cost, limit);
        mPlan.result.lengthCuts += cuts.size() - 2;

        for (size_t k : cuts)
        {
            const Ring& r = mRings[size_t(chain[std::min(k, n - 1)].ring)];
            for (Index e : r.side[size_t(k < n ? chain[k].in : 1 - chain[n - 1].in)])
                mPlan.rail[e] = 1;
        }

        std::vector<std::vector<int>> maps(n);
        for (size_t i = 0; i + 1 < n; ++i)
            maps[i] = columnMap(mRings[size_t(chain[i].ring)], 1 - chain[i].in, mRings[size_t(chain[i + 1].ring)], chain[i + 1].in);
        int carried = -1;
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
            carried = laySegment(chain, maps, cuts[k], cuts[k + 1], carried);
    }

    /** Rings at the open ends that close in, like the rings of a dome, belong to the cap. */
    void trimEnds(std::vector<Link>& chain) const
    {
        std::vector<double> radii;
        for (const Link& l : chain)
            radii.push_back(0.5 * (mRings[size_t(l.ring)].radius[0] + mRings[size_t(l.ring)].radius[1]));
        std::nth_element(radii.begin(), radii.begin() + std::ptrdiff_t(radii.size() / 2), radii.end());
        const double median = radii[radii.size() / 2];
        while (chain.size() > 1 && mRings[size_t(chain.front().ring)].radius[size_t(chain.front().in)] < 0.75 * median)
            chain.erase(chain.begin());
        while (chain.size() > 1 && mRings[size_t(chain.back().ring)].radius[size_t(1 - chain.back().in)] < 0.75 * median)
            chain.pop_back();
    }

    /**
     * Loops to cut, always the first and last: as few as keep every piece within the limit, then the least seen, with a
     * nudge towards even pieces where seeing is equal.
     */
    static std::vector<size_t> placeCuts(const std::vector<double>& pos, const std::vector<double>& cost, double limit)
    {
        const size_t n = pos.size() - 1;
        double mean = 0.0;
        for (double c : cost)
            mean += c;
        mean = std::max(mean / double(cost.size()), 1e-12);
        struct Best
        {
            size_t count = std::numeric_limits<size_t>::max();
            double cost = 0.0;
            size_t from = 0;
        };
        std::vector<Best> best(n + 1);
        best[0] = {0, 0.0, 0};
        for (size_t i = 1; i <= n; ++i)
        {
            const double here = i < n ? cost[i] / mean : 0.0;
            for (size_t j = i; j-- > 0;)
            {
                const double gap = pos[i] - pos[j];
                if (gap > limit && j + 1 < i)
                    break;
                if (best[j].count == std::numeric_limits<size_t>::max())
                    continue;
                const double even = std::isfinite(limit) ? 0.5 * (gap / limit) * (gap / limit) : 0.0;
                Best c{best[j].count + (i < n ? 1 : 0), best[j].cost + here + even, j};
                if (c.count < best[i].count || (c.count == best[i].count && c.cost < best[i].cost))
                    best[i] = c;
            }
        }
        std::vector<size_t> cuts{n};
        for (size_t i = n; i > 0; i = best[i].from)
            cuts.push_back(best[i].from);
        std::reverse(cuts.begin(), cuts.end());
        return cuts;
    }

    /** Rings [a, b) become one strip, cut along its least seen column. Returns the column the rail leaves on. */
    int laySegment(const std::vector<Link>& chain, const std::vector<std::vector<int>>& maps, size_t a, size_t b, int carried)
    {
        const Ring& first = mRings[size_t(chain[a].ring)];
        const int cols = int(first.faces.size());
        auto railOf = [&](int col, std::vector<Index>* edges) {
            double c = 0.0;
            for (size_t i = a; i < b; ++i)
            {
                if (col < 0)
                    return std::pair{std::numeric_limits<double>::infinity(), -1};
                const Index e = mRings[size_t(chain[i].ring)].cross[size_t(col)];
                c += mCost[e];
                if (edges)
                    edges->push_back(e);
                col = i + 1 < chain.size() ? maps[i][size_t(col)] : -1;
            }
            return std::pair{c, col};
        };
        int bestCol = 0;
        double bestCost = std::numeric_limits<double>::infinity();
        for (int col = 0; col < cols; ++col)
        {
            double c = railOf(col, nullptr).first;
            // Lining up with the previous strip's rail reads as one seam when the costs are close.
            if (col == carried)
                c *= 0.97;
            if (c < bestCost)
                bestCost = c, bestCol = col;
        }
        std::vector<Index> rail;
        const int leaving = railOf(bestCol, &rail).second;
        for (Index e : rail)
            mPlan.rail[e] = 1;
        const int id = int(mPlan.kind.size());
        mPlan.kind.push_back(Kind::Strip);
        for (size_t i = a; i < b; ++i)
            for (Index f : mRings[size_t(chain[i].ring)].faces)
                mPlan.island[f] = id;
        ++mPlan.result.strips;
        return leaving;
    }

    /** Faces left over, in connected regions: flat ones become caps, the rest get auto seams. */
    void groupCaps()
    {
        std::vector<Index> stack;
        for (Index f : mFaces)
        {
            if (mPlan.island[f] != -1)
                continue;
            const int id = int(mPlan.kind.size());
            std::vector<Index> region;
            mPlan.island[f] = id;
            stack.push_back(f);
            while (!stack.empty())
            {
                const Index g = stack.back();
                stack.pop_back();
                region.push_back(g);
                mMesh.forEachFaceCorner(g, [&](Index c) {
                    const Index o = mMesh.cornerRadial(c);
                    if (o == c || mMesh.cornerRadial(o) != c)
                        return;
                    const Index h = mMesh.cornerFace(o);
                    if (mInSet[h] && mPlan.island[h] == -1)
                    {
                        mPlan.island[h] = id;
                        stack.push_back(h);
                    }
                });
            }
            glm::dvec3 n(0.0);
            for (Index g : region)
                n += mMesh.faceAreaVector(g);
            bool flat = glm::length(n) > 1e-12;
            if (flat)
            {
                n = glm::normalize(n);
                for (Index g : region)
                    flat &= glm::dot(mMesh.faceNormal(g), n) > 0.2;
            }
            mPlan.kind.push_back(flat ? Kind::Cap : Kind::Other);
            if (flat)
                ++mPlan.result.caps;
            else
                mPlan.result.otherFaces += region.size();
        }
    }

    size_t mChainIndex = 0;
};

void writeHoseSeams(geo::Mesh& m, std::span<const Index> faces, const Plan& plan, bool keep)
{
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (Index f : faces)
        m.forEachFaceCorner(f, [&](Index c) {
            const Index e = m.cornerEdge(c);
            const Index o = m.cornerRadial(c);
            bool cut = plan.rail[e] != 0 || o == c || m.cornerRadial(o) != c;
            if (!cut)
            {
                const int other = plan.island[m.cornerFace(o)];
                cut = other != plan.island[f];
            }
            seam[e] = cut || (keep && seam[e]);
        });
}

std::vector<Index> facesOf(const Plan& plan, std::span<const Index> faces, Kind kind)
{
    std::vector<Index> out;
    for (Index f : faces)
        if (plan.kind[size_t(plan.island[f])] == kind)
            out.push_back(f);
    return out;
}

Plan plan(geo::Mesh& mesh, std::span<const Index> faceIds, const HoseOptions& opts, std::vector<Index>& faces)
{
    faces = liveFaces(mesh, faceIds);
    Plan p = Analysis(mesh, faces, opts).run();
    writeHoseSeams(mesh, faces, p, opts.keepSeams);
    if (std::vector<Index> other = facesOf(p, faces, Kind::Other); !other.empty())
    {
        AutoSeamOptions ao;
        ao.views = opts.views;
        ao.visibility = opts.visibility;
        ao.keepSeams = true;
        autoSeams(mesh, other, ao);
        log::warn("hose: {} faces are not tubes or flat ends; they got auto seams and a plain unwrap", other.size());
    }
    return p;
}
} // namespace

HoseResult hoseSeams(geo::Mesh& mesh, std::span<const Index> faceIds, const HoseOptions& opts)
{
    LILA_ZONE();
    std::vector<Index> faces;
    return plan(mesh, faceIds, opts, faces).result;
}

HoseResult hoseUv(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const HoseOptions& opts)
{
    LILA_ZONE();
    std::vector<Index> faces;
    Plan p = plan(mesh, faceIds, opts, faces);
    HoseResult result = p.result;

    ChartOptions co;
    co.uvSet = uvSet;
    // buildCharts takes an empty list as every face.
    auto charts = [&](Kind kind) {
        std::vector<Index> f = facesOf(p, faces, kind);
        return f.empty() ? std::vector<Chart>{} : buildCharts(mesh, f, co);
    };
    std::vector<Chart> strips = charts(Kind::Strip);
    std::vector<uint8_t> straight(strips.size(), 0);
    tbb::parallel_for(size_t(0), strips.size(), [&](size_t i) {
        Chart& ch = strips[i];
        // No usable uvs, so straighten keeps 3D scale like the caps.
        std::fill(ch.uv.begin(), ch.uv.end(), glm::dvec2(0.0));
        straight[i] = straightenChart(ch) != StraightenResult::Failed;
        if (!straight[i])
            unwrapChart(ch);
    });
    result.unstraightened = size_t(std::count(straight.begin(), straight.end(), 0));
    writeCharts(mesh, strips, uvSet);

    std::vector<Chart> caps = charts(Kind::Cap);
    for (Chart& ch : caps)
    {
        glm::dvec3 n(0.0);
        for (Index f : ch.faces)
            n += mesh.faceAreaVector(f);
        glm::dvec3 u, v;
        planeFrame(glm::normalize(n), u, v);
        for (Index i = 0; i < ch.vertexCount(); ++i)
            ch.uv[i] = {glm::dot(ch.pos[i], u), glm::dot(ch.pos[i], v)};
    }
    writeCharts(mesh, caps, uvSet);

    std::vector<Chart> other = charts(Kind::Other);
    unwrapCharts(other);
    writeCharts(mesh, other, uvSet);
    return result;
}
} // namespace lila::uv
