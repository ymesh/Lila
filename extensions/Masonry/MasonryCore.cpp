// Masonry layout: courses of stones laid along a footprint in a chosen bond, corners turned by alternating which wall
// runs through, openings with their arches, lintels and sills, courses stepped over sloping ground and under a sloping
// top, overlaps cut back, then a little wander so it doesn't look machine laid.

#include "MasonryCore.h"
#include "MasonryArch.h"
#include "MasonryRuin.h"

#include <lila/Boolean.h>

#include <glm/gtc/matrix_transform.hpp>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>

namespace masonry
{
namespace
{
glm::dvec3 flat(const glm::dvec3& v) { return {v.x, 0.0, v.z}; }

glm::dvec3 flatDir(const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 d = flat(b - a);
    const double len = glm::length(d);
    return len > 1e-12 ? d / len : glm::dvec3(1, 0, 0);
}

/** Left of a direction in the ground plane. */
glm::dvec3 leftOf(const glm::dvec3& d) { return {-d.z, 0.0, d.x}; }

double crossY(const glm::dvec3& a, const glm::dvec3& b) { return a.z * b.x - a.x * b.z; }

glm::dvec3 miterOffsetImpl(const glm::dvec3& dIn, const glm::dvec3& dOut, double dist)
{
    const glm::dvec3 nIn = leftOf(dIn), nOut = leftOf(dOut);
    glm::dvec3 m = nIn + nOut;
    const double len = glm::length(m);
    if (len < 1e-9)
        return nOut * dist;
    m /= len;
    return m * (dist / std::max(glm::dot(m, nIn), 0.25));
}

/** A polyline measured by ground length, sampled at any distance (wrapping when cyclic), with the ground and wall top under each point. */
struct Line
{
    std::vector<glm::dvec3> pts;
    std::vector<double> tops;
    std::vector<double> at; // cumulative length at each point
    bool cyclic = false;

    void build()
    {
        at.assign(pts.size(), 0.0);
        for (size_t k = 1; k < pts.size(); ++k)
            at[k] = at[k - 1] + glm::length(flat(pts[k] - pts[k - 1]));
        if (cyclic && pts.size() > 1)
        {
            pts.push_back(pts.front());
            tops.push_back(tops.front());
            at.push_back(at.back() + glm::length(flat(pts.front() - pts[pts.size() - 2])));
        }
    }
    double length() const { return at.empty() ? 0.0 : at.back(); }

    /** Segment and fraction along it at distance u. */
    std::pair<size_t, double> locate(double u) const
    {
        const double total = length();
        if (pts.size() < 2 || total <= 0.0)
            return {0, 0.0};
        if (cyclic)
            u = std::fmod(std::fmod(u, total) + total, total);
        if (u <= 0.0)
            return {0, u / std::max(at[1], 1e-9)};
        if (u >= total)
        {
            const size_t k = pts.size() - 2;
            return {k, 1.0 + (u - total) / std::max(at[k + 1] - at[k], 1e-9)};
        }
        const size_t k = std::min(size_t(std::upper_bound(at.begin(), at.end(), u) - at.begin()) - 1, pts.size() - 2);
        const double seg = at[k + 1] - at[k];
        return {k, seg > 0.0 ? (u - at[k]) / seg : 0.0};
    }
    double paramAt(size_t seg, double frac) const
    {
        if (pts.size() < 2)
            return 0.0;
        seg = std::min(seg, pts.size() - 2);
        return at[seg] + frac * (at[seg + 1] - at[seg]);
    }
    glm::dvec3 sample(double u) const
    {
        if (pts.size() < 2 || length() <= 0.0)
            return pts.empty() ? glm::dvec3(0.0) : pts.front();
        const auto [k, t] = locate(u);
        return pts[k] + (pts[k + 1] - pts[k]) * t;
    }
    double baseAt(double u) const
    {
        if (pts.size() < 2)
            return pts.empty() ? 0.0 : pts.front().y;
        const auto [k, t] = locate(u);
        return glm::mix(pts[k].y, pts[k + 1].y, std::clamp(t, 0.0, 1.0));
    }
    double topAt(double u) const
    {
        if (pts.size() < 2)
            return tops.empty() ? 0.0 : tops.front();
        const auto [k, t] = locate(u);
        return glm::mix(tops[k], tops[k + 1], std::clamp(t, 0.0, 1.0));
    }
    /** Distance along the line of the point on it nearest p, in plan. */
    double closest(const glm::dvec3& p) const
    {
        double best = 0.0, bestD = std::numeric_limits<double>::max();
        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            const glm::dvec3 a = flat(pts[k]), d = flat(pts[k + 1]) - a;
            const double len2 = glm::dot(d, d);
            const double t = len2 > 0.0 ? std::clamp(glm::dot(flat(p) - a, d) / len2, 0.0, 1.0) : 0.0;
            const double dist = glm::length(flat(p) - (a + d * t));
            if (dist < bestD)
            {
                bestD = dist;
                best = at[k] + t * (at[k + 1] - at[k]);
            }
        }
        return best;
    }
    glm::dvec3 dirAt(double u) const
    {
        if (pts.size() < 2)
            return {1, 0, 0};
        const auto [k, t] = locate(u);
        return flatDir(pts[k], pts[k + 1]);
    }
};

/** A stretch of wall between corners or ends; a closed footprint with no corners is one cyclic run. */
struct Run
{
    std::vector<glm::dvec3> pts;
    std::vector<double> tops;
    bool cyclic = false;
    int startCorner = -1, endCorner = -1; // footprint vertex, or -1 for an open end
    size_t first = 0;                     // footprint vertex the run starts at
    glm::dvec3 before{0.0}, after{0.0};   // directions of the neighbouring runs at the corners
};

struct Clean
{
    std::vector<glm::dvec3> pts; // y is the ground
    std::vector<double> tops;
    bool closed = false;
    double base = 0.0;  // lowest ground
    double top = 0.0;   // highest top
    bool level = true;  // one ground height and one top everywhere
};

Clean clean(const Footprint& f, double height)
{
    Clean c;
    c.closed = f.closed;
    for (size_t k = 0; k < f.points.size(); ++k)
    {
        const glm::dvec3& p = f.points[k];
        const double h = k < f.heights.size() ? std::max(0.0, f.heights[k]) : height;
        if (c.pts.empty() || glm::length(flat(p - c.pts.back())) > 1e-3)
        {
            c.pts.push_back(p);
            c.tops.push_back(p.y + h);
        }
    }
    if (c.closed && c.pts.size() > 2 && glm::length(flat(c.pts.front() - c.pts.back())) <= 1e-3)
    {
        c.pts.pop_back();
        c.tops.pop_back();
    }
    if (c.pts.size() < 3)
        c.closed = false;
    c.base = 1e300;
    c.top = -1e300;
    for (size_t k = 0; k < c.pts.size(); ++k)
    {
        c.base = std::min(c.base, c.pts[k].y);
        c.top = std::max(c.top, c.tops[k]);
    }
    for (size_t k = 0; k < c.pts.size(); ++k)
        c.level = c.level && std::abs(c.pts[k].y - c.base) < 1e-6 && std::abs(c.tops[k] - c.top) < 1e-6;
    return c;
}

std::vector<Run> splitRuns(const Clean& c, double cornerAngleDeg)
{
    const size_t n = c.pts.size();
    std::vector<Run> runs;
    if (n < 2)
        return runs;
    const double cosLimit = std::cos(glm::radians(std::clamp(cornerAngleDeg, 1.0, 179.0)));
    auto dir = [&](size_t i) { return flatDir(c.pts[i % n], c.pts[(i + 1) % n]); };
    std::vector<size_t> corners;
    const size_t first = c.closed ? 0 : 1, last = c.closed ? n : n - 1;
    for (size_t v = first; v < last; ++v)
        if (glm::dot(dir((v + n - 1) % n), dir(v)) < cosLimit)
            corners.push_back(v);

    if (c.closed && corners.empty())
    {
        Run r;
        r.pts = c.pts;
        r.tops = c.tops;
        r.cyclic = true;
        runs.push_back(std::move(r));
        return runs;
    }
    // Run ends: corners, plus the two ends of an open footprint.
    std::vector<size_t> stops = corners;
    if (!c.closed)
    {
        stops.insert(stops.begin(), 0);
        stops.push_back(n - 1);
    }
    const size_t count = c.closed ? stops.size() : stops.size() - 1;
    for (size_t k = 0; k < count; ++k)
    {
        const size_t a = stops[k], b = c.closed ? stops[(k + 1) % stops.size()] : stops[k + 1];
        Run r;
        r.first = a;
        for (size_t v = a;; v = (v + 1) % n)
        {
            r.pts.push_back(c.pts[v]);
            r.tops.push_back(c.tops[v]);
            if (v == b && r.pts.size() > 1)
                break;
        }
        const bool startIsCorner = c.closed || k > 0;
        const bool endIsCorner = c.closed || k + 1 < count;
        if (startIsCorner)
        {
            r.startCorner = int(a);
            r.before = dir((a + n - 1) % n);
        }
        if (endIsCorner)
        {
            r.endCorner = int(b);
            r.after = dir(b);
        }
        runs.push_back(std::move(r));
    }
    return runs;
}

/** Offset of run point i sideways by dist (left positive), mitred at bends and at corners with the next run. */
glm::dvec3 runOffset(const Run& r, size_t i, double dist)
{
    const size_t n = r.pts.size();
    glm::dvec3 dIn, dOut;
    if (r.cyclic)
    {
        dIn = flatDir(r.pts[(i + n - 1) % n], r.pts[i]);
        dOut = flatDir(r.pts[i], r.pts[(i + 1) % n]);
    }
    else
    {
        dOut = i + 1 < n ? flatDir(r.pts[i], r.pts[i + 1]) : (r.endCorner >= 0 ? r.after : flatDir(r.pts[i - 1], r.pts[i]));
        dIn = i > 0 ? flatDir(r.pts[i - 1], r.pts[i]) : (r.startCorner >= 0 ? r.before : dOut);
    }
    return miterOffsetImpl(dIn, dOut, dist);
}

/** The run's line offset sideways by dist (left positive); point k of the line is point k of the run. */
Line offsetLine(const Run& r, double dist)
{
    Line l;
    l.cyclic = r.cyclic;
    for (size_t i = 0; i < r.pts.size(); ++i)
        l.pts.push_back(r.pts[i] + runOffset(r, i, dist));
    l.tops = r.tops;
    l.build();
    return l;
}

struct Unit
{
    double width;
    Role role;
    double depth;
};

struct Pattern
{
    std::vector<Unit> lead;
    std::vector<Unit> cycle;
    const Unit& operator[](size_t k) const { return k < lead.size() ? lead[k] : cycle[(k - lead.size()) % cycle.size()]; }
};

/** Random lengths for the rough bonds, seeded per wall and course; dry stone puts a through stone every few. */
Pattern roughPattern(Bond bond, int course, const Units& u, double stretcherDepth, double headerDepth, uint64_t seed, bool top)
{
    const double lo = bond == Bond::Ashlar ? 0.55 : (bond == Bond::Rubble ? 0.4 : 0.5);
    const double hi = bond == Bond::Ashlar ? 1.5 : (bond == Bond::Rubble ? 1.3 : 1.6);
    Pattern p;
    for (int k = 0; k < 48; ++k)
    {
        const double r = unitRandom(mixKey(seed, uint64_t(course), uint64_t(k), 61));
        Unit unit{u.length * (lo + (hi - lo) * r), Role::Stretcher, stretcherDepth};
        if (bond == Bond::DryStone && (top || (k % 5 == 4 && course % 2 == 1)))
            unit = {std::min(u.length, std::max(u.depth, u.length * (0.5 + 0.4 * r))), Role::Header, headerDepth};
        p.cycle.push_back(unit);
    }
    if (course & 1)
        p.lead.push_back({p.cycle[0].width * 0.5, p.cycle[0].role == Role::Header ? Role::Header : Role::Bat, p.cycle[0].depth});
    return p;
}

/** Herringbone: stones at 45 degrees side by side; a unit is how far one stone moves the next along the course. */
double herringboneUnit(const Units& u) { return (u.height + u.joint) * std::sqrt(2.0) - u.joint; }

Pattern pattern(Bond bond, int course, const Units& u, double stretcherDepth, double headerDepth)
{
    const double j = u.joint;
    const Unit S{u.length, Role::Stretcher, stretcherDepth};
    const Unit H{u.depth, Role::Header, headerDepth};
    const Unit B{(u.length - j) * 0.5, Role::Bat, stretcherDepth};
    const Unit Q{(u.depth - j) * 0.5, Role::Closer, headerDepth};
    const bool odd = (course & 1) != 0;
    switch (bond)
    {
    case Bond::Stack: return {{}, {S}};
    case Bond::English: return odd ? Pattern{{H, Q}, {H}} : Pattern{{}, {S}};
    case Bond::Flemish: return odd ? Pattern{{H, Q}, {S, H}} : Pattern{{}, {S, H}};
    case Bond::Header: return odd ? Pattern{{Q}, {H}} : Pattern{{}, {H}};
    case Bond::GardenWall:
        if (course % 4 == 3)
            return {{H, Q}, {H}};
        return odd ? Pattern{{B}, {S}} : Pattern{{}, {S}};
    case Bond::Herringbone:
    {
        const Unit R{herringboneUnit(u), Role::Stretcher, stretcherDepth};
        return odd ? Pattern{{{R.width * 0.5, Role::Stretcher, stretcherDepth}}, {R}} : Pattern{{}, {R}};
    }
    case Bond::Running:
    default: return odd ? Pattern{{B}, {S}} : Pattern{{}, {S}};
    }
}

struct Interval
{
    double a, b;
    Unit unit;
};

/**
 * One course on one face: units laid in from both ends toward a point near the middle, the gap left there filled by
 * one cut stone, so cuts land mid wall instead of at a corner.
 */
std::vector<Interval> layCourse(const Pattern& pat, double t0, double t1, double mid, double joint, double minLen)
{
    std::vector<Interval> fromStart, fromEnd, out;
    if (t1 - t0 < minLen)
        return out;
    // On a span shorter than the drift, the middle could fall outside it and an end stone would run past the far end.
    mid = std::clamp(mid, t0, t1);
    double pos = t0;
    for (size_t k = 0; k < (size_t(1) << 20); ++k)
    {
        const Unit& u = pat[k];
        if (pos + u.width > mid)
            break;
        fromStart.push_back({pos, pos + u.width, u});
        pos += u.width + joint;
    }
    pos = t1;
    for (size_t k = 0; k < (size_t(1) << 20); ++k)
    {
        const Unit& u = pat[k];
        if (pos - u.width < mid)
            break;
        fromEnd.push_back({pos - u.width, pos, u});
        pos -= u.width + joint;
    }
    const double a = fromStart.empty() ? t0 - joint : fromStart.back().b;
    const double b = fromEnd.empty() ? t1 + joint : fromEnd.back().a;
    const double fill = b - a - 2.0 * joint;
    const Unit& neighbour = !fromStart.empty() ? fromStart.back().unit : (!fromEnd.empty() ? fromEnd.back().unit : pat[0]);
    std::vector<Interval> fillers;
    if (fill >= minLen)
    {
        const Unit filler{0.0, Role::Filler, neighbour.depth};
        const double widest = std::max(pat[0].width, pat.cycle[0].width);
        if (fill > widest + minLen + joint)
        {
            const double w = (fill - joint) * 0.5;
            fillers.push_back({a + joint, a + joint + w, filler});
            fillers.push_back({b - joint - w, b - joint, filler});
        }
        else if (std::abs(fill - pat[0].width) <= pat[0].width * 0.1)
            fillers.push_back({a + joint, b - joint, {0.0, pat[0].role, pat[0].depth}});
        else
            fillers.push_back({a + joint, b - joint, filler});
    }
    else if (fill > -joint)
    {
        // Too little room for a stone: the stones either side take it up instead.
        const double extra = fill + joint;
        if (!fromStart.empty() && !fromEnd.empty())
        {
            fromStart.back().b += extra * 0.5;
            fromEnd.back().a -= extra * 0.5;
        }
        else if (!fromStart.empty())
            fromStart.back().b += extra;
        else if (!fromEnd.empty())
            fromEnd.back().a -= extra;
    }
    out = std::move(fromStart);
    out.insert(out.end(), fillers.begin(), fillers.end());
    out.insert(out.end(), fromEnd.rbegin(), fromEnd.rend());
    return out;
}

// --- Oriented boxes and wedges ---

/** Where along one of s's axes another stone lies, relative to s's centre. */
std::pair<double, double> projectOnto(const Stone& s, int axis, const Stone& other)
{
    double lo = 1e300, hi = -1e300;
    for (const glm::dvec3& p : stoneCorners(other))
    {
        const double t = glm::dot(p - s.center, s.frame[axis]);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    return {lo, hi};
}

/**
 * Cuts s back along its length or depth so it clears `other` by a joint, keeping the larger side. False when what would
 * remain is too small to be a stone.
 */
bool trimAgainst(Stone& s, const Stone& other, double joint, double minLength, double minDepth)
{
    int bestAxis = -1;
    double bestLo = 0.0, bestHi = 0.0, bestKeep = -1.0;
    for (int axis : {0, 2})
    {
        const double e = s.size[axis] * 0.5;
        const auto [lo, hi] = projectOnto(s, axis, other);
        const double cutLo = lo - joint, cutHi = hi + joint;
        if (cutHi <= -e || cutLo >= e)
            continue;
        const double leftHi = std::min(e, cutLo), rightLo = std::max(-e, cutHi);
        const double left = leftHi + e, right = e - rightLo;
        const double keep = std::max(left, right);
        const double minimum = axis == 0 ? minLength : minDepth;
        if (keep < minimum || keep >= 2.0 * e - 1e-9)
            continue;
        const double frac = keep / (2.0 * e);
        if (frac > bestKeep)
        {
            bestKeep = frac;
            bestAxis = axis;
            if (left >= right)
            {
                bestLo = -e;
                bestHi = leftHi;
            }
            else
            {
                bestLo = rightLo;
                bestHi = e;
            }
        }
    }
    if (bestAxis < 0)
        return false;
    s.center += s.frame[bestAxis] * ((bestLo + bestHi) * 0.5);
    s.size[bestAxis] = bestHi - bestLo;
    if (bestAxis == 0)
        s.role = Role::Closer;
    return true;
}

/** Stones in a coarse grid, filled as they are laid, for finding the ones a new stone might touch. */
struct Grid
{
    double cell;
    std::unordered_map<uint64_t, std::vector<int>> cells;

    template <class Fn> void forCells(const Stone& s, double pad, Fn&& fn) const
    {
        const glm::dvec3 e = halfExtentAabb(s) + pad;
        const glm::dvec3 lo = glm::floor((s.center - e) / cell), hi = glm::floor((s.center + e) / cell);
        for (int64_t x = int64_t(lo.x); x <= int64_t(hi.x); ++x)
            for (int64_t y = int64_t(lo.y); y <= int64_t(hi.y); ++y)
                for (int64_t z = int64_t(lo.z); z <= int64_t(hi.z); ++z)
                    fn(StoneGrid::pack(x, y, z));
    }
    void insert(const Stone& s, int index)
    {
        forCells(s, 0.0, [&](uint64_t k) { cells[k].push_back(index); });
    }
    std::vector<int> around(const Stone& s, double pad) const
    {
        std::vector<int> out;
        forCells(s, pad, [&](uint64_t k) {
            auto it = cells.find(k);
            if (it != cells.end())
                out.insert(out.end(), it->second.begin(), it->second.end());
        });
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
};

glm::dmat3 rotationXyz(double ax, double ay, double az)
{
    const glm::dmat4 r = glm::rotate(glm::rotate(glm::rotate(glm::dmat4(1.0), az, glm::dvec3(0, 0, 1)), ay, glm::dvec3(0, 1, 0)), ax,
                                     glm::dvec3(1, 0, 0));
    return glm::dmat3(r);
}

int faceRank(Face f) { return f == Face::Through ? 0 : (f == Face::Outer ? 1 : 2); }

/** Course heights of a wall: course c sits on base + c * pitch. */
struct Courses
{
    int count = 0;
    double pitch = 0.0;
    double h = 0.0; // stone height, pitch less the bed joint
    double base = 0.0;
};

Courses coursesOf(const Clean& c, const WallParams& p)
{
    Courses k;
    const double j = std::max(0.0, p.stone.joint);
    const double total = c.top - c.base;
    k.base = c.base;
    if (total <= 0.0)
        return k;
    // A herringbone course is as tall as a stone standing at 45 degrees.
    const double stone = p.bond == Bond::Herringbone ? (p.stone.length + p.stone.height) * std::sqrt(0.5) : p.stone.height;
    k.count = std::max(1, int(std::lround(total / (stone + j))));
    k.pitch = total / k.count;
    k.h = std::max(0.5, k.pitch - j);
    return k;
}

/** Wall thickness at a height above the lowest ground, narrowed by the batter on both faces. */
double thicknessAt(const WallParams& p, double above)
{
    const double t = std::max(p.thickness, 1.0);
    if (p.batter <= 0.0)
        return t;
    return std::max(t * 0.25, t - 2.0 * std::max(0.0, above) * std::tan(glm::radians(std::clamp(p.batter, 0.0, 45.0))));
}

bool leftIsOuterOf(const Clean& c)
{
    if (!c.closed)
        return true;
    double area = 0.0;
    for (size_t i = 0; i < c.pts.size(); ++i)
    {
        const glm::dvec3& a = c.pts[i];
        const glm::dvec3& b = c.pts[(i + 1) % c.pts.size()];
        area += a.x * b.z - b.x * a.z;
    }
    return area < 0.0;
}

/** A stretch of course to lay, on a face line, and whether its ends meet an opening. */
struct Span
{
    double a, b;
    bool jambA = false, jambB = false;
};

/** Where on a face line a course may go: above the ground, under the top, as sampled every step. */
std::vector<Span> groundSpans(const Line& line, bool level, double y, double h, double pitch, double step, bool* loop, double* shift)
{
    const double total = line.length();
    *loop = false;
    *shift = 0.0;
    auto valid = [&](double u) { return line.baseAt(u) < y + 0.9 * h && line.topAt(u) >= y + 0.5 * pitch; };
    if (level)
    {
        if (!valid(0.0))
            return {};
        *loop = line.cyclic;
        return {{0.0, total}};
    }
    const int n = std::max(2, int(std::ceil(total / std::max(step, 1e-3))));
    std::vector<char> ok(size_t(n) + 1);
    for (int k = 0; k <= n; ++k)
        ok[size_t(k)] = valid(total * k / n);
    std::vector<Span> out;
    if (line.cyclic)
    {
        int bad = -1;
        for (int k = 0; k < n && bad < 0; ++k)
            if (!ok[size_t(k)])
                bad = k;
        if (bad < 0)
        {
            *loop = true;
            return {{0.0, total}};
        }
        *shift = total * bad / n;
        int start = -1;
        for (int i = 0; i <= n; ++i)
        {
            const int k = (bad + i) % n;
            const double u = total * (bad + i) / n;
            if (ok[size_t(k)] && i < n)
            {
                if (start < 0)
                    start = i;
            }
            else if (start >= 0)
            {
                out.push_back({total * (bad + start) / n, u - total / n});
                start = -1;
            }
        }
        return out;
    }
    int start = -1;
    for (int k = 0; k <= n; ++k)
    {
        if (ok[size_t(k)])
        {
            if (start < 0)
                start = k;
            if (k == n)
                out.push_back({total * start / n, total});
        }
        else if (start >= 0)
        {
            out.push_back({total * start / n, total * (k - 1) / n});
            start = -1;
        }
    }
    return out;
}

/** Spans less the excluded ranges (sorted or not); a cut end is a jamb. Ranges are tried as given and shifted by +-period for loops. */
/** Spans with the cuts taken out. jambs marks the ends a cut makes as an opening's jambs. */
std::vector<Span> subtract(std::vector<Span> spans, const std::vector<std::pair<double, double>>& cuts, double period, bool jambs = true)
{
    for (const auto& cut0 : cuts)
        for (double off : {0.0, period, -period})
        {
            if (off != 0.0 && period <= 0.0)
                continue;
            const double ca = cut0.first + off, cb = cut0.second + off;
            std::vector<Span> next;
            for (const Span& s : spans)
            {
                if (cb <= s.a || ca >= s.b)
                {
                    next.push_back(s);
                    continue;
                }
                if (ca > s.a)
                    next.push_back({s.a, ca, s.jambA, jambs});
                if (cb < s.b)
                    next.push_back({cb, s.b, jambs, s.jambB});
            }
            spans = std::move(next);
        }
    return spans;
}

/** One opening placed on a run: where its centre is on the run's centre line, and its courses. */
struct PlacedOpening
{
    const Opening* o = nullptr;
    size_t seg = 0;
    double frac = 0.0;
    double centre = 0.0; // centre line param
    int sillCourse = 0, springCourse = 0, topCourse = 0, lintelCourses = 0;
    ArchSpec arch;
    double archTop = 0.0; // highest corner of the arch's stones above the springing
    double springY = 0.0;
    double bearing = 0.0, sillExtra = 0.0;
};

/**
 * Course stones with nothing under them (every stone below a short rough one, or the void over an arch crown) grow down
 * onto the highest stone beneath, a half joint above it, unless growing would meet another stone.
 */
void growOntoBeds(std::vector<Stone>& stones, size_t first, double pitch, double joint)
{
    std::vector<int> floating;
    countFloating(stones, pitch * 0.1, &floating);
    if (floating.empty())
        return;
    StoneGrid grid;
    grid.cell = std::max(pitch * 2.0, 20.0);
    grid.build(stones, 0.0);
    std::stable_sort(floating.begin(), floating.end(), [&](int a, int b) { return stones[size_t(a)].center.y < stones[size_t(b)].center.y; });
    for (int k : floating)
    {
        if (size_t(k) < first)
            continue;
        const Stone& s = stones[size_t(k)];
        const double bottom = s.center.y - halfExtentAabb(s).y;
        Stone below = s;
        below.size.y = pitch * 3.0;
        below.center = s.center - s.frame[1] * ((s.size.y + below.size.y) * 0.5);
        double bed = -1e300;
        for (int o : grid.around(below, 0.0))
            if (o != k && overlaps(below, stones[size_t(o)], joint * 0.5))
            {
                const Stone& t = stones[size_t(o)];
                const double top = t.center.y + halfExtentAabb(t).y;
                if (top <= bottom + 1e-6)
                    bed = std::max(bed, top);
            }
        const double up = std::max(s.frame[1].y, 0.5);
        const double grow = (bottom - bed - joint * 0.5) / up;
        if (bed < -1e299 || grow <= 0.0)
            continue;
        Stone grown = s;
        grown.size.y += grow;
        grown.center -= s.frame[1] * (grow * 0.5);
        bool clear = true;
        for (int o : grid.around(grown, 0.0))
            if (o != k && overlaps(grown, stones[size_t(o)], 0.0))
            {
                clear = false;
                break;
            }
        if (!clear)
            continue;
        stones[size_t(k)] = grown;
        grid.build(stones, 0.0);
    }
}

/** Stones of a wall, before trimming. Elements (arch, lintel, sill) come first and are never trimmed. */
struct Laid
{
    std::vector<Stone> elements;
    std::vector<Stone> course;
};

Stone stoneIn(const glm::dvec3& origin, const glm::dvec3& along, const glm::dvec3& normal, const ArchStone& a, double depth, double zOffset)
{
    Stone s;
    const glm::dvec3 x3 = along * a.axisX.x + kUp * a.axisX.y;
    const glm::dvec3 y3 = along * a.axisY.x + kUp * a.axisY.y;
    glm::dvec3 z3 = glm::cross(x3, y3);
    s.frame = glm::dmat3(x3, y3, z3);
    s.center = origin + along * a.center.x + kUp * a.center.y + normal * zOffset;
    s.size = {a.top, a.height, depth};
    s.taper = float(std::clamp(a.bottom / std::max(a.top, 1e-9), 0.02, 1.0));
    s.shape = s.taper < 0.999f ? Shape::Wedge : Shape::Box;
    s.role = a.role;
    return s;
}
} // namespace

glm::dvec3 miterOffset(const glm::dvec3& dIn, const glm::dvec3& dOut, double dist) { return miterOffsetImpl(dIn, dOut, dist); }

uint64_t hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

double unitRandom(uint64_t k) { return double(hash64(k) >> 11) * (1.0 / 9007199254740992.0); }

uint64_t mixKey(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    uint64_t h = hash64(a);
    h = hash64(h ^ b);
    h = hash64(h ^ c);
    h = hash64(h ^ d);
    return hash64(h ^ e);
}

const char* bondName(Bond b)
{
    switch (b)
    {
    case Bond::Running: return "running";
    case Bond::Stack: return "stack";
    case Bond::English: return "english";
    case Bond::Flemish: return "flemish";
    case Bond::Header: return "header";
    case Bond::GardenWall: return "gardenWall";
    case Bond::Herringbone: return "herringbone";
    case Bond::Ashlar: return "ashlar";
    case Bond::Rubble: return "rubble";
    case Bond::DryStone: return "dryStone";
    }
    return "running";
}

bool parseBond(std::string_view name, Bond* out)
{
    for (Bond b : {Bond::Running, Bond::Stack, Bond::English, Bond::Flemish, Bond::Header, Bond::GardenWall, Bond::Herringbone, Bond::Ashlar,
                   Bond::Rubble, Bond::DryStone})
        if (name == bondName(b))
        {
            *out = b;
            return true;
        }
    return false;
}

std::vector<std::string> bondNames()
{
    std::vector<std::string> out;
    for (Bond b : {Bond::Running, Bond::Stack, Bond::English, Bond::Flemish, Bond::Header, Bond::GardenWall, Bond::Herringbone, Bond::Ashlar,
                   Bond::Rubble, Bond::DryStone})
        out.emplace_back(bondName(b));
    return out;
}

const char* roleName(Role r)
{
    static const char* names[] = {"stretcher", "header", "bat", "closer", "filler", "voussoir", "keystone", "lintel",
                                  "sill", "jamb", "column", "step", "band", "slab", "rubble", "paving"};
    return names[std::min(size_t(r), std::size(names) - 1)];
}

const char* archName(ArchKind k)
{
    switch (k)
    {
    case ArchKind::Lintel: return "lintel";
    case ArchKind::Round: return "round";
    case ArchKind::Pointed: return "pointed";
    case ArchKind::Segmental: return "segmental";
    case ArchKind::Flat: return "flat";
    }
    return "round";
}

bool parseArch(std::string_view name, ArchKind* out)
{
    for (ArchKind k : {ArchKind::Lintel, ArchKind::Round, ArchKind::Pointed, ArchKind::Segmental, ArchKind::Flat})
        if (name == archName(k))
        {
            *out = k;
            return true;
        }
    return false;
}

glm::dmat4 stoneMatrix(const Stone& s)
{
    glm::dmat4 m(s.frame);
    m[3] = glm::dvec4(s.center, 1.0);
    return m;
}

glm::dvec3 halfExtentAabb(const Stone& s)
{
    const glm::dvec3 h = s.size * 0.5;
    glm::dvec3 e(0.0);
    for (int k = 0; k < 3; ++k)
        e += glm::abs(s.frame[k]) * h[k];
    return e;
}

std::array<glm::dvec3, 8> stoneCorners(const Stone& s)
{
    std::array<glm::dvec3, 8> c;
    const glm::dvec3 h = s.size * 0.5;
    const double bottom = s.shape == Shape::Wedge ? double(s.taper) : 1.0;
    for (int i = 0; i < 8; ++i)
    {
        glm::dvec3 sign((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0);
        if (!(i & 2))
            sign.x *= bottom;
        c[size_t(i)] = s.center + s.frame * (sign * h);
    }
    return c;
}

namespace
{
bool overlapsBoxes(const Stone& a, const Stone& b, double tolerance)
{
    const glm::dvec3 ha = glm::max(a.size * 0.5 - tolerance * 0.5, glm::dvec3(0.0));
    const glm::dvec3 hb = glm::max(b.size * 0.5 - tolerance * 0.5, glm::dvec3(0.0));
    const glm::dmat3 r = glm::transpose(a.frame) * b.frame; // b's axes in a's frame
    const glm::dvec3 t = glm::transpose(a.frame) * (b.center - a.center);
    glm::dmat3 absR;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            absR[j][i] = std::abs(r[j][i]) + 1e-12;
    auto row = [&](const glm::dmat3& m, int i) { return glm::dvec3(m[0][i], m[1][i], m[2][i]); };
    for (int i = 0; i < 3; ++i)
        if (std::abs(t[i]) > ha[i] + glm::dot(row(absR, i), hb))
            return false;
    for (int j = 0; j < 3; ++j)
        if (std::abs(glm::dot(t, r[j])) > glm::dot(ha, absR[j]) + hb[j])
            return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            const double ra = ha[i1] * absR[j][i2] + ha[i2] * absR[j][i1];
            const double rb = hb[j1] * absR[j2][i] + hb[j2] * absR[j1][i];
            const double d = t[i2] * r[j][i1] - t[i1] * r[j][i2];
            if (std::abs(d) > ra + rb)
                return false;
        }
    return true;
}

/** Face normals and edge directions of a stone's hull (a box or a wedge), world space. */
void hullAxes(const Stone& s, std::vector<glm::dvec3>& normals, std::vector<glm::dvec3>& edges)
{
    const glm::dvec3 x = s.frame[0], y = s.frame[1], z = s.frame[2];
    normals.push_back(y);
    normals.push_back(z);
    edges.push_back(x);
    edges.push_back(z);
    if (s.shape != Shape::Wedge)
    {
        normals.push_back(x);
        edges.push_back(y);
        return;
    }
    // The slanted sides: from a bottom corner (x * taper) up to a top corner (x).
    const double hx = s.size.x * 0.5, hy = s.size.y * 0.5;
    for (double side : {-1.0, 1.0})
    {
        const glm::dvec3 e = x * (side * hx * (1.0 - double(s.taper))) + y * (2.0 * hy);
        edges.push_back(glm::normalize(e));
        normals.push_back(glm::normalize(glm::cross(e, z)));
    }
}
} // namespace

bool overlaps(const Stone& a, const Stone& b, double tolerance)
{
    if (a.shape != Shape::Wedge && b.shape != Shape::Wedge)
        return overlapsBoxes(a, b, tolerance);
    // Shrink each hull toward its centre by half the tolerance, then a general separating axis test.
    auto shrunk = [&](const Stone& s) {
        std::array<glm::dvec3, 8> c = stoneCorners(s);
        for (glm::dvec3& p : c)
        {
            const glm::dvec3 d = p - s.center;
            const double len = glm::length(d);
            if (len > 1e-12)
                p -= d / len * std::min(len * 0.99, tolerance * 0.5);
        }
        return c;
    };
    const std::array<glm::dvec3, 8> ca = shrunk(a), cb = shrunk(b);
    std::vector<glm::dvec3> na, ea, nb, eb;
    hullAxes(a, na, ea);
    hullAxes(b, nb, eb);
    std::vector<glm::dvec3> axes = na;
    axes.insert(axes.end(), nb.begin(), nb.end());
    for (const glm::dvec3& u : ea)
        for (const glm::dvec3& v : eb)
        {
            const glm::dvec3 c = glm::cross(u, v);
            if (glm::dot(c, c) > 1e-12)
                axes.push_back(glm::normalize(c));
        }
    for (const glm::dvec3& ax : axes)
    {
        double loA = 1e300, hiA = -1e300, loB = 1e300, hiB = -1e300;
        for (const glm::dvec3& p : ca)
        {
            loA = std::min(loA, glm::dot(p, ax));
            hiA = std::max(hiA, glm::dot(p, ax));
        }
        for (const glm::dvec3& p : cb)
        {
            loB = std::min(loB, glm::dot(p, ax));
            hiB = std::max(hiB, glm::dot(p, ax));
        }
        if (hiA <= loB || hiB <= loA)
            return false;
    }
    return true;
}

uint64_t StoneGrid::pack(int64_t x, int64_t y, int64_t z)
{
    constexpr int64_t off = 1 << 20;
    return (uint64_t(x + off) & 0x1FFFFF) | ((uint64_t(y + off) & 0x1FFFFF) << 21) | ((uint64_t(z + off) & 0x1FFFFF) << 42);
}

void StoneGrid::build(std::span<const Stone> stones, double pad)
{
    entries.clear();
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const glm::dvec3 e = halfExtentAabb(stones[k]) + pad;
        const glm::dvec3 lo = glm::floor((stones[k].center - e) / cell), hi = glm::floor((stones[k].center + e) / cell);
        for (int64_t x = int64_t(lo.x); x <= int64_t(hi.x); ++x)
            for (int64_t y = int64_t(lo.y); y <= int64_t(hi.y); ++y)
                for (int64_t z = int64_t(lo.z); z <= int64_t(hi.z); ++z)
                    entries.push_back({pack(x, y, z), int(k)});
    }
    std::sort(entries.begin(), entries.end());
}

std::vector<int> StoneGrid::around(const Stone& s, double pad) const
{
    std::vector<int> out;
    const glm::dvec3 e = halfExtentAabb(s) + pad;
    const glm::dvec3 lo = glm::floor((s.center - e) / cell), hi = glm::floor((s.center + e) / cell);
    for (int64_t x = int64_t(lo.x); x <= int64_t(hi.x); ++x)
        for (int64_t y = int64_t(lo.y); y <= int64_t(hi.y); ++y)
            for (int64_t z = int64_t(lo.z); z <= int64_t(hi.z); ++z)
            {
                const uint64_t key = pack(x, y, z);
                auto it = std::lower_bound(entries.begin(), entries.end(), std::pair<uint64_t, int>{key, INT32_MIN});
                for (; it != entries.end() && it->first == key; ++it)
                    out.push_back(it->second);
            }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Layout layoutWall(const Footprint& footprint, const WallParams& p, int wall)
{
    Layout out;
    const Clean c = clean(footprint, std::max(0.0, p.height));
    if (c.pts.size() < 2 || c.top <= c.base)
        return out;
    const Units& u = p.stone;
    const double j = std::max(0.0, u.joint);
    const double T0 = std::max(p.thickness, 1.0);
    const bool single = T0 < u.depth * 1.5;
    const double minLen = std::max(2.0, u.length * 0.2);
    const Courses K = coursesOf(c, p);
    out.courses = K.count;
    out.pitch = K.pitch;
    out.base = K.base;
    const double h = K.h;
    const bool leftIsOuter = leftIsOuterOf(c);
    const double batterAngle = single ? 0.0 : glm::radians(std::clamp(p.batter, 0.0, 45.0));
    const double bandProject = p.bandProject > 0.0 ? p.bandProject : u.depth / 3.0;
    const double corniceProject = p.corniceProject > 0.0 ? p.corniceProject : u.depth * 0.25;

    const std::vector<Run> runs = splitRuns(c, p.cornerAngle);
    const size_t n = c.pts.size();
    // Footprint distance at each clean point, to find openings.
    std::vector<double> along(n + 1, 0.0);
    for (size_t k = 1; k <= n; ++k)
        along[k] = along[k - 1] + glm::length(flat(c.pts[k % n] - c.pts[k - 1]));
    const double footLength = c.closed ? along[n] : along[n - 1];

    Laid laid;
    for (size_t ri = 0; ri < runs.size(); ++ri)
    {
        const Run& r = runs[ri];
        const Line centre = offsetLine(r, 0.0);
        const size_t segs = r.pts.size() - (r.cyclic ? 0 : 1);

        // Openings on this run, with their courses snapped to the coursing.
        std::vector<PlacedOpening> openings;
        for (const Opening& o : p.openings)
        {
            if (o.width <= 0.0 || o.height <= 0.0 || footLength <= 0.0)
                continue;
            double at = c.closed ? std::fmod(std::fmod(o.at, footLength) + footLength, footLength) : std::clamp(o.at, 0.0, footLength);
            size_t seg = std::min(size_t(std::upper_bound(along.begin(), along.begin() + (c.closed ? n + 1 : n), at) - along.begin()), (c.closed ? n : n - 1)) - 1;
            const double segLen = along[seg + 1] - along[seg];
            const double frac = segLen > 0.0 ? (at - along[seg]) / segLen : 0.0;
            const size_t local = (seg + n - r.first) % n;
            if (local >= segs)
                continue;
            PlacedOpening po;
            po.o = &o;
            po.seg = local;
            po.frac = frac;
            po.centre = centre.paramAt(local, frac);
            const double ground = centre.baseAt(po.centre);
            const double w = o.width;
            po.sillCourse = std::clamp(int(std::lround((ground + std::max(0.0, o.sill) - K.base) / K.pitch)), 0, K.count - 1);
            const double wantTop = ground + std::max(0.0, o.sill) + o.height;
            po.arch.kind = o.arch;
            po.arch.span = w;
            po.arch.ring = o.ring > 0.0 ? o.ring : std::max(u.length * 0.6, u.height * 1.5);
            po.arch.unit = u.height + j;
            po.arch.joint = j;
            po.arch.rise = o.rise;
            po.bearing = std::max(u.length * 0.5, w * 0.15);
            po.sillExtra = u.length * 0.25;
            if (o.arch == ArchKind::Lintel)
            {
                po.topCourse = std::clamp(int(std::lround((wantTop - K.base) / K.pitch)), po.sillCourse + 1, K.count);
                po.springCourse = po.topCourse;
                po.lintelCourses = std::max(1, int(std::lround(std::max(K.pitch, w * 0.15) / K.pitch)));
            }
            else
            {
                const double rise = archRise(po.arch);
                po.springCourse = std::clamp(int(std::lround((wantTop - rise - K.base) / K.pitch)), po.sillCourse + 1, K.count);
                po.topCourse = po.springCourse;
                for (const ArchStone& a : archStones(po.arch))
                    for (double side : {-0.5, 0.5})
                        po.archTop = std::max(po.archTop, (a.center + a.axisX * (a.top * side) + a.axisY * (a.height * 0.5)).y);
            }
            po.springY = K.base + po.springCourse * K.pitch;
            openings.push_back(po);
        }

        // The outer face first: its through stones are the inner face's too.
        const std::vector<double> sides = single ? std::vector<double>{0.0} : (leftIsOuter ? std::vector<double>{1.0, -1.0} : std::vector<double>{-1.0, 1.0});
        const size_t runStart = laid.course.size();
        std::map<int, std::vector<std::pair<glm::dvec3, glm::dvec3>>> throughEnds; // by course, each through stone's two ends
        for (double side : sides)
        {
            const Face face = single ? Face::Through : ((side > 0.0) == leftIsOuter ? Face::Outer : Face::Inner);
            if (face == Face::Inner)
                for (size_t k = runStart; k < laid.course.size(); ++k)
                {
                    const Stone& t = laid.course[k];
                    if (t.face == Face::Through)
                        throughEnds[t.course].push_back({t.center - t.frame[0] * (t.size.x * 0.5), t.center + t.frame[0] * (t.size.x * 0.5)});
                }
            std::map<int64_t, Line> lines; // by thickness in 1/1000 cm, so a battered wall makes one per course
            for (int course = 0; course < K.count; ++course)
            {
                const double y = K.base + course * K.pitch;
                const double T = thicknessAt(p, y + K.pitch - K.base);
                const double Tmid = thicknessAt(p, y + K.pitch * 0.5 - K.base);
                const double stretcherDepth = single ? T0 : std::min(u.depth, (T - j) * 0.5);
                const double headerDepth = single ? T0 : std::min(u.length, T);
                const int64_t tk = int64_t(std::llround(Tmid * 1000.0));
                auto found = lines.find(tk);
                if (found == lines.end())
                    found = lines.emplace(tk, offsetLine(r, side * Tmid * 0.5)).first;
                const Line& line = found->second;
                const double total = line.length();

                bool loop = false;
                double shift = 0.0;
                std::vector<Span> spans = groundSpans(line, c.level, y, h, K.pitch, std::max(1.0, u.length * 0.25), &loop, &shift);
                // Openings: the face line's own params for the opening's centre.
                std::vector<std::pair<double, double>> cuts;
                for (const PlacedOpening& po : openings)
                {
                    if (!po.o->through && face == Face::Inner)
                        continue;
                    const double uc = line.paramAt(po.seg, po.frac);
                    const double hw = po.o->width * 0.5;
                    double half = -1.0;
                    if (course >= po.sillCourse && course < po.springCourse)
                        half = hw;
                    else if (po.o->arch == ArchKind::Lintel && course >= po.topCourse && course < po.topCourse + po.lintelCourses)
                        half = hw + po.bearing + j;
                    else if (po.o->arch != ArchKind::Lintel && course >= po.springCourse)
                    {
                        const double ext = extradosHalfWidth(po.arch, y - po.springY);
                        if (ext >= 0.0 && y - po.springY < po.archTop)
                            half = ext + j;
                    }
                    else if (po.o->sill > 0.0 && po.o->sillStone && course == po.sillCourse - 1)
                        half = hw + po.sillExtra + j;
                    if (half > 0.0)
                        cuts.push_back({uc - half, uc + half});
                }
                if (!cuts.empty() && loop)
                {
                    // A loop cut open: start the course at the first cut.
                    loop = false;
                    shift = cuts.front().second;
                    spans = {{shift, shift + total}};
                }
                spans = subtract(std::move(spans), cuts, line.cyclic ? total : 0.0);

                // The inner face fits between the outer face's through stones and lays none of its own.
                const bool throughs = face == Face::Inner && headerDepth >= T - j - 1e-6;
                if (throughs)
                {
                    std::vector<std::pair<double, double>> taken;
                    for (const auto& [a, b] : throughEnds[course])
                    {
                        const double ua = line.closest(a), ub = line.closest(b);
                        taken.push_back({std::min(ua, ub) - j, std::max(ua, ub) + j});
                    }
                    if (!taken.empty() && loop)
                    {
                        loop = false;
                        std::ranges::sort(taken);
                        shift = taken.front().second;
                        spans = {{shift, shift + total}};
                    }
                    spans = subtract(std::move(spans), taken, line.cyclic ? total : 0.0, false);
                }

                const bool rough = p.bond == Bond::Ashlar || p.bond == Bond::Rubble || p.bond == Bond::DryStone;
                Pattern pat = rough ? roughPattern(p.bond, course, u, stretcherDepth, headerDepth, mixKey(p.seed, uint64_t(wall), ri, uint64_t(side > 0.0), 7),
                                                   course == K.count - 1)
                                    : pattern(p.bond, course, u, stretcherDepth, headerDepth);
                if (throughs)
                {
                    pat.lead.clear();
                    for (Unit& unit : pat.cycle)
                        if (unit.depth >= T - j - 1e-6)
                            unit = {unit.width, Role::Stretcher, stretcherDepth};
                    if (!rough)
                        pat.cycle = {{u.length, Role::Stretcher, stretcherDepth}};
                }
                for (size_t si = 0; si < spans.size(); ++si)
                {
                    const Span& sp = spans[si];
                    double t0 = sp.a, t1 = sp.b;
                    Pattern used = pat;
                    if (loop)
                    {
                        // No ends to start from: the course starts where the bond says, round the loop.
                        used.lead.clear();
                        const double off = (course & 1) ? (used.cycle[0].width + j) * 0.5 : 0.0;
                        t1 = total - j;
                        t0 += off;
                        t1 += off;
                    }
                    else if (!line.cyclic)
                    {
                        auto cornerTrim = [&](int corner, const glm::dvec3& dIn, const glm::dvec3& dOut, bool atEnd, double ownerDepth) {
                            const double s = std::max(std::abs(crossY(dIn, dOut)), 0.35);
                            const bool owner = ((course + corner) % 2 == 0) == atEnd;
                            const glm::dvec3 vertex = atEnd ? r.pts.back() : r.pts.front();
                            const glm::dvec3 miter = vertex + miterOffset(dIn, dOut, side * Tmid * 0.5);
                            const bool convex = glm::dot(flat(miter - vertex), atEnd ? dIn : -dOut) > 1e-9 || single;
                            // Inside a turn the faces meet at the mitre, and stones pushed out of one face move into the other.
                            if (!convex)
                                return owner ? 0.0 : j + std::max(0.0, p.protrude);
                            if (single)
                                return owner ? -(Tmid * 0.5) / s : (Tmid * 0.5) / s + j;
                            return owner ? 0.0 : (ownerDepth + j) / s;
                        };
                        if (r.startCorner >= 0 && t0 <= 1e-6)
                            t0 += cornerTrim(r.startCorner, r.before, flatDir(r.pts[0], r.pts[1]), false, pat[0].depth);
                        if (r.endCorner >= 0 && t1 >= total - 1e-6)
                            t1 -= cornerTrim(r.endCorner, flatDir(r.pts[r.pts.size() - 2], r.pts.back()), r.after, true, pat[0].depth);
                    }
                    const double drift = (unitRandom(mixKey(p.seed, uint64_t(wall), ri, uint64_t(course), 17 + si * 31)) - 0.5) * u.length * 0.8;
                    double mid = (t0 + t1) * 0.5 + drift;
                    if (t1 - t0 > 2.0 * u.length)
                        mid = std::clamp(mid, t0 + u.length, t1 - u.length);
                    const std::vector<Interval> ivs = layCourse(used, t0, t1, mid, j, minLen);
                    for (size_t ii = 0; ii < ivs.size(); ++ii)
                    {
                        const Interval& iv = ivs[ii];
                        const glm::dvec3 A = line.sample(iv.a), B = line.sample(iv.b);
                        const double width = glm::length(flat(B - A));
                        if (width < 1e-3)
                            continue;
                        const glm::dvec3 alongDir = flatDir(A, B);
                        // Into the wall: away from the face this side lays.
                        glm::dvec3 into = side > 0.0 ? -leftOf(alongDir) : leftOf(alongDir);
                        glm::dvec3 x = alongDir;
                        if (glm::dot(glm::cross(x, kUp), into) < 0.0)
                            x = -x;
                        Stone s;
                        s.frame = glm::dmat3(x, kUp, into);
                        s.size = {width, h, iv.unit.depth};
                        const glm::dvec3 faceMid = (A + B) * 0.5;
                        s.center = faceMid + (side == 0.0 ? glm::dvec3(0.0) : into * (iv.unit.depth * 0.5));
                        s.center.y = y + h * 0.5;
                        s.wall = wall;
                        s.course = course;
                        s.run = int32_t(ri);
                        s.role = iv.unit.role;
                        s.face = iv.unit.depth >= T - j - 1e-6 ? Face::Through : face;
                        // A through stone reaches both faces, even when the wall is a little thicker than a brick is long.
                        if (s.face == Face::Through && side != 0.0 && s.size.z < T)
                        {
                            s.size.z = T;
                            s.center = glm::dvec3(faceMid.x, s.center.y, faceMid.z) + into * (T * 0.5);
                        }
                        s.grounded = line.baseAt(iv.a) >= y - K.pitch * 0.5 || line.baseAt(iv.b) >= y - K.pitch * 0.5 || course == 0;
                        if ((ii == 0 && sp.jambA) || (ii + 1 == ivs.size() && sp.jambB))
                            s.role = Role::Jamb;
                        // Centre line range, through the face line's segment fractions.
                        const auto [ka, fa] = line.locate(iv.a);
                        const auto [kb, fb] = line.locate(iv.b);
                        s.t0 = centre.paramAt(ka, fa);
                        s.t1 = centre.paramAt(kb, fb);
                        if (s.t1 < s.t0)
                            s.t1 += centre.length();
                        if (p.bond == Bond::Herringbone)
                        {
                            // The stone stands at 45 degrees in its unit, leaning the other way on alternate courses.
                            double len = (h - u.height * std::sqrt(0.5)) * std::sqrt(2.0);
                            // The end stones of a stretch are cut so they don't stand out past it.
                            if (ii == 0 || ii + 1 == ivs.size())
                                len = std::min(len, width * std::sqrt(2.0) + u.height * 0.5);
                            len = std::max(len, u.height);
                            const double a = (course & 1) ? -0.25 * kPi : 0.25 * kPi;
                            const glm::dvec3 fx = s.frame[0], fy = s.frame[1];
                            s.frame[0] = fx * std::cos(a) + fy * std::sin(a);
                            s.frame[1] = fy * std::cos(a) - fx * std::sin(a);
                            s.size = {len, u.height, iv.unit.depth};
                        }
                        else if (p.bond == Bond::Rubble || p.bond == Bond::DryStone)
                        {
                            // Rough stones: lower than the course and a little shorter, sitting on their bed.
                            const double r1 = unitRandom(mixKey(p.seed, uint64_t(wall), laid.course.size(), 71));
                            const double r2 = unitRandom(mixKey(p.seed, uint64_t(wall), laid.course.size(), 72));
                            const double hh = h * (0.7 + 0.3 * r1);
                            s.center.y -= (h - hh) * 0.5;
                            s.size.y = hh;
                            s.size.x = std::max(minLen, s.size.x * (0.86 + 0.14 * r2));
                        }
                        // String courses and the cornice stand out of the outer face.
                        const double above = y - K.base;
                        double project = 0.0;
                        for (double band : p.bands)
                            if (band >= above - 1e-6 && band < above + K.pitch - 1e-6)
                                project = bandProject;
                        if (p.cornice > 0 && course >= K.count - p.cornice)
                            project = corniceProject * (course - (K.count - p.cornice) + 1);
                        if (project > 0.0 && s.face != Face::Inner)
                        {
                            const glm::dvec3 outward = s.face == Face::Through ? (leftIsOuter ? leftOf(alongDir) : -leftOf(alongDir)) : -into;
                            s.center += outward * (project * 0.5);
                            s.size.z += project;
                            s.role = Role::Band;
                        }
                        if (batterAngle > 0.0 && s.face != Face::Through)
                        {
                            const double cs = std::cos(batterAngle), sn = std::sin(batterAngle);
                            const glm::dvec3 up = s.frame[1], in = s.frame[2];
                            s.frame[1] = up * cs + in * sn;
                            s.frame[2] = in * cs - up * sn;
                        }
                        laid.course.push_back(s);
                    }
                }
            }
        }

        // The openings' own stones, laid through the wall (or in the outer face, for a niche).
        for (const PlacedOpening& po : openings)
        {
            const double uc = po.centre;
            const glm::dvec3 P = centre.sample(uc);
            const glm::dvec3 d = centre.dirAt(uc);
            const glm::dvec3 outerN = leftIsOuter ? leftOf(d) : -leftOf(d);
            const double T = thicknessAt(p, po.springY - K.base);
            const double nicheDepth = single ? T0 : std::min(u.depth, (T - j) * 0.5);
            const double depth = po.o->through || single ? T : nicheDepth;
            const double zOff = po.o->through || single ? 0.0 : (T - nicheDepth) * 0.5;
            const glm::dvec3 origin(P.x, po.springY, P.z);
            std::vector<ArchStone> pieces;
            if (po.o->arch == ArchKind::Lintel)
            {
                const double hgt = po.lintelCourses * K.pitch - j;
                ArchStone l;
                l.center = {0.0, hgt * 0.5};
                l.top = l.bottom = po.o->width + 2.0 * po.bearing;
                l.height = hgt;
                l.role = Role::Lintel;
                pieces.push_back(l);
            }
            else
                pieces = archStones(po.arch);
            if (po.o->sill > 0.0 && po.o->sillStone && po.sillCourse > 0)
            {
                ArchStone sl;
                const double sillY = K.base + (po.sillCourse - 1) * K.pitch;
                sl.center = {0.0, sillY - po.springY + h * 0.5};
                sl.top = sl.bottom = po.o->width + 2.0 * po.sillExtra;
                sl.height = h;
                sl.role = Role::Sill;
                pieces.push_back(sl);
            }
            for (size_t k = 0; k < pieces.size(); ++k)
            {
                const ArchStone& a = pieces[k];
                double z = zOff, dep = depth;
                if (a.role == Role::Sill)
                {
                    // Sills stand out of the outer face to shed water.
                    const double lip = u.depth * 0.2;
                    dep += lip;
                    z += lip * 0.5;
                }
                Stone s = stoneIn(origin, d, outerN, a, dep, z);
                s.wall = wall;
                s.run = int32_t(ri);
                s.course = std::clamp(int(std::floor((s.center.y - K.base) / K.pitch)), 0, K.count - 1);
                s.face = po.o->through || single ? Face::Through : Face::Outer;
                s.region = -1 - int(po.o - p.openings.data()); // resolved by the caller into the opening's region
                laid.elements.push_back(s);
            }
        }
    }

    // Elements first and untouched, then through stones and the outer face, then the inner face fits round them.
    std::vector<size_t> order(laid.course.size());
    for (size_t k = 0; k < order.size(); ++k)
        order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (laid.course[a].course != laid.course[b].course)
            return laid.course[a].course < laid.course[b].course;
        return faceRank(laid.course[a].face) < faceRank(laid.course[b].face);
    });
    Grid grid{std::max({u.length, u.height * 2.0, 20.0}), {}};
    for (Stone& e : laid.elements)
    {
        grid.insert(e, int(out.stones.size()));
        out.stones.push_back(e);
    }
    const double minDepth = std::max(1.0, std::min(u.depth, T0) * 0.3);
    // Most of a joint between any two stones, so the wander below can't close it.
    const double clearance = -0.75 * j;
    for (size_t k : order)
    {
        Stone s = laid.course[k];
        bool alive = true, trimmed = false;
        for (int pass = 0; pass < 6 && alive; ++pass)
        {
            const Stone* hit = nullptr;
            for (int other : grid.around(s, j))
                if (overlaps(s, out.stones[size_t(other)], clearance))
                {
                    hit = &out.stones[size_t(other)];
                    break;
                }
            if (!hit)
                break;
            alive = s.shape == Shape::Box && trimAgainst(s, *hit, j, minLen, minDepth);
            trimmed = true;
            if (pass == 5 && alive)
                for (int other : grid.around(s, j))
                    if (overlaps(s, out.stones[size_t(other)], clearance))
                        alive = false;
        }
        if (!alive)
        {
            ++out.dropped;
            continue;
        }
        out.trimmed += trimmed ? 1 : 0;
        grid.insert(s, int(out.stones.size()));
        out.stones.push_back(s);
    }

    // Wander: half the joint at most per course stone, split between moving and tilting, so neighbours never meet.
    const double budget = 0.25 * j * std::clamp(p.jitter, 0.0, 1.0);
    for (size_t k = 0; k < out.stones.size(); ++k)
    {
        Stone& s = out.stones[k];
        auto rnd = [&](uint64_t m) { return unitRandom(mixKey(p.seed, uint64_t(wall), k, m, 91)); };
        s.random = float(rnd(0));
        if (k < laid.elements.size())
            continue;
        const double step = budget / std::sqrt(3.0);
        const glm::dvec3 shift((rnd(1) * 2 - 1) * step, (rnd(2) * 2 - 1) * step, -rnd(3) * std::max(0.0, p.protrude));
        const double halfDiag = glm::length(s.size) * 0.5;
        const double maxAngle = halfDiag > 0.0 ? (budget / 3.0) / halfDiag : 0.0;
        const glm::dmat3 tilt = rotationXyz((rnd(4) * 2 - 1) * maxAngle, (rnd(5) * 2 - 1) * maxAngle, (rnd(6) * 2 - 1) * maxAngle);
        s.center += s.frame * shift;
        s.frame = s.frame * tilt;
    }
    growOntoBeds(out.stones, laid.elements.size(), K.pitch, j);
    return out;
}

namespace
{
/** A slab piece lying in place: a vertical prism over its top faces' outline, between y0 and y1. */
struct SlabOutline
{
    std::vector<std::array<glm::dvec2, 2>> edges; // every top face's edges, so edges inside the outline come in pairs
    double y0 = 0.0, y1 = 0.0;
    glm::dvec2 lo{1e300}, hi{-1e300};
};

bool inPlaceSlab(const Built& b, const Stone& s)
{
    return s.role == Role::Slab && !s.loose && s.mesh >= 0 && size_t(s.mesh) < b.pieces.size() && s.frame[1].y > 0.9999;
}

SlabOutline outlineOf(const Built& b, const Stone& s)
{
    SlabOutline o;
    const geo::Mesh& m = b.pieces[size_t(s.mesh)];
    const geo::Bounds bb = m.bounds();
    o.y0 = s.center.y + bb.min.y;
    o.y1 = s.center.y + bb.max.y;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceNormal(f).y < 0.5)
            continue;
        const std::vector<geo::Index> fp = m.facePoints(f);
        for (size_t i = 0; i < fp.size(); ++i)
        {
            const glm::dvec3 a = s.center + m.position(fp[i]), c = s.center + m.position(fp[(i + 1) % fp.size()]);
            o.edges.push_back({glm::dvec2(a.x, a.z), glm::dvec2(c.x, c.z)});
            o.lo = glm::min(o.lo, glm::dvec2(a.x, a.z));
            o.hi = glm::max(o.hi, glm::dvec2(a.x, a.z));
        }
    }
    return o;
}

double cross2(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

bool crosses(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, const glm::dvec2& d)
{
    const double d1 = cross2(c, d, a), d2 = cross2(c, d, b), d3 = cross2(a, b, c), d4 = cross2(a, b, d);
    return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

bool insideOutline(const SlabOutline& o, const glm::dvec2& p)
{
    bool in = false;
    for (const auto& [a, c] : o.edges)
        if ((a.y > p.y) != (c.y > p.y) && p.x < (c.x - a.x) * (p.y - a.y) / (c.y - a.y) + a.x)
            in = !in;
    return in;
}

/** Convex hull of the part of s between heights y0 and y1, seen from above, shrunk toward its middle by shrink. */
std::vector<glm::dvec2> sectionOf(const Stone& s, double y0, double y1, double shrink)
{
    const std::array<glm::dvec3, 8> c = stoneCorners(s);
    std::vector<glm::dvec2> pts;
    for (const glm::dvec3& q : c)
        if (q.y >= y0 && q.y <= y1)
            pts.push_back({q.x, q.z});
    for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
        {
            if (i & bit)
                continue;
            const glm::dvec3& a = c[size_t(i)];
            const glm::dvec3& d = c[size_t(i | bit)];
            for (double y : {y0, y1})
                if ((a.y - y) * (d.y - y) < 0.0)
                {
                    const glm::dvec3 q = a + (d - a) * ((y - a.y) / (d.y - a.y));
                    pts.push_back({q.x, q.z});
                }
        }
    if (pts.size() < 3)
        return {};
    std::sort(pts.begin(), pts.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<glm::dvec2> hull(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        while (k >= 2 && cross2(hull[k - 2], hull[k - 1], pts[i]) <= 0.0)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;)
    {
        while (k >= t && cross2(hull[k - 2], hull[k - 1], pts[i]) <= 0.0)
            --k;
        hull[k++] = pts[i];
    }
    hull.resize(k > 0 ? k - 1 : 0);
    if (hull.size() < 3)
        return {};
    glm::dvec2 mid(0.0);
    for (const glm::dvec2& q : hull)
        mid += q / double(hull.size());
    for (glm::dvec2& q : hull)
    {
        const double len = glm::length(q - mid);
        if (len > 1e-12)
            q -= (q - mid) / len * std::min(len * 0.99, shrink);
    }
    return hull;
}

bool insideConvex(const std::vector<glm::dvec2>& hull, const glm::dvec2& p)
{
    for (size_t i = 0; i < hull.size(); ++i)
        if (cross2(hull[i], hull[(i + 1) % hull.size()], p) <= 0.0)
            return false;
    return true;
}

bool sectionMeets(const std::vector<glm::dvec2>& hull, const SlabOutline& o)
{
    if (hull.empty())
        return false;
    for (size_t i = 0; i < hull.size(); ++i)
        for (const auto& [a, c] : o.edges)
            if (crosses(hull[i], hull[(i + 1) % hull.size()], a, c))
                return true;
    if (insideOutline(o, hull[0]))
        return true;
    for (const auto& e : o.edges)
        if (insideConvex(hull, e[0]))
            return true;
    return false;
}

bool outlinesMeet(const SlabOutline& a, const SlabOutline& b)
{
    for (const auto& [p, q] : a.edges)
        for (const auto& [r, s] : b.edges)
            if (crosses(p, q, r, s))
                return true;
    return (!a.edges.empty() && insideOutline(b, (a.edges[0][0] + a.edges[0][1]) * 0.5)) ||
           (!b.edges.empty() && insideOutline(a, (b.edges[0][0] + b.edges[0][1]) * 0.5));
}
} // namespace

std::vector<std::pair<int, int>> overlappingPairs(const Built& b, std::span<const int> which, double tolerance)
{
    std::vector<std::pair<int, int>> out;
    std::vector<int> boxes, slabs;
    for (int k : which)
        (inPlaceSlab(b, b.stones[size_t(k)]) ? slabs : boxes).push_back(k);

    double cell = 20.0;
    for (int k : boxes)
        cell = std::max(cell, std::max({b.stones[size_t(k)].size.x, b.stones[size_t(k)].size.y, b.stones[size_t(k)].size.z}));
    Grid grid{cell, {}};
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const Stone& s = b.stones[size_t(boxes[i])];
        for (int other : grid.around(s, 0.0))
            if (overlaps(s, b.stones[size_t(boxes[size_t(other)])], tolerance))
                out.push_back({boxes[size_t(other)], boxes[i]});
        grid.insert(s, int(i));
    }

    std::vector<SlabOutline> outlines;
    for (int k : slabs)
        outlines.push_back(outlineOf(b, b.stones[size_t(k)]));
    const double h = tolerance * 0.5;
    for (size_t i = 0; i < slabs.size(); ++i)
    {
        const SlabOutline& o = outlines[i];
        for (int k : boxes)
        {
            const Stone& s = b.stones[size_t(k)];
            const glm::dvec3 e = halfExtentAabb(s);
            if (s.center.y + e.y <= o.y0 + h || s.center.y - e.y >= o.y1 - h || s.center.x + e.x <= o.lo.x || s.center.x - e.x >= o.hi.x ||
                s.center.z + e.z <= o.lo.y || s.center.z - e.z >= o.hi.y)
                continue;
            if (sectionMeets(sectionOf(s, o.y0 + h, o.y1 - h, tolerance), o))
                out.push_back({slabs[i], k});
        }
        // Pieces of one slab lying in place fit by construction.
        for (size_t j = 0; j < i; ++j)
        {
            const SlabOutline& p = outlines[j];
            const Stone& a = b.stones[size_t(slabs[i])];
            const Stone& c = b.stones[size_t(slabs[j])];
            if (a.region == c.region && std::abs(o.y0 - p.y0) < 1e-6 && std::abs(o.y1 - p.y1) < 1e-6)
                continue;
            if (std::min(o.y1, p.y1) - std::max(o.y0, p.y0) <= tolerance || o.hi.x <= p.lo.x || p.hi.x <= o.lo.x || o.hi.y <= p.lo.y || p.hi.y <= o.lo.y)
                continue;
            if (outlinesMeet(o, p))
                out.push_back({slabs[j], slabs[i]});
        }
    }
    return out;
}

int fitAgainst(std::vector<Stone>& stones, std::span<const Stone> fixed, double joint)
{
    double cell = 20.0;
    for (const Stone& f : fixed)
        cell = std::max(cell, std::max({f.size.x, f.size.y, f.size.z}));
    Grid grid{cell, {}};
    for (size_t k = 0; k < fixed.size(); ++k)
        grid.insert(fixed[k], int(k));
    const double j = std::max(0.0, joint);
    int dropped = 0;
    std::vector<Stone> out;
    out.reserve(stones.size());
    for (Stone s : stones)
    {
        const double minLen = std::max(1.0, s.size.x * 0.25), minDepth = std::max(1.0, s.size.z * 0.25);
        bool alive = true;
        for (int pass = 0; pass < 6 && alive; ++pass)
        {
            const Stone* hit = nullptr;
            for (int other : grid.around(s, j))
                if (overlaps(s, fixed[size_t(other)], -j))
                {
                    hit = &fixed[size_t(other)];
                    break;
                }
            if (!hit)
                break;
            alive = s.shape == Shape::Box && pass < 5 && trimAgainst(s, *hit, j, minLen, minDepth);
        }
        if (alive)
            out.push_back(s);
        else
            ++dropped;
    }
    stones = std::move(out);
    return dropped;
}

size_t countOverlaps(std::span<const Stone> stones, double tolerance)
{
    double cell = 20.0;
    for (const Stone& s : stones)
        cell = std::max(cell, std::max({s.size.x, s.size.y, s.size.z}));
    Grid grid{cell, {}};
    size_t count = 0;
    for (size_t k = 0; k < stones.size(); ++k)
    {
        for (int other : grid.around(stones[k], 0.0))
            if (overlaps(stones[k], stones[size_t(other)], tolerance))
                ++count;
        grid.insert(stones[k], int(k));
    }
    return count;
}

geo::Mesh hexMesh(const std::array<glm::dvec3, 8>& pts)
{
    glm::dvec3 mid(0.0);
    for (const glm::dvec3& q : pts)
        mid += q / 8.0;
    std::vector<glm::dvec3> P(pts.begin(), pts.end());
    std::vector<geo::Index> sizes, idx;
    for (int axis = 0; axis < 3; ++axis)
    {
        const int i = (axis + 1) % 3, k = (axis + 2) % 3;
        for (int side = 0; side < 2; ++side)
        {
            std::array<geo::Index, 4> q;
            int n = 0;
            for (int c : {0, 1, 3, 2})
                q[size_t(n++)] = geo::Index((side << axis) | ((c & 1) << i) | (((c >> 1) & 1) << k));
            glm::dvec3 nrm(0.0), centre(0.0);
            for (size_t m = 0; m < 4; ++m)
            {
                nrm += glm::cross(P[q[m]], P[q[(m + 1) % 4]]);
                centre += P[q[m]] / 4.0;
            }
            if (glm::dot(nrm, centre - mid) < 0.0)
                std::reverse(q.begin(), q.end());
            sizes.push_back(4);
            idx.insert(idx.end(), q.begin(), q.end());
        }
    }
    return geo::Mesh::fromPolygons(P, sizes, idx);
}

std::vector<geo::Mesh> coreCells(const Footprint& footprint, const WallParams& p, std::span<const Stone> stones, int wall, double grid)
{
    std::vector<geo::Mesh> out;
    const Clean c = clean(footprint, std::max(0.0, p.height));
    if (c.pts.size() < 2 || c.top <= c.base)
        return out;
    const Courses K = coursesOf(c, p);
    const double j = std::max(0.0, p.stone.joint);
    const double recess = std::max(0.0, p.recess);
    const std::vector<Run> runs = splitRuns(c, p.cornerAngle);
    const double gap = 2.0 * j + 1e-3 + std::max(0.0, p.protrude);
    const double endReach = (std::max(p.stone.length, p.stone.depth) + j) * 1.5;
    auto half = [&](double y) {
        const double t = thicknessAt(p, y - K.base);
        return std::max(t * 0.5 - recess, t * 0.25);
    };

    for (size_t ri = 0; ri < runs.size(); ++ri)
    {
        const Run& r = runs[ri];
        const Line centre = offsetLine(r, 0.0);
        const double total = centre.length();
        // Per course, the stretches of centre line some stone covers.
        std::vector<std::vector<std::pair<double, double>>> cover(size_t(K.count));
        for (const Stone& s : stones)
        {
            if (s.wall != wall || s.run != int32_t(ri) || s.t1 < s.t0 || s.course < 0 || s.course >= K.count || s.role == Role::Rubble)
                continue;
            auto& list = cover[size_t(s.course)];
            if (r.cyclic && s.t1 > total)
            {
                list.push_back({s.t0, total});
                list.push_back({0.0, s.t1 - total});
            }
            else
                list.push_back({s.t0, s.t1});
        }
        for (auto& list : cover)
        {
            std::sort(list.begin(), list.end());
            std::vector<std::pair<double, double>> merged;
            for (const auto& iv : list)
            {
                if (!merged.empty() && iv.first <= merged.back().second + gap)
                    merged.back().second = std::max(merged.back().second, iv.second);
                else
                    merged.push_back(iv);
            }
            // To the vertex where another run meets this one; a free end stops behind the end stones like the faces do.
            const bool freeStart = !r.cyclic && r.startCorner < 0, freeEnd = !r.cyclic && r.endCorner < 0;
            for (auto& iv : merged)
            {
                if (iv.first < endReach || (r.cyclic && iv.first < gap))
                    iv.first = freeStart ? recess : 0.0;
                if (iv.second > total - endReach || (r.cyclic && iv.second > total - gap))
                    iv.second = freeEnd ? total - recess : total;
            }
            if (r.cyclic && merged.size() > 1 && merged.front().first <= 0.0 && merged.back().second >= total)
            {
                // Joined across the loop's start.
                merged.front().first = merged.back().first - total;
                merged.pop_back();
            }
            list = std::move(merged);
        }
        auto same = [&](const std::vector<std::pair<double, double>>& a, const std::vector<std::pair<double, double>>& b) {
            if (a.size() != b.size())
                return false;
            for (size_t k = 0; k < a.size(); ++k)
                if (std::abs(a[k].first - b[k].first) > gap || std::abs(a[k].second - b[k].second) > gap)
                    return false;
            return true;
        };
        // Sections across the core at centre-line param t: left and right points on the ground plane, without y.
        auto section = [&](double t, double hw, bool atVertex, size_t vertex) -> std::pair<glm::dvec3, glm::dvec3> {
            glm::dvec3 P, off;
            if (atVertex)
            {
                P = r.pts[vertex % r.pts.size()];
                off = runOffset(r, vertex % r.pts.size(), hw);
            }
            else
            {
                P = centre.sample(t);
                off = leftOf(centre.dirAt(t)) * hw;
            }
            return {P + off, P - off};
        };
        int c0 = 0;
        while (c0 < K.count)
        {
            int c1 = c0;
            if (p.batter <= 0.0)
                while (c1 + 1 < K.count && same(cover[size_t(c0)], cover[size_t(c1 + 1)]))
                    ++c1;
            const double y0 = K.base + c0 * K.pitch;
            for (const auto& iv : cover[size_t(c0)])
            {
                // The band's top meets the course above where that course covers this stretch, else stops under the top course's stone tops.
                bool coveredAbove = false;
                if (c1 + 1 < K.count)
                    for (const auto& up : cover[size_t(c1 + 1)])
                        coveredAbove = coveredAbove || (up.first < iv.second && up.second > iv.first);
                const double y1 = coveredAbove ? K.base + (c1 + 1) * K.pitch : K.base + c1 * K.pitch + K.h - recess;
                if (y1 <= y0)
                    continue;
                const double h0 = half(y0), h1 = half(y1);
                const bool ring = r.cyclic && iv.second - iv.first >= total - 1e-6;
                std::vector<std::pair<double, int>> cuts; // t, and vertex index or -1
                const size_t np = r.pts.size();
                if (ring)
                    for (size_t v = 0; v <= np; ++v)
                        cuts.push_back({centre.at[std::min(v, centre.at.size() - 1)], int(v % np)});
                else
                {
                    const bool startAtCorner = !r.cyclic && iv.first <= 1e-6;
                    const bool endAtCorner = !r.cyclic && iv.second >= total - 1e-6;
                    cuts.push_back({iv.first, startAtCorner ? 0 : -1});
                    for (size_t v = 1; v + (r.cyclic ? 0 : 1) < centre.at.size(); ++v)
                        for (double off : {0.0, -total, total})
                        {
                            const double tv = centre.at[v] + off;
                            if (tv > iv.first + 1e-6 && tv < iv.second - 1e-6)
                                cuts.push_back({tv, int(v % np)});
                        }
                    cuts.push_back({iv.second, endAtCorner ? int(np - 1) : -1});
                    std::sort(cuts.begin(), cuts.end());
                }
                if (grid > 0.0)
                {
                    std::vector<std::pair<double, int>> lines;
                    for (size_t k = 0; k + 1 < cuts.size(); ++k)
                    {
                        const double ta = cuts[k].first, tb = cuts[k + 1].first;
                        const glm::dvec3 pa = centre.sample(ta), pb = centre.sample(tb);
                        for (int axis : {0, 2})
                        {
                            const double lo = std::min(pa[axis], pb[axis]), hi = std::max(pa[axis], pb[axis]);
                            for (double g = std::ceil(lo / grid) * grid; g < hi; g += grid)
                            {
                                const double f = (g - pa[axis]) / (pb[axis] - pa[axis]);
                                if (f > 1e-6 && f < 1.0 - 1e-6)
                                    lines.push_back({ta + (tb - ta) * f, -1});
                            }
                        }
                    }
                    cuts.insert(cuts.end(), lines.begin(), lines.end());
                    std::sort(cuts.begin(), cuts.end());
                }
                for (size_t k = 0; k + 1 < cuts.size(); ++k)
                {
                    const auto [aL0, aR0] = section(cuts[k].first, h0, cuts[k].second >= 0, size_t(std::max(0, cuts[k].second)));
                    const auto [bL0, bR0] = section(cuts[k + 1].first, h0, cuts[k + 1].second >= 0, size_t(std::max(0, cuts[k + 1].second)));
                    const auto [aL1, aR1] = section(cuts[k].first, h1, cuts[k].second >= 0, size_t(std::max(0, cuts[k].second)));
                    const auto [bL1, bR1] = section(cuts[k + 1].first, h1, cuts[k + 1].second >= 0, size_t(std::max(0, cuts[k + 1].second)));
                    auto at = [](const glm::dvec3& q, double y) { return glm::dvec3(q.x, y, q.z); };
                    // Corner order: bit 0 along (a, b), bit 1 up, bit 2 across (left, right).
                    const std::array<glm::dvec3, 8> hex = {at(aL0, y0), at(bL0, y0), at(aL1, y1), at(bL1, y1),
                                                           at(aR0, y0), at(bR0, y0), at(aR1, y1), at(bR1, y1)};
                    geo::Mesh cell = hexMesh(hex);
                    lila::scene::materials::assign(cell, kMortarSlot, {});
                    out.push_back(std::move(cell));
                }
            }
            c0 = c1 + 1;
        }
    }
    return out;
}

geo::Mesh coreMesh(const Footprint& footprint, const WallParams& p)
{
    const Layout l = layoutWall(footprint, p);
    geo::Mesh core;
    for (geo::Mesh& m : coreCells(footprint, p, l.stones))
        core.append(m);
    if (core.numFaces() > 0)
        lila::scene::materials::assign(core, kMortarSlot, {});
    return core;
}

namespace
{
/** A drum: a cylinder along Y with its bottom scaled by taper, chamfered top and bottom rims unless detail is Box. */
geo::Mesh drumMesh(const StoneShape& s)
{
    const double r = std::max(0.05, s.size.x * 0.5), hy = std::max(0.05, s.size.y * 0.5);
    const int sides = s.detail == Detail::Worn ? 24 : (s.detail == Detail::Clean ? 16 : 8);
    const double b = s.detail == Detail::Box ? 0.0 : std::clamp(s.bevel, 0.0, 0.4 * std::min(r, hy));
    // Rings from bottom to top: radius and height.
    std::vector<std::pair<double, double>> rings;
    const double rb = r * std::clamp(s.taper, 0.02, 1.0);
    if (b > 0.0)
        rings = {{rb - b, -hy}, {rb, -hy + b}, {r, hy - b}, {r - b, hy}};
    else
        rings = {{rb, -hy}, {r, hy}};
    std::vector<glm::dvec3> pts;
    for (const auto& [rad, y] : rings)
        for (int k = 0; k < sides; ++k)
        {
            const double a = 2.0 * 3.14159265358979323846 * k / sides;
            pts.push_back({rad * std::cos(a), y, rad * std::sin(a)});
        }
    std::vector<geo::Index> sizes, idx;
    const geo::Index R = geo::Index(rings.size());
    for (geo::Index ring = 0; ring + 1 < R; ++ring)
        for (int k = 0; k < sides; ++k)
        {
            const geo::Index a = ring * sides + geo::Index(k), bb = ring * sides + geo::Index((k + 1) % sides);
            sizes.push_back(4);
            idx.insert(idx.end(), {a, a + geo::Index(sides), bb + geo::Index(sides), bb});
        }
    // Angles run anticlockwise seen from below, so the bottom cap in order faces down and the top cap reversed faces up.
    sizes.push_back(geo::Index(sides));
    for (int k = 0; k < sides; ++k)
        idx.push_back(geo::Index(k));
    sizes.push_back(geo::Index(sides));
    for (int k = sides - 1; k >= 0; --k)
        idx.push_back((R - 1) * sides + geo::Index(k));
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    // Make sure the faces point out: the side quads were wound one way, the caps the other.
    double vol = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const std::vector<geo::Index> fp = m.facePoints(f);
            for (size_t k = 1; k + 1 < fp.size(); ++k)
                vol += glm::dot(m.position(fp[0]), glm::cross(m.position(fp[k]), m.position(fp[k + 1])));
        }
    if (vol < 0.0)
    {
        for (size_t f = 0, o = 0; f < sizes.size(); o += sizes[f], ++f)
            std::reverse(idx.begin() + std::ptrdiff_t(o), idx.begin() + std::ptrdiff_t(o + sizes[f]));
        m = geo::Mesh::fromPolygons(pts, sizes, idx);
    }
    // Cylindrical uvs on the sides, planar on the caps.
    const double tile = std::max(s.tile, 1e-3);
    const glm::dvec2 offset(unitRandom(uint64_t(s.seed) * 2 + 101), unitRandom(uint64_t(s.seed) * 2 + 102));
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const bool cap = std::abs(m.faceNormal(f).y) > 0.9;
        double firstAngle = 0.0;
        bool haveFirst = false;
        for (geo::Index cn : m.faceCorners(f))
        {
            const glm::dvec3& q = m.position(m.cornerPoint(cn));
            if (cap)
                uv[cn] = glm::vec2(float(q.x / tile + offset.x), float(q.z / tile + offset.y));
            else
            {
                double a = std::atan2(q.z, q.x);
                if (!haveFirst)
                {
                    firstAngle = a;
                    haveFirst = true;
                }
                // Keep one quad on one side of the seam.
                while (a - firstAngle > 3.14159265358979323846)
                    a -= 2.0 * 3.14159265358979323846;
                while (firstAngle - a > 3.14159265358979323846)
                    a += 2.0 * 3.14159265358979323846;
                uv[cn] = glm::vec2(float(a * r / tile + offset.x), float((q.y + hy) / tile + offset.y));
            }
        }
    }
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            const glm::dvec3 d = m.position(m.edgePoint(e, 1)) - m.position(m.edgePoint(e, 0));
            // Rim edges (horizontal) are hard, the edges running up the side are smooth.
            hard[e] = std::abs(d.y) < 1e-9;
        }
    lila::scene::materials::assign(m, kStoneSlot, {});
    return m;
}
} // namespace

namespace
{
/** Every edge between exactly two faces and every face with real area. */
bool watertight(const geo::Mesh& m)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> p = m.facePoints(f);
        glm::dvec3 n(0.0);
        for (size_t k = 1; k + 1 < p.size(); ++k)
            n += glm::cross(m.position(p[k]) - m.position(p[0]), m.position(p[k + 1]) - m.position(p[0]));
        if (glm::length(n) * 0.5 <= 1e-4)
            return false;
    }
    return m.validate().empty();
}

/**
 * The frame of one chip's cut: z is the facet's outward normal, the bisector of the faces meeting at the edge or corner
 * tilted by up to 20 degrees, so the facet never runs nearly parallel to a face (which leaves a step or a sliver). x runs
 * along the edge, twisted up to 25 degrees about z so the chip's ends are angled. The origin is `depth` inside the edge.
 */
glm::dmat4 chipFrame(const glm::dvec3& at, const glm::dvec3& out, int along, bool edge, double size, double r0, double r1, double r2)
{
    const double tilt = glm::radians(20.0), twist = glm::radians(25.0);
    glm::dvec3 x(0.0);
    x[edge ? along : (along + 1) % 3] = 1.0;
    glm::dvec3 n = glm::normalize(out);
    const glm::dvec3 side = glm::normalize(glm::cross(n, x));
    n = glm::normalize(n + glm::tan((r0 * 2.0 - 1.0) * tilt) * side + glm::tan((r1 * 2.0 - 1.0) * tilt) * glm::normalize(x - glm::dot(x, n) * n));
    x = glm::normalize(x - glm::dot(x, n) * n);
    const double t = (r2 * 2.0 - 1.0) * twist;
    x = glm::normalize(glm::cos(t) * x + glm::sin(t) * glm::cross(n, x));
    const glm::dvec3 y = glm::cross(n, x);
    const double depth = size * 0.7;
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x, 0.0);
    m[1] = glm::dvec4(y, 0.0);
    m[2] = glm::dvec4(n, 0.0);
    m[3] = glm::dvec4(at - n * depth, 1.0);
    return m;
}

/** Chips knocked out of a worn stone's edges and corners: one angled facet cut per chip, more and bigger with s.chips. */
geo::Mesh chipped(geo::Mesh m, const glm::dvec3& e, const StoneShape& s)
{
    const double amount = std::clamp(s.chips, 0.0, 1.0);
    const int count = int(std::lround(2.0 + 10.0 * amount));
    const double minE = std::min({e.x, e.y, e.z});
    std::vector<geo::Mesh> cutters;
    cutters.reserve(size_t(count));
    for (int k = 0; k < count; ++k)
    {
        auto rnd = [&](int n) { return unitRandom(mixKey(s.seed, uint64_t(k), uint64_t(n), 733)); };
        // On an edge (two axes at their extremes) or a corner (three), chosen by seed.
        glm::dvec3 at, out(0.0);
        const int along = int(rnd(0) * 3.0) % 3;
        const bool edge = rnd(1) < 0.7;
        for (int a = 0; a < 3; ++a)
        {
            if (a == along && edge)
                at[a] = (rnd(2) * 2.0 - 1.0) * e[a] * 0.8;
            else
            {
                const double side = rnd(3 + a) < 0.5 ? -1.0 : 1.0;
                at[a] = side * e[a];
                out[a] = side;
            }
        }
        const double size = minE * (0.12 + 0.35 * amount) * (0.5 + rnd(6));
        const glm::dmat4 xf = chipFrame(at, out, along, edge, size, rnd(7), rnd(8), rnd(9));
        // The facet's face sits at z = 0; the box reaches outward past the stone and well beyond the chip's ends.
        const double wide = size * 4.0, len = edge ? size * (1.2 + 1.2 * rnd(10)) : wide;
        std::array<glm::dvec3, 8> box;
        // The ends lean out, so a chip runs out in a ramp rather than stopping at a wall.
        for (int i = 0; i < 8; ++i)
        {
            const double z = (i & 4) ? wide * 2.0 : 0.0, x = len + (edge ? z * 1.5 : 0.0);
            box[size_t(i)] = glm::dvec3(xf * glm::dvec4((i & 1) ? x : -x, (i & 2) ? wide : -wide, z, 1.0));
        }
        cutters.push_back(hexMesh(box));
    }
    lila::scene::BooleanOptions bo;
    bo.op = lila::scene::BooleanOp::Difference;
    bo.splitNgons = false;
    // One cut at a time, each kept only if the stone stays watertight: a hole shows as a black gap in Unreal.
    for (const geo::Mesh& c : cutters)
    {
        const std::vector<lila::scene::BooleanOperand> one = {{&m, glm::dmat4(1.0)}, {&c, glm::dmat4(1.0)}};
        lila::scene::BooleanResult r = lila::scene::meshBoolean(one, glm::dmat4(1.0), bo);
        if (r.ok() && r.mesh.numFaces() >= 6 && watertight(r.mesh))
            m = std::move(r.mesh);
    }
    return m;
}
} // namespace

int erosionLevel(const Stone& s)
{
    const double v = 0.55 * double(s.age) + 0.6 * double(s.damage) + 0.25 * double(s.wet) * double(s.age);
    return std::clamp(int(v * 3.0), 0, 2);
}

geo::Mesh stoneMesh(const StoneShape& s)
{
    if (s.shape == Shape::Drum)
        return drumMesh(s);
    const glm::dvec3 e = glm::max(s.size * 0.5, glm::dvec3(0.05));
    const double minE = std::min({e.x, e.y, e.z});
    const double b = s.detail == Detail::Box ? 0.0 : std::clamp(s.bevel, 0.0, 0.45 * minE);
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    auto face = [&](std::vector<geo::Index> f, const glm::dvec3& outward) {
        glm::dvec3 nrm(0.0);
        for (size_t k = 0; k < f.size(); ++k)
            nrm += glm::cross(pts[f[k]], pts[f[(k + 1) % f.size()]]);
        if (glm::dot(nrm, outward) < 0.0)
            std::reverse(f.begin(), f.end());
        sizes.push_back(geo::Index(f.size()));
        idx.insert(idx.end(), f.begin(), f.end());
    };
    auto sign = [](int corner, int axis) { return (corner >> axis) & 1 ? 1.0 : -1.0; };

    if (b <= 1e-6)
    {
        for (int c = 0; c < 8; ++c)
            pts.push_back(glm::dvec3(sign(c, 0), sign(c, 1), sign(c, 2)) * e);
        for (int axis = 0; axis < 3; ++axis)
            for (int side = 0; side < 2; ++side)
            {
                const int i = (axis + 1) % 3, j = (axis + 2) % 3;
                std::vector<geo::Index> f;
                for (int q : {0, 1, 3, 2})
                    f.push_back(geo::Index((side << axis) | ((q & 1) << i) | (((q >> 1) & 1) << j)));
                glm::dvec3 outward(0.0);
                outward[axis] = side ? 1.0 : -1.0;
                face(f, outward);
            }
    }
    else
    {
        // Each box corner splits into three points, one on each face meeting there.
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 sg(sign(c, 0), sign(c, 1), sign(c, 2));
            double knock = 0.0;
            if (s.detail == Detail::Worn)
            {
                const double r = unitRandom(uint64_t(s.seed) * 8 + uint64_t(c));
                knock = std::min(std::clamp(s.wear, 0.0, 1.0) * b * (0.3 + 1.7 * r * r), 0.4 * minE - b);
                knock = std::max(knock, 0.0);
            }
            // The knock moves each point within its own face, so faces stay flat and never sink behind the mortar.
            for (int axis = 0; axis < 3; ++axis)
            {
                glm::dvec3 q;
                for (int k = 0; k < 3; ++k)
                    q[k] = sg[k] * (k == axis ? e[k] : e[k] - b - knock);
                pts.push_back(q);
            }
        }
        auto at = [](int corner, int axis) { return geo::Index(corner * 3 + axis); };
        for (int axis = 0; axis < 3; ++axis)
        {
            const int i = (axis + 1) % 3, j = (axis + 2) % 3;
            for (int side = 0; side < 2; ++side)
            {
                std::vector<geo::Index> f;
                for (int q : {0, 1, 3, 2})
                    f.push_back(at((side << axis) | ((q & 1) << i) | (((q >> 1) & 1) << j), axis));
                glm::dvec3 outward(0.0);
                outward[axis] = side ? 1.0 : -1.0;
                face(f, outward);
            }
            // Chamfers along this axis, between the faces of the other two.
            for (int si = 0; si < 2; ++si)
                for (int sj = 0; sj < 2; ++sj)
                {
                    const int lo = (si << i) | (sj << j), hi = lo | (1 << axis);
                    glm::dvec3 outward(0.0);
                    outward[i] = si ? 1.0 : -1.0;
                    outward[j] = sj ? 1.0 : -1.0;
                    face({at(lo, i), at(hi, i), at(hi, j), at(lo, j)}, outward);
                }
        }
        for (int c = 0; c < 8; ++c)
            face({at(c, 0), at(c, 1), at(c, 2)}, glm::dvec3(sign(c, 0), sign(c, 1), sign(c, 2)));
    }
    if (s.shape == Shape::Wedge)
    {
        // Narrow the bottom along X: every point scaled by where it sits between the bottom and top.
        const double taper = std::clamp(s.taper, 0.02, 1.0);
        for (glm::dvec3& q : pts)
            q.x *= taper + (1.0 - taper) * std::clamp((q.y + e.y) / (2.0 * e.y), 0.0, 1.0);
    }

    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    if (s.detail == Detail::Worn && s.chips > 0.0)
        m = chipped(std::move(m), e, s);
    // Box-projected UVs at the given scale, offset per seed so stones read different parts of the texture.
    const glm::dvec2 offset(unitRandom(uint64_t(s.seed) * 2 + 101), unitRandom(uint64_t(s.seed) * 2 + 102));
    const double tile = std::max(s.tile, 1e-3);
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 nrm = glm::abs(m.faceNormal(f));
        const int axis = nrm.x >= nrm.y && nrm.x >= nrm.z ? 0 : (nrm.y >= nrm.z ? 1 : 2);
        const int ua = axis == 0 ? 2 : 0, va = axis == 1 ? 2 : 1;
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::dvec3& q = m.position(m.cornerPoint(c));
            uv[c] = glm::vec2(float((q[ua] + e[ua]) / tile + offset.x), float((q[va] + e[va]) / tile + offset.y));
        }
    }
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    std::fill(hard.begin(), hard.end(), true);
    lila::scene::materials::assign(m, kStoneSlot, {});
    return m;
}

std::array<geo::Mesh, 3> stoneLods(StoneShape s)
{
    std::array<geo::Mesh, 3> out;
    const Detail levels[] = {Detail::Worn, Detail::Clean, Detail::Box};
    for (size_t k = 0; k < out.size(); ++k)
    {
        s.detail = levels[k];
        out[k] = stoneMesh(s);
    }
    return out;
}

geo::Mesh prismMesh(const std::array<glm::dvec3, 4>& ground, double y0, double y1)
{
    std::vector<glm::dvec3> pts;
    for (const glm::dvec3& g : ground)
        pts.push_back({g.x, y0, g.z});
    for (const glm::dvec3& g : ground)
        pts.push_back({g.x, y1, g.z});
    glm::dvec3 mid(0.0);
    for (const glm::dvec3& q : pts)
        mid += q / 8.0;
    std::vector<geo::Index> sizes, idx;
    auto quad = [&](std::array<geo::Index, 4> q) {
        glm::dvec3 nrm(0.0), centre(0.0);
        for (size_t k = 0; k < 4; ++k)
        {
            nrm += glm::cross(pts[q[k]], pts[q[(k + 1) % 4]]);
            centre += pts[q[k]] / 4.0;
        }
        if (glm::dot(nrm, centre - mid) < 0.0)
            std::reverse(q.begin(), q.end());
        sizes.push_back(4);
        idx.insert(idx.end(), q.begin(), q.end());
    };
    quad({0, 1, 2, 3});
    quad({4, 5, 6, 7});
    for (geo::Index k = 0; k < 4; ++k)
        quad({k, geo::Index((k + 1) % 4), geo::Index((k + 1) % 4 + 4), geo::Index(k + 4)});
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

std::vector<geo::Mesh> collisionHulls(const Footprint& footprint, const WallParams& p, int bands)
{
    std::vector<geo::Mesh> out;
    const Clean c = clean(footprint, std::max(0.0, p.height));
    const size_t n = c.pts.size();
    if (n < 2 || c.top <= c.base)
        return out;
    const double half = std::max(p.thickness, 1.0) * 0.5;
    const int count = std::clamp(bands, 1, 16);
    auto dir = [&](size_t i) { return flatDir(c.pts[i % n], c.pts[(i + 1) % n]); };
    auto offsetAt = [&](size_t i) {
        if (c.closed)
            return miterOffset(dir((i + n - 1) % n), dir(i), half);
        const glm::dvec3 dOut = i + 1 < n ? dir(i) : dir(n - 2);
        return miterOffset(i > 0 ? dir(i - 1) : dOut, dOut, half);
    };
    const size_t segs = c.closed ? n : n - 1;
    for (size_t s = 0; s < segs; ++s)
    {
        const size_t a = s, b = (s + 1) % n;
        const glm::dvec3 oa = offsetAt(a), ob = offsetAt(b);
        const std::array<glm::dvec3, 4> ground = {c.pts[a] + oa, c.pts[b] + ob, c.pts[b] - ob, c.pts[a] - oa};
        const double lo = std::min(c.pts[a].y, c.pts[b].y), hi = std::max(c.tops[a], c.tops[b]);
        for (int band = 0; band < count; ++band)
        {
            const double y0 = lo + (hi - lo) * band / count, y1 = lo + (hi - lo) * (band + 1) / count;
            out.push_back(prismMesh(ground, y0, y1));
        }
    }
    return out;
}

geo::Mesh mergeStones(const Built& b, std::span<const int> stones, int lod, const glm::dvec3& origin)
{
    geo::Mesh out;
    const int level = std::clamp(lod, 0, 2);
    for (int k : stones)
    {
        const size_t r = size_t(b.reference[size_t(k)]);
        geo::Mesh m = b.lods.empty() ? b.meshes[r] : b.lods[r][size_t(level)];
        Stone s = b.stones[size_t(k)];
        s.center -= origin;
        const glm::dmat4 xf = stoneMatrix(s);
        for (glm::dvec3& q : m.positions())
            q = glm::dvec3(xf * glm::dvec4(q, 1.0));
        const float height = size_t(k) < b.heightFraction.size() ? b.heightFraction[size_t(k)] : 0.0f;
        lila::geo::colors::add(m, kStoneColorSet, glm::vec4(s.random, height, s.moss, s.wet));
        std::span<glm::vec2> uv2 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(2));
        std::fill(uv2.begin(), uv2.end(), glm::vec2(s.age, s.damage));
        out.append(m);
    }
    return out;
}

Chunks chunkStones(std::span<const Stone> stones, double size)
{
    Chunks c;
    const double cell = std::max(size, 1.0);
    std::vector<glm::ivec2> at(stones.size());
    for (size_t k = 0; k < stones.size(); ++k)
        at[k] = glm::ivec2(int(std::floor(stones[k].center.x / cell)), int(std::floor(stones[k].center.z / cell)));
    auto less = [](const glm::ivec2& a, const glm::ivec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; };
    c.cells = at;
    std::sort(c.cells.begin(), c.cells.end(), less);
    c.cells.erase(std::unique(c.cells.begin(), c.cells.end()), c.cells.end());
    c.of.resize(stones.size());
    for (size_t k = 0; k < stones.size(); ++k)
        c.of[k] = int(std::lower_bound(c.cells.begin(), c.cells.end(), at[k], less) - c.cells.begin());
    return c;
}

std::vector<Footprint> footprintsFromEdges(const geo::Mesh& m)
{
    std::vector<Footprint> out;
    std::vector<geo::Index> loose;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) == 0)
            loose.push_back(e);
    if (loose.empty())
        return out;
    std::span<const float> heights = m.pointAttrs().get<float>(kWallHeightAttr);
    std::map<geo::Index, std::vector<geo::Index>> at; // point -> loose edges
    for (geo::Index e : loose)
        for (int k = 0; k < 2; ++k)
            at[m.edgePoint(e, k)].push_back(e);
    std::map<geo::Index, bool> used;
    auto walk = [&](geo::Index start, geo::Index firstEdge) {
        Footprint f;
        auto addPoint = [&](geo::Index p) {
            f.points.push_back(m.position(p));
            if (!heights.empty())
                f.heights.push_back(double(heights[p]));
        };
        addPoint(start);
        geo::Index p = start, e = firstEdge;
        while (e != geo::kInvalid && !used[e])
        {
            used[e] = true;
            p = m.edgeOtherPoint(e, p);
            if (p == start)
            {
                f.closed = true;
                break;
            }
            addPoint(p);
            e = geo::kInvalid;
            const auto& next = at[p];
            if (next.size() == 2)
                for (geo::Index nx : next)
                    if (!used[nx])
                        e = nx;
        }
        // Heights of zero everywhere mean the attribute is there but unset.
        if (std::all_of(f.heights.begin(), f.heights.end(), [](double v) { return v <= 0.0; }))
            f.heights.clear();
        if (f.points.size() >= 2)
            out.push_back(std::move(f));
    };
    // Lines first, from their ends (and junctions), then whatever is left is loops.
    for (const auto& [p, edges] : at)
        if (edges.size() != 2)
            for (geo::Index e : edges)
                if (!used[e])
                    walk(p, e);
    for (geo::Index e : loose)
        if (!used[e])
            walk(m.edgePoint(e, 0), e);
    return out;
}

geo::Mesh edgesFromFootprints(std::span<const Footprint> footprints)
{
    geo::Mesh m;
    bool anyHeights = false;
    for (const Footprint& f : footprints)
        anyHeights = anyHeights || !f.heights.empty();
    std::vector<float> heights;
    for (const Footprint& f : footprints)
    {
        std::vector<geo::Index> ids;
        for (size_t k = 0; k < f.points.size(); ++k)
        {
            ids.push_back(m.addPoint(f.points[k]));
            heights.push_back(k < f.heights.size() ? float(f.heights[k]) : 0.0f);
        }
        for (size_t k = 0; k + 1 < ids.size(); ++k)
            m.addEdge(ids[k], ids[k + 1]);
        if (f.closed && ids.size() > 2)
            m.addEdge(ids.back(), ids.front());
    }
    if (anyHeights)
    {
        std::span<float> h = m.pointAttrs().getOrAdd<float>(kWallHeightAttr);
        for (size_t k = 0; k < h.size() && k < heights.size(); ++k)
            h[k] = heights[k];
    }
    return m;
}

void makeMeshes(Built& b, const BuildOptions& o)
{
    const int variants = std::max(1, o.variants);
    using Key = std::tuple<int, int64_t, int64_t, int64_t, int64_t, int, int>;
    std::map<Key, int> refs;
    struct ShapeRef
    {
        glm::dvec3 size;
        int variant;
        Shape shape;
        double taper;
        int erosion;
    };
    std::vector<ShapeRef> shapes;
    b.reference.assign(b.stones.size(), 0);
    b.variant.assign(b.stones.size(), 0);
    std::vector<std::pair<size_t, int>> pieceRefs; // stone, piece
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const Stone& s = b.stones[k];
        const int v = std::min(variants - 1, int(double(s.random) * variants));
        b.variant[k] = v;
        if (s.mesh >= 0)
        {
            pieceRefs.push_back({k, s.mesh});
            continue;
        }
        // Sizes rounded down to a quarter centimetre (never grown into a neighbour), so the common stones share a handful of meshes.
        const glm::dvec3 q = glm::floor(s.size * 4.0);
        const int64_t taper = s.shape == Shape::Box ? 1000 : int64_t(std::lround(double(s.taper) * 1000.0));
        const int erosion = o.chips > 0.0 ? erosionLevel(s) : 0;
        const Key key{int(s.shape), int64_t(q.x), int64_t(q.y), int64_t(q.z), taper, v, erosion};
        auto [it, added] = refs.emplace(key, int(shapes.size()));
        if (added)
            shapes.push_back({q / 4.0, v, s.shape, double(taper) / 1000.0, erosion});
        b.reference[k] = it->second;
    }
    const size_t shaped = shapes.size();
    std::map<int, int> pieceMesh;
    for (const auto& [k, piece] : pieceRefs)
    {
        auto [it, added] = pieceMesh.emplace(piece, int(shaped + pieceMesh.size()));
        b.reference[k] = it->second;
    }
    b.meshes.assign(shaped + pieceMesh.size(), geo::Mesh());
    b.lods.clear();
    if (o.lods)
        b.lods.resize(b.meshes.size());
    tbb::parallel_for(size_t(0), shaped, [&](size_t r) {
        StoneShape shape;
        shape.size = shapes[r].size;
        shape.bevel = o.bevel;
        shape.wear = o.wear;
        shape.seed = uint32_t(hash64(uint64_t(o.wall.seed) * 1000003ull + uint64_t(shapes[r].variant)));
        shape.detail = o.detail;
        shape.tile = o.tile;
        shape.shape = shapes[r].shape;
        shape.taper = shapes[r].taper;
        shape.chips = o.chips > 0.0 ? std::min(1.0, o.chips * (1.0 + 0.8 * shapes[r].erosion)) : 0.0;
        b.meshes[r] = stoneMesh(shape);
        if (o.lods)
            b.lods[r] = stoneLods(shape);
    });
    for (const auto& [piece, r] : pieceMesh)
    {
        geo::Mesh m = size_t(piece) < b.pieces.size() ? b.pieces[size_t(piece)] : geo::Mesh();
        lila::scene::materials::assign(m, kStoneSlot, {});
        b.meshes[size_t(r)] = m;
        if (o.lods)
            b.lods[size_t(r)] = {m, m, m};
    }
    // Height up its wall, by course, or by position for stones off the courses.
    std::map<int, std::pair<int, int>> courseRange;
    for (const Stone& s : b.stones)
    {
        auto [it, added] = courseRange.emplace(s.wall, std::pair<int, int>{s.course, s.course});
        it->second.first = std::min(it->second.first, s.course);
        it->second.second = std::max(it->second.second, s.course);
    }
    b.heightFraction.assign(b.stones.size(), 0.0f);
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const auto& [lo, hi] = courseRange[b.stones[k].wall];
        b.heightFraction[k] = hi > lo ? float(double(b.stones[k].course - lo) / double(hi - lo)) : 0.0f;
    }
}

Built buildWalls(std::span<const Footprint> footprints, const BuildOptions& o)
{
    Built b;
    for (size_t w = 0; w < footprints.size(); ++w)
    {
        Layout l = layoutWall(footprints[w], o.wall, int(w));
        b.courses = std::max(b.courses, l.courses);
        b.pitch = std::max(b.pitch, l.pitch);
        b.trimmed += l.trimmed;
        b.dropped += l.dropped;
        const int wallRegion = int(b.regions.size());
        b.regions.push_back("wall_" + std::to_string(w));
        const int openingBase = int(b.regions.size());
        for (const Opening& op : o.wall.openings)
            b.regions.push_back(b.regions[size_t(wallRegion)] + "/" + (op.name.empty() ? std::string("opening") : op.name));
        for (Stone& s : l.stones)
            s.region = s.region < 0 ? openingBase + (-1 - s.region) : wallRegion;
        for (geo::Mesh& cell : coreCells(footprints[w], o.wall, l.stones, int(w)))
        {
            b.core.append(cell);
            b.coreHulls.push_back(std::move(cell));
        }
        b.stones.insert(b.stones.end(), l.stones.begin(), l.stones.end());
    }
    if (b.core.numFaces() > 0)
        lila::scene::materials::assign(b.core, kMortarSlot, {});
    makeMeshes(b, o);
    return b;
}
} // namespace masonry
