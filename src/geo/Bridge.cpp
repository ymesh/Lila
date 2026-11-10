#include "geo/Bridge.h"

#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/OpsCommon.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace lila::geo
{
namespace
{
using namespace detail;
using D3 = glm::dvec3;

constexpr double kEps = 1e-12;
constexpr int kDense = 64;

D3 unit(const D3& v)
{
    const double l = glm::length(v);
    return l > kEps ? v / l : D3(0.0);
}

double smoothstep(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/** Rotation taking unit a onto unit b (Rodrigues). */
glm::dmat3 rotationBetween(const D3& a, const D3& b)
{
    const D3 v = glm::cross(a, b);
    const double c = glm::dot(a, b), s = glm::length(v);
    if (s < 1e-12)
    {
        if (c > 0.0)
            return glm::dmat3(1.0);
        const D3 axis = unit(glm::cross(a, std::abs(a.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0)));
        return glm::dmat3(2.0 * glm::outerProduct(axis, axis)) - glm::dmat3(1.0);
    }
    const D3 k = v / s;
    const glm::dmat3 K(D3(0, k.z, -k.y), D3(-k.z, 0, k.x), D3(k.y, -k.x, 0));
    return glm::dmat3(1.0) + s * K + (1.0 - c) * K * K;
}

glm::dmat3 rotationAbout(const D3& axis, double radians)
{
    const D3 k = unit(axis);
    const glm::dmat3 K(D3(0, k.z, -k.y), D3(-k.z, 0, k.x), D3(k.y, -k.x, 0));
    return glm::dmat3(1.0) + std::sin(radians) * K + (1.0 - std::cos(radians)) * K * K;
}

/** Normalised arc length at each point; a closed run's total includes the closing edge. */
std::vector<double> arcParams(const std::vector<D3>& p, bool closed)
{
    std::vector<double> s(p.size(), 0.0);
    double acc = 0.0;
    for (size_t i = 1; i < p.size(); ++i)
        s[i] = acc += glm::distance(p[i - 1], p[i]);
    const double total = acc + (closed && p.size() > 1 ? glm::distance(p.back(), p.front()) : 0.0);
    for (double& v : s)
        v = total > kEps ? v / total : 0.0;
    return s;
}

/** A polyline with its cumulative length, sampled by fraction of that length. */
struct Dense
{
    std::vector<D3> p;
    std::vector<double> s;

    void finish()
    {
        s.assign(p.size(), 0.0);
        for (size_t i = 1; i < p.size(); ++i)
            s[i] = s[i - 1] + glm::distance(p[i - 1], p[i]);
    }
    double length() const { return s.empty() ? 0.0 : s.back(); }
    D3 at(double f) const
    {
        if (p.size() < 2 || length() < kEps)
            return p.empty() ? D3(0.0) : p.front();
        const double x = std::clamp(f, 0.0, 1.0) * length();
        size_t k = size_t(std::upper_bound(s.begin(), s.end(), x) - s.begin());
        k = std::clamp<size_t>(k, 1, p.size() - 1);
        const double seg = s[k] - s[k - 1];
        return glm::mix(p[k - 1], p[k], seg > kEps ? (x - s[k - 1]) / seg : 0.0);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Pairing borders

glm::dvec3 pathCentre(const Mesh& mesh, const EdgePath& p)
{
    glm::dvec3 sum(0.0);
    for (Index pt : p.points)
        sum += mesh.position(pt);
    return p.points.empty() ? sum : sum / double(p.points.size());
}

/** Which way the border opens: away from the faces along it. Zero for wire edges. */
glm::dvec3 openSide(const Mesh& mesh, const EdgePath& p)
{
    glm::dvec3 sum(0.0);
    for (Index e : p.edges)
    {
        if (mesh.edgeCorner(e) == kInvalid)
            continue;
        const glm::dvec3 mid = (mesh.position(mesh.edgePoint(e, 0)) + mesh.position(mesh.edgePoint(e, 1))) * 0.5;
        const glm::dvec3 d = mid - mesh.faceCentroid(mesh.cornerFace(mesh.edgeCorner(e)));
        if (const double len = glm::length(d); len > 1e-12)
            sum += d / len;
    }
    return sum;
}

/** A hole's border (or a wire loop) opens every way, so any direction suits it. */
bool opensToward(const glm::dvec3& open, const EdgePath& p, const glm::dvec3& d)
{
    return glm::length(open) < 0.25 * double(p.edges.size()) || glm::dot(open, d) > 0.0;
}

/** A closed loop faces a partner lying off its plane (a gap in a tube), not one beside it in the plane (two holes in a sheet). */
bool loopsFace(const Mesh& mesh, const EdgePath& loop, const glm::dvec3& centre, const glm::dvec3& other)
{
    glm::dvec3 normal(0.0);
    const size_t n = loop.points.size();
    for (size_t i = 0; i < n; ++i)
        normal += glm::cross(mesh.position(loop.points[i]) - centre, mesh.position(loop.points[(i + 1) % n]) - centre);
    double radius = 0.0;
    for (Index p : loop.points)
        radius += glm::distance(mesh.position(p), centre);
    radius /= double(std::max<size_t>(n, 1));
    // Concentric loops (an annulus) face each other whatever their planes.
    const glm::dvec3 d = other - centre;
    if (glm::length(normal) < 1e-12 || glm::length(d) < 0.25 * radius)
        return true;
    return std::abs(glm::dot(glm::normalize(normal), glm::normalize(d))) > 0.5;
}

/** The path cut at its corners into open runs; a closed path with fewer than two corners stays whole. */
std::vector<EdgePath> splitAtCorners(const Mesh& mesh, const EdgePath& p)
{
    // Sharper than an octagon's turn, so rounded borders stay whole.
    const double cosCorner = std::cos(glm::radians(50.0));
    const size_t np = p.points.size(), ne = p.edges.size();
    auto dir = [&](size_t e) {
        const glm::dvec3 d = mesh.position(p.points[(e + 1) % np]) - mesh.position(p.points[e]);
        const double len = glm::length(d);
        return len > 1e-12 ? d / len : d;
    };
    std::vector<size_t> corners;
    for (size_t i = p.closed ? 0 : 1; i < (p.closed ? np : np - 1); ++i)
        if (glm::dot(dir((i + ne - 1) % ne), dir(i)) < cosCorner)
            corners.push_back(i);
    if (p.closed && corners.size() < 2)
        return {p};
    if (!p.closed)
    {
        corners.insert(corners.begin(), 0);
        corners.push_back(np - 1);
    }
    std::vector<EdgePath> runs;
    const size_t count = p.closed ? corners.size() : corners.size() - 1;
    for (size_t k = 0; k < count; ++k)
    {
        const size_t a = corners[k];
        const size_t b = p.closed && k + 1 == corners.size() ? corners[0] + ne : corners[k + 1];
        EdgePath run;
        for (size_t i = a; i < b; ++i)
        {
            run.points.push_back(p.points[i % np]);
            run.edges.push_back(p.edges[i % ne]);
        }
        run.points.push_back(p.points[b % np]);
        runs.push_back(std::move(run));
    }
    return runs;
}

using PathPair = std::pair<EdgePath, EdgePath>;

/**
 * The two runs of one border to bridge: with four or more runs, the longest opposite pair, matching lengths first and
 * nearest after; with two or three (a sliver between two curves, a triangle), the two longest, sharing a corner.
 */
std::optional<PathPair> sidesToBridge(const Mesh& mesh, const EdgePath& p)
{
    const std::vector<EdgePath> runs = splitAtCorners(mesh, p);
    const size_t n = runs.size();
    if (n < 2 || (n == 2 && !p.closed))
        return std::nullopt;
    auto adjacent = [&](size_t i, size_t j) { return j == i + 1 || (p.closed && i == 0 && j == n - 1); };
    size_t bestI = 0, bestJ = 0;
    std::tuple<size_t, int, double> best{0, 0, 0.0};
    bool found = false;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
        {
            if (n >= 4 && adjacent(i, j))
                continue;
            const size_t li = runs[i].edges.size(), lj = runs[j].edges.size();
            const double d = glm::distance(pathCentre(mesh, runs[i]), pathCentre(mesh, runs[j]));
            // Longer shorter side wins, then equal lengths, then nearer.
            const std::tuple<size_t, int, double> key{std::min(li, lj), li == lj ? 1 : 0, -d};
            if (!found || key > best)
            {
                found = true;
                best = key;
                bestI = i, bestJ = j;
            }
        }
    if (!found)
        return std::nullopt;
    return PathPair{runs[bestI], runs[bestJ]};
}

bool bridgeable(const Mesh& m, Index e)
{
    return e < m.edgeCapacity() && m.edgeValid(e) && m.edgeFaceCount(e) <= 1;
}

/** +1 or -1 for a path wholly on one side of the mirror, 0 for one on or across the plane. */
int pathSide(const SymmetryMap& s, const EdgePath& p)
{
    int side = 0;
    for (Index pt : p.points)
        if (const int k = s.side(pt); k != 0)
        {
            if (side != 0 && k != side)
                return 0;
            side = k;
        }
    return side;
}

std::vector<PathPair> planPairs(const Mesh& mesh, std::span<const Index> edges, BridgeConnect connect, const SymmetryMap* symmetry = nullptr)
{
    std::vector<Index> border;
    for (Index e : edges)
        if (bridgeable(mesh, e))
            border.push_back(e);
    std::sort(border.begin(), border.end());
    border.erase(std::unique(border.begin(), border.end()), border.end());
    std::vector<EdgePath> paths = chainEdges(mesh, border);
    std::erase_if(paths, [](const EdgePath& p) { return p.edges.empty(); });
    const size_t n = paths.size();
    std::vector<glm::dvec3> centres, open;
    std::vector<int> sides;
    const bool mirrored = symmetry && !symmetry->isRadial() && !symmetry->empty();
    for (const EdgePath& p : paths)
    {
        centres.push_back(pathCentre(mesh, p));
        open.push_back(openSide(mesh, p));
        sides.push_back(mirrored ? pathSide(*symmetry, p) : 0);
    }
    std::vector<PathPair> out;

    if (connect != BridgeConnect::Pairs && n >= 2)
    {
        // Start at one end of the farthest pair and step to the nearest loop left.
        size_t start = 0;
        double far = -1.0;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                if (const double d = glm::distance(centres[i], centres[j]); d > far)
                {
                    far = d;
                    start = i;
                }
        std::vector<size_t> order{start};
        std::vector<uint8_t> used(n, 0);
        used[start] = 1;
        while (order.size() < n)
        {
            size_t best = 0;
            double bd = std::numeric_limits<double>::max();
            for (size_t j = 0; j < n; ++j)
                if (!used[j])
                    if (const double d = glm::distance(centres[order.back()], centres[j]); d < bd)
                    {
                        bd = d;
                        best = j;
                    }
            used[best] = 1;
            order.push_back(best);
        }
        for (size_t k = 0; k + 1 < n; ++k)
            out.push_back({paths[order[k]], paths[order[k + 1]]});
        if (connect == BridgeConnect::ChainClosed && n >= 3)
            out.push_back({paths[order.back()], paths[order.front()]});
        return out;
    }

    // Borders that open toward each other pair first, so a tube with two gaps bridges the gaps, not the band between them.
    struct Candidate
    {
        bool across;
        double score;
        size_t a, b;
    };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
        {
            if (paths[i].closed != paths[j].closed || sides[i] * sides[j] < 0)
                continue;
            if (paths[i].closed && !loopsFace(mesh, paths[i], centres[i], centres[j]))
                continue;
            const bool facing = opensToward(open[i], paths[i], centres[j] - centres[i]) && opensToward(open[j], paths[j], centres[i] - centres[j]);
            const bool same = paths[i].edges.size() == paths[j].edges.size();
            candidates.push_back({!facing, glm::distance(centres[i], centres[j]) * (same ? 1.0 : 1.5), i, j});
        }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& x, const Candidate& y) { return std::tie(x.across, x.score) < std::tie(y.across, y.score); });

    std::vector<uint8_t> paired(n, 0);
    for (const Candidate& c : candidates)
        if (!paired[c.a] && !paired[c.b])
        {
            paired[c.a] = paired[c.b] = 1;
            out.push_back({paths[c.a], paths[c.b]});
        }
    for (size_t i = 0; i < n; ++i)
        if (!paired[i])
            if (std::optional<PathPair> sides = sidesToBridge(mesh, paths[i]))
                out.push_back(std::move(*sides));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// One bridge

/** A border chain in the order the strip runs along it. */
struct Side
{
    std::vector<Index> pts;
    std::vector<Index> edges; // edges[i] joins pts[i] and pts[i + 1]; a closed side's last joins back to pts[0]
    bool closed = false;
    bool wire = true;
};

void reverseSide(Side& s)
{
    if (s.closed)
    {
        std::reverse(s.pts.begin() + 1, s.pts.end());
        std::reverse(s.edges.begin(), s.edges.end());
    }
    else
    {
        std::reverse(s.pts.begin(), s.pts.end());
        std::reverse(s.edges.begin(), s.edges.end());
    }
}

void rotateSide(Side& s, size_t start)
{
    if (!s.closed || start == 0)
        return;
    std::rotate(s.pts.begin(), s.pts.begin() + std::ptrdiff_t(start), s.pts.end());
    std::rotate(s.edges.begin(), s.edges.begin() + std::ptrdiff_t(start), s.edges.end());
}

/** The side read the way a face filling the gap runs along it: against the existing face. */
Side sideFrom(const Mesh& m, const EdgePath& p)
{
    Side s{p.points, p.edges, p.closed, true};
    for (Index e : s.edges)
        if (m.edgeCorner(e) != kInvalid)
            s.wire = false;
    const Index e0 = s.edges[0];
    if (const Index c = m.edgeCorner(e0); c != kInvalid && m.cornerPoint(c) == s.pts[0])
        reverseSide(s);
    return s;
}

/** The existing face's corner at side point i, kInvalid on wire. */
Index sideCorner(const Mesh& m, const Side& s, size_t i)
{
    const size_t ne = s.edges.size();
    for (size_t ei : {i < ne ? i : ne - 1, i > 0 ? i - 1 : (s.closed ? ne - 1 : 0)})
    {
        const Index e = s.edges[ei];
        if (const Index c = m.edgeCorner(e); c != kInvalid)
            return m.cornerPoint(c) == s.pts[i] ? c : (m.cornerNextPoint(c) == s.pts[i] ? m.cornerNext(c) : kInvalid);
    }
    return kInvalid;
}

/** What the strip needs at each side point: where it leaves the mesh and the edge flow behind it. */
struct SidePoint
{
    D3 pos;
    D3 out;                 // unit, square to the side, in the faces' tangent plane, away from them
    std::vector<D3> behind; // up to two points across the faces, nearest first
    Index corner = kInvalid;
};

/** Per side point, with other the far side, whose points are never read as edge flow behind this one. */
std::vector<SidePoint> sidePoints(const Mesh& m, const Side& s, const Side& other, const D3& otherCentre)
{
    const std::unordered_set<Index> far(other.pts.begin(), other.pts.end());
    const size_t n = s.pts.size();
    std::unordered_set<Index> own(s.edges.begin(), s.edges.end());
    std::vector<SidePoint> out(n);
    for (size_t k = 0; k < n; ++k)
    {
        SidePoint& sp = out[k];
        sp.pos = m.position(s.pts[k]);
        sp.corner = sideCorner(m, s, k);
        const D3 prev = k > 0 ? m.position(s.pts[k - 1]) : (s.closed ? m.position(s.pts[n - 1]) : sp.pos);
        const D3 next = k + 1 < n ? m.position(s.pts[k + 1]) : (s.closed ? m.position(s.pts[0]) : sp.pos);
        const D3 along = unit(next - prev);
        D3 nrm(0.0), away(0.0);
        auto visit = [&](size_t ei) {
            const Index e = s.edges[ei];
            if (const Index c = m.edgeCorner(e); c != kInvalid)
            {
                const Index f = m.cornerFace(c);
                nrm += m.faceNormal(f);
                away += sp.pos - m.faceCentroid(f);
            }
        };
        if (k > 0 || s.closed)
            visit(k > 0 ? k - 1 : s.edges.size() - 1);
        if (k + 1 < n || s.closed)
            visit(k < s.edges.size() ? k : s.edges.size() - 1);
        const D3 want = glm::length(away) > kEps ? away : otherCentre - sp.pos;
        D3 d = unit(glm::cross(along, nrm));
        if (glm::length(d) < 0.5)
            d = unit(want - along * glm::dot(want, along));
        else if (glm::dot(d, want) < 0.0)
            d = -d;
        sp.out = d;

        // Across the faces: the inner edge leaving most directly away from the gap, then straight on.
        if (s.wire)
            continue;
        const Index p0 = s.pts[k];
        Index best = kInvalid;
        double bestDot = -0.2;
        m.forEachPointEdge(p0, [&](Index e) {
            if (own.count(e) || m.edgeFaceCount(e) < 2)
                return;
            if (const double dd = glm::dot(unit(m.position(m.edgeOtherPoint(e, p0)) - sp.pos), sp.out); dd < bestDot)
            {
                bestDot = dd;
                best = e;
            }
        });
        if (best == kInvalid || far.count(m.edgeOtherPoint(best, p0)))
            continue;
        Index prevP = p0, cur = m.edgeOtherPoint(best, p0), inE = best;
        sp.behind.push_back(m.position(cur));
        Index straight = kInvalid;
        double sd = 0.5;
        const D3 heading = unit(m.position(cur) - m.position(prevP));
        m.forEachPointEdge(cur, [&](Index e) {
            if (e == inE)
                return;
            if (const double dd = glm::dot(unit(m.position(m.edgeOtherPoint(e, cur)) - m.position(cur)), heading); dd > sd)
            {
                sd = dd;
                straight = e;
            }
        });
        if (straight != kInvalid && !far.count(m.edgeOtherPoint(straight, cur)))
            sp.behind.push_back(m.position(m.edgeOtherPoint(straight, cur)));
    }
    return out;
}

/** Where a column meets one side: on point a, or t of the way to point b (indices into the side). */
struct Station
{
    size_t a = 0, b = 0;
    double t = 0.0;
    bool exact() const { return a == b || t <= 1e-12; }
};

struct StationData
{
    D3 pos, out;
    std::vector<D3> behind;
};

StationData atStation(const std::vector<SidePoint>& sp, const Station& st)
{
    if (st.exact())
        return {sp[st.a].pos, sp[st.a].out, sp[st.a].behind};
    const SidePoint &x = sp[st.a], &y = sp[st.b];
    StationData d{glm::mix(x.pos, y.pos, st.t), unit(glm::mix(x.out, y.out, st.t)), {}};
    for (size_t j = 0; j < std::min(x.behind.size(), y.behind.size()); ++j)
        d.behind.push_back(glm::mix(x.behind[j], y.behind[j], st.t));
    return d;
}

/** Columns of the big side for each small point, in order, by arc length; forced[i] pins small point i to a column. */
std::vector<size_t> assignColumns(const std::vector<D3>& big, const std::vector<D3>& small, bool closed,
                                  const std::unordered_map<size_t, size_t>& forced, double& cost)
{
    const size_t N = big.size(), n = small.size();
    const std::vector<double> Pb = arcParams(big, closed), Ps = arcParams(small, closed);
    std::vector<size_t> col(n, 0);
    for (size_t i = 1; i < n; ++i)
    {
        const size_t lo = col[i - 1] + 1, hi = N - (n - i);
        size_t best = lo;
        if (!closed && i + 1 == n)
            best = N - 1;
        else if (auto it = forced.find(i); it != forced.end())
            best = std::clamp(it->second, lo, hi);
        else
        {
            double bd = std::numeric_limits<double>::max();
            for (size_t c = lo; c <= hi; ++c)
                if (const double d = std::abs(Pb[c] - Ps[i]); d < bd)
                {
                    bd = d;
                    best = c;
                }
        }
        col[i] = best;
    }
    cost = 0.0;
    for (size_t i = 0; i < n; ++i)
        cost += glm::dot(small[i] - big[col[i]], small[i] - big[col[i]]);
    return col;
}

/**
 * Moves small points one column so the extra big-side columns come in even runs between them: two extra columns end
 * in a 3-to-1 of quads, one alone needs a triangle. Odd runs pair with the next odd run; pinned points stay.
 */
void evenUp(std::vector<size_t>& col, size_t N, bool closed, const std::unordered_map<size_t, size_t>& forced)
{
    const size_t n = col.size();
    const size_t segs = closed ? n : n - 1;
    auto extra = [&](size_t k) { return (k + 1 < n ? col[k + 1] : N) - col[k] - 1; };
    std::vector<size_t> odd;
    for (size_t k = 0; k < segs; ++k)
        if (extra(k) % 2)
            odd.push_back(k);
    for (size_t i = 0; i + 1 < odd.size(); i += 2)
    {
        const size_t k1 = odd[i], k2 = odd[i + 1];
        bool pinned = false;
        for (size_t p = k1 + 1; p <= k2; ++p)
            pinned = pinned || forced.count(p);
        if (pinned || (!closed && k2 + 1 >= n))
            continue;
        for (size_t p = k1 + 1; p <= k2; ++p)
            ++col[p];
    }
}

/** Circle through points (all in one plane), or nothing when they do not lie on one within tol. */
struct Circle
{
    D3 centre, u, w;
    double radius = 0.0;
    double angle(const D3& p) const { return std::atan2(glm::dot(p - centre, w), glm::dot(p - centre, u)); }
    D3 at(double a) const { return centre + radius * (std::cos(a) * u + std::sin(a) * w); }
};

std::optional<Circle> fitCircle(const std::vector<D3>& pts, double tol)
{
    if (pts.size() < 3)
        return std::nullopt;
    D3 c(0.0);
    for (const D3& p : pts)
        c += p;
    c /= double(pts.size());
    D3 n(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
        n += glm::cross(pts[i] - c, pts[(i + 1) % pts.size()] - c);
    if (glm::length(n) < kEps)
        return std::nullopt;
    n = glm::normalize(n);
    for (const D3& p : pts)
        if (std::abs(glm::dot(p - c, n)) > tol)
            return std::nullopt;
    Circle out;
    out.u = unit(pts[0] - c - n * glm::dot(pts[0] - c, n));
    out.w = glm::cross(n, out.u);
    // Kasa fit: x^2 + y^2 + D x + E y + F = 0 in least squares.
    glm::dmat3 AtA(0.0);
    D3 Atb(0.0);
    for (const D3& p : pts)
    {
        const double x = glm::dot(p - c, out.u), y = glm::dot(p - c, out.w);
        const D3 row(x, y, 1.0);
        AtA += glm::outerProduct(row, row);
        Atb += row * -(x * x + y * y);
    }
    if (std::abs(glm::determinant(AtA)) < 1e-30)
        return std::nullopt;
    const D3 sol = glm::inverse(AtA) * Atb;
    const double cx = -sol.x / 2.0, cy = -sol.y / 2.0, r2 = cx * cx + cy * cy - sol.z;
    if (r2 <= 0.0)
        return std::nullopt;
    out.centre = c + cx * out.u + cy * out.w;
    out.radius = std::sqrt(r2);
    for (const D3& p : pts)
        if (std::abs(glm::distance(p, out.centre) - out.radius) > tol)
            return std::nullopt;
    return out;
}

double wrapPi(double a)
{
    while (a > std::numbers::pi)
        a -= 2.0 * std::numbers::pi;
    while (a < -std::numbers::pi)
        a += 2.0 * std::numbers::pi;
    return a;
}

/** Unit direction and curvature vector (per unit length) at p0 from the points behind it, nearest first. */
void slopeAndBend(const D3& p0, const std::vector<D3>& behind, const D3& fallback, D3& dir, D3& bend)
{
    bend = D3(0.0);
    if (behind.empty())
    {
        dir = fallback;
        return;
    }
    if (behind.size() == 1)
    {
        dir = unit(p0 - behind[0]);
        return;
    }
    const double h1 = glm::distance(p0, behind[0]), h2 = glm::distance(behind[0], behind[1]);
    const double s0 = -(h1 + h2), s1 = -h1, s2 = 0.0;
    if (h1 < kEps || h2 < kEps)
    {
        dir = unit(p0 - behind[0]);
        return;
    }
    const D3 &y0 = behind[1], &y1 = behind[0], &y2 = p0;
    const D3 d1 = y0 * ((s2 - s1) / ((s0 - s1) * (s0 - s2))) + y1 * ((s2 - s0) / ((s1 - s0) * (s1 - s2)))
                  + y2 * (((s2 - s0) + (s2 - s1)) / ((s2 - s0) * (s2 - s1)));
    const D3 d2 = 2.0 * (y0 / ((s0 - s1) * (s0 - s2)) + y1 / ((s1 - s0) * (s1 - s2)) + y2 / ((s2 - s0) * (s2 - s1)));
    const double l = glm::length(d1);
    if (l < kEps)
    {
        dir = unit(p0 - behind[0]);
        return;
    }
    dir = d1 / l;
    bend = (d2 - dir * glm::dot(d2, dir)) / (l * l);
}

D3 quintic(const D3& p0, const D3& v0, const D3& a0, const D3& p1, const D3& v1, const D3& a1, double t)
{
    const double t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
    const double h0 = 1 - 10 * t3 + 15 * t4 - 6 * t5, h1 = t - 6 * t3 + 8 * t4 - 3 * t5, h2 = 0.5 * t2 - 1.5 * t3 + 1.5 * t4 - 0.5 * t5;
    const double h3 = 0.5 * t3 - t4 + 0.5 * t5, h4 = -4 * t3 + 7 * t4 - 3 * t5, h5 = 10 * t3 - 15 * t4 + 6 * t5;
    return h0 * p0 + h1 * v0 + h2 * a0 + h3 * a1 + h4 * v1 + h5 * p1;
}

double polylineLength(const std::vector<D3>& p)
{
    double l = 0.0;
    for (size_t i = 1; i < p.size(); ++i)
        l += glm::distance(p[i - 1], p[i]);
    return l;
}

enum class ColumnFit : uint8_t
{
    Line,
    Arc,
    Curve,
    Blend,
};

/** The column's curve from a (source) to b (target) as a dense polyline, before smoothness. */
std::vector<D3> columnCurve(const StationData& a, const StationData& b, const BridgeOptions& o, bool bothWire, ColumnFit* fit)
{
    std::vector<D3> out;
    const double L = glm::distance(a.pos, b.pos);
    auto blend = [&] {
        const D3 v0 = a.out * (L * o.tensionSource), v1 = -b.out * (L * o.tensionTarget);
        for (int i = 0; i <= kDense; ++i)
        {
            const double t = double(i) / kDense, t2 = t * t, t3 = t2 * t;
            out.push_back((2 * t3 - 3 * t2 + 1) * a.pos + (t3 - 2 * t2 + t) * v0 + (-2 * t3 + 3 * t2) * b.pos + (t3 - t2) * v1);
        }
        if (fit)
            *fit = ColumnFit::Blend;
    };
    if (L < kEps || o.curve == BridgeCurve::Linear || bothWire)
    {
        out = {a.pos, b.pos};
        if (fit)
            *fit = ColumnFit::Line;
        return out;
    }
    if (o.curve == BridgeCurve::Blend || (a.behind.empty() && b.behind.empty()))
    {
        blend();
        return out;
    }

    // Auto: the edge flow either side, nearest-first behind each end.
    double spacing = 0.0;
    for (const auto* s : {&a, &b})
        if (!s->behind.empty())
            spacing = std::max(spacing, glm::distance(s->pos, s->behind[0]));
    const double tol = 1e-3 * std::max(L, spacing);
    std::vector<D3> all;
    for (auto it = a.behind.rbegin(); it != a.behind.rend(); ++it)
        all.push_back(*it);
    all.push_back(a.pos);
    all.push_back(b.pos);
    for (const D3& p : b.behind)
        all.push_back(p);

    const D3 ab = (b.pos - a.pos) / L;
    bool line = true;
    for (const D3& p : all)
        if (glm::length((p - a.pos) - ab * glm::dot(p - a.pos, ab)) > tol)
            line = false;
    for (const D3& p : a.behind)
        line = line && glm::dot(p - a.pos, ab) < 0.0;
    for (const D3& p : b.behind)
        line = line && glm::dot(p - b.pos, ab) > 0.0;
    if (line)
    {
        out = {a.pos, b.pos};
        if (fit)
            *fit = ColumnFit::Line;
        return out;
    }

    if (all.size() >= 4)
        if (std::optional<Circle> c = fitCircle(all, tol))
        {
            const double ta = c->angle(a.pos), tb = c->angle(b.pos);
            // Sweep away from the points behind a (or toward those behind b).
            double dirSign = 0.0;
            if (!a.behind.empty())
                dirSign = -std::copysign(1.0, wrapPi(c->angle(a.behind[0]) - ta));
            else
                dirSign = std::copysign(1.0, wrapPi(c->angle(b.behind[0]) - tb));
            double sweep = dirSign * (tb - ta);
            while (sweep <= 0.0)
                sweep += 2.0 * std::numbers::pi;
            while (sweep > 2.0 * std::numbers::pi)
                sweep -= 2.0 * std::numbers::pi;
            // A gap wider than the rest of the circle means the fit is a coincidence, not the shape.
            if (sweep < 1.5 * std::numbers::pi)
            {
                for (int i = 0; i <= kDense; ++i)
                    out.push_back(c->at(ta + dirSign * sweep * double(i) / kDense));
                if (fit)
                    *fit = ColumnFit::Arc;
                return out;
            }
        }

    D3 da, ka, db, kb;
    slopeAndBend(a.pos, a.behind, a.out, da, ka);
    slopeAndBend(b.pos, b.behind, b.out, db, kb);
    double T = L;
    for (int iter = 0; iter < 3; ++iter)
    {
        out.clear();
        for (int i = 0; i <= kDense; ++i)
            out.push_back(quintic(a.pos, da * (T * o.tensionSource), ka * (T * T), b.pos, -db * (T * o.tensionTarget), kb * (T * T),
                                  double(i) / kDense));
        T = polylineLength(out);
    }
    if (fit)
        *fit = ColumnFit::Curve;
    return out;
}

double profileFalloff(BridgeProfile shape, double x)
{
    x = std::clamp(x, 0.0, 1.0);
    switch (shape)
    {
    case BridgeProfile::Smooth: return x * x * (3.0 - 2.0 * x);
    case BridgeProfile::Sphere: return std::sqrt(std::max(0.0, 2.0 * x - x * x));
    case BridgeProfile::Root: return std::sqrt(x);
    case BridgeProfile::Sharp: return x * x;
    case BridgeProfile::Linear: return x;
    case BridgeProfile::InverseSquare: return x * (2.0 - x);
    case BridgeProfile::Constant: return x > 0.0 ? 1.0 : 0.0;
    }
    return x;
}

double sampleRamp(const std::vector<double>& ramp, double v)
{
    if (ramp.empty())
        return 1.0;
    if (ramp.size() == 1)
        return ramp[0];
    const double x = std::clamp(v, 0.0, 1.0) * double(ramp.size() - 1);
    const size_t k = std::min(size_t(x), ramp.size() - 2);
    return glm::mix(ramp[k], ramp[k + 1], x - double(k));
}

/** The border joining the end of one side to the end of the other, walking away from skipEdge; empty if none. */
std::vector<Index> endBorder(const Mesh& m, Index from, Index skipEdge, Index to, const std::unordered_set<Index>& stop,
                             std::vector<Index>& chainEdges)
{
    std::vector<Index> chain{from};
    chainEdges.clear();
    Index cur = from, prevE = skipEdge;
    for (int step = 0; step < 1024; ++step)
    {
        Index next = kInvalid;
        int count = 0;
        m.forEachPointEdge(cur, [&](Index e) {
            if (e != prevE && m.isBoundaryEdge(e))
            {
                next = e;
                ++count;
            }
        });
        if (count != 1)
            return {};
        const Index p = m.edgeOtherPoint(next, cur);
        chain.push_back(p);
        chainEdges.push_back(next);
        if (p == to)
            return chain;
        if (stop.count(p))
            return {};
        cur = p;
        prevE = next;
    }
    return {};
}

/** Attribute sources of a strip point: corners and points with weights. */
struct Sources
{
    std::vector<Index> corners, points;
    std::vector<float> cornerW, pointW;
};

class PairBridge
{
public:
    PairBridge(Mesh& m, const EdgePath& p, const EdgePath& q, const BridgeOptions& o, BridgeResult& r) : mMesh(m), mO(o), mR(r)
    {
        mS0 = sideFrom(m, p);
        mS1 = sideFrom(m, q);
    }

    void run();

private:
    // Half rows in strip space: 0 is the source, 2R the target.
    D3 base(size_t c, int h) const;
    D3 shaped(size_t c, int h) const;
    Index pointAt(size_t c, int h);
    Sources sourcesAt(size_t c, int h) const;
    bool setUp();
    void pickRows();
    void weldEnd(bool last);
    void buildFaces();
    void merge();

    Mesh& mMesh;
    const BridgeOptions& mO;
    BridgeResult& mR;
    Side mS0, mS1;
    std::vector<SidePoint> mP0, mP1;
    bool mClosed = false;
    bool mBigIs0 = true;
    size_t mN = 0;
    /** Per column: where it meets each side, and its curve. */
    std::vector<Station> mSt0, mSt1;
    std::vector<Dense> mCurve;
    /** Per column, the band (counted from the big side) in which it ends; R for columns reaching the small side. */
    std::vector<int> mDrop;
    int mRows = 1;
    std::vector<double> mRowV; // arc fraction at each half row
    /** Open ends: point ids per strip row, extra border points per band, and the displacement of each half row. */
    struct End
    {
        bool active = false;
        std::vector<Index> ids;
        std::vector<std::vector<Index>> extras;
        std::vector<D3> shift;
        std::vector<Index> existingCorner; // per row, the side face's corner at a reused border point
    };
    End mEnd[2];
    std::vector<D3> mCentre; // per half row
    D3 mAxis{0.0};
    D3 mNormal{0.0};
    std::unordered_map<uint64_t, Index> mIds;
    std::vector<Index> mNewPoints;
    Index mSrcFace = kInvalid;
};

bool PairBridge::setUp()
{
    const Mesh& m = mMesh;
    if (mS0.closed != mS1.closed)
    {
        mR.warnings.push_back("a closed loop cannot bridge to an open chain");
        return false;
    }
    mClosed = mS0.closed;
    const size_t minPts = mClosed ? 3 : 2;
    if (mS0.pts.size() < minPts || mS1.pts.size() < minPts)
        return false;
    // Read the target the same way round as the source; a wire side has no faces to say which way, so pairing decides.
    if (!mS1.wire)
        reverseSide(mS1);

    auto positions = [&](const Side& s) {
        std::vector<D3> out;
        for (Index p : s.pts)
            out.push_back(m.position(p));
        return out;
    };

    // Explicit pairs: the first fixes both starts, the rest pin columns.
    std::vector<std::pair<Index, Index>> pins;
    for (auto [a, b] : mO.pairs)
    {
        const bool aIn0 = std::count(mS0.pts.begin(), mS0.pts.end(), a), bIn1 = std::count(mS1.pts.begin(), mS1.pts.end(), b);
        const bool aIn1 = std::count(mS1.pts.begin(), mS1.pts.end(), a), bIn0 = std::count(mS0.pts.begin(), mS0.pts.end(), b);
        if (aIn0 && bIn1)
            pins.push_back({a, b});
        else if (aIn1 && bIn0)
            pins.push_back({b, a});
    }
    if (mClosed && !pins.empty())
    {
        rotateSide(mS0, size_t(std::find(mS0.pts.begin(), mS0.pts.end(), pins[0].first) - mS0.pts.begin()));
        rotateSide(mS1, size_t(std::find(mS1.pts.begin(), mS1.pts.end(), pins[0].second) - mS1.pts.begin()));
    }

    mBigIs0 = mS0.pts.size() >= mS1.pts.size();
    Side& big = mBigIs0 ? mS0 : mS1;
    Side& small = mBigIs0 ? mS1 : mS0;
    const std::vector<D3> bigP = positions(big);
    const size_t n = small.pts.size();

    auto forcedFor = [&](const Side& sm) {
        std::unordered_map<size_t, size_t> f;
        for (auto [a, b] : pins)
        {
            const Index bp = mBigIs0 ? a : b, sp = mBigIs0 ? b : a;
            const size_t bi = size_t(std::find(big.pts.begin(), big.pts.end(), bp) - big.pts.begin());
            const size_t si = size_t(std::find(sm.pts.begin(), sm.pts.end(), sp) - sm.pts.begin());
            if (bi < big.pts.size() && si < sm.pts.size())
                f[si] = bi;
        }
        return f;
    };

    // Search the small side's start and direction (crossing only when it pays well, or for wire).
    const bool pinnedStart = mClosed && !pins.empty();
    double bestCost[2] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    size_t bestStart[2] = {0, 0};
    for (int crossed = 0; crossed < 2; ++crossed)
    {
        Side trial = small;
        if (crossed)
            reverseSide(trial);
        for (size_t s = 0; s < (mClosed && !pinnedStart ? n : 1); ++s)
        {
            Side rot = trial;
            rotateSide(rot, s);
            double cost = 0.0;
            assignColumns(bigP, positions(rot), mClosed, forcedFor(rot), cost);
            if (cost < bestCost[crossed])
            {
                bestCost[crossed] = cost;
                bestStart[crossed] = s;
            }
        }
    }
    const bool anyWire = mS0.wire || mS1.wire;
    bool crossed = anyWire ? bestCost[1] < bestCost[0] : (pins.empty() && bestCost[1] < 0.25 * bestCost[0]);
    crossed = crossed != (mO.reverseSource != mO.reverseTarget);
    if (crossed)
        reverseSide(small);
    if (mClosed)
        rotateSide(small, bestStart[crossed ? 1 : 0]);
    if (mClosed && mO.offset != 0)
        rotateSide(small, size_t(((mO.offset % int(n)) + int(n)) % int(n)));

    double cost = 0.0;
    std::vector<size_t> col = assignColumns(bigP, positions(small), mClosed, forcedFor(small), cost);
    if (mO.transition == BridgeTransition::Quads)
        evenUp(col, big.pts.size(), mClosed, forcedFor(small));
    mN = big.pts.size();

    // Stations: big side exact at every column, small side in between where it has no point.
    const std::vector<double> Pb = arcParams(bigP, mClosed);
    std::vector<Station> bigSt(mN), smallSt(mN);
    for (size_t c = 0; c < mN; ++c)
        bigSt[c] = {c, c, 0.0};
    const size_t segs = mClosed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        const size_t c0 = col[i], c1 = i + 1 < n ? col[i + 1] : mN;
        const double p0 = Pb[c0], p1 = c1 < mN ? Pb[c1] : 1.0;
        for (size_t c = c0; c < c1; ++c)
        {
            const double t = c == c0 ? 0.0 : (p1 - p0 > kEps ? (Pb[c] - p0) / (p1 - p0) : 0.0);
            smallSt[c] = {i, (i + 1) % n, t};
        }
    }
    if (!mClosed)
        smallSt[mN - 1] = {n - 1, n - 1, 0.0};
    mSt0 = mBigIs0 ? bigSt : smallSt;
    mSt1 = mBigIs0 ? smallSt : bigSt;

    mDrop.assign(mN, 0);
    for (size_t c = 0; c < mN; ++c)
        mDrop[c] = smallSt[c].exact() ? std::numeric_limits<int>::max() : -1;

    D3 c0(0.0), c1(0.0);
    for (Index p : mS0.pts)
        c0 += m.position(p);
    for (Index p : mS1.pts)
        c1 += m.position(p);
    c0 /= double(mS0.pts.size());
    c1 /= double(mS1.pts.size());
    mP0 = sidePoints(m, mS0, mS1, c1);
    mP1 = sidePoints(m, mS1, mS0, c0);

    for (const Side* s : {&mS0, &mS1})
        for (Index e : s->edges)
            if (mSrcFace == kInvalid && m.edgeCorner(e) != kInvalid)
                mSrcFace = m.cornerFace(m.edgeCorner(e));
    return true;
}

void PairBridge::pickRows()
{
    const bool bothWire = mS0.wire && mS1.wire;
    mCurve.assign(mN, {});
    std::vector<StationData> a(mN), b(mN);
    for (size_t c = 0; c < mN; ++c)
    {
        a[c] = atStation(mP0, mSt0[c]);
        b[c] = atStation(mP1, mSt1[c]);
    }

    if (mO.curve == BridgeCurve::Path && mO.path.size() >= 2 && polylineLength(mO.path) > kEps)
    {
        // The path carried from one side's centre to the other's, each side's section turned along with it.
        D3 cA(0.0), cB(0.0);
        for (size_t c = 0; c < mN; ++c)
        {
            cA += a[c].pos;
            cB += b[c].pos;
        }
        cA /= double(mN);
        cB /= double(mN);
        const D3 ps = mO.path.front(), pe = mO.path.back();
        const glm::dmat3 rot = rotationBetween(unit(pe - ps), unit(cB - cA));
        const double scale = glm::distance(pe, ps) > kEps ? glm::distance(cB, cA) / glm::distance(pe, ps) : 1.0;
        Dense path;
        for (const D3& p : mO.path)
            path.p.push_back(cA + rot * (p - ps) * scale);
        path.finish();
        std::vector<D3> along(kDense + 1), tangent(kDense + 1);
        for (int i = 0; i <= kDense; ++i)
            along[size_t(i)] = path.at(double(i) / kDense);
        for (int i = 0; i <= kDense; ++i)
            tangent[size_t(i)] = unit(along[size_t(std::min(i + 1, kDense))] - along[size_t(std::max(i - 1, 0))]);
        std::vector<glm::dmat3> fromStart(kDense + 1, glm::dmat3(1.0));
        for (int i = 1; i <= kDense; ++i)
            fromStart[size_t(i)] = rotationBetween(tangent[size_t(i - 1)], tangent[size_t(i)]) * fromStart[size_t(i - 1)];
        const glm::dmat3 endInv = glm::transpose(fromStart[kDense]);
        for (size_t c = 0; c < mN; ++c)
        {
            const D3 oA = a[c].pos - cA, oB = b[c].pos - cB;
            Dense& d = mCurve[c];
            for (int i = 0; i <= kDense; ++i)
            {
                const double t = double(i) / kDense;
                const D3 straight = glm::mix(a[c].pos, b[c].pos, t);
                const D3 curved = along[size_t(i)] + (1.0 - t) * (fromStart[size_t(i)] * oA) + t * (fromStart[size_t(i)] * endInv * oB);
                d.p.push_back(glm::mix(straight, curved, mO.smoothness));
            }
            d.finish();
        }
    }
    else
    {
        if (mO.curve == BridgeCurve::Path)
            mR.warnings.push_back("Path needs a curve; bridged with Blend");
        BridgeOptions o = mO;
        if (o.curve == BridgeCurve::Path)
            o.curve = BridgeCurve::Blend;
        for (size_t c = 0; c < mN; ++c)
        {
            std::vector<D3> curve = columnCurve(a[c], b[c], o, bothWire, nullptr);
            Dense& d = mCurve[c];
            const size_t k = curve.size() - 1;
            for (size_t i = 0; i <= size_t(kDense); ++i)
            {
                const double t = double(i) / kDense;
                const D3 cv = k == 1 ? glm::mix(curve[0], curve[1], t) : curve[std::min(i, k)];
                d.p.push_back(glm::mix(glm::mix(a[c].pos, b[c].pos, t), cv, mO.smoothness));
            }
            d.finish();
        }
    }

    // Spacing of the edge flow either side, for row count and grading.
    double hA = 0.0, hB = 0.0, len = 0.0;
    int nA = 0, nB = 0;
    for (size_t c = 0; c < mN; ++c)
    {
        len += mCurve[c].length();
        if (!a[c].behind.empty())
        {
            hA += glm::distance(a[c].pos, a[c].behind[0]);
            ++nA;
        }
        if (!b[c].behind.empty())
        {
            hB += glm::distance(b[c].pos, b[c].behind[0]);
            ++nB;
        }
    }
    len /= double(mN);
    hA = nA ? hA / nA : 0.0;
    hB = nB ? hB / nB : 0.0;
    double edgeLen = 0.0;
    int ne = 0;
    for (const Side* s : {&mS0, &mS1})
        for (Index e : s->edges)
        {
            edgeLen += mMesh.edgeLength(e);
            ++ne;
        }
    edgeLen = ne ? edgeLen / ne : 1.0;

    // Open ends: the border joining the sides' ends, which may fix the row count.
    std::vector<Index> endChain[2], endEdges[2];
    if (!mClosed)
    {
        std::unordered_set<Index> stop(mS0.pts.begin(), mS0.pts.end());
        stop.insert(mS1.pts.begin(), mS1.pts.end());
        for (int e = 0; e < 2; ++e)
        {
            const size_t c = e ? mN - 1 : 0;
            const Index from = mS0.pts[mSt0[c].a], to = mS1.pts[mSt1[c].a];
            if (from == to)
            {
                endChain[e] = {from};
                continue;
            }
            if (!mO.weldEnds)
                continue;
            const Index skip = mS0.edges[e ? mS0.edges.size() - 1 : 0];
            endChain[e] = endBorder(mMesh, from, skip, to, stop, endEdges[e]);
        }
    }

    int rows = mO.divisions + 1;
    if (mO.divisions < 0)
    {
        int fromEnds = -1;
        for (int e = 0; e < 2; ++e)
            if (endChain[e].size() >= 2)
                fromEnds = std::max(fromEnds, int(endChain[e].size()) - 1);
        const double h = nA && nB ? (hA + hB) / 2.0 : (nA ? hA : (nB ? hB : edgeLen));
        rows = fromEnds > 0 ? fromEnds : int(std::lround(len / std::max(h, kEps)));
    }
    mRows = std::clamp(rows, 1, 256);

    // Rows graded from the spacing behind the source to that behind the target (Auto), else even.
    const int H = 2 * mRows;
    mRowV.assign(size_t(H) + 1, 0.0);
    const bool grade = mO.curve == BridgeCurve::Auto && nA && nB && hA > kEps && hB > kEps;
    for (int h = 0; h <= H; ++h)
    {
        const double x = double(h) / H;
        // Spacing grows linearly from hA to hB: its integral, normalised.
        mRowV[size_t(h)] = grade ? (hA * x + 0.5 * (hB - hA) * x * x) / (0.5 * (hA + hB)) : x;
    }

    // Columns that do not reach the small side end in a band: 3-to-1 quads in pairs, one 2-to-1 for an odd count.
    {
        std::vector<size_t> keep;
        for (size_t c = 0; c < mN; ++c)
            if (mDrop[c] != -1)
                keep.push_back(c);
        const int R = mRows;
        for (size_t k = 0; k < (mClosed ? keep.size() : keep.size() - 1); ++k)
        {
            const size_t c0 = keep[k], c1 = k + 1 < keep.size() ? keep[k + 1] : keep[0] + mN;
            std::vector<size_t> gone;
            for (size_t c = c0 + 1; c < c1; ++c)
                gone.push_back(c % mN);
            if (gone.empty())
                continue;
            if (mO.transition == BridgeTransition::Triangles)
            {
                for (size_t c : gone)
                    mDrop[c] = R / 2;
                continue;
            }
            // Pairs of columns end together (3-to-1); an odd one out (2-to-1, one triangle) sits mid-run, where it shows least.
            std::vector<std::vector<size_t>> groups;
            const size_t twos = gone.size() / 2, single = gone.size() % 2 ? twos / 2 : ~size_t(0);
            for (size_t g = 0, idx = 0; idx < gone.size(); ++g)
            {
                if (g == single)
                    groups.push_back({gone[idx++]});
                else
                {
                    groups.push_back({gone[idx], gone[idx + 1]});
                    idx += 2;
                }
            }
            const int G = int(groups.size());
            if (G <= R)
            {
                const int start = (R - G) / 2;
                for (int g = 0; g < G; ++g)
                    for (size_t c : groups[size_t(g)])
                        mDrop[c] = start + g;
            }
            else
            {
                for (int g = 0; g < G; ++g)
                    for (size_t c : groups[size_t(g)])
                        mDrop[c] = std::min(g, R - 1);
            }
        }
    }

    // Open ends: weld the rows to the border between the sides' ends.
    for (int e = 0; e < 2; ++e)
    {
        End& end = mEnd[e];
        end.shift.assign(size_t(H) + 1, D3(0.0));
        end.extras.assign(size_t(mRows), {});
        end.existingCorner.assign(size_t(mRows) + 1, kInvalid);
        if (mClosed || endChain[e].empty())
            continue;
        end.active = true;
        const size_t c = e ? mN - 1 : 0;
        end.ids.assign(size_t(mRows) + 1, kInvalid);
        end.ids[0] = mS0.pts[mSt0[c].a];
        end.ids[size_t(mRows)] = mS1.pts[mSt1[c].a];
        std::vector<Index>& chain = endChain[e];
        if (chain.size() == 1)
        {
            std::fill(end.ids.begin(), end.ids.end(), chain[0]);
            for (int h = 0; h <= H; ++h)
                end.shift[size_t(h)] = mMesh.position(chain[0]) - mCurve[c].at(mRowV[size_t(h)]);
            continue;
        }
        std::vector<Index>& edges = endEdges[e];
        auto fractions = [&] {
            std::vector<double> f(chain.size(), 0.0);
            for (size_t i = 1; i < chain.size(); ++i)
                f[i] = f[i - 1] + glm::distance(mMesh.position(chain[i - 1]), mMesh.position(chain[i]));
            for (double& x : f)
                x = f.back() > kEps ? x / f.back() : 0.0;
            return f;
        };
        const std::vector<double> f = fractions();
        const size_t inner = chain.size() - 2;
        std::vector<int> rowOf(chain.size(), -1); // chain point -> strip row it serves
        rowOf[0] = 0;
        rowOf[chain.size() - 1] = mRows;
        if (inner == size_t(mRows - 1))
            for (size_t i = 1; i + 1 < chain.size(); ++i)
                rowOf[i] = int(i);
        else
        {
            const double tol = mO.weldTolerance / mRows;
            size_t next = 1;
            for (int r = 1; r < mRows; ++r)
            {
                const double want = mRowV[size_t(2 * r)];
                size_t best = 0;
                double bd = tol;
                for (size_t i = next; i + 1 < chain.size(); ++i)
                    if (const double d = std::abs(f[i] - want); d <= bd)
                    {
                        bd = d;
                        best = i;
                    }
                if (best)
                {
                    rowOf[best] = r;
                    next = best + 1;
                }
            }
        }
        // Rows with no point split the border where they fall.
        std::vector<Index> rowIds(size_t(mRows) + 1, kInvalid);
        for (size_t i = 0; i < chain.size(); ++i)
            if (rowOf[i] >= 0)
                rowIds[size_t(rowOf[i])] = chain[i];
        for (int r = 1; r < mRows; ++r)
        {
            if (rowIds[size_t(r)] != kInvalid)
            {
                ++mR.welded;
                continue;
            }
            const std::vector<double> fr = fractions();
            const double want = mRowV[size_t(2 * r)];
            size_t j = 0;
            while (j + 2 < chain.size() && fr[j + 1] < want)
                ++j;
            const double span = fr[j + 1] - fr[j];
            const double t = span > kEps ? std::clamp((want - fr[j]) / span, 0.05, 0.95) : 0.5;
            const Index edge = edges[j];
            const bool forward = mMesh.edgePoint(edge, 0) == chain[j];
            const Index np = mMesh.splitEdge(edge, forward ? t : 1.0 - t);
            const Index other = mMesh.lastSplitEdge();
            chain.insert(chain.begin() + std::ptrdiff_t(j + 1), np);
            edges[j] = forward ? edge : other;
            edges.insert(edges.begin() + std::ptrdiff_t(j + 1), forward ? other : edge);
            rowOf.insert(rowOf.begin() + std::ptrdiff_t(j + 1), r);
            rowIds[size_t(r)] = np;
            ++mR.split;
        }
        for (size_t i = 0; i < chain.size(); ++i)
            if (rowOf[i] < 0)
            {
                // A border point between rows: it stays a corner of the end face of its band.
                int band = 0;
                for (size_t k = 0; k < i; ++k)
                    if (rowOf[k] >= 0)
                        band = rowOf[k];
                end.extras[size_t(std::min(band, mRows - 1))].push_back(chain[i]);
                ++mR.unwelded;
            }
        end.ids = rowIds;
        for (int r = 0; r <= mRows; ++r)
        {
            const size_t ci = size_t(std::find(chain.begin(), chain.end(), rowIds[size_t(r)]) - chain.begin());
            const size_t ei = ci < edges.size() ? ci : edges.size() - 1;
            if (const Index cr = mMesh.edgeCorner(edges[ei]); cr != kInvalid)
                end.existingCorner[size_t(r)] = mMesh.cornerPoint(cr) == rowIds[size_t(r)] ? cr : mMesh.cornerNext(cr);
            end.shift[size_t(2 * r)] = mMesh.position(rowIds[size_t(r)]) - mCurve[c].at(mRowV[size_t(2 * r)]);
        }
        for (int h = 1; h < H; h += 2)
            end.shift[size_t(h)] = 0.5 * (end.shift[size_t(h - 1)] + end.shift[size_t(h + 1)]);
    }

    // Row centres and the bridge's axis for taper, profile and twist.
    mCentre.assign(size_t(H) + 1, D3(0.0));
    for (int h = 0; h <= H; ++h)
    {
        for (size_t c = 0; c < mN; ++c)
            mCentre[size_t(h)] += base(c, h);
        mCentre[size_t(h)] /= double(mN);
    }
    mAxis = unit(mCentre[size_t(H)] - mCentre[0]);
    for (size_t k = 0; k < mP0.size(); ++k)
        if (mP0[k].corner != kInvalid)
            mNormal += mMesh.faceNormal(mMesh.cornerFace(mP0[k].corner));
    for (size_t k = 0; k < mP1.size(); ++k)
        if (mP1[k].corner != kInvalid)
            mNormal += mMesh.faceNormal(mMesh.cornerFace(mP1[k].corner));
}

D3 PairBridge::base(size_t c, int h) const
{
    D3 p = mCurve[c].at(mRowV[size_t(h)]);
    if (mClosed)
        return p;
    // Rows the open ends moved carry over to the columns near them, fading out.
    const double F = std::clamp(double(mN) / 3.0, 1.0, 4.0);
    for (int e = 0; e < 2; ++e)
        if (mEnd[e].active)
        {
            const double d = e ? double(mN - 1 - c) : double(c);
            if (d < F)
                p += mEnd[e].shift[size_t(h)] * smoothstep(1.0 - d / F);
        }
    return p;
}

D3 PairBridge::shaped(size_t c, int h) const
{
    D3 p = base(c, h);
    const int H = 2 * mRows;
    if (h == 0 || h == H)
        return p;
    const double v = double(h) / H;
    const D3& centre = mCentre[size_t(h)];
    if (mO.taper != 1.0)
    {
        const double bias = std::clamp(mO.bias, 0.01, 0.99);
        const double x = v < bias ? v / bias : (1.0 - v) / (1.0 - bias);
        const double s = 1.0 + (mO.taper - 1.0) * smoothstep(x) * sampleRamp(mO.taperCurve, v);
        p = centre + (p - centre) * s;
    }
    if (mO.profileFactor != 0.0)
    {
        const double amount = mO.profileFactor * profileFalloff(mO.profileShape, 1.0 - std::abs(2.0 * v - 1.0));
        if (mClosed)
            p = centre + (p - centre) * (1.0 + amount);
        else
        {
            const size_t c0 = c > 0 ? c - 1 : c, c1 = c + 1 < mN ? c + 1 : c;
            const D3 across = base(c1, h) - base(c0, h);
            const D3 along = base(c, std::min(h + 1, H)) - base(c, std::max(h - 1, 0));
            D3 n = unit(glm::cross(across, along));
            if (glm::dot(n, mNormal) < 0.0)
                n = -n;
            p += n * (amount * mCurve[c].length());
        }
    }
    if (mO.twist != 0.0 && glm::length(mAxis) > 0.5)
        p = centre + rotationAbout(mAxis, glm::radians(mO.twist) * v) * (p - centre);
    return p;
}

Sources PairBridge::sourcesAt(size_t c, int h) const
{
    Sources s;
    const double v = double(h) / (2 * mRows);
    auto add = [&](const std::vector<SidePoint>& sp, const Side& side, const Station& st, double w) {
        if (w <= 0.0)
            return;
        const double ta = st.exact() ? 1.0 : 1.0 - st.t;
        for (auto [k, wk] : {std::pair{st.a, ta}, std::pair{st.b, st.exact() ? 0.0 : st.t}})
        {
            if (wk <= 0.0)
                continue;
            if (sp[k].corner != kInvalid)
            {
                s.corners.push_back(sp[k].corner);
                s.cornerW.push_back(float(w * wk));
            }
            s.points.push_back(side.pts[k]);
            s.pointW.push_back(float(w * wk));
        }
    };
    add(mP0, mS0, mSt0[c], 1.0 - v);
    add(mP1, mS1, mSt1[c], v);
    return s;
}

Index PairBridge::pointAt(size_t c, int h)
{
    const int H = 2 * mRows;
    if (!mClosed && (c == 0 || c == mN - 1) && mEnd[c ? 1 : 0].active && h % 2 == 0)
        return mEnd[c ? 1 : 0].ids[size_t(h / 2)];
    if (h == 0 && mSt0[c].exact())
        return mS0.pts[mSt0[c].a];
    if (h == H && mSt1[c].exact())
        return mS1.pts[mSt1[c].a];
    const uint64_t key = (uint64_t(c) << 32) | uint32_t(h);
    if (auto it = mIds.find(key); it != mIds.end())
        return it->second;
    const Index np = mMesh.addPoint(shaped(c, h));
    const Sources s = sourcesAt(c, h);
    if (!s.points.empty())
        mMesh.pointAttrs().mix(np, s.points, s.pointW);
    mIds[key] = np;
    mNewPoints.push_back(np);
    return np;
}

void PairBridge::buildFaces()
{
    const int R = mRows;
    // Rows counted from the big side: alive at row b iff b <= drop.
    auto alive = [&](size_t c, int b) { return mDrop[c] >= b; };
    auto toStrip = [&](int bigHalfRow) { return mBigIs0 ? bigHalfRow : 2 * R - bigHalfRow; };

    struct Ref
    {
        size_t c;
        int h; // strip half row
    };
    std::vector<std::vector<Ref>> polys;
    for (int b = 0; b < R; ++b)
    {
        std::vector<size_t> bottom;
        for (size_t c = 0; c < mN; ++c)
            if (alive(c, b + 1))
                bottom.push_back(c);
        const size_t pairs = mClosed ? bottom.size() : bottom.size() - 1;
        const int top = toStrip(2 * b), mid = toStrip(2 * b + 1), bot = toStrip(2 * b + 2);
        for (size_t k = 0; k < pairs; ++k)
        {
            const size_t b0 = bottom[k], b1 = bottom[(k + 1) % bottom.size()];
            std::vector<size_t> t{b0};
            for (size_t c = (b0 + 1) % mN; c != b1; c = (c + 1) % mN)
                if (alive(c, b))
                    t.push_back(c);
            t.push_back(b1);
            const size_t edgesTop = t.size() - 1;
            if (edgesTop == 1)
                polys.push_back({{b0, top}, {b1, top}, {b1, bot}, {b0, bot}});
            else if (edgesTop == 2 && mO.transition == BridgeTransition::Quads)
            {
                polys.push_back({{t[0], top}, {t[1], top}, {t[1], mid}, {t[0], bot}});
                polys.push_back({{t[1], top}, {t[2], top}, {t[2], bot}, {t[1], mid}});
                polys.push_back({{t[0], bot}, {t[1], mid}, {t[2], bot}});
                ++mR.triangles;
            }
            else if (edgesTop == 3 && mO.transition == BridgeTransition::Quads)
            {
                polys.push_back({{t[0], top}, {t[1], top}, {t[1], mid}, {t[0], bot}});
                polys.push_back({{t[1], top}, {t[2], top}, {t[2], mid}, {t[1], mid}});
                polys.push_back({{t[2], top}, {t[3], top}, {t[3], bot}, {t[2], mid}});
                polys.push_back({{t[0], bot}, {t[1], mid}, {t[2], mid}, {t[3], bot}});
            }
            else
            {
                for (size_t j = 0; j + 1 < edgesTop; ++j)
                {
                    polys.push_back({{t[j], top}, {t[j + 1], top}, {t[0], bot}});
                    ++mR.triangles;
                }
                polys.push_back({{t[edgesTop - 1], top}, {t[edgesTop], top}, {t[edgesTop], bot}, {t[0], bot}});
            }
        }
    }

    // Points, with the open ends' leftover border points as extra corners of their band's face.
    struct Poly
    {
        std::vector<Index> pts;
        std::vector<Sources> src;
    };
    std::vector<Poly> out;
    for (const std::vector<Ref>& refs : polys)
    {
        Poly poly;
        for (size_t i = 0; i < refs.size(); ++i)
        {
            const Ref& x = refs[i];
            const Index id = pointAt(x.c, x.h);
            Sources s = sourcesAt(x.c, x.h);
            const bool atEnd = !mClosed && (x.c == 0 || x.c == mN - 1) && mEnd[x.c ? 1 : 0].active;
            if (atEnd && x.h % 2 == 0)
                if (const Index cr = mEnd[x.c ? 1 : 0].existingCorner[size_t(x.h / 2)]; cr != kInvalid)
                    s = Sources{{cr}, {}, {1.0f}, {}};
            poly.pts.push_back(id);
            poly.src.push_back(std::move(s));
            const Ref& y = refs[(i + 1) % refs.size()];
            if (atEnd && y.c == x.c && std::abs(y.h - x.h) == 2)
            {
                const End& end = mEnd[x.c ? 1 : 0];
                std::vector<Index> extra = end.extras[size_t(std::min(x.h, y.h) / 2)];
                if (y.h < x.h)
                    std::reverse(extra.begin(), extra.end());
                for (Index p : extra)
                {
                    poly.pts.push_back(p);
                    poly.src.push_back({});
                }
            }
        }
        // Collapsed ends repeat a point; drop the repeats.
        Poly clean;
        for (size_t i = 0; i < poly.pts.size(); ++i)
            if (clean.pts.empty() || clean.pts.back() != poly.pts[i])
            {
                clean.pts.push_back(poly.pts[i]);
                clean.src.push_back(poly.src[i]);
            }
        while (clean.pts.size() > 1 && clean.pts.front() == clean.pts.back())
        {
            clean.pts.pop_back();
            clean.src.pop_back();
        }
        if (clean.pts.size() >= 3)
            out.push_back(std::move(clean));
    }

    // Wind to continue the faces along the borders; with none (wire), facing away from the middle.
    bool flip = false;
    bool decided = false;
    std::unordered_set<Index> sideEdges;
    for (const Side* s : {&mS0, &mS1})
        sideEdges.insert(s->edges.begin(), s->edges.end());
    for (const Poly& p : out)
    {
        for (size_t i = 0; i < p.pts.size() && !decided; ++i)
        {
            const Index u = p.pts[i], w = p.pts[(i + 1) % p.pts.size()];
            const Index e = mMesh.findEdge(u, w);
            if (e == kInvalid || !sideEdges.count(e))
                continue;
            if (const Index c = mMesh.edgeCorner(e); c != kInvalid)
            {
                flip = mMesh.cornerPoint(c) == u;
                decided = true;
            }
        }
        if (decided)
            break;
    }
    if (!decided && !out.empty())
    {
        D3 mid(0.0);
        size_t count = 0;
        for (const Poly& p : out)
            for (Index id : p.pts)
            {
                mid += mMesh.position(id);
                ++count;
            }
        mid /= double(count);
        double sum = 0.0;
        for (const Poly& p : out)
        {
            D3 n(0.0), c(0.0);
            for (size_t i = 0; i < p.pts.size(); ++i)
            {
                n += glm::cross(mMesh.position(p.pts[i]), mMesh.position(p.pts[(i + 1) % p.pts.size()]));
                c += mMesh.position(p.pts[i]);
            }
            c /= double(p.pts.size());
            sum += glm::dot(n, c - mid);
        }
        flip = sum < 0.0;
    }

    for (Poly& p : out)
    {
        if (flip)
        {
            std::reverse(p.pts.begin(), p.pts.end());
            std::reverse(p.src.begin(), p.src.end());
        }
        const Index f = addFaceFrom(mMesh, p.pts, mSrcFace, {});
        if (f == kInvalid)
            continue;
        mR.faces.push_back(f);
        Index c = mMesh.faceCorner(f);
        for (size_t k = 0; k < p.pts.size(); ++k, c = mMesh.cornerNext(c))
            if (!p.src[k].corners.empty())
                mixCorners(mMesh, c, p.src[k].corners, p.src[k].cornerW);
    }
}

void PairBridge::merge()
{
    // Each big-side point merges into the small-side point its column is nearest, at mergeFactor between the sides.
    Side& big = mBigIs0 ? mS0 : mS1;
    Side& small = mBigIs0 ? mS1 : mS0;
    const std::vector<Station>& st = mBigIs0 ? mSt1 : mSt0;
    std::unordered_map<Index, std::vector<Index>> into;
    std::unordered_map<Index, D3> at;
    const double f = std::clamp(mO.mergeFactor, 0.0, 1.0);
    for (size_t c = 0; c < mN; ++c)
    {
        const Index bp = big.pts[c];
        const Index sp = small.pts[st[c].exact() || st[c].t < 0.5 ? st[c].a : st[c].b];
        if (bp == sp)
            continue;
        into[sp].push_back(bp);
        if (st[c].exact())
        {
            const D3 src = mMesh.position(mBigIs0 ? bp : sp), tgt = mMesh.position(mBigIs0 ? sp : bp);
            at[sp] = glm::mix(src, tgt, f);
        }
    }
    for (auto& [sp, bps] : into)
    {
        if (!mMesh.pointValid(sp))
            continue;
        if (auto it = at.find(sp); it != at.end())
            mMesh.setPosition(sp, it->second);
        std::erase_if(bps, [&](Index p) { return !mMesh.pointValid(p); });
        if (!bps.empty())
            mergeToTarget(mMesh, bps, sp);
    }
    ++mR.pairsBridged;
}

void PairBridge::run()
{
    if (!setUp())
        return;
    if (mO.merge)
    {
        merge();
        return;
    }
    pickRows();
    buildFaces();
    mR.rows = std::max(mR.rows, mRows);
    ++mR.pairsBridged;

    if (mO.relax > 0 && !mNewPoints.empty())
    {
        for (int it = 0; it < mO.relax; ++it)
        {
            std::vector<std::pair<Index, D3>> next;
            for (Index p : mNewPoints)
            {
                D3 sum(0.0);
                int k = 0;
                mMesh.forEachPointEdge(p, [&](Index e) {
                    sum += mMesh.position(mMesh.edgeOtherPoint(e, p));
                    ++k;
                });
                if (k)
                    next.push_back({p, 0.5 * mMesh.position(p) + 0.5 * sum / double(k)});
            }
            for (auto& [p, pos] : next)
                mMesh.setPosition(p, pos);
        }
    }
}
} // namespace

BridgeResult bridge(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts)
{
    LILA_ZONE();

    BridgeResult r;
    for (const PathPair& pair : planPairs(mesh, edges, opts.connect, opts.symmetry))
    {
        // Later pairs were planned before earlier ones split end borders or merged points; skip any that went stale.
        bool fresh = true;
        for (const EdgePath* p : {&pair.first, &pair.second})
            for (Index e : p->edges)
                fresh = fresh && mesh.edgeValid(e) && mesh.edgeFaceCount(e) <= 1;
        if (!fresh)
        {
            r.warnings.push_back("a border changed before its turn to bridge; bridge it again");
            continue;
        }
        PairBridge(mesh, pair.first, pair.second, opts, r).run();
    }
    if (opts.smoothingAngle >= 0.0 && !r.faces.empty())
    {
        std::vector<Index> newEdges;
        for (Index f : r.faces)
            if (mesh.faceValid(f))
                mesh.forEachFaceCorner(f, [&](Index c) { newEdges.push_back(mesh.cornerEdge(c)); });
        std::sort(newEdges.begin(), newEdges.end());
        newEdges.erase(std::unique(newEdges.begin(), newEdges.end()), newEdges.end());
        setHardEdgesByAngle(mesh, opts.smoothingAngle, newEdges);
    }
    return r;
}

BridgeResult bridgeFaces(Mesh& mesh, std::span<const Index> faces, const BridgeOptions& opts)
{
    LILA_ZONE();

    std::unordered_set<Index> sel;
    for (Index f : faces)
        if (mesh.faceValid(f))
            sel.insert(f);
    // The regions' borders that another face keeps once the selection is gone.
    std::vector<Index> rim;
    for (Index f : sel)
        mesh.forEachFaceCorner(f, [&](Index c) {
            const Index e = mesh.cornerEdge(c);
            bool other = false;
            mesh.forEachEdgeCorner(e, [&](Index ec) { other = other || !sel.count(mesh.cornerFace(ec)); });
            if (other)
                rim.push_back(e);
        });
    std::sort(rim.begin(), rim.end());
    rim.erase(std::unique(rim.begin(), rim.end()), rim.end());
    std::vector<Index> list(sel.begin(), sel.end());
    removeFacesClean(mesh, list);
    std::erase_if(rim, [&](Index e) { return !mesh.edgeValid(e); });
    return bridge(mesh, rim, opts);
}

std::vector<Index> bridgeEdgeLoops(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts)
{
    LILA_ZONE();

    std::vector<PathPair> pairs = planPairs(mesh, edges, BridgeConnect::Pairs, opts.symmetry);
    if (pairs.empty())
        return {};
    BridgeResult r;
    PairBridge(mesh, pairs[0].first, pairs[0].second, opts, r).run();
    if (opts.smoothingAngle >= 0.0 && !r.faces.empty())
    {
        std::vector<Index> newEdges;
        for (Index f : r.faces)
            mesh.forEachFaceCorner(f, [&](Index c) { newEdges.push_back(mesh.cornerEdge(c)); });
        setHardEdgesByAngle(mesh, opts.smoothingAngle, newEdges);
    }
    return r.faces;
}

std::vector<std::vector<Index>> planBridges(const Mesh& mesh, std::span<const Index> edges)
{
    LILA_ZONE();

    std::vector<std::vector<Index>> out;
    for (const PathPair& p : planPairs(mesh, edges, BridgeConnect::Pairs))
    {
        std::vector<Index> both = p.first.edges;
        both.insert(both.end(), p.second.edges.begin(), p.second.edges.end());
        out.push_back(std::move(both));
    }
    return out;
}

std::vector<Index> bridgeEdges(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts)
{
    return bridge(mesh, edges, opts).faces;
}
} // namespace lila::geo
