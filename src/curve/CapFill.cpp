#include "curve/CapFill.h"
#include "curve/Cdt.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Skeleton.h"
#include "geo/Triangulate.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace lila::curve
{
namespace
{
double orient(const D2& a, const D2& b, const D2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

/** Positive when d lies inside the circle through the counter-clockwise a, b, c. */
double incircle(const D2& a, const D2& b, const D2& c, const D2& d)
{
    double adx = a.x - d.x, ady = a.y - d.y, bdx = b.x - d.x, bdy = b.y - d.y, cdx = c.x - d.x, cdy = c.y - d.y;
    double ad = adx * adx + ady * ady, bd = bdx * bdx + bdy * bdy, cd = cdx * cdx + cdy * cdy;
    return adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) + ad * (bdx * cdy - bdy * cdx);
}

uint64_t edgeKey(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

/** Constrained Delaunay triangulation in the unit square: Bowyer-Watson insertion, then constraint edges recovered. */
class Cdt
{
public:
    struct Tri
    {
        std::array<int, 3> v{};
        std::array<int, 3> n{-1, -1, -1}; // n[i] lies across the edge opposite v[i]
        bool dead = false;
    };

    std::vector<D2> pts;
    std::vector<Tri> tris;
    std::unordered_set<uint64_t> constrained;
    int real = 0;

    explicit Cdt(std::vector<D2> points) : pts(std::move(points))
    {
        real = int(pts.size());
        pts.push_back(D2(-100.0, -100.0));
        pts.push_back(D2(101.0, -100.0));
        pts.push_back(D2(0.5, 100.0));
        tris.push_back({{real, real + 1, real + 2}, {-1, -1, -1}, false});
    }

    void insert(int p)
    {
        int t0 = locate(pts[size_t(p)]);
        if (t0 < 0)
            return;
        std::vector<int> cav = {t0};
        std::vector<uint8_t>& in = mMark;
        in.resize(tris.size(), 0);
        in[size_t(t0)] = 1;
        for (size_t k = 0; k < cav.size(); ++k)
        {
            const Tri& t = tris[size_t(cav[k])];
            for (int i = 0; i < 3; ++i)
            {
                int nb = t.n[size_t(i)];
                if (nb < 0 || in[size_t(nb)])
                    continue;
                const Tri& u = tris[size_t(nb)];
                if (incircle(pts[size_t(u.v[0])], pts[size_t(u.v[1])], pts[size_t(u.v[2])], pts[size_t(p)]) > 0.0)
                {
                    in[size_t(nb)] = 1;
                    cav.push_back(nb);
                }
            }
        }
        struct Bnd
        {
            int a, b, outside;
        };
        std::vector<Bnd> bnd;
        for (int c : cav)
        {
            const Tri& t = tris[size_t(c)];
            for (int i = 0; i < 3; ++i)
            {
                int nb = t.n[size_t(i)];
                if (nb >= 0 && in[size_t(nb)])
                    continue;
                bnd.push_back({t.v[size_t((i + 1) % 3)], t.v[size_t((i + 2) % 3)], nb});
            }
        }
        for (int c : cav)
        {
            tris[size_t(c)].dead = true;
            in[size_t(c)] = 0;
        }
        std::unordered_map<int, int> byStart, byEnd;
        size_t first = tris.size();
        for (const Bnd& e : bnd)
        {
            int id = int(tris.size());
            Tri t;
            t.v = {e.a, e.b, p};
            t.n[2] = e.outside;
            tris.push_back(t);
            touch(id);
            byStart[e.a] = id;
            byEnd[e.b] = id;
            if (e.outside >= 0)
            {
                Tri& o = tris[size_t(e.outside)];
                for (int k = 0; k < 3; ++k)
                    if (o.v[size_t((k + 1) % 3)] == e.b && o.v[size_t((k + 2) % 3)] == e.a)
                        o.n[size_t(k)] = id;
            }
        }
        for (size_t id = first; id < tris.size(); ++id)
        {
            Tri& t = tris[id];
            auto s = byStart.find(t.v[1]);
            auto e = byEnd.find(t.v[0]);
            t.n[0] = s != byStart.end() ? s->second : -1;
            t.n[1] = e != byEnd.end() ? e->second : -1;
        }
        mLast = int(tris.size()) - 1;
    }

    bool hasEdge(int a, int b) const
    {
        for (int t : around(a))
            for (int v : tris[size_t(t)].v)
                if (v == b)
                    return true;
        return false;
    }

    /** Forces edge a-b into the triangulation; false when it could not. */
    bool recover(int a, int b, int depth = 0)
    {
        if (a == b || depth > 64)
            return a == b;
        if (hasEdge(a, b))
        {
            constrained.insert(edgeKey(a, b));
            return true;
        }
        const D2 A = pts[size_t(a)], B = pts[size_t(b)];
        auto between = [&](int c) {
            D2 d = B - A;
            double s = glm::dot(pts[size_t(c)] - A, d) / glm::dot(d, d);
            return s > 1e-12 && s < 1.0 - 1e-12;
        };
        auto onLine = [&](int c) { return std::abs(orient(A, B, pts[size_t(c)])) <= 1e-14 * glm::length(B - A); };
        // The triangle around a the segment leaves through.
        int cur = -1, right = -1, left = -1;
        for (int t : around(a))
        {
            if (cur >= 0)
                break;
            for (int i = 0; i < 3; ++i)
            {
                if (tris[t].v[size_t(i)] != a)
                    continue;
                int c = tris[t].v[size_t((i + 1) % 3)], d = tris[t].v[size_t((i + 2) % 3)];
                if (onLine(c) && between(c))
                    return recover(a, c, depth + 1) && recover(c, b, depth + 1);
                if (onLine(d) && between(d))
                    return recover(a, d, depth + 1) && recover(d, b, depth + 1);
                if (orient(A, B, pts[size_t(c)]) < 0.0 && orient(A, B, pts[size_t(d)]) > 0.0)
                {
                    cur = t;
                    right = c;
                    left = d;
                }
            }
        }
        if (cur < 0)
            return false;
        std::vector<int> removed = {cur};
        std::vector<int> leftChain = {a, left}, rightChain = {a, right};
        for (size_t guard = 0; guard < tris.size(); ++guard)
        {
            const Tri& t = tris[size_t(cur)];
            int nb = -1;
            for (int i = 0; i < 3; ++i)
            {
                int u = t.v[size_t((i + 1) % 3)], w = t.v[size_t((i + 2) % 3)];
                if ((u == right && w == left) || (u == left && w == right))
                    nb = t.n[size_t(i)];
            }
            if (nb < 0)
                return false;
            const Tri& n = tris[size_t(nb)];
            int e = -1;
            for (int k = 0; k < 3; ++k)
                if (n.v[size_t(k)] != right && n.v[size_t(k)] != left)
                    e = n.v[size_t(k)];
            removed.push_back(nb);
            cur = nb;
            if (e == b)
                break;
            if (onLine(e))
                return between(e) && recover(a, e, depth + 1) && recover(e, b, depth + 1);
            if (orient(A, B, pts[size_t(e)]) < 0.0)
            {
                right = e;
                rightChain.push_back(e);
            }
            else
            {
                left = e;
                leftChain.push_back(e);
            }
        }
        leftChain.push_back(b);
        rightChain.push_back(b);
        for (int r : removed)
            tris[size_t(r)].dead = true;
        triangulatePseudo(leftChain);
        triangulatePseudo(rightChain);
        rebuildAdjacency();
        constrained.insert(edgeKey(a, b));
        return true;
    }

    void rebuildAdjacency()
    {
        std::vector<Tri> alive;
        for (const Tri& t : tris)
            if (!t.dead)
                alive.push_back({t.v, {-1, -1, -1}, false});
        tris = std::move(alive);
        std::unordered_map<uint64_t, std::pair<int, int>> half;
        auto dk = [](int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
        for (size_t t = 0; t < tris.size(); ++t)
            for (int i = 0; i < 3; ++i)
                half[dk(tris[t].v[size_t((i + 1) % 3)], tris[t].v[size_t((i + 2) % 3)])] = {int(t), i};
        for (size_t t = 0; t < tris.size(); ++t)
            for (int i = 0; i < 3; ++i)
            {
                auto it = half.find(dk(tris[t].v[size_t((i + 2) % 3)], tris[t].v[size_t((i + 1) % 3)]));
                tris[t].n[size_t(i)] = it != half.end() ? it->second.first : -1;
            }
        mVertTri.assign(pts.size(), -1);
        for (size_t t = 0; t < tris.size(); ++t)
            touch(int(t));
        mMark.assign(tris.size(), 0);
        mLast = 0;
    }

private:
    std::vector<uint8_t> mMark;
    std::vector<int> mVertTri; // per point, a live triangle using it
    int mLast = 0;

    void touch(int t)
    {
        if (mVertTri.size() < pts.size())
            mVertTri.resize(pts.size(), -1);
        for (int v : tris[size_t(t)].v)
            mVertTri[size_t(v)] = t;
    }

    /** The live triangles using point a, walked across their shared edges. */
    std::vector<int> around(int a) const
    {
        std::vector<int> out;
        int t0 = size_t(a) < mVertTri.size() ? mVertTri[size_t(a)] : -1;
        if (t0 < 0 || tris[size_t(t0)].dead)
            for (size_t t = 0; t < tris.size() && t0 < 0; ++t)
                if (!tris[t].dead && (tris[t].v[0] == a || tris[t].v[1] == a || tris[t].v[2] == a))
                    t0 = int(t);
        if (t0 < 0)
            return out;
        out.push_back(t0);
        for (size_t k = 0; k < out.size(); ++k)
        {
            const Tri& t = tris[size_t(out[k])];
            for (int i = 0; i < 3; ++i)
            {
                int nb = t.n[size_t(i)];
                if (t.v[size_t(i)] == a || nb < 0 || tris[size_t(nb)].dead || std::find(out.begin(), out.end(), nb) != out.end())
                    continue;
                const auto& v = tris[size_t(nb)].v;
                if (v[0] == a || v[1] == a || v[2] == a)
                    out.push_back(nb);
            }
        }
        return out;
    }

    int locate(const D2& p)
    {
        int t = mLast;
        if (t < 0 || size_t(t) >= tris.size() || tris[size_t(t)].dead)
            for (t = int(tris.size()) - 1; t >= 0 && tris[size_t(t)].dead; --t)
                ;
        int rot = 0;
        for (size_t guard = 0; t >= 0 && guard < tris.size() + 64; ++guard)
        {
            const Tri& tr = tris[size_t(t)];
            bool moved = false;
            for (int k = 0; k < 3; ++k)
            {
                int i = (k + rot) % 3;
                const D2& a = pts[size_t(tr.v[size_t((i + 1) % 3)])];
                const D2& b = pts[size_t(tr.v[size_t((i + 2) % 3)])];
                if (orient(a, b, p) < 0.0 && tr.n[size_t(i)] >= 0)
                {
                    t = tr.n[size_t(i)];
                    moved = true;
                    break;
                }
            }
            rot = (rot + 1) % 3;
            if (!moved)
                return t;
        }
        for (size_t k = 0; k < tris.size(); ++k)
        {
            const Tri& tr = tris[k];
            if (tr.dead)
                continue;
            const D2 &a = pts[size_t(tr.v[0])], &b = pts[size_t(tr.v[1])], &c = pts[size_t(tr.v[2])];
            if (orient(a, b, p) >= -1e-15 && orient(b, c, p) >= -1e-15 && orient(c, a, p) >= -1e-15)
                return int(k);
        }
        return -1;
    }

    void addTri(int a, int b, int c)
    {
        if (orient(pts[size_t(a)], pts[size_t(b)], pts[size_t(c)]) < 0.0)
            std::swap(b, c);
        tris.push_back({{a, b, c}, {-1, -1, -1}, false});
        touch(int(tris.size()) - 1);
    }

    /** Delaunay triangles over a chain a, p1 .. pk, b lying to one side of a-b (Anglada 1997). */
    void triangulatePseudo(const std::vector<int>& chain)
    {
        if (chain.size() < 3)
            return;
        int a = chain.front(), b = chain.back();
        size_t ci = 1;
        for (size_t k = 2; k + 1 < chain.size(); ++k)
        {
            int c = chain[ci];
            D2 pa = pts[size_t(a)], pb = pts[size_t(b)], pc = pts[size_t(c)];
            if (orient(pa, pb, pc) < 0.0)
                std::swap(pa, pb);
            if (incircle(pa, pb, pc, pts[size_t(chain[k])]) > 0.0)
                ci = k;
        }
        addTri(a, chain[ci], b);
        triangulatePseudo(std::vector<int>(chain.begin(), chain.begin() + std::ptrdiff_t(ci) + 1));
        triangulatePseudo(std::vector<int>(chain.begin() + std::ptrdiff_t(ci), chain.end()));
    }
};

double cornerSkew(const D2& prev, const D2& p, const D2& next)
{
    D2 u = prev - p, v = next - p;
    double a = std::atan2(std::abs(u.x * v.y - u.y * v.x), glm::dot(u, v));
    return std::abs(glm::degrees(a) - 90.0);
}

bool convexCcw(const std::vector<D2>& q)
{
    for (size_t i = 0; i < q.size(); ++i)
        if (orient(q[i], q[(i + 1) % q.size()], q[(i + 2) % q.size()]) <= 0.0)
            return false;
    return true;
}

double polygonArea(const std::vector<int>& poly, const std::function<D2(int)>& at)
{
    double a = 0.0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        D2 p = at(poly[i]), q = at(poly[(i + 1) % poly.size()]);
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5;
}

double quadSkew(const std::vector<D2>& q)
{
    double s = 0.0;
    for (size_t c = 0; c < 4; ++c)
        s = std::max(s, cornerSkew(q[(c + 3) % 4], q[c], q[(c + 1) % 4]));
    return s;
}

using QuadSplit = std::pair<double, std::vector<std::vector<int>>>;

/** The best split of the polygon's corners left in mask: worst skew, the first quad's corners and what is left after it. */
struct MaskSplit
{
    double skew = 1e300;
    std::array<int, 4> quad{};
    uint64_t rest = 0;
};

const MaskSplit& splitMasked(const std::vector<D2>& p, uint64_t mask, double maxSkew, std::unordered_map<uint64_t, MaskSplit>& memo)
{
    if (auto it = memo.find(mask); it != memo.end())
        return it->second;
    std::vector<int> idx;
    for (int i = 0; i < int(p.size()); ++i)
        if (mask >> i & 1)
            idx.push_back(i);
    MaskSplit best;
    size_t n = idx.size();
    for (size_t i = 0; n >= 4 && n % 2 == 0 && i < (n == 4 ? 1 : n); ++i)
    {
        std::array<int, 4> quad = {idx[i], idx[(i + 1) % n], idx[(i + 2) % n], idx[(i + 3) % n]};
        std::vector<D2> q = {p[size_t(quad[0])], p[size_t(quad[1])], p[size_t(quad[2])], p[size_t(quad[3])]};
        double s = quadSkew(q);
        if (!convexCcw(q) || s > maxSkew || s >= best.skew)
            continue;
        uint64_t rest = n == 4 ? 0 : mask & ~(uint64_t(1) << quad[1] | uint64_t(1) << quad[2]);
        double worst = rest ? std::max(s, splitMasked(p, rest, maxSkew, memo).skew) : s;
        if (worst < best.skew)
            best = {worst, quad, rest};
    }
    return memo[mask] = best;
}

/** Best split of an even polygon into convex quads (worst corner skew, quads), or none. */
QuadSplit splitIntoQuads(const std::vector<int>& poly, const std::function<D2(int)>& at, double maxSkew)
{
    QuadSplit out{1e300, {}};
    if (poly.size() < 4 || poly.size() % 2 || poly.size() > 64)
        return out;
    std::vector<D2> p;
    for (int v : poly)
        p.push_back(at(v));
    // Removing quads in different orders leaves the same corners again and again: each remainder is solved once.
    std::unordered_map<uint64_t, MaskSplit> memo;
    uint64_t all = poly.size() == 64 ? ~uint64_t(0) : (uint64_t(1) << poly.size()) - 1;
    const MaskSplit& top = splitMasked(p, all, maxSkew, memo);
    if (top.skew > maxSkew)
        return out;
    out.first = top.skew;
    for (uint64_t mask = all; mask;)
    {
        const MaskSplit& m = memo.at(mask);
        out.second.push_back({poly[size_t(m.quad[0])], poly[size_t(m.quad[1])], poly[size_t(m.quad[2])], poly[size_t(m.quad[3])]});
        mask = m.rest;
    }
    return out;
}

/** The boundary loop of a patch of faces, or empty when it is not one simple loop. */
std::vector<int> patchBoundary(const std::vector<const std::vector<int>*>& faces)
{
    std::map<std::pair<int, int>, int> half;
    for (const auto* f : faces)
        for (size_t i = 0; i < f->size(); ++i)
            ++half[{(*f)[i], (*f)[(i + 1) % f->size()]}];
    std::map<int, int> next;
    for (auto& [e, count] : half)
        if (!half.count({e.second, e.first}))
        {
            if (next.count(e.first))
                return {};
            next[e.first] = e.second;
        }
    if (next.empty())
        return {};
    std::vector<int> loop = {next.begin()->first};
    while (loop.size() <= next.size())
    {
        auto it = next.find(loop.back());
        if (it == next.end())
            return {};
        if (it->second == loop.front())
            break;
        loop.push_back(it->second);
    }
    return loop.size() == next.size() ? loop : std::vector<int>{};
}

/**
 * Pairs up leftover triangles joined through a few quads: the strip they make has an even outline that splits into
 * quads only, removing both triangles.
 */
void pairTriangles(std::vector<std::vector<int>>& faces, const std::function<D2(int)>& at, double maxSkew)
{
    for (int pass = 0; pass < 8; ++pass)
    {
        std::map<std::pair<int, int>, std::vector<int>> byEdge;
        for (size_t f = 0; f < faces.size(); ++f)
            for (size_t i = 0; i < faces[f].size(); ++i)
            {
                int a = faces[f][i], b = faces[f][(i + 1) % faces[f].size()];
                byEdge[{std::min(a, b), std::max(a, b)}].push_back(int(f));
            }
        auto neighbours = [&](int f) {
            std::vector<int> out;
            const auto& v = faces[size_t(f)];
            for (size_t i = 0; i < v.size(); ++i)
            {
                int a = v[i], b = v[(i + 1) % v.size()];
                for (int g : byEdge[{std::min(a, b), std::max(a, b)}])
                    if (g != f)
                        out.push_back(g);
            }
            return out;
        };
        std::vector<uint8_t> gone(faces.size(), 0);
        std::vector<std::vector<int>> added;
        bool changed = false;
        for (size_t t = 0; t < faces.size(); ++t)
        {
            if (gone[t] || faces[t].size() != 3)
                continue;
            // Paths from t through up to five quads to another triangle.
            std::vector<std::vector<int>> paths = {{int(t)}};
            bool done = false;
            for (int depth = 0; depth < 6 && !done; ++depth)
            {
                std::vector<std::vector<int>> grown;
                for (size_t pi = 0; pi < paths.size() && !done; ++pi)
                    for (int g : neighbours(paths[pi].back()))
                    {
                        const std::vector<int>& path = paths[pi];
                        if (gone[size_t(g)] || std::find(path.begin(), path.end(), g) != path.end())
                            continue;
                        std::vector<int> p = path;
                        p.push_back(g);
                        if (faces[size_t(g)].size() == 4)
                        {
                            grown.push_back(p);
                            continue;
                        }
                        if (faces[size_t(g)].size() != 3 || p.size() < 3)
                            continue;
                        std::vector<const std::vector<int>*> patch;
                        for (int f : p)
                            patch.push_back(&faces[size_t(f)]);
                        std::vector<int> loop = patchBoundary(patch);
                        if (loop.size() != 2 * (p.size() - 2) + 4)
                            continue;
                        // A strip folding back on itself would replace faces it doesn't cover.
                        double patchArea = 0.0;
                        for (const auto* f : patch)
                            patchArea += polygonArea(*f, at);
                        if (std::abs(patchArea - polygonArea(loop, at)) > 1e-9 * std::max(1.0, std::abs(patchArea)))
                            continue;
                        auto split = splitIntoQuads(loop, at, maxSkew);
                        if (split.second.empty())
                            continue;
                        for (int f : p)
                            gone[size_t(f)] = 1;
                        for (auto& q : split.second)
                            added.push_back(std::move(q));
                        done = changed = true;
                        break;
                    }
                paths = std::move(grown);
                if (done)
                    break;
            }
        }
        if (!changed)
            return;
        std::vector<std::vector<int>> kept;
        for (size_t f = 0; f < faces.size(); ++f)
            if (!gone[f])
                kept.push_back(std::move(faces[f]));
        for (auto& f : added)
            kept.push_back(std::move(f));
        faces = std::move(kept);
    }
}
} // namespace

/** Grid, triangulation and quad pairing over the whole region. */
static CapFillResult fillCore(const std::vector<std::vector<glm::dvec2>>& loops, const CapFillOptions& o)
{
    LILA_ZONE();

    CapFillResult res;
    std::vector<std::array<int, 2>> segs;
    for (const auto& l : loops)
    {
        if (l.size() < 3)
            continue;
        int base = int(res.points.size());
        for (size_t i = 0; i < l.size(); ++i)
        {
            res.points.push_back(l[i]);
            segs.push_back({base + int(i), base + int((i + 1) % l.size())});
        }
    }
    size_t nb = res.points.size();
    if (nb < 3)
    {
        res.error = "nothing to fill";
        return res;
    }
    D2 lo(1e300), hi(-1e300);
    for (const D2& p : res.points)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    double extent = std::max(hi.x - lo.x, hi.y - lo.y);
    if (!(extent > 0.0))
    {
        res.error = "the region has no size";
        return res;
    }
    std::vector<double> lens;
    for (auto [a, b] : segs)
        lens.push_back(glm::length(res.points[size_t(b)] - res.points[size_t(a)]));
    std::nth_element(lens.begin(), lens.begin() + std::ptrdiff_t(lens.size() / 2), lens.end());
    double h = o.spacing > 0.0 ? o.spacing : lens[lens.size() / 2];
    h = std::max(h, extent * 1e-4);

    // Grid points inside, kept off the boundary: rows are scanned against every edge for in and out.
    std::vector<D2> grid;
    {
        D2 start = lo + D2(h * 0.5);
        int nx = int((hi.x - start.x) / h) + 1, ny = int((hi.y - start.y) / h) + 1;
        if (double(nx) * double(ny) > 4e6)
        {
            res.error = "the fill spacing is too fine for the region";
            return res;
        }
        double margin = o.margin * h;
        double cell = std::max(h, margin);
        std::unordered_map<int64_t, std::vector<int>> buckets;
        auto bkey = [](int64_t i, int64_t j) { return (i << 32) ^ (j & 0xffffffff); };
        for (size_t s = 0; s < segs.size(); ++s)
        {
            D2 a = res.points[size_t(segs[s][0])], b = res.points[size_t(segs[s][1])];
            D2 mn = glm::min(a, b) - D2(margin) - lo, mx = glm::max(a, b) + D2(margin) - lo;
            for (int64_t i = int64_t(std::floor(mn.x / cell)); i <= int64_t(std::floor(mx.x / cell)); ++i)
                for (int64_t j = int64_t(std::floor(mn.y / cell)); j <= int64_t(std::floor(mx.y / cell)); ++j)
                    buckets[bkey(i, j)].push_back(int(s));
        }
        for (int j = 0; j < ny; ++j)
        {
            double y = start.y + j * h;
            std::vector<double> xs;
            for (auto [a, b] : segs)
            {
                const D2& p = res.points[size_t(a)];
                const D2& q = res.points[size_t(b)];
                if ((p.y > y) != (q.y > y))
                    xs.push_back(p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y));
            }
            std::sort(xs.begin(), xs.end());
            for (int i = 0; i < nx; ++i)
            {
                double x = start.x + i * h;
                size_t crossings = size_t(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin());
                if (crossings % 2 == 0)
                    continue;
                D2 p(x, y);
                bool clear = true;
                auto it = buckets.find(bkey(int64_t(std::floor((x - lo.x) / cell)), int64_t(std::floor((y - lo.y) / cell))));
                if (it != buckets.end())
                    for (int s : it->second)
                    {
                        D2 a = res.points[size_t(segs[size_t(s)][0])], b = res.points[size_t(segs[size_t(s)][1])];
                        D2 d = b - a;
                        double l2 = glm::dot(d, d);
                        double t = l2 > 0.0 ? std::clamp(glm::dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
                        if (glm::length(a + d * t - p) < margin)
                        {
                            clear = false;
                            break;
                        }
                    }
                if (clear)
                    grid.push_back(p);
            }
        }
    }

    // Triangulate in the unit square; repeated boundary points share one vertex.
    std::vector<D2> unit;
    std::vector<int> uid(nb + grid.size());
    std::map<std::pair<double, double>, int> seen;
    auto toUnit = [&](const D2& p) { return (p - lo) / extent; };
    for (size_t i = 0; i < nb + grid.size(); ++i)
    {
        D2 p = i < nb ? res.points[i] : grid[i - nb];
        auto [it, fresh] = seen.try_emplace({p.x, p.y}, int(unit.size()));
        if (fresh)
            unit.push_back(toUnit(p));
        uid[i] = it->second;
    }
    std::vector<int> firstOf(unit.size(), -1);
    for (size_t i = 0; i < uid.size(); ++i)
        if (firstOf[size_t(uid[i])] < 0)
            firstOf[size_t(uid[i])] = int(i);
    Cdt cdt(unit);
    for (int i = 0; i < int(unit.size()); ++i)
        cdt.insert(i);
    cdt.rebuildAdjacency();
    for (auto [a, b] : segs)
        if (!cdt.recover(uid[size_t(a)], uid[size_t(b)]))
        {
            res.error = "could not keep a boundary edge in the fill";
            return res;
        }

    // Inside is an odd number of boundary crossings in from the outer triangle's corners.
    size_t T = cdt.tris.size();
    std::vector<int> depth(T, -1);
    std::vector<size_t> queue;
    for (size_t t = 0; t < T; ++t)
        for (int v : cdt.tris[t].v)
            if (v >= cdt.real && depth[t] < 0)
            {
                depth[t] = 0;
                queue.push_back(t);
            }
    for (size_t k = 0; k < queue.size(); ++k)
    {
        size_t t = queue[k];
        const auto& tr = cdt.tris[t];
        for (int i = 0; i < 3; ++i)
        {
            int n = tr.n[size_t(i)];
            if (n < 0 || depth[size_t(n)] >= 0)
                continue;
            bool wall = cdt.constrained.count(edgeKey(tr.v[size_t((i + 1) % 3)], tr.v[size_t((i + 2) % 3)])) > 0;
            depth[size_t(n)] = depth[t] + (wall ? 1 : 0);
            queue.push_back(size_t(n));
        }
    }
    std::vector<int> inside;
    for (size_t t = 0; t < T; ++t)
        if (depth[t] % 2 == 1)
            inside.push_back(int(t));

    for (const D2& g : grid)
        res.points.push_back(g);
    auto P = [&](int u) { return res.points[size_t(firstOf[size_t(u)])]; };

    // Pair triangles into quads, best shaped first.
    std::unordered_map<int, int> slot;
    for (size_t k = 0; k < inside.size(); ++k)
        slot[inside[k]] = int(k);
    struct Pair
    {
        double skew;
        int a, b;
        std::array<int, 4> q;
    };
    std::vector<Pair> pairs;
    if (o.quads)
        for (size_t k = 0; k < inside.size(); ++k)
        {
            const auto& tr = cdt.tris[size_t(inside[k])];
            for (int i = 0; i < 3; ++i)
            {
                int n = tr.n[size_t(i)];
                auto it = slot.find(n);
                if (it == slot.end() || it->second <= int(k))
                    continue;
                int u = tr.v[size_t((i + 1) % 3)], w = tr.v[size_t((i + 2) % 3)];
                if (cdt.constrained.count(edgeKey(u, w)))
                    continue;
                const auto& tn = cdt.tris[size_t(n)];
                int d = -1;
                for (int v : tn.v)
                    if (v != u && v != w)
                        d = v;
                std::array<int, 4> q = {tr.v[size_t(i)], u, d, w};
                std::vector<D2> qp = {P(q[0]), P(q[1]), P(q[2]), P(q[3])};
                if (!convexCcw(qp))
                    continue;
                double skew = 0.0;
                for (int c = 0; c < 4; ++c)
                    skew = std::max(skew, cornerSkew(qp[size_t((c + 3) % 4)], qp[size_t(c)], qp[size_t((c + 1) % 4)]));
                if (skew <= o.maxQuadSkew)
                    pairs.push_back({skew, int(k), it->second, q});
            }
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.skew < y.skew; });
    std::vector<uint8_t> used(inside.size(), 0);
    std::vector<std::vector<int>> faces;
    for (const Pair& p : pairs)
    {
        if (used[size_t(p.a)] || used[size_t(p.b)])
            continue;
        used[size_t(p.a)] = used[size_t(p.b)] = 1;
        faces.push_back({p.q[0], p.q[1], p.q[2], p.q[3]});
    }
    for (size_t k = 0; k < inside.size(); ++k)
        if (!used[k])
        {
            const auto& v = cdt.tris[size_t(inside[k])].v;
            faces.push_back({v[0], v[1], v[2]});
        }
    if (o.quads)
        pairTriangles(faces, [&](int u) { return P(u); }, o.maxQuadSkew + 10.0);

    // Relax the interior toward its neighbours, never folding a face.
    size_t nu = unit.size();
    std::vector<uint8_t> fixed(nu, 0);
    for (size_t i = 0; i < nb; ++i)
        fixed[size_t(uid[i])] = 1;
    std::vector<D2> pos(nu);
    for (size_t u = 0; u < nu; ++u)
        pos[u] = P(int(u));
    std::vector<std::vector<int>> nbrs(nu), vfaces(nu);
    for (size_t f = 0; f < faces.size(); ++f)
        for (size_t c = 0; c < faces[f].size(); ++c)
        {
            int a = faces[f][c], b = faces[f][(c + 1) % faces[f].size()];
            nbrs[size_t(a)].push_back(b);
            nbrs[size_t(b)].push_back(a);
            vfaces[size_t(a)].push_back(int(f));
        }
    auto faceOk = [&](int f) {
        std::vector<D2> q;
        for (int v : faces[size_t(f)])
            q.push_back(pos[size_t(v)]);
        return q.size() == 3 ? orient(q[0], q[1], q[2]) > 0.0 : convexCcw(q);
    };
    for (int it = 0; it < o.relaxIterations; ++it)
        for (size_t u = 0; u < nu; ++u)
        {
            if (fixed[u] || nbrs[u].empty())
                continue;
            D2 avg(0.0);
            for (int v : nbrs[u])
                avg += pos[size_t(v)];
            avg /= double(nbrs[u].size());
            D2 old = pos[u];
            pos[u] = old + (avg - old) * 0.6;
            for (int f : vfaces[u])
                if (!faceOk(f))
                {
                    pos[u] = old;
                    break;
                }
        }
    for (size_t u = 0; u < nu; ++u)
        if (!fixed[u])
            res.points[size_t(firstOf[u])] = pos[u];
    for (std::vector<int>& f : faces)
        for (int& v : f)
            v = firstOf[size_t(v)];
    res.faces = std::move(faces);
    return res;
}

CapFillResult capFill(const std::vector<std::vector<glm::dvec2>>& loops, const CapFillOptions& o)
{
    LILA_ZONE();

    if (o.borderRing <= 0.0)
        return fillCore(loops, o);
    // A ring of quads following the boundary first (the skeleton's wavefront a little way in), then the grid inside it.
    std::vector<std::vector<D2>> used;
    double minEdge = 1e300, total = 0.0;
    std::vector<double> lens;
    for (const auto& l : loops)
    {
        if (l.size() < 3)
            continue;
        used.push_back(l);
        for (size_t i = 0; i < l.size(); ++i)
        {
            double len = glm::length(l[(i + 1) % l.size()] - l[i]);
            minEdge = std::min(minEdge, len);
            total += len;
            lens.push_back(len);
        }
    }
    if (lens.empty())
        return fillCore(loops, o);
    std::nth_element(lens.begin(), lens.begin() + std::ptrdiff_t(lens.size() / 2), lens.end());
    double h = o.spacing > 0.0 ? o.spacing : lens[lens.size() / 2];
    if (!(minEdge > total * 1e-9))
        return fillCore(loops, o);
    SkeletonOptions so;
    so.maxTime = h * o.borderRing;
    StraightSkeleton sk = straightSkeleton(used, so);
    // A region narrower than the ring is better as one grid fill than as a fan of ring faces.
    if (!sk.ok() || sk.front.empty())
        return fillCore(loops, o);

    CapFillResult res;
    size_t nb = 0;
    for (const auto& l : used)
        nb += l.size();
    // Skeleton nodes past the input ones become points after the boundary.
    std::vector<int> nodePoint(sk.nodes.size(), -1);
    res.points.assign(sk.nodes.begin(), sk.nodes.begin() + std::ptrdiff_t(nb));
    for (size_t n = 0; n < nb; ++n)
        nodePoint[n] = int(n);
    auto pointOf = [&](int n) {
        if (nodePoint[size_t(n)] < 0)
        {
            nodePoint[size_t(n)] = int(res.points.size());
            res.points.push_back(sk.nodes[size_t(n)]);
        }
        return nodePoint[size_t(n)];
    };
    for (const std::vector<int>& face : sk.faces)
    {
        std::vector<int> f;
        for (int n : face)
        {
            int p = pointOf(n);
            if (f.empty() || (f.back() != p && glm::length(res.points[size_t(f.back())] - res.points[size_t(p)]) > 0.0))
                f.push_back(p);
        }
        while (f.size() > 1 && glm::length(res.points[size_t(f.front())] - res.points[size_t(f.back())]) <= 0.0)
            f.pop_back();
        if (f.size() < 3)
            continue;
        if (f.size() <= 4)
        {
            res.faces.push_back(std::move(f));
            continue;
        }
        // Where the wavefront had an event the face has more corners: quads where they fit, else triangles.
        auto at = [&](int v) { return res.points[size_t(v)]; };
        auto split = splitIntoQuads(f, at, 75.0);
        if (f.size() % 2 == 0 && !split.second.empty())
        {
            for (auto& q : split.second)
                res.faces.push_back(std::move(q));
            continue;
        }
        std::vector<D2> poly;
        for (int v : f)
            poly.push_back(at(v));
        std::vector<geo::Index> tri;
        geo::earClip(poly, tri);
        for (size_t k = 0; k + 2 < tri.size(); k += 3)
            res.faces.push_back({f[tri[k]], f[tri[k + 1]], f[tri[k + 2]]});
    }
    // The inside of the ring, filled on its own and joined on by the front points.
    std::vector<std::vector<D2>> inner;
    std::vector<std::vector<int>> innerIds;
    for (const std::vector<int>& loop : sk.front)
    {
        inner.emplace_back();
        innerIds.emplace_back();
        for (int n : loop)
        {
            inner.back().push_back(sk.nodes[size_t(n)]);
            innerIds.back().push_back(pointOf(n));
        }
    }
    if (!inner.empty())
    {
        CapFillOptions io = o;
        io.borderRing = 0.0;
        io.spacing = h;
        CapFillResult in = fillCore(inner, io);
        if (!in.ok())
        {
            res.error = in.error;
            return res;
        }
        std::vector<int> map(in.points.size());
        size_t k = 0;
        for (const auto& ids : innerIds)
            for (int id : ids)
                map[k++] = id;
        for (; k < in.points.size(); ++k)
        {
            map[k] = int(res.points.size());
            res.points.push_back(in.points[k]);
        }
        for (const std::vector<int>& f : in.faces)
        {
            std::vector<int> g;
            for (int v : f)
                g.push_back(map[size_t(v)]);
            res.faces.push_back(std::move(g));
        }
    }
    return res;
}
CdtResult constrainedDelaunay(const std::vector<D2>& points, const std::vector<std::pair<int, int>>& segments, bool insideOnly)
{
    LILA_ZONE();

    CdtResult res;
    if (points.size() < 3)
        return res;
    D2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const D2& p : points)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    double extent = std::max(hi.x - lo.x, hi.y - lo.y);
    if (!(extent > 0.0))
        return res;
    std::vector<D2> unit;
    std::vector<int> uid(points.size()), firstOf;
    std::map<std::pair<double, double>, int> seen;
    for (size_t i = 0; i < points.size(); ++i)
    {
        auto [it, fresh] = seen.try_emplace({points[i].x, points[i].y}, int(unit.size()));
        if (fresh)
        {
            unit.push_back((points[i] - lo) / extent);
            firstOf.push_back(int(i));
        }
        uid[i] = it->second;
    }
    Cdt cdt(unit);
    for (int i = 0; i < int(unit.size()); ++i)
        cdt.insert(i);
    cdt.rebuildAdjacency();
    for (auto [a, b] : segments)
    {
        if (a < 0 || b < 0 || size_t(a) >= points.size() || size_t(b) >= points.size() || uid[size_t(a)] == uid[size_t(b)])
            continue;
        if (!cdt.recover(uid[size_t(a)], uid[size_t(b)]))
            ++res.lostSegments;
    }
    size_t T = cdt.tris.size();
    std::vector<int> depth(T, -1);
    std::vector<size_t> queue;
    for (size_t t = 0; t < T; ++t)
        for (int v : cdt.tris[t].v)
            if (v >= cdt.real && depth[t] < 0)
            {
                depth[t] = 0;
                queue.push_back(t);
            }
    for (size_t k = 0; k < queue.size(); ++k)
    {
        size_t t = queue[k];
        const auto& tr = cdt.tris[t];
        for (int i = 0; i < 3; ++i)
        {
            int n = tr.n[size_t(i)];
            if (n < 0 || depth[size_t(n)] >= 0)
                continue;
            bool wall = cdt.constrained.count(edgeKey(tr.v[size_t((i + 1) % 3)], tr.v[size_t((i + 2) % 3)])) > 0;
            depth[size_t(n)] = depth[t] + (wall ? 1 : 0);
            queue.push_back(size_t(n));
        }
    }
    for (size_t t = 0; t < T; ++t)
    {
        const auto& v = cdt.tris[t].v;
        if (v[0] >= cdt.real || v[1] >= cdt.real || v[2] >= cdt.real)
            continue;
        if (insideOnly && depth[t] % 2 != 1)
            continue;
        std::array<int, 3> tri = {firstOf[size_t(v[0])], firstOf[size_t(v[1])], firstOf[size_t(v[2])]};
        if (orient(points[size_t(tri[0])], points[size_t(tri[1])], points[size_t(tri[2])]) < 0.0)
            std::swap(tri[1], tri[2]);
        res.triangles.push_back(tri);
    }
    return res;
}
} // namespace lila::curve
