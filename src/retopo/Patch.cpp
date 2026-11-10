#include "retopo/Patch.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <tuple>

namespace lila::retopo
{
namespace
{
constexpr int kMaxSides = 6;
constexpr size_t kMaxVariants = 64;
/** Pattern solves tried before the search gives up on the rest, so very long sides cannot stall it. */
constexpr long kSolveBudget = 4'000'000;

using Params = std::array<int, kMaxSides>;
using Chain = std::vector<Index>;

/**
 * One way to fill the boundary. The pattern is a star: a centre with a spoke of length s[i] to every side, and around
 * each corner either one grid or, when e[i] > 0, a valence-3 point e[i] from the centre with three grids around it (a
 * 3-5 style pair that widens both sides at that corner by e[i]). The centre's valence is the side count plus the number
 * of such corners. Four sides with matching opposite counts use a plain grid instead.
 */
struct Candidate
{
    /** Boundary indices the pattern treats as corners: the boundary's own, a subset of them, or chosen points. */
    std::vector<int> corners;
    int padding = 0;
    bool single = false;
    bool grid = false;
    Params s{};
    Params e{};
    double score = 0.0;
    /** Sum of every irregular point's distance from valence 4. */
    int irregular = 0;

    auto key() const { return std::tie(score, corners, padding, single, grid, s, e); }
    bool operator<(const Candidate& o) const { return key() < o.key(); }
};

std::vector<int> countsFor(const std::vector<int>& corners, int n)
{
    std::vector<int> out(corners.size());
    for (size_t i = 0; i < corners.size(); ++i)
        out[i] = i + 1 < corners.size() ? corners[i + 1] - corners[i] : n - corners[i] + corners[0];
    return out;
}

/** What each side has left for the two spokes either side of it once the corner widenings e are taken off. */
bool spokeRoom(const std::vector<int>& l, const Params& e, Params& r)
{
    const int n = int(l.size());
    for (int i = 0; i < n; ++i)
    {
        r[i] = l[i] - e[i] - e[(i + 1) % n];
        if (r[i] < 2)
            return false;
    }
    return true;
}

/**
 * Spokes from each side's room r, a side's room being the sum of the spokes either side of its own. Unique for two,
 * three, five and six sides; false when no positive integer spokes fit.
 */
bool solveSpokes(int n, const Params& r, Params& s)
{
    auto at = [&](int i) { return r[((i % n) + n) % n]; };
    for (int i = 0; i < n; ++i)
    {
        int twice = 0;
        if (n == 2)
            twice = at(i + 1);
        else if (n == 3)
            twice = at(i + 1) + at(i + 2) - at(i);
        else if (n == 5)
            twice = at(i + 1) - at(i + 3) + at(i) - at(i + 2) + at(i + 4);
        else if (n == 6)
            twice = at(i + 1) - at(i + 3) + at(i + 5);
        if (twice < 2 || twice % 2 != 0)
            return false;
        s[i] = twice / 2;
    }
    for (int i = 0; i < n; ++i)
        if (s[(i + n - 1) % n] + s[(i + 1) % n] != r[i])
            return false;
    return true;
}

/** Lower is better: regularity first, then compact pairs and a centred star. */
double starScore(const std::vector<int>& l, const Params& s, const Params& e, int irregular)
{
    const int n = int(l.size());
    double avg = 0.0, esum = 0.0;
    int smin = std::numeric_limits<int>::max(), smax = 0;
    for (int i = 0; i < n; ++i)
    {
        avg += l[i];
        esum += e[i];
        smin = std::min(smin, s[i]);
        smax = std::max(smax, s[i]);
    }
    avg /= n;
    const int spread = n == 4 ? std::abs(s[1] - s[3]) + std::abs(s[0] - s[2]) : smax - smin;
    return 100.0 * irregular + 10.0 * esum / avg + 5.0 * spread / avg;
}

/** Where a four-sided star's centre may sit along a room of r: every place when few, else a spread including the middle. */
std::vector<int> slideStops(int r)
{
    constexpr int kStops = 7;
    std::vector<int> out;
    if (r - 1 <= kStops)
        for (int v = 1; v < r; ++v)
            out.push_back(v);
    else
    {
        for (int t = 0; t < kStops; ++t)
            out.push_back(1 + int(std::lround(double(r - 2) * t / (kStops - 1))));
        out.push_back(r / 2);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return out;
}

struct Search
{
    int n = 0;
    long budget = kSolveBudget;
    std::vector<Candidate> found;

    void keep(Candidate c)
    {
        found.push_back(std::move(c));
        if (found.size() > 8 * kMaxVariants)
            prune();
    }

    void prune()
    {
        std::sort(found.begin(), found.end());
        if (found.size() > kMaxVariants)
            found.resize(kMaxVariants);
    }

    /** Every star and grid that fits these pattern corners after the padding rings. */
    void gather(const std::vector<int>& corners, int padding, double penalty)
    {
        const int sides = int(corners.size());
        std::vector<int> l = countsFor(corners, n);
        for (int& x : l)
        {
            x -= 2 * padding;
            if (x < 1)
                return;
        }
        Candidate base;
        base.corners = corners;
        base.padding = padding;
        if (sides == 4 && l[0] == l[2] && l[1] == l[3])
        {
            Candidate c = base;
            c.grid = true;
            c.score = penalty;
            keep(c);
        }
        for (int mask = 0; mask < (1 << sides) && budget > 0; ++mask)
        {
            int bits[2] = {-1, -1}, k = 0;
            for (int j = 0; j < sides; ++j)
                if (mask & (1 << j))
                {
                    if (k < 2)
                        bits[k] = j;
                    ++k;
                }
            // A regular centre makes the same mesh as a star with fewer pairs (or the plain grid), and on two sides a pair
            // at either corner makes the same two valence-3 poles.
            if (k > 2 || sides + k < 3 || sides + k == 4 || (sides == 2 && mask != 1))
                continue;
            const int irregular = std::abs(sides + k - 4) + k;
            auto widest = [&](int j) { return j < 0 ? 0 : std::min(l[(j + sides - 1) % sides], l[j]) - 2; };
            const int max0 = k >= 1 ? widest(bits[0]) : 0, max1 = k >= 2 ? widest(bits[1]) : 0;
            for (int e0 = k >= 1 ? 1 : 0; e0 <= max0 && budget > 0; ++e0)
                for (int e1 = k >= 2 ? 1 : 0; e1 <= max1 && budget > 0; ++e1)
                {
                    --budget;
                    Params e{}, r{};
                    if (bits[0] >= 0)
                        e[bits[0]] = e0;
                    if (bits[1] >= 0)
                        e[bits[1]] = e1;
                    if (!spokeRoom(l, e, r))
                        continue;
                    auto add = [&](const Params& s) {
                        Candidate c = base;
                        c.s = s;
                        c.e = e;
                        c.irregular = irregular;
                        c.score = penalty + starScore(l, s, e, irregular);
                        keep(std::move(c));
                    };
                    if (sides != 4)
                    {
                        Params s{};
                        if (solveSpokes(sides, r, s))
                            add(s);
                        continue;
                    }
                    if (r[0] != r[2] || r[1] != r[3])
                        continue;
                    // On four sides the centre slides freely, and each place puts the poles somewhere else.
                    for (int s1 : slideStops(r[0]))
                        for (int s0 : slideStops(r[1]))
                            add({s0, s1, r[1] - s0, r[0] - s1});
                }
        }
    }
};

double turnDegreesAt(const std::vector<glm::dvec3>& pts, int i)
{
    const int n = int(pts.size());
    const glm::dvec3 u = pts[i] - pts[(i + n - 1) % n], v = pts[(i + 1) % n] - pts[i];
    const double lu = glm::length(u), lv = glm::length(v);
    if (lu <= 0.0 || lv <= 0.0)
        return 0.0;
    return glm::degrees(std::acos(std::clamp(glm::dot(u, v) / (lu * lv), -1.0, 1.0)));
}

/**
 * The best fills, best first: all corners kept if any pattern fits them, else the fewest corners dropped (the flattest
 * first), else two chosen points opposite each other.
 */
std::vector<Candidate> findCandidates(const PatchBoundary& b, int padding)
{
    LILA_ZONE();

    Search search;
    search.n = int(b.points.size());
    const int sides = int(b.corners.size());
    std::vector<double> turn(sides);
    for (int i = 0; i < sides; ++i)
        turn[i] = turnDegreesAt(b.points, b.corners[i]);
    // A ring takes two off every side, the same as shortening every spoke by one, so padding never makes a pattern fit
    // that did not fit without it: the fewest that fit is always none.
    const int maxPad = std::max(padding, 0);
    for (int keep = sides; keep >= 2; --keep)
    {
        for (int pad = maxPad; pad >= 0 && search.found.empty(); --pad)
            for (int mask = 0; mask < (1 << sides); ++mask)
            {
                std::vector<int> sub;
                double penalty = 0.0;
                for (int i = 0; i < sides; ++i)
                {
                    if (mask & (1 << i))
                        sub.push_back(b.corners[i]);
                    else
                        penalty += 1000.0 + turn[i];
                }
                if (int(sub.size()) == keep)
                    search.gather(sub, pad, penalty);
            }
        if (!search.found.empty())
            break;
    }
    if (search.found.empty() && search.n >= 6)
        for (int pad = maxPad; pad >= 0 && search.found.empty(); --pad)
            for (size_t i = 0; i < b.corners.size(); ++i)
            {
                const int c = b.corners[i], opposite = (c + search.n / 2) % search.n;
                if (std::find(b.corners.begin(), b.corners.begin() + i, opposite) != b.corners.begin() + i)
                    continue;
                search.gather({std::min(c, opposite), std::max(c, opposite)}, pad, 10000.0);
            }
    search.prune();
    return search.found;
}

/** Builds a candidate's quads over the boundary points, interior points appended at the origin. */
struct Builder
{
    std::vector<glm::dvec3> pos;
    std::vector<std::vector<Index>> faces;
    std::map<std::pair<Index, Index>, Chain> chains;
    bool broken = false;

    Index point()
    {
        pos.push_back(glm::dvec3(0.0));
        return Index(pos.size() - 1);
    }

    void link(Chain c) { chains[{c.front(), c.back()}] = std::move(c); }

    void spoke(Index a, Index b, int len)
    {
        Chain c = {a};
        for (int i = 1; i < len; ++i)
            c.push_back(point());
        c.push_back(b);
        link(std::move(c));
    }

    Chain chain(Index a, Index b) const
    {
        if (auto it = chains.find({a, b}); it != chains.end())
            return it->second;
        if (auto it = chains.find({b, a}); it != chains.end())
            return Chain(it->second.rbegin(), it->second.rend());
        return {};
    }

    /** A grid over corners a, b, c, d in the boundary's winding, its sides taken from the chains between them. */
    void grid(Index a, Index b, Index c, Index d)
    {
        const Chain bottom = chain(a, b), right = chain(b, c), top = chain(d, c), left = chain(a, d);
        if (bottom.size() < 2 || left.size() < 2 || top.size() != bottom.size() || right.size() != left.size())
        {
            broken = true;
            return;
        }
        const size_t w = bottom.size() - 1, h = left.size() - 1;
        std::vector<Index> g((w + 1) * (h + 1));
        auto at = [&](size_t i, size_t j) -> Index& { return g[j * (w + 1) + i]; };
        for (size_t j = 0; j <= h; ++j)
            for (size_t i = 0; i <= w; ++i)
            {
                if (j == 0)
                    at(i, j) = bottom[i];
                else if (j == h)
                    at(i, j) = top[i];
                else if (i == 0)
                    at(i, j) = left[j];
                else if (i == w)
                    at(i, j) = right[j];
                else
                    at(i, j) = point();
            }
        for (size_t j = 0; j < h; ++j)
            for (size_t i = 0; i < w; ++i)
                faces.push_back({at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
    }

    /** One ring of quads inside the sides, corners keeping a single quad; returns the inner sides, each two shorter. */
    std::vector<Chain> ring(const std::vector<Chain>& sides)
    {
        const size_t m = sides.size();
        std::vector<Index> inner(m);
        for (Index& x : inner)
            x = point();
        std::vector<Chain> out(m);
        for (size_t i = 0; i < m; ++i)
        {
            const Chain& side = sides[i];
            const size_t l = side.size() - 1;
            Chain& in = out[i];
            in.push_back(inner[i]);
            for (size_t j = 2; j + 1 < l; ++j)
                in.push_back(point());
            in.push_back(inner[(i + 1) % m]);
            const Chain& prev = sides[(i + m - 1) % m];
            faces.push_back({prev[prev.size() - 2], side[0], side[1], inner[i]});
            for (size_t j = 1; j + 1 < l; ++j)
                faces.push_back({side[j], side[j + 1], in[j], in[j - 1]});
        }
        return out;
    }

    void star(const std::vector<Chain>& sides, const Params& s, const Params& e)
    {
        const int m = int(sides.size());
        auto wrap = [m](int i) { return ((i % m) + m) % m; };
        const Index centre = point();
        std::vector<Index> t(m), a(m, geo::kInvalid), b(m, geo::kInvalid);
        for (int i = 0; i < m; ++i)
        {
            const Chain& side = sides[i];
            const int l = int(side.size()) - 1;
            if (l != s[wrap(i - 1)] + s[wrap(i + 1)] + e[i] + e[wrap(i + 1)])
            {
                broken = true;
                return;
            }
            std::vector<int> marks = {0};
            if (e[i] > 0)
                marks.push_back(s[wrap(i - 1)]);
            marks.push_back(s[wrap(i - 1)] + e[i]);
            if (e[wrap(i + 1)] > 0)
                marks.push_back(l - s[wrap(i + 1)]);
            marks.push_back(l);
            for (size_t k = 0; k + 1 < marks.size(); ++k)
                link(Chain(side.begin() + marks[k], side.begin() + marks[k + 1] + 1));
            t[i] = side[s[wrap(i - 1)] + e[i]];
            if (e[i] > 0)
                b[i] = side[s[wrap(i - 1)]];
            if (e[wrap(i + 1)] > 0)
                a[wrap(i + 1)] = side[l - s[wrap(i + 1)]];
        }
        for (int i = 0; i < m; ++i)
            spoke(centre, t[i], s[i]);
        for (int j = 0; j < m; ++j)
        {
            const Index k = sides[j][0], before = t[wrap(j - 1)];
            if (e[j] == 0)
            {
                grid(centre, before, k, t[j]);
                continue;
            }
            const Index p = point();
            spoke(centre, p, e[j]);
            spoke(p, a[j], s[wrap(j - 1)]);
            spoke(p, b[j], s[j]);
            grid(centre, before, a[j], p);
            grid(p, a[j], k, b[j]);
            grid(p, b[j], t[j], centre);
        }
    }
};

bool candidateFits(const Candidate& c, int n)
{
    const std::vector<int> l = countsFor(c.corners, n);
    for (size_t i = 0; i < l.size(); ++i)
        if (l[i] - 2 * c.padding < 1)
            return false;
    return true;
}

Builder build(const PatchBoundary& b, const Candidate& c)
{
    LILA_ZONE();

    Builder out;
    out.pos = b.points;
    const int n = int(b.points.size());
    if (c.single)
    {
        out.faces.push_back({0, 1, 2, 3});
        return out;
    }
    if (!candidateFits(c, n))
    {
        out.broken = true;
        return out;
    }
    std::vector<Chain> sides(c.corners.size());
    for (size_t i = 0; i < c.corners.size(); ++i)
    {
        const int from = c.corners[i], to = i + 1 < c.corners.size() ? c.corners[i + 1] : c.corners[0] + n;
        for (int k = from; k <= to; ++k)
            sides[i].push_back(Index(k % n));
    }
    for (int r = 0; r < c.padding; ++r)
        sides = out.ring(sides);
    if (c.grid)
    {
        for (const Chain& side : sides)
            out.link(side);
        out.grid(sides[0][0], sides[1][0], sides[2][0], sides[3][0]);
    }
    else
        out.star(sides, c.s, c.e);
    return out;
}

std::vector<std::vector<Index>> neighbours(size_t count, const std::vector<std::vector<Index>>& faces)
{
    std::vector<std::vector<Index>> out(count);
    for (const auto& f : faces)
        for (size_t i = 0; i < f.size(); ++i)
        {
            const Index a = f[i], b = f[(i + 1) % f.size()];
            out[a].push_back(b);
            out[b].push_back(a);
        }
    for (auto& v : out)
    {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return out;
}

/** Uniform-weight harmonic positions for every point at or past `fixed` that is not pinned. */
void harmonic(std::vector<glm::dvec3>& pos, const std::vector<std::vector<Index>>& nbr, Index fixed, const std::vector<uint8_t>& pinned)
{
    LILA_ZONE();

    const Index count = Index(pos.size());
    std::vector<int> slot(count, -1);
    int unknowns = 0;
    for (Index v = fixed; v < count; ++v)
        if (!pinned[v])
            slot[v] = unknowns++;
    if (unknowns == 0)
        return;
    std::vector<Eigen::Triplet<double>> trip;
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(unknowns, 3);
    for (Index v = 0; v < count; ++v)
    {
        const int row = slot[v];
        if (row < 0)
            continue;
        trip.emplace_back(row, row, double(nbr[v].size()));
        for (Index u : nbr[v])
        {
            if (slot[u] >= 0)
                trip.emplace_back(row, slot[u], -1.0);
            else
                for (int k = 0; k < 3; ++k)
                    rhs(row, k) += pos[u][k];
        }
    }
    Eigen::SparseMatrix<double> a(unknowns, unknowns);
    a.setFromTriplets(trip.begin(), trip.end());
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(a);
    if (solver.info() != Eigen::Success)
        return;
    const Eigen::MatrixXd x = solver.solve(rhs);
    if (solver.info() != Eigen::Success || !x.allFinite())
        return;
    for (Index v = 0; v < count; ++v)
        if (slot[v] >= 0)
            pos[v] = glm::dvec3(x(slot[v], 0), x(slot[v], 1), x(slot[v], 2));
}

glm::dvec3 quadNormal(const std::vector<glm::dvec3>& pos, const std::vector<Index>& f)
{
    return glm::cross(pos[f[2]] - pos[f[0]], pos[f[3]] - pos[f[1]]);
}

std::vector<Index> polesOf(size_t count, Index fixed, const std::vector<std::vector<Index>>& faces)
{
    std::vector<int> valence(count, 0);
    for (const auto& f : faces)
        for (Index v : f)
            ++valence[v];
    std::vector<Index> out;
    for (Index v = fixed; v < count; ++v)
        if (valence[v] != 4)
            out.push_back(v);
    return out;
}

/** Moves the pins part way toward the pull and re-solves the rest around them, backing off while any quad flips. */
void lean(std::vector<glm::dvec3>& pos, const std::vector<std::vector<Index>>& faces, const std::vector<std::vector<Index>>& nbr,
          Index fixed, const std::vector<Index>& pins, const glm::dvec3& pull, std::vector<uint8_t>& pinned)
{
    LILA_ZONE();

    std::vector<glm::dvec3> ref(faces.size());
    for (size_t f = 0; f < faces.size(); ++f)
        ref[f] = quadNormal(pos, faces[f]);
    const std::vector<glm::dvec3> start = pos;
    for (double t = 0.5; t > 0.05; t *= 0.5)
    {
        pos = start;
        for (Index p : pins)
        {
            pos[p] += (pull - pos[p]) * t;
            pinned[p] = 1;
        }
        harmonic(pos, nbr, fixed, pinned);
        bool folded = false;
        for (size_t f = 0; f < faces.size() && !folded; ++f)
            folded = glm::dot(quadNormal(pos, faces[f]), ref[f]) <= 0.0 && glm::dot(ref[f], ref[f]) > 0.0;
        if (!folded)
            return;
    }
    pos = start;
    for (Index p : pins)
        pinned[p] = 0;
}

/** Puts the interior on the surface, then evens it out along the surface without letting any quad flip. */
void relaxOnSurface(std::vector<glm::dvec3>& pos, const std::vector<std::vector<Index>>& faces, const std::vector<std::vector<Index>>& nbr,
                    Index fixed, const std::vector<uint8_t>& pinned, const geo::MeshBvh& surface, int iterations)
{
    LILA_ZONE();

    auto project = [&](const glm::dvec3& p) {
        geo::BvhClosest hit;
        return surface.closestPoint(p, hit) ? hit.point : p;
    };
    const Index count = Index(pos.size());
    for (Index v = fixed; v < count; ++v)
        pos[v] = project(pos[v]);
    std::vector<std::vector<Index>> around(count);
    for (size_t f = 0; f < faces.size(); ++f)
        for (Index v : faces[f])
            around[v].push_back(Index(f));
    for (int it = 0; it < iterations; ++it)
        for (Index v = fixed; v < count; ++v)
        {
            if (pinned[v] || nbr[v].empty())
                continue;
            glm::dvec3 normal(0.0), avg(0.0);
            for (Index f : around[v])
                normal += quadNormal(pos, faces[f]);
            const double len = glm::length(normal);
            if (len <= 0.0)
                continue;
            normal /= len;
            for (Index u : nbr[v])
                avg += pos[u];
            avg /= double(nbr[v].size());
            glm::dvec3 step = avg - pos[v];
            step -= normal * glm::dot(step, normal);
            const glm::dvec3 old = pos[v];
            pos[v] = project(old + step);
            for (Index f : around[v])
                if (glm::dot(quadNormal(pos, faces[f]), normal) <= 0.0)
                {
                    pos[v] = old;
                    break;
                }
        }
}

/** Point at a fractional index along the closed boundary. */
glm::dvec3 alongLoop(const std::vector<glm::dvec3>& pts, double t)
{
    const int n = int(pts.size());
    const double wrapped = std::fmod(t, double(n));
    const int i = std::clamp(int(wrapped), 0, n - 1);
    const double f = wrapped - i;
    return pts[i] * (1.0 - f) + pts[(i + 1) % n] * f;
}

/** Boundary offsets from a side's first corner to each of the star's points along it. */
std::vector<int> sideMarks(const Candidate& c, int side)
{
    const int m = int(c.corners.size()), p = c.padding;
    const int before = c.s[(side + m - 1) % m], after = c.s[(side + 1) % m], next = (side + 1) % m;
    const int l = 2 * p + before + after + c.e[side] + c.e[next];
    return {0, p, p + before, p + before + c.e[side], l - p - after, l - p, l};
}

/**
 * A star candidate scaled down by `factor` over the same boundary resampled to match, so its poles land about where
 * the full-size candidate's would, for a fraction of the solve.
 */
std::pair<PatchBoundary, Candidate> scaledDown(const PatchBoundary& b, const Candidate& c, double factor)
{
    auto shrink = [factor](int v) { return v > 0 ? std::max(1, int(std::lround(v / factor))) : 0; };
    Candidate q = c;
    q.padding = shrink(c.padding);
    for (int i = 0; i < kMaxSides; ++i)
    {
        q.s[i] = shrink(c.s[i]);
        q.e[i] = shrink(c.e[i]);
    }
    PatchBoundary out;
    for (size_t i = 0; i < c.corners.size(); ++i)
    {
        const std::vector<int> from = sideMarks(c, int(i)), to = sideMarks(q, int(i));
        out.corners.push_back(int(out.points.size()));
        for (int k = 0; k < to.back(); ++k)
        {
            size_t j = 0;
            while (!(to[j] <= k && k < to[j + 1]))
                ++j;
            const double t = from[j] + double(k - to[j]) * (from[j + 1] - from[j]) / double(to[j + 1] - to[j]);
            out.points.push_back(alongLoop(b.points, c.corners[i] + t));
        }
    }
    q.corners = out.corners;
    return {std::move(out), std::move(q)};
}

/** Where a candidate's poles land on average once placed, measured on a scaled-down copy when the patch is large. */
std::optional<glm::dvec3> poleCentre(const PatchBoundary& b, const Candidate& c)
{
    LILA_ZONE();

    constexpr double kProxyPoints = 64.0;
    if (c.grid || c.single)
        return std::nullopt;
    const double factor = double(b.points.size()) / kProxyPoints;
    std::pair<PatchBoundary, Candidate> small;
    if (factor > 1.0)
        small = scaledDown(b, c, factor);
    const PatchBoundary& boundary = factor > 1.0 ? small.first : b;
    Builder trial = build(boundary, factor > 1.0 ? small.second : c);
    if (trial.broken)
        return std::nullopt;
    const Index n = Index(boundary.points.size());
    harmonic(trial.pos, neighbours(trial.pos.size(), trial.faces), n, std::vector<uint8_t>(trial.pos.size(), 0));
    const std::vector<Index> poles = polesOf(trial.pos.size(), n, trial.faces);
    if (poles.empty())
        return std::nullopt;
    glm::dvec3 mean(0.0);
    for (Index p : poles)
        mean += trial.pos[p];
    return mean / double(poles.size());
}

std::string validate(const PatchBoundary& b)
{
    const int n = int(b.points.size());
    if (n < 3)
        return "Patch boundary needs at least three points";
    if (b.corners.size() < 2 || b.corners.size() > size_t(kMaxSides))
        return "Patch boundary needs two to six corners";
    for (size_t i = 0; i < b.corners.size(); ++i)
        if (b.corners[i] < 0 || b.corners[i] >= n || (i > 0 && b.corners[i] <= b.corners[i - 1]))
            return "Patch corners must be ascending indices into the boundary";
    if (n % 2 != 0)
        return "Patch boundary needs an even number of edges to fill with quads";
    return {};
}
} // namespace

std::vector<int> patchSideCounts(const PatchBoundary& boundary)
{
    const int n = int(boundary.points.size());
    for (size_t i = 0; i < boundary.corners.size(); ++i)
        if (boundary.corners[i] < 0 || boundary.corners[i] >= n || (i > 0 && boundary.corners[i] <= boundary.corners[i - 1]))
            return {};
    if (boundary.corners.empty())
        return {};
    return countsFor(boundary.corners, n);
}

bool patchSidesFillable(const std::vector<int>& sideCounts)
{
    if (sideCounts.size() < 2 || sideCounts.size() > size_t(kMaxSides))
        return false;
    int total = 0;
    for (int c : sideCounts)
    {
        if (c < 1)
            return false;
        total += c;
    }
    // Four edges make one quad; from six up a pattern always fits, if need be by letting corners go.
    return total % 2 == 0 && (total == 4 || total >= 6);
}

PatchResult fillPatch(const PatchBoundary& boundary, const PatchOptions& opts, const geo::MeshBvh* surface)
{
    LILA_ZONE();

    PatchResult result;
    result.error = validate(boundary);
    if (!result.ok())
        return result;
    const Index n = Index(boundary.points.size());
    std::vector<Candidate> candidates;
    if (n == 4)
    {
        Candidate quad;
        quad.single = true;
        candidates.push_back(quad);
    }
    else
        candidates = findCandidates(boundary, opts.padding);
    if (candidates.empty())
    {
        result.error = "No quad pattern fits these side counts";
        return result;
    }
    result.variants = int(candidates.size());
    result.variant = ((opts.variant % result.variants) + result.variants) % result.variants;

    if (opts.pull && result.variants > 1)
    {
        double best = std::numeric_limits<double>::max();
        // A pull moves poles around; it never trades a layout for a less regular one.
        for (int i = 0; i < result.variants; ++i)
        {
            if (candidates[i].irregular != candidates[0].irregular)
                continue;
            const std::optional<glm::dvec3> centre = poleCentre(boundary, candidates[i]);
            if (!centre)
                continue;
            const double d = glm::length(*centre - *opts.pull);
            if (d < best)
            {
                best = d;
                result.variant = i;
            }
        }
    }

    Builder built = build(boundary, candidates[result.variant]);
    if (built.broken)
    {
        result.error = "Patch pattern failed to build";
        return result;
    }
    const auto nbr = neighbours(built.pos.size(), built.faces);
    std::vector<uint8_t> pinned(built.pos.size(), 0);
    harmonic(built.pos, nbr, n, pinned);
    result.poles = polesOf(built.pos.size(), n, built.faces);
    if (opts.pull && built.pos.size() > n)
    {
        std::vector<Index> pins = result.poles;
        if (pins.empty())
        {
            Index nearest = n;
            for (Index v = n; v < Index(built.pos.size()); ++v)
                if (glm::length(built.pos[v] - *opts.pull) < glm::length(built.pos[nearest] - *opts.pull))
                    nearest = v;
            pins.push_back(nearest);
        }
        lean(built.pos, built.faces, nbr, n, pins, *opts.pull, pinned);
    }
    if (surface && !surface->empty() && opts.lift && glm::length(*opts.lift) > 0.0)
    {
        const glm::dvec3 dir = glm::normalize(*opts.lift);
        for (Index v = n; v < Index(built.pos.size()); ++v)
        {
            // Out along the cap first; back the other way only when nothing lies ahead.
            geo::BvhHit hit;
            if (surface->raycast(built.pos[v], dir, hit) || surface->raycast(built.pos[v], -dir, hit))
                built.pos[v] = hit.point;
        }
    }
    if (surface && !surface->empty())
        relaxOnSurface(built.pos, built.faces, nbr, n, pinned, *surface, std::max(opts.relaxIterations, 0));

    result.positions = std::move(built.pos);
    result.faces = std::move(built.faces);
    return result;
}

std::vector<int> patchCorners(const std::vector<glm::dvec3>& loop, int wanted, double minTurnDegrees)
{
    LILA_ZONE();

    const int n = int(loop.size());
    wanted = std::min(std::clamp(wanted, 2, kMaxSides), n);
    if (wanted <= 0)
        return {};
    std::vector<double> turn(n), arc(n, 0.0);
    for (int i = 0; i < n; ++i)
        turn[i] = turnDegreesAt(loop, i);
    for (int i = 1; i < n; ++i)
        arc[i] = arc[i - 1] + glm::length(loop[i] - loop[i - 1]);
    const double total = arc[n - 1] + glm::length(loop[0] - loop[n - 1]);

    std::vector<int> order(n);
    for (int i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return turn[a] > turn[b]; });
    std::vector<int> picked;
    for (int i : order)
        if (int(picked.size()) < wanted && turn[i] > minTurnDegrees)
            picked.push_back(i);
    if (picked.empty())
        picked.push_back(order[0]);
    std::vector<uint8_t> used(n, 0);
    for (int i : picked)
        used[i] = 1;
    while (int(picked.size()) < wanted)
    {
        int best = -1;
        double bestGap = -1.0;
        for (int i = 0; i < n; ++i)
        {
            if (used[i])
                continue;
            double gap = std::numeric_limits<double>::max();
            for (int p : picked)
            {
                const double d = std::abs(arc[i] - arc[p]);
                gap = std::min(gap, std::min(d, total - d));
            }
            if (gap > bestGap)
            {
                bestGap = gap;
                best = i;
            }
        }
        used[best] = 1;
        picked.push_back(best);
    }
    std::sort(picked.begin(), picked.end());
    return picked;
}
} // namespace lila::retopo
