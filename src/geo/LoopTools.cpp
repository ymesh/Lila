#include "geo/LoopTools.h"

#include "geo/Ops.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>

namespace lila::geo
{
namespace looptools
{
/** The edge of face f at point p other than e. */
Index otherEdgeAt(const Mesh& m, Index f, Index p, Index e)
{
    const Index c = m.findCorner(f, p);
    if (c == kInvalid)
        return kInvalid;
    const Index out = m.cornerEdge(c), in = m.cornerEdge(m.cornerPrev(c));
    return out == e ? in : out;
}

/** e's faces: [0] the one wound along e from `from` (its left), [1] the other. kInvalid where e has none. */
std::array<Index, 2> edgeSides(const Mesh& m, Index e, Index from)
{
    std::array<Index, 2> s{kInvalid, kInvalid};
    m.forEachEdgeCorner(e, [&](Index c) {
        Index& slot = m.cornerPoint(c) == from && s[0] == kInvalid ? s[0] : s[1];
        if (slot == kInvalid)
            slot = m.cornerFace(c);
    });
    return s;
}

/** The edges crossed going round point p from edge `from` through face f toward `to`. */
struct Fan
{
    std::vector<Index> rungs;
    /** faces[k] lies before rungs[k]; with reached, the last face lies before `to`. */
    std::vector<Index> faces;
    bool reached = false;
    /** Stopped at a border: the last rung has no face beyond it. */
    bool border = false;
};

Fan walkFan(const Mesh& m, Index p, Index from, Index f, Index to)
{
    Fan w;
    Index cur = from;
    const Index start = f;
    for (int guard = 0; f != kInvalid && guard < 256; ++guard)
    {
        w.faces.push_back(f);
        const Index e = otherEdgeAt(m, f, p, cur);
        if (e == kInvalid || e == from)
            break;
        if (e == to)
        {
            w.reached = true;
            break;
        }
        w.rungs.push_back(e);
        f = m.oppositeFace(e, f);
        if (f == kInvalid)
            w.border = m.isBoundaryEdge(e);
        if (f == start)
            break;
        cur = e;
    }
    return w;
}

/** The edge carrying e's loop on past p, or the one most nearly opposite e when the loop stops there. */
Index loopContinuation(const Mesh& m, Index e, Index p)
{
    if (const Index x = edgeLoopNext(m, e, p); x != kInvalid)
        return x;
    const glm::dvec3 at = m.position(p);
    const glm::dvec3 d = glm::normalize(m.position(m.edgeOtherPoint(e, p)) - at);
    Index best = kInvalid;
    double bestDot = -0.5;
    m.forEachPointEdge(p, [&](Index x) {
        if (x == e)
            return;
        const glm::dvec3 dx = m.position(m.edgeOtherPoint(x, p)) - at;
        const double len = glm::length(dx);
        if (len < 1e-12)
            return;
        if (const double c = glm::dot(d, dx / len); c < bestDot)
        {
            bestDot = c;
            best = x;
        }
    });
    return best;
}

/** The chain's direction at points[i]. */
glm::dvec3 chainTangent(const Mesh& m, const EdgePath& path, size_t i)
{
    const size_t n = path.points.size();
    const Index prev = path.closed || i > 0 ? path.points[(i + n - 1) % n] : path.points[i];
    const Index next = path.closed || i + 1 < n ? path.points[(i + 1) % n] : path.points[i];
    const glm::dvec3 d = m.position(next) - m.position(prev);
    const double len = glm::length(d);
    return len > 1e-12 ? d / len : glm::dvec3(0.0);
}

bool touches(const Mesh& m, Index f, const KnifePoint& k)
{
    switch (k.kind)
    {
    case KnifePoint::Kind::Point: return m.findCorner(f, k.id) != kInvalid;
    case KnifePoint::Kind::Face: return k.id == f;
    case KnifePoint::Kind::Edge:
    {
        bool found = false;
        m.forEachEdgeCorner(k.id, [&](Index c) { found = found || m.cornerFace(c) == f; });
        return found;
    }
    }
    return false;
}

/** Where the offset loop crosses an edge or a face near a chain point. */
struct Spotter
{
    const Mesh& m;
    const OffsetLoopOptions& o;

    /** On rung r out from p. along: r carries the chain on (a wrapped end), so the distance is not squared to it. */
    KnifePoint onEdge(Index r, Index p, const glm::dvec3& tangent, bool along) const
    {
        const Index q = m.edgeOtherPoint(r, p);
        const glm::dvec3 d = m.position(q) - m.position(p);
        const double len = glm::length(d);
        double t = o.distance;
        if (o.absolute)
        {
            double square = 1.0;
            if (!along && len > 1e-12 && glm::length(tangent) > 0.5)
            {
                const double c = glm::dot(d / len, tangent);
                square = std::max(0.25, std::sqrt(std::max(0.0, 1.0 - c * c)));
            }
            t = len > 1e-12 ? o.distance / square / len : 0.5;
        }
        t = std::clamp(t, 1e-3, 0.95);
        KnifePoint k = KnifePoint::edge(r, m.edgePoint(r, 0) == p ? t : 1.0 - t);
        k.position = glm::mix(m.position(p), m.position(q), t);
        return k;
    }

    /** Inside face f, the distance off both of its edges a and b at p. */
    KnifePoint inFace(Index f, Index p, Index a, Index b) const
    {
        const glm::dvec3 at = m.position(p), toCentre = m.faceCentroid(f) - at;
        auto unit = [&](Index e) {
            const glm::dvec3 d = m.position(m.edgeOtherPoint(e, p)) - at;
            const double len = glm::length(d);
            return len > 1e-12 ? d / len : d;
        };
        const glm::dvec3 ua = unit(a), ub = unit(b);
        glm::dvec3 w = ua + ub;
        // Straight through p: square off the edges, into the face.
        if (glm::length(w) < 1e-6)
            w = glm::cross(m.faceNormal(f), ua);
        if (glm::length(w) < 1e-12)
            w = toCentre;
        w = glm::normalize(w);
        if (glm::dot(w, toCentre) < 0.0)
            w = -w;
        const double sinHalf = std::max(0.2, std::sqrt(std::max(0.0, 0.5 * (1.0 - glm::dot(ua, ub)))));
        const double depth = glm::dot(toCentre, w);
        // A fraction measures against the face's depth off the chain, as a rung's length does in a quad.
        const double h = o.absolute ? o.distance : std::clamp(o.distance, 0.0, 0.95) * 2.0 * depth * sinHalf;
        const double along = std::min(h / sinHalf, 1.8 * std::max(0.0, depth));
        return KnifePoint::face(f, at + w * along);
    }
};

/** One side's offset loop along one chain, appended to plan as segments. */
struct SideCutter
{
    const Mesh& m;
    const Spotter& spot;
    KnifePlan& plan;
    std::optional<KnifePoint> prev;
    std::optional<std::pair<KnifePoint, Index>> first;

    /** Adds k, joined to the spot before it through face f when both lie on f. */
    void add(const KnifePoint& k, Index f)
    {
        if (prev && f != kInvalid && touches(m, f, *prev) && touches(m, f, k))
            plan.segments.push_back({f, *prev, k, false});
        if (!first)
            first = std::pair{k, f};
        prev = k;
    }

    void cut() { prev.reset(); }

    /**
     * Spots met walking out from the chain: spots[k] follows faces[k]. Outward runs away from the chain (toward the
     * next point); inward runs back to it, so the walk is emitted reversed, starting fresh.
     */
    void emit(const std::vector<KnifePoint>& spots, const std::vector<Index>& faces, bool outward)
    {
        if (outward)
        {
            for (size_t k = 0; k < spots.size(); ++k)
                add(spots[k], faces[k]);
            return;
        }
        cut();
        for (size_t k = spots.size(); k-- > 0;)
            add(spots[k], k + 1 < faces.size() ? faces[k + 1] : kInvalid);
    }

    std::vector<KnifePoint> rungSpots(const Fan& w, Index p, const glm::dvec3& tangent) const
    {
        std::vector<KnifePoint> out;
        for (Index r : w.rungs)
            out.push_back(spot.onEdge(r, p, tangent, false));
        return out;
    }
};

void planSide(const Mesh& m, const EdgePath& path, int side, const Spotter& spot, OffsetEnds ends, KnifePlan& plan)
{
    const size_t np = path.points.size(), ne = path.edges.size();
    if (ne == 0)
        return;
    SideCutter sc{m, spot, plan};
    auto sideFace = [&](size_t ei) { return edgeSides(m, path.edges[ei], path.points[ei])[size_t(side)]; };
    for (size_t i = 0; i < np; ++i)
    {
        const Index p = path.points[i];
        const bool hasIn = path.closed || i > 0, hasOut = path.closed || i + 1 < np;
        const size_t ia = (i + ne - 1) % ne;
        const Index a = hasIn ? path.edges[ia] : kInvalid, b = hasOut ? path.edges[i] : kInvalid;
        const Index fa = hasIn ? sideFace(ia) : kInvalid, fb = hasOut ? sideFace(i) : kInvalid;
        const glm::dvec3 tangent = chainTangent(m, path, i);

        if (hasIn && hasOut)
        {
            bool fromB = fa == kInvalid;
            if (fa != kInvalid)
            {
                const Fan w = walkFan(m, p, a, fa, b);
                if (w.reached && w.rungs.empty())
                {
                    sc.add(spot.inFace(fa, p, a, b), fa);
                    continue;
                }
                sc.emit(sc.rungSpots(w, p, tangent), w.faces, true);
                if (w.reached)
                    continue;
                sc.cut();
                fromB = w.border;
            }
            else
                sc.cut();
            if (fromB && fb != kInvalid)
            {
                const Fan w = walkFan(m, p, b, fb, a);
                sc.emit(sc.rungSpots(w, p, tangent), w.faces, false);
            }
            continue;
        }

        // An open chain's end: hasIn means it is the last point, so the walk out from it runs on along the path.
        const Index e = hasIn ? a : b, fe = hasIn ? fa : fb;
        if (fe == kInvalid)
        {
            sc.cut();
            continue;
        }
        if (m.isBoundaryPoint(p))
        {
            const Fan w = walkFan(m, p, e, fe, kInvalid);
            sc.emit(sc.rungSpots(w, p, tangent), w.faces, hasIn);
            continue;
        }
        const Index x = loopContinuation(m, e, p);
        if (ends == OffsetEnds::Converge || x == kInvalid)
        {
            KnifePoint k = KnifePoint::point(p);
            k.position = m.position(p);
            if (!hasIn)
                sc.cut();
            sc.add(k, fe);
            continue;
        }
        const Fan w = walkFan(m, p, e, fe, x);
        std::vector<KnifePoint> spots = sc.rungSpots(w, p, tangent);
        if (w.reached)
            spots.push_back(spot.onEdge(x, p, tangent, true));
        sc.emit(spots, w.faces, hasIn);
    }
    if (path.closed && sc.first && sc.prev)
        sc.add(sc.first->first, sc.first->second);
}
} // namespace looptools

KnifePlan offsetEdgeLoopPlan(const Mesh& mesh, std::span<const Index> edges, const OffsetLoopOptions& opts)
{
    LILA_ZONE();

    using namespace looptools;
    std::vector<Index> valid;
    for (Index e : edges)
        if (mesh.edgeValid(e))
            valid.push_back(e);
    const SymmetryMap* sym = opts.symmetry && !opts.symmetry->empty() ? opts.symmetry : nullptr;
    int drive = 0;
    if (sym)
    {
        drive = sym->majorityWedge(pointsOfEdges(mesh, valid));
        valid = sym->expandEdges(mesh, valid);
    }
    KnifePlan plan;
    const Spotter spot{mesh, opts};
    for (const EdgePath& path : chainEdges(mesh, valid))
    {
        // A chain off the driving side gets the image of its driving partner instead, so one-sided offsets mirror.
        if (sym && std::ranges::none_of(path.points, [&](Index p) { return sym->drives(p, drive); }))
            continue;
        if (opts.side != OffsetSide::Right)
            planSide(mesh, path, 0, spot, opts.ends, plan);
        if (opts.side != OffsetSide::Left)
            planSide(mesh, path, 1, spot, opts.ends, plan);
    }
    if (sym)
        addSymmetricImages(mesh, plan, *sym);
    return plan;
}

std::vector<Index> offsetEdgeLoops(Mesh& mesh, std::span<const Index> edges, const OffsetLoopOptions& opts, bool removeOriginal)
{
    LILA_ZONE();

    const KnifePlan plan = offsetEdgeLoopPlan(mesh, edges, opts);
    std::vector<Index> original(edges.begin(), edges.end());
    if (opts.symmetry && !opts.symmetry->empty())
        original = opts.symmetry->expandEdges(mesh, original);
    std::vector<Index> made = knifeApply(mesh, plan);
    if (!removeOriginal || made.empty())
        return made;
    dissolveEdges(mesh, original, true);
    std::erase_if(made, [&](Index e) { return !mesh.edgeValid(e); });
    return made;
}

namespace looptools
{
/**
 * A line across a loop point: the anchors a and b either side, the points beyond them (kInvalid when none), and where
 * the point goes between the anchors, by its count of edges to each so moving points spread evenly.
 */
struct Across
{
    Index a = kInvalid, b = kInvalid, a2 = kInvalid, b2 = kInvalid;
    double t = 0.5;
};

struct Anchor
{
    Index point = kInvalid, beyond = kInvalid;
    int steps = 0;
};

/** The first point past q, on the line from p through q, that is not in sel: moving points shape no tangent. */
Index beyondSelected(const Mesh& m, Index rung, Index q, Index p, const std::vector<uint8_t>& sel)
{
    Index from = q, e = edgeLoopNext(m, rung, q);
    for (int guard = 0; e != kInvalid && guard < 1024; ++guard)
    {
        const Index r = m.edgeOtherPoint(e, from);
        if (r == p || r == q)
            return kInvalid;
        if (!sel[r])
            return r;
        e = edgeLoopNext(m, e, r);
        from = r;
    }
    return kInvalid;
}

/**
 * Out from p along rung to the first point not in sel, and the point beyond that: the curve is anchored on points that
 * stay, running straight on through moving ones. Stops on a moving point at a border, or {kInvalid, kInvalid} round a ring.
 */
Anchor anchorAcross(const Mesh& m, Index rung, Index p, const std::vector<uint8_t>& sel)
{
    Index from = p, e = rung;
    for (int steps = 1; steps < 1024; ++steps)
    {
        const Index q = m.edgeOtherPoint(e, from);
        if (q == p)
            break;
        if (!sel[q])
            return {q, beyondSelected(m, e, q, from, sel), steps};
        const Index next = edgeLoopNext(m, e, q);
        if (next == kInvalid)
            return {q, kInvalid, steps};
        from = q;
        e = next;
    }
    return {};
}

/** The line between the anchors out along rungs ra and rb, or nullopt when either side has none. */
std::optional<Across> acrossLine(const Mesh& m, Index p, Index ra, Index rb, const std::vector<uint8_t>& sel)
{
    const Anchor a = anchorAcross(m, ra, p, sel), b = anchorAcross(m, rb, p, sel);
    if (a.point == kInvalid || b.point == kInvalid)
        return std::nullopt;
    return Across{a.point, b.point, a.beyond, b.beyond, double(a.steps) / double(a.steps + b.steps)};
}

/** Of the rungs, the one most square to the tangent. */
Index squarest(const Mesh& m, Index p, const std::vector<Index>& rungs, const glm::dvec3& tangent)
{
    Index best = kInvalid;
    double bestCos = 2.0;
    for (Index r : rungs)
    {
        const glm::dvec3 d = m.position(m.edgeOtherPoint(r, p)) - m.position(p);
        const double len = glm::length(d);
        const double c = len > 1e-12 ? std::abs(glm::dot(d / len, tangent)) : 1.0;
        if (c < bestCos)
        {
            bestCos = c;
            best = r;
        }
    }
    return best;
}

/** The crossing edges on one side of chain point p between its chain edges a and b (either may be kInvalid at an end). */
std::vector<Index> sideRungs(const Mesh& m, Index p, Index a, Index fa, Index b, Index fb)
{
    std::vector<Index> out;
    auto take = [&](const Fan& w) { out.insert(out.end(), w.rungs.begin(), w.rungs.end()); };
    if (a != kInvalid && b != kInvalid)
    {
        if (fa != kInvalid)
        {
            const Fan w = walkFan(m, p, a, fa, b);
            take(w);
            if (w.reached || !w.border)
                return out;
        }
        if (fb != kInvalid)
            take(walkFan(m, p, b, fb, a));
        return out;
    }
    const Index e = a != kInvalid ? a : b, fe = a != kInvalid ? fa : fb;
    if (fe == kInvalid)
        return out;
    const Index to = m.isBoundaryPoint(p) ? kInvalid : loopContinuation(m, e, p);
    take(walkFan(m, p, e, fe, to));
    return out;
}

void chainLines(const Mesh& m, const EdgePath& path, const std::vector<uint8_t>& sel, std::map<Index, std::vector<Across>>& lines)
{
    const size_t np = path.points.size(), ne = path.edges.size();
    for (size_t i = 0; i < np; ++i)
    {
        const Index p = path.points[i];
        const bool hasIn = path.closed || i > 0, hasOut = path.closed || i + 1 < np;
        const size_t ia = (i + ne - 1) % ne;
        const Index a = hasIn ? path.edges[ia] : kInvalid, b = hasOut ? path.edges[i] : kInvalid;
        const std::array<Index, 2> sa = hasIn ? edgeSides(m, a, path.points[ia]) : std::array<Index, 2>{kInvalid, kInvalid};
        const std::array<Index, 2> sb = hasOut ? edgeSides(m, b, p) : std::array<Index, 2>{kInvalid, kInvalid};
        const glm::dvec3 tangent = chainTangent(m, path, i);
        Index rung[2];
        for (int s = 0; s < 2; ++s)
            rung[s] = squarest(m, p, sideRungs(m, p, a, sa[size_t(s)], b, sb[size_t(s)]), tangent);
        if (rung[0] == kInvalid || rung[1] == kInvalid)
            continue;
        if (const std::optional<Across> l = acrossLine(m, p, rung[0], rung[1], sel))
            lines[p].push_back(*l);
    }
}

void looseLines(const Mesh& m, Index p, const std::vector<uint8_t>& sel, std::map<Index, std::vector<Across>>& lines)
{
    std::vector<Index> used;
    m.forEachPointEdge(p, [&](Index e) {
        const Index partner = edgeLoopNext(m, e, p);
        if (partner == kInvalid || std::ranges::count(used, e) || std::ranges::count(used, partner))
            return;
        used.push_back(e);
        used.push_back(partner);
        if (const std::optional<Across> l = acrossLine(m, p, e, partner, sel))
            lines[p].push_back(*l);
    });
}

/**
 * On the circle through p0, p1 and p2, the point t of the way from p1 to p2 along the arc between them that does not
 * pass p0. Straight between p1 and p2 when the three are in line.
 */
glm::dvec3 arcPoint(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, double t)
{
    const glm::dvec3 u = p0 - p1, v = p2 - p1, w = glm::cross(u, v);
    const double ww = glm::dot(w, w);
    if (ww < 1e-12 * glm::dot(u, u) * glm::dot(v, v))
        return glm::mix(p1, p2, t);
    const glm::dvec3 c = p1 + (glm::dot(u, u) * glm::cross(v, w) + glm::dot(v, v) * glm::cross(w, u)) / (2.0 * ww);
    const glm::dvec3 n = w / std::sqrt(ww), r1 = p1 - c, r2 = p2 - c, r0 = p0 - c;
    auto angle = [&](const glm::dvec3& a, const glm::dvec3& b) { return std::atan2(glm::dot(glm::cross(a, b), n), glm::dot(a, b)); };
    double phi = angle(r1, r2);
    const double psi = angle(r1, r0);
    if (psi * phi > 0.0 && std::abs(psi) < std::abs(phi))
        phi -= std::copysign(2.0 * glm::pi<double>(), phi);
    const double a = t * phi;
    return c + r1 * std::cos(a) + glm::cross(n, r1) * std::sin(a) + n * glm::dot(n, r1) * (1.0 - std::cos(a));
}

/** The chord turned into the surface's tangent plane at a point, chord long. */
std::optional<glm::dvec3> normalTangent(const glm::dvec3& normal, const glm::dvec3& chord)
{
    if (glm::length(normal) < 0.5)
        return std::nullopt;
    const glm::dvec3 t = chord - normal * glm::dot(chord, normal);
    const double len = glm::length(t);
    if (len < 1e-9 * std::max(1.0, glm::length(chord)))
        return std::nullopt;
    return t / len * glm::length(chord);
}

glm::dvec3 hermite(const glm::dvec3& a, const glm::dvec3& ma, const glm::dvec3& b, const glm::dvec3& mb, double u)
{
    const double u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * ma + (-2 * u3 + 3 * u2) * b + (u3 - u2) * mb;
}

/** The point t of the way along the Hermite span by arc length, so points at even t sit evenly along it. */
glm::dvec3 atLength(const glm::dvec3& a, const glm::dvec3& ma, const glm::dvec3& b, const glm::dvec3& mb, double t)
{
    constexpr int n = 64;
    std::array<double, n + 1> len{};
    glm::dvec3 prev = a;
    for (int k = 1; k <= n; ++k)
    {
        const glm::dvec3 q = hermite(a, ma, b, mb, double(k) / n);
        len[size_t(k)] = len[size_t(k - 1)] + glm::distance(prev, q);
        prev = q;
    }
    if (len[n] < 1e-12)
        return glm::mix(a, b, t);
    const double want = t * len[n];
    const size_t k = size_t(std::clamp(long(std::lower_bound(len.begin(), len.end(), want) - len.begin()), 1L, long(n)));
    const double span = len[k] - len[k - 1];
    const double u = (double(k - 1) + (span > 1e-15 ? (want - len[k - 1]) / span : 0.0)) / n;
    return hermite(a, ma, b, mb, u);
}

struct Flow
{
    const Mesh& m;
    const EdgeFlowOptions& o;
    std::vector<glm::dvec3> normals;

    glm::dvec3 normal(Index p) const { return normals[p]; }

    /** Where line l puts its point: t of the way from a to b, on the curve through the points beyond, eased by amount. */
    glm::dvec3 target(const std::vector<glm::dvec3>& pos, const Across& l) const
    {
        const glm::dvec3 A = pos[l.a], B = pos[l.b], chord = B - A;
        const glm::dvec3 flat = A + chord * l.t;
        const double ease = o.amount * (1.0 - o.tension);
        // Arcs through the points beyond, blended toward the nearer end: a circle comes back exactly, evenly spaced.
        // A point beyond that folds back over the span would make its arc go the long way round, so it is ignored.
        auto arcFrom = [&](Index far, const glm::dvec3& near, const glm::dvec3& other, double t) -> std::optional<glm::dvec3> {
            if (!o.edgeFlow || far == kInvalid || glm::dot(near - pos[far], other - pos[far]) < 0.0)
                return std::nullopt;
            return arcPoint(pos[far], near, other, t);
        };
        const std::optional<glm::dvec3> fromA = arcFrom(l.a2, A, B, l.t);
        const std::optional<glm::dvec3> fromB = arcFrom(l.b2, B, A, 1.0 - l.t);
        if (fromA && fromB)
            return flat + ease * ((1.0 - l.t) * *fromA + l.t * *fromB - flat);
        if (fromA || fromB)
            return flat + ease * ((fromA ? *fromA : *fromB) - flat);

        std::optional<glm::dvec3> ma = normalTangent(normal(l.a), chord), mb = normalTangent(normal(l.b), chord);
        if (!ma && mb)
            ma = (3.0 * chord - *mb) * 0.5;
        if (!mb && ma)
            mb = (3.0 * chord - *ma) * 0.5;
        const glm::dvec3 onCurve = atLength(A, ma.value_or(chord), B, mb.value_or(chord), l.t);
        return flat + ease * (onCurve - flat);
    }

    /**
     * The move for a point on these lines. A loose point's lines each place it along themselves; what they say off
     * their own direction is averaged, so a bend both see is not taken twice.
     */
    glm::dvec3 displacement(const std::vector<glm::dvec3>& pos, const std::vector<Across>& ls, const glm::dvec3& v) const
    {
        if (ls.size() == 1)
            return target(pos, ls[0]) - v;
        glm::dvec3 along(0.0), off(0.0);
        for (const Across& l : ls)
        {
            const glm::dvec3 d = target(pos, l) - v, chord = pos[l.b] - pos[l.a];
            const double len = glm::length(chord);
            const glm::dvec3 a = len > 1e-12 ? chord / len * glm::dot(d, chord / len) : glm::dvec3(0.0);
            along += a;
            off += d - a;
        }
        return along + off / double(ls.size());
    }
};

/** Eases the moved points of a chain toward the four-point curve through their loop neighbours, square to the loop. */
void smoothAlong(const EdgePath& path, const std::vector<uint8_t>& movable, double amount, std::vector<glm::dvec3>& pos)
{
    const size_t n = path.points.size();
    auto at = [&](long i) { return pos[path.points[size_t((i % long(n) + long(n)) % long(n))]]; };
    std::vector<std::pair<Index, glm::dvec3>> next;
    for (size_t i = 0; i < n; ++i)
    {
        const Index p = path.points[i];
        if (!movable[p] || (!path.closed && (i == 0 || i + 1 == n)))
            continue;
        const long k = long(i);
        const bool wide = path.closed ? n >= 5 : i >= 2 && i + 2 < n;
        const glm::dvec3 target = wide ? (9.0 * (at(k - 1) + at(k + 1)) - (at(k - 2) + at(k + 2))) / 16.0 : (at(k - 1) + at(k + 1)) * 0.5;
        glm::dvec3 d = target - pos[p];
        const glm::dvec3 t = at(k + 1) - at(k - 1);
        if (const double len = glm::length(t); len > 1e-12)
            d -= t / len * glm::dot(d, t / len);
        next.push_back({p, pos[p] + amount * d});
    }
    for (auto& [p, q] : next)
        pos[p] = q;
}
} // namespace looptools

PointMoves editEdgeFlow(const Mesh& mesh, std::span<const Index> edges, std::span<const Index> points, const EdgeFlowOptions& opts)
{
    LILA_ZONE();

    using namespace looptools;
    std::vector<Index> valid;
    for (Index e : edges)
        if (mesh.edgeValid(e))
            valid.push_back(e);
    const std::vector<EdgePath> chains = chainEdges(mesh, valid);
    std::vector<Index> considered;
    for (const EdgePath& path : chains)
        considered.insert(considered.end(), path.points.begin(), path.points.end());
    for (Index p : points)
        if (mesh.pointValid(p))
            considered.push_back(p);
    considered = uniqueSorted(considered);
    const std::vector<uint8_t> sel = makeMask(mesh.pointCapacity(), considered);

    std::map<Index, std::vector<Across>> lines;
    for (const EdgePath& path : chains)
        chainLines(mesh, path, sel, lines);
    for (Index p : points)
        if (mesh.pointValid(p))
            looseLines(mesh, p, sel, lines);

    Flow flow{mesh, opts, std::vector<glm::dvec3>(mesh.pointCapacity(), glm::dvec3(0.0))};
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
        {
            const glm::dvec3 a = mesh.faceAreaVector(f);
            mesh.forEachFaceCorner(f, [&](Index c) { flow.normals[mesh.cornerPoint(c)] += a; });
        }
    for (glm::dvec3& n : flow.normals)
        if (const double len = glm::length(n); len > 1e-12)
            n /= len;

    std::vector<glm::dvec3> pos(mesh.positions().begin(), mesh.positions().end());
    std::vector<uint8_t> movable(mesh.pointCapacity(), 0);
    for (auto& [p, ls] : lines)
        movable[p] = ls.empty() ? 0 : 1;
    for (int it = 0; it < std::max(1, opts.iterations); ++it)
    {
        std::vector<std::pair<Index, glm::dvec3>> next;
        for (auto& [p, ls] : lines)
        {
            if (ls.empty())
                continue;
            next.push_back({p, pos[p] + flow.displacement(pos, ls, pos[p])});
        }
        for (auto& [p, q] : next)
            pos[p] = q;
        if (opts.loopSmooth != 0.0)
            for (const EdgePath& path : chains)
                smoothAlong(path, movable, opts.loopSmooth, pos);
    }

    PointMoves out;
    for (Index p : considered)
        out.push_back({p, pos[p]});
    return out;
}

void flowSelectionOfPoints(const Mesh& mesh, std::span<const Index> points, std::vector<Index>& edges, std::vector<Index>& loose)
{
    LILA_ZONE();

    edges.clear();
    loose.clear();
    const std::vector<uint8_t> in = makeMask(mesh.pointCapacity(), points);
    std::vector<int> degree(mesh.pointCapacity(), 0);
    std::vector<Index> between;
    for (Index p : uniqueSorted(points))
        if (mesh.pointValid(p))
            mesh.forEachPointEdge(p, [&](Index e) {
                const Index q = mesh.edgeOtherPoint(e, p);
                if (!in[q])
                    return;
                ++degree[p];
                if (p < q)
                    between.push_back(e);
            });
    std::vector<uint8_t> onChain(mesh.pointCapacity(), 0);
    for (Index e : between)
    {
        const Index p = mesh.edgePoint(e, 0), q = mesh.edgePoint(e, 1);
        if (degree[p] <= 2 && degree[q] <= 2)
        {
            edges.push_back(e);
            onChain[p] = onChain[q] = 1;
        }
    }
    for (Index p : uniqueSorted(points))
        if (mesh.pointValid(p) && !onChain[p])
            loose.push_back(p);
}
} // namespace lila::geo
