#include "MaskCompute.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <queue>

namespace weathering::masks
{
using namespace lila;
using geo::Index;

namespace
{
/** Corner names, render::kGrungeAttrs order. */
constexpr std::array<const char*, kMaskCount> kCornerAttrs = {"grungeEdge", "grungeCavity", "grungeAo", "grungeDust", "grungeStreak", "grungeFade"};

uint32_t maskHash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double maskUnit(uint32_t seed, uint32_t a, uint32_t b) { return double(maskHash(seed * 0x9e3779b9u ^ maskHash(a * 0x85ebca6bu + b))) / 4294967296.0; }

geo::Mesh toWorld(const geo::Mesh& m, const glm::dmat4& world)
{
    geo::Mesh w = m;
    for (glm::dvec3& q : w.positions())
        q = glm::dvec3(world * glm::dvec4(q, 1.0));
    return w;
}

/** Self and neighbours as world-space BVHs: rays see both, contacts only the others. */
struct Scene
{
    geo::Mesh self, others;
    geo::MeshBvh selfBvh, othersBvh;
    bool hasOthers = false;

    /** Nearest hit of either, and the hit face's normal. */
    bool cast(const glm::dvec3& o, const glm::dvec3& d, double tMax, bool backfaces, double& t, glm::dvec3& n) const
    {
        geo::BvhHit a, b;
        const bool ha = !selfBvh.empty() && selfBvh.raycast(o, d, a, tMax, backfaces);
        const bool hb = hasOthers && othersBvh.raycast(o, d, b, tMax, backfaces);
        if (!ha && !hb)
            return false;
        const bool useA = ha && (!hb || a.t <= b.t);
        t = useA ? a.t : b.t;
        n = useA ? self.faceNormal(a.face) : others.faceNormal(b.face);
        return true;
    }

    bool blocked(const glm::dvec3& o, const glm::dvec3& d, double tMax) const
    {
        return (!selfBvh.empty() && selfBvh.occluded(o, d, tMax)) || (hasOthers && othersBvh.occluded(o, d, tMax));
    }
};

/** A sample: one point under one smoothing group's normal. */
struct Sample
{
    Index point = 0;
    glm::dvec3 normal{0.0};
};

/** A streak source: a horizontal-ish edge water runs down the wall from, with the wall's normal. */
struct StreakSource
{
    glm::dvec3 a{0.0}, b{0.0};
    glm::dvec3 wallNormal{0.0};
};

/** Geodesic distance over the edges from the given seed points, capped at limit. */
std::vector<double> edgeDistance(const geo::Mesh& w, const std::vector<char>& seed, double limit)
{
    std::vector<double> d(w.pointCapacity(), limit);
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    for (Index p = 0; p < w.pointCapacity(); ++p)
        if (seed[p])
        {
            d[p] = 0.0;
            q.push({0.0, p});
        }
    while (!q.empty())
    {
        auto [dist, p] = q.top();
        q.pop();
        if (dist > d[p])
            continue;
        w.forEachPointEdge(p, [&](Index e) {
            const Index o = w.edgeOtherPoint(e, p);
            const double nd = dist + glm::length(w.position(o) - w.position(p));
            if (nd < d[o])
            {
                d[o] = nd;
                q.push({nd, o});
            }
        });
    }
    return d;
}

double heightOf(const glm::dvec3& p, const glm::dvec3& up) { return glm::dot(p, up); }

/** Whether an edge wears: on the boundary, or convex past the feature angle; never where it rests on something. */
bool sharpConvex(const geo::Mesh& w, const std::vector<glm::dvec3>& faceN, Index e, double cosFeature, const std::vector<char>& contactEdge)
{
    if (contactEdge[e])
        return false;
    if (w.edgeFaceCount(e) != 2)
        return true;
    const Index c0 = w.edgeCorner(e);
    const Index f0 = w.cornerFace(c0), f1 = w.cornerFace(w.cornerRadial(c0));
    if (glm::dot(faceN[f0], faceN[f1]) > cosFeature)
        return false;
    return glm::dot(w.faceCentroid(f1) - w.faceCentroid(f0), faceN[f0]) < 0.0;
}

/** Per corner, its signed distance to each of up to four sharp edge lines of its face (the longest ones), kFarEdge for none. */
std::vector<glm::vec4> sharpEdgeDistances(const geo::Mesh& w, const std::vector<glm::dvec3>& faceN, double cosFeature,
                                          const std::vector<char>& contactEdge)
{
    std::vector<glm::vec4> out(w.cornerCapacity(), glm::vec4(kFarEdge));
    for (Index f = 0; f < w.faceCapacity(); ++f)
    {
        if (!w.faceValid(f))
            continue;
        struct Line
        {
            glm::dvec3 a, inward;
            double length;
        };
        std::vector<Line> lines;
        w.forEachFaceCorner(f, [&](Index c) {
            const Index e = w.cornerEdge(c);
            if (!sharpConvex(w, faceN, e, cosFeature, contactEdge))
                return;
            const glm::dvec3 a = w.position(w.cornerPoint(c)), b = w.position(w.cornerNextPoint(c));
            const glm::dvec3 along = b - a;
            const double len = glm::length(along);
            if (len < 1e-9)
                return;
            // Corners run counter-clockwise about the normal, so normal x along points into the face.
            lines.push_back({a, glm::normalize(glm::cross(faceN[f], along)), len});
        });
        // A line through a reflex edge of a concave face cuts back across it; only lines the whole face lies inside of count.
        std::erase_if(lines, [&](const Line& l) {
            bool behind = false;
            w.forEachFaceCorner(f, [&](Index c) { behind = behind || glm::dot(w.position(w.cornerPoint(c)) - l.a, l.inward) < -0.5; });
            return behind;
        });
        std::sort(lines.begin(), lines.end(), [](const Line& x, const Line& y) { return x.length > y.length; });
        if (lines.size() > 4)
            lines.resize(4);
        w.forEachFaceCorner(f, [&](Index c) {
            glm::vec4 d(kFarEdge);
            for (size_t k = 0; k < lines.size(); ++k)
                d[int(k)] = float(glm::dot(w.position(w.cornerPoint(c)) - lines[k].a, lines[k].inward));
            out[c] = d;
        });
    }
    return out;
}
/** Per corner, signed distances to up to four lines of its face picked by keep (the longest), kFarEdge for none. */
template <class Keep> std::vector<glm::vec4> faceLineDistances(const geo::Mesh& w, const std::vector<glm::dvec3>& faceN, Keep keep)
{
    std::vector<glm::vec4> out(w.cornerCapacity(), glm::vec4(kFarEdge));
    for (Index f = 0; f < w.faceCapacity(); ++f)
    {
        if (!w.faceValid(f))
            continue;
        struct Line
        {
            glm::dvec3 a, inward;
            double length;
        };
        std::vector<Line> lines;
        w.forEachFaceCorner(f, [&](Index c) {
            if (!keep(w.cornerEdge(c)))
                return;
            const glm::dvec3 a = w.position(w.cornerPoint(c)), b = w.position(w.cornerNextPoint(c));
            const double len = glm::length(b - a);
            if (len > 1e-9)
                lines.push_back({a, glm::normalize(glm::cross(faceN[f], b - a)), len});
        });
        // A line through a reflex edge of a concave face cuts back across it; only lines the whole face lies inside of count.
        std::erase_if(lines, [&](const Line& l) {
            bool behind = false;
            w.forEachFaceCorner(f, [&](Index c) { behind = behind || glm::dot(w.position(w.cornerPoint(c)) - l.a, l.inward) < -0.5; });
            return behind;
        });
        std::sort(lines.begin(), lines.end(), [](const Line& x, const Line& y) { return x.length > y.length; });
        if (lines.size() > 4)
            lines.resize(4);
        w.forEachFaceCorner(f, [&](Index c) {
            glm::vec4 d(kFarEdge);
            for (size_t k = 0; k < lines.size(); ++k)
                d[int(k)] = float(glm::dot(w.position(w.cornerPoint(c)) - lines[k].a, lines[k].inward));
            out[c] = d;
        });
    }
    return out;
}
} // namespace

Result compute(const geo::Mesh& mesh, const glm::dmat4& world, std::span<const Neighbour> neighbours, const Settings& s)
{
    LILA_ZONE();

    Result r;
    for (std::vector<float>& v : r.corners)
        v.assign(mesh.cornerCapacity(), 0.0f);
    if (mesh.numFaces() == 0)
        return r;

    Scene sc;
    sc.self = toWorld(mesh, world);
    sc.selfBvh.build(sc.self);
    for (const Neighbour& n : neighbours)
        if (n.mesh && n.mesh->numFaces() > 0)
        {
            sc.others.append(toWorld(*n.mesh, n.world));
            sc.hasOthers = true;
        }
    if (sc.hasOthers)
        sc.othersBvh.build(sc.others);

    const geo::Mesh& w = sc.self;
    const glm::dvec3 up = glm::normalize(s.up);
    const geo::Bounds box = w.bounds();
    const double eps = std::max(0.02, 1e-4 * glm::length(box.max - box.min));
    const double cosFeature = std::cos(glm::radians(std::clamp(s.edgeAngle, 1.0, 179.0)));

    std::vector<glm::dvec3> faceN(w.faceCapacity(), glm::dvec3(0.0));
    for (Index f = 0; f < w.faceCapacity(); ++f)
        if (w.faceValid(f))
            faceN[f] = w.faceNormal(f);

    // Feature edges: convex ones seed wear, concave ones dirt. Sills and overhang roots seed streaks.
    std::vector<char> convexPt(w.pointCapacity(), 0), concavePt(w.pointCapacity(), 0);
    std::vector<StreakSource> sources;
    auto faceBelow = [&](Index f, double h) {
        bool below = false;
        w.forEachFaceCorner(f, [&](Index c) { below = below || heightOf(w.position(w.cornerPoint(c)), up) < h - 1e-3; });
        return below;
    };
    for (Index e = 0; e < w.edgeCapacity(); ++e)
    {
        if (!w.edgeValid(e) || w.edgeFaceCount(e) != 2)
            continue;
        const Index c0 = w.edgeCorner(e);
        const Index f0 = w.cornerFace(c0), f1 = w.cornerFace(w.cornerRadial(c0));
        const glm::dvec3 n0 = faceN[f0], n1 = faceN[f1];
        if (glm::dot(n0, n1) > cosFeature)
            continue;
        const bool convex = glm::dot(w.faceCentroid(f1) - w.faceCentroid(f0), n0) < 0.0;
        const Index pa = w.edgePoint(e, 0), pb = w.edgePoint(e, 1);
        (convex ? convexPt : concavePt)[pa] = 1;
        (convex ? convexPt : concavePt)[pb] = 1;
        const glm::dvec3 a = w.position(pa), b = w.position(pb);
        const glm::dvec3 along = b - a;
        if (std::abs(glm::dot(along, up)) > 0.2 * glm::length(along))
            continue;
        const double h = std::min(heightOf(a, up), heightOf(b, up));
        for (int k = 0; k < 2; ++k)
        {
            const Index wall = k == 0 ? f0 : f1, other = k == 0 ? f1 : f0;
            const glm::dvec3 wn = faceN[wall], on = faceN[other];
            if (std::abs(glm::dot(wn, up)) > 0.3 || !faceBelow(wall, h))
                continue;
            // A sill: the wall turns into an up-facing ledge that something covers (an opening's top), so water pools.
            if (convex && glm::dot(on, up) > 0.7)
            {
                double t;
                glm::dvec3 hn;
                if (sc.cast(w.faceCentroid(other) + up * eps, up, s.coverHeight, true, t, hn))
                    sources.push_back({a, b, wn});
            }
            // An overhang's root: the wall meets a down-facing underside, so drips run down from it.
            if (!convex && glm::dot(on, up) < -0.7)
                sources.push_back({a, b, wn});
        }
    }
    // Contact edges: concave ones, and ones lying on the ground plane or on a neighbour's surface (a wall's foot, a
    // block against another). Dirt and occlusion run from them per pixel; they never wear.
    std::vector<char> contactEdge(w.edgeCapacity(), 0);
    const double touch = std::max(2.0, eps * 4.0);
    auto onSomething = [&](const glm::dvec3& q) {
        if (s.groundPlane && std::abs(heightOf(q, up) - s.groundHeight) < touch)
            return true;
        geo::BvhClosest c;
        return sc.hasOthers && sc.othersBvh.closestPoint(q, c, touch);
    };
    for (Index e = 0; e < w.edgeCapacity(); ++e)
    {
        if (!w.edgeValid(e))
            continue;
        const glm::dvec3 a = w.position(w.edgePoint(e, 0)), b = w.position(w.edgePoint(e, 1));
        bool concave = false;
        if (w.edgeFaceCount(e) == 2)
        {
            const Index c0 = w.edgeCorner(e);
            const Index f0 = w.cornerFace(c0), f1 = w.cornerFace(w.cornerRadial(c0));
            concave = glm::dot(faceN[f0], faceN[f1]) <= cosFeature && glm::dot(w.faceCentroid(f1) - w.faceCentroid(f0), faceN[f0]) > 0.0;
        }
        contactEdge[e] = concave || (onSomething(a) && onSomething(b) && onSomething((a + b) * 0.5)) ? 1 : 0;
    }
    const std::vector<double> convexD = edgeDistance(w, convexPt, std::max(s.edgeWidth, 1e-6) * 2.0);
    const std::vector<double> concaveD = edgeDistance(w, concavePt, std::max(s.cavityWidth, 1e-6) * 2.0);

    // Samples: each point once per smoothing group of the faces round it, so a hard-edged block's sides, top and
    // bottom each read their own values at a shared corner.
    std::vector<Sample> samples;
    std::vector<Index> cornerSample(w.cornerCapacity(), geo::kInvalid);
    const double cosSmooth = std::cos(glm::radians(40.0));
    for (Index p = 0; p < w.pointCapacity(); ++p)
    {
        if (!w.pointValid(p))
            continue;
        const size_t first = samples.size();
        w.forEachPointCorner(p, [&](Index c) {
            const glm::dvec3 fn = faceN[w.cornerFace(c)];
            glm::dvec3 sum(0.0);
            w.forEachPointCorner(p, [&](Index o) {
                if (glm::dot(faceN[w.cornerFace(o)], fn) > cosSmooth)
                    sum += w.faceAreaVector(w.cornerFace(o));
            });
            const glm::dvec3 n = glm::length(sum) > 1e-12 ? glm::normalize(sum) : fn;
            for (size_t k = first; k < samples.size(); ++k)
                if (glm::dot(samples[k].normal, n) > 0.999)
                {
                    cornerSample[c] = Index(k);
                    return;
                }
            cornerSample[c] = Index(samples.size());
            samples.push_back({p, n});
        });
    }
    r.samples = samples.size();

    // Hemisphere directions shared by every sample, turned per sample by a seeded angle.
    const int rays = std::clamp(s.aoRays, 1, 256);
    std::vector<glm::dvec3> hemiDirs(static_cast<size_t>(rays));
    for (int k = 0; k < rays; ++k)
    {
        const double u = (double(k) + 0.5) / double(rays);
        const double phi = double(k) * std::numbers::pi * (3.0 - std::sqrt(5.0));
        const double sinT = std::sqrt(u), cosT = std::sqrt(1.0 - u);
        hemiDirs[size_t(k)] = {std::cos(phi) * sinT, std::sin(phi) * sinT, cosT};
    }

    std::array<std::vector<float>, kMaskCount> values;
    for (std::vector<float>& v : values)
        v.assign(samples.size(), 0.0f);
    tbb::parallel_for(size_t(0), samples.size(), [&](size_t i) {
        const Sample& smp = samples[i];
        const glm::dvec3 p = w.position(smp.point);
        const glm::dvec3 n = smp.normal;
        const glm::dvec3 o = p + n * eps;
        const double facing = glm::dot(n, up);
        const double height = heightOf(p, up) - s.groundHeight;

        values[kEdge][i] = float(std::clamp(1.0 - convexD[smp.point] / std::max(s.edgeWidth, 1e-6), 0.0, 1.0));

        double contact = concaveD[smp.point];
        if (sc.hasOthers)
        {
            geo::BvhClosest c;
            if (sc.othersBvh.closestPoint(p, c, s.cavityWidth))
                contact = std::min(contact, c.distance);
        }
        if (s.groundPlane && height > -eps && facing < 0.5)
            contact = std::min(contact, std::max(height, 0.0));
        values[kCavity][i] = float(std::clamp(1.0 - contact / std::max(s.cavityWidth, 1e-6), 0.0, 1.0));

        // Occlusion over a cosine-weighted hemisphere, turned per sample so neighbours don't band.
        const glm::dvec3 t0 = glm::normalize(glm::cross(std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0), n));
        const glm::dvec3 t1 = glm::cross(n, t0);
        const double turn = maskUnit(s.seed, uint32_t(smp.point), uint32_t(i)) * 2.0 * std::numbers::pi;
        const double ct = std::cos(turn), st = std::sin(turn);
        int hits = 0;
        for (const glm::dvec3& h : hemiDirs)
        {
            const double x = h.x * ct - h.y * st, y = h.x * st + h.y * ct;
            const glm::dvec3 d = t0 * x + t1 * y + n * h.z;
            bool hit = sc.blocked(o, d, s.aoRadius);
            if (!hit && s.groundPlane && glm::dot(d, up) < -1e-6 && height > -eps)
                hit = (heightOf(o, up) - s.groundHeight) / -glm::dot(d, up) < s.aoRadius;
            hits += hit ? 1 : 0;
        }
        values[kAo][i] = float(double(hits) / double(rays));

        if (facing > 0.3)
        {
            const double open = sc.blocked(o, up, s.dustCover) ? 0.0 : 1.0;
            values[kDust][i] = float(std::clamp((facing - 0.3) / 0.7, 0.0, 1.0) * open);
        }

        if (std::abs(facing) < 0.5)
        {
            double streak = 0.0;
            for (const StreakSource& src : sources)
            {
                if (glm::dot(src.wallNormal, n) < 0.8 || std::abs(glm::dot(p - src.a, src.wallNormal)) > std::max(2.0, s.edgeWidth))
                    continue;
                glm::dvec3 along = (src.b - src.a) - up * glm::dot(src.b - src.a, up);
                const double len = glm::length(along);
                if (len < 1e-6)
                    continue;
                along /= len;
                const double x = glm::dot(p - src.a, along);
                const double spread = std::max(s.streakSpread, 1e-6);
                if (x < -spread || x > len + spread)
                    continue;
                const double u = std::clamp(x / len, 0.0, 1.0);
                const double drop = heightOf(src.a + (src.b - src.a) * u, up) - heightOf(p, up);
                if (drop < -1e-3 || drop > s.streakLength)
                    continue;
                const double ends = x < 0.0 ? 1.0 + x / spread : x > len ? 1.0 - (x - len) / spread : 1.0;
                streak = std::max(streak, (1.0 - std::max(drop, 0.0) / s.streakLength) * ends);
            }
            // Under an overhang, own or another object's: the first thing straight up is an underside.
            double t;
            glm::dvec3 hn;
            if (sc.cast(o, up, s.streakLength, false, t, hn) && glm::dot(hn, up) < -0.5)
                streak = std::max(streak, 1.0 - t / s.streakLength);
            values[kStreak][i] = float(std::clamp(streak, 0.0, 1.0));
        }

        if (facing < 0.5)
        {
            double h = std::numeric_limits<double>::max();
            double t;
            glm::dvec3 hn;
            if (sc.cast(o, -up, s.fadeHeight, false, t, hn))
                h = t;
            if (s.groundPlane && height > -eps)
                h = std::min(h, std::max(height, 0.0));
            values[kFade][i] = float(std::clamp(1.0 - h / std::max(s.fadeHeight, 1e-6), 0.0, 1.0));
        }
    });

    // Dust is one value per face: open to the sky from its middle, by how much it faces up. Corners differ in what is
    // above them, which would draw seams across a large roof.
    std::vector<float> faceDust(w.faceCapacity(), 0.0f);
    for (Index f = 0; f < w.faceCapacity(); ++f)
    {
        if (!w.faceValid(f))
            continue;
        const double facing = glm::dot(faceN[f], up);
        if (facing <= 0.3)
            continue;
        const double open = sc.blocked(w.faceCentroid(f) + faceN[f] * eps, up, s.dustCover) ? 0.0 : 1.0;
        faceDust[f] = float(std::clamp((facing - 0.3) / 0.7, 0.0, 1.0) * open);
    }
    // A corner's value stands for its whole face once interpolated: on a face much bigger than a mask's reach that
    // smears it over the face, so it fades with face size. The per-pixel fields carry those masks on big faces instead.
    std::vector<double> faceSize(w.faceCapacity(), 0.0);
    for (Index f = 0; f < w.faceCapacity(); ++f)
        if (w.faceValid(f))
            w.forEachFaceCorner(f, [&](Index c) {
                faceSize[f] = std::max(faceSize[f], glm::length(w.position(w.cornerNextPoint(c)) - w.position(w.cornerPoint(c))));
            });
    std::array<double, kMaskCount> reach{};
    reach[kEdge] = s.edgeWidth;
    reach[kCavity] = s.cavityWidth;
    reach[kAo] = s.aoRadius * 0.5;
    reach[kDust] = 1e30; // set per face below, so it never varies across one
    reach[kStreak] = s.streakLength;
    reach[kFade] = s.fadeHeight;
    for (Index c = 0; c < w.cornerCapacity(); ++c)
        if (w.cornerValid(c) && cornerSample[c] != geo::kInvalid)
        {
            const double size = faceSize[w.cornerFace(c)];
            for (int k = 0; k < kMaskCount; ++k)
            {
                // Full up to the mask's reach, gone by twice it.
                const double keep = std::clamp((2.0 * reach[size_t(k)] - size) / std::max(reach[size_t(k)], 1e-9), 0.0, 1.0);
                r.corners[size_t(k)][c] = float(values[size_t(k)][cornerSample[c]] * keep);
            }
            r.corners[kDust][c] = faceDust[w.cornerFace(c)];
        }
    r.edgeDistance = sharpEdgeDistances(w, faceN, cosFeature, contactEdge);
    r.contactDistance = faceLineDistances(w, faceN, [&](Index e) { return contactEdge[e] != 0; });

    // Height above what is below, per corner of side faces, from one base per face (what is under its lowest corner),
    // so it stays linear over the face and ground fade holds per pixel without seams.
    r.height.assign(w.cornerCapacity(), kFarEdge);
    for (Index f = 0; f < w.faceCapacity(); ++f)
    {
        if (!w.faceValid(f) || std::abs(glm::dot(faceN[f], up)) >= 0.5)
            continue;
        Index lowest = geo::kInvalid;
        w.forEachFaceCorner(f, [&](Index c) {
            if (lowest == geo::kInvalid || heightOf(w.position(w.cornerPoint(c)), up) < heightOf(w.position(w.cornerPoint(lowest)), up))
                lowest = c;
        });
        const glm::dvec3 low = w.position(w.cornerPoint(lowest));
        double below = std::numeric_limits<double>::max();
        double t;
        glm::dvec3 hn;
        if (sc.cast(low + faceN[f] * eps, -up, 1e5, false, t, hn))
            below = t;
        if (s.groundPlane && heightOf(low, up) - s.groundHeight > -eps)
            below = std::min(below, std::max(heightOf(low, up) - s.groundHeight, 0.0));
        if (below >= 1e5)
            continue;
        const double base = heightOf(low, up) - below;
        w.forEachFaceCorner(f, [&](Index c) { r.height[c] = float(heightOf(w.position(w.cornerPoint(c)), up) - base); });
    }

    // Streak sources over each side face: the two nearest above it that share its plane and overlap it sideways.
    r.streakLines.assign(w.cornerCapacity(), glm::vec4(kFarEdge, 0.0f, kFarEdge, 0.0f));
    for (Index f = 0; f < w.faceCapacity(); ++f)
    {
        if (!w.faceValid(f) || std::abs(glm::dot(faceN[f], up)) >= 0.5 || sources.empty())
            continue;
        double top = -1e300;
        w.forEachFaceCorner(f, [&](Index c) { top = std::max(top, heightOf(w.position(w.cornerPoint(c)), up)); });
        struct Pick
        {
            const StreakSource* src;
            double above;
        };
        std::vector<Pick> picks;
        for (const StreakSource& src : sources)
        {
            if (glm::dot(src.wallNormal, faceN[f]) < 0.8 || std::abs(glm::dot(w.faceCentroid(f) - src.a, faceN[f])) > std::max(2.0, s.edgeWidth))
                continue;
            const double h = std::max(heightOf(src.a, up), heightOf(src.b, up));
            if (h < top - s.streakLength - 1.0 || h > top + s.streakLength)
                continue;
            picks.push_back({&src, std::abs(h - top)});
        }
        std::sort(picks.begin(), picks.end(), [](const Pick& x, const Pick& y) { return x.above < y.above; });
        w.forEachFaceCorner(f, [&](Index c) {
            glm::vec4 v(kFarEdge, 0.0f, kFarEdge, 0.0f);
            const glm::dvec3 p = w.position(w.cornerPoint(c));
            for (size_t k = 0; k < picks.size() && k < 2; ++k)
            {
                const StreakSource& src = *picks[k].src;
                glm::dvec3 along = (src.b - src.a) - up * glm::dot(src.b - src.a, up);
                const double len = glm::length(along);
                if (len < 1e-6)
                    continue;
                along /= len;
                const double u = glm::dot(p - src.a, along) / len;
                const double drop = heightOf(src.a, up) + (heightOf(src.b, up) - heightOf(src.a, up)) * u - heightOf(p, up);
                v[int(k) * 2] = float(drop);
                v[int(k) * 2 + 1] = float(u);
            }
            r.streakLines[c] = v;
        });
    }
    return r;
}

void writeAttributes(geo::Mesh& mesh, const Result& r)
{
    auto vec4s = [&](const char* name, const std::vector<glm::vec4>& src) {
        if (src.empty())
            return;
        auto d = mesh.cornerAttrs().getOrAdd<glm::vec4>(name);
        for (size_t c = 0; c < d.size() && c < src.size(); ++c)
            d[c] = src[c];
    };
    vec4s("grungeContactDistance", r.contactDistance);
    vec4s("grungeStreakLines", r.streakLines);
    if (!r.height.empty())
    {
        auto d = mesh.cornerAttrs().getOrAdd<float>("grungeHeight");
        for (size_t c = 0; c < d.size() && c < r.height.size(); ++c)
            d[c] = r.height[c];
    }
    if (!r.edgeDistance.empty())
    {
        auto d = mesh.cornerAttrs().getOrAdd<glm::vec4>("grungeEdgeDistance");
        for (size_t c = 0; c < d.size() && c < r.edgeDistance.size(); ++c)
            d[c] = r.edgeDistance[c];
    }
    for (int k = 0; k < kMaskCount; ++k)
    {
        auto v = mesh.cornerAttrs().getOrAdd<float>(kCornerAttrs[size_t(k)]);
        const std::vector<float>& src = r.corners[size_t(k)];
        for (size_t c = 0; c < v.size() && c < src.size(); ++c)
            v[c] = src[c];
    }
}
} // namespace weathering::masks
