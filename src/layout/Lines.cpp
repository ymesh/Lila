#include "layout/Lines.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Knife.h"
#include "geo/Normals.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::layout
{
namespace
{
constexpr size_t kMaxCutTriangles = 400'000;

double endFactor(const ResolvedRun& run, const LineSample& s, double hw, bool& outside)
{
    outside = false;
    const double len = run.path.length();
    const bool atA = s.s <= 1e-12, atB = s.s >= len - 1e-12;
    if (s.beyond > 0.0)
    {
        const EndStyle style = atA ? run.endA : run.endB;
        const bool free = atA ? run.freeA : run.freeB;
        if (free && style == EndStyle::Square)
            outside = true;
        if (free && style == EndStyle::Fade)
            outside = true;
    }
    double f = 1.0;
    // Fade ends grow shallow over three widths before the end.
    if (run.freeA && run.endA == EndStyle::Fade)
        f = std::min(f, std::clamp(s.s / (6.0 * hw), 0.0, 1.0));
    if (run.freeB && run.endB == EndStyle::Fade)
        f = std::min(f, std::clamp((len - s.s) / (6.0 * hw), 0.0, 1.0));
    (void)atB;
    return f;
}

/** A line beside the run, `offset` to its left (negative: right), extended past each end by that end's reach. */
std::vector<glm::dvec3> besideLine(const ResolvedRun& run, double offset, double extendA, double extendB)
{
    const double step = run.width * 0.5;
    const Path& p = run.path;
    std::vector<glm::dvec3> out;
    if (p.empty())
        return out;
    for (size_t k = 0; k < p.size(); ++k)
    {
        const glm::dvec3 t = p.tangentAt(p.lengths[k]);
        glm::dvec3 side = glm::cross(p.normals[k], t);
        const double len = glm::length(side);
        out.push_back(len > 1e-12 ? p.points[k] + side / len * offset : p.points[k]);
    }
    // Each extension first steps a groove's width out, so the end has a point where the groove meets the surface.
    const glm::dvec3 ta = p.tangentAt(0.0), tb = p.tangentAt(p.length());
    const glm::dvec3 a = out.front(), b = out.back();
    if (extendA > 0.0)
        out.insert(out.begin(), a - ta * std::min(extendA, step));
    if (extendA > step)
        out.insert(out.begin(), a - ta * extendA);
    if (extendB > 0.0)
        out.push_back(b + tb * std::min(extendB, step));
    if (extendB > step)
        out.push_back(b + tb * extendB);
    return out;
}

/** Longest edge of the face under p: how far a cut must run to leave it. */
double faceReach(const geo::Mesh& m, const geo::MeshBvh& bvh, const glm::dvec3& p)
{
    geo::BvhClosest c;
    if (!bvh.closestPoint(p, c) || !m.faceValid(c.face))
        return 0.0;
    double longest = 0.0;
    m.forEachFaceCorner(c.face, [&](Index corner) { longest = std::max(longest, m.edgeLength(m.cornerEdge(corner))); });
    return longest;
}

/**
 * A knife spot at p on face f: on the face's point or edge when it lies on one (a cut ending on the mesh border must
 * reach it as an edge spot, or the knife drops it as stopping inside the face), else inside the face.
 */
geo::KnifePoint knifePointAt(const geo::Mesh& m, Index f, const glm::dvec3& p)
{
    double longest = 0.0;
    m.forEachFaceCorner(f, [&](Index c) { longest = std::max(longest, m.edgeLength(m.cornerEdge(c))); });
    const double eps = std::max(1e-9, longest * 1e-6);
    geo::KnifePoint best = geo::KnifePoint::face(f, p);
    double bestD = eps;
    m.forEachFaceCorner(f, [&](Index c) {
        const Index e = m.cornerEdge(c);
        const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1)), d = b - a;
        const double len2 = glm::dot(d, d);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
        const double dist = glm::length(a + d * t - p);
        if (dist > bestD)
            return;
        bestD = dist;
        if (t * std::sqrt(len2) <= eps)
            best = geo::KnifePoint::point(m.edgePoint(e, 0));
        else if ((1.0 - t) * std::sqrt(len2) <= eps)
            best = geo::KnifePoint::point(m.edgePoint(e, 1));
        else
            best = geo::KnifePoint::edge(e, t);
    });
    return best;
}

/** Cuts the mesh along a polyline near its surface. Returns the edges made. */
size_t cutAlong(geo::Mesh& mesh, const std::vector<glm::dvec3>& line)
{
    if (line.size() < 2)
        return 0;
    const geo::MeshBvh bvh(mesh);
    std::vector<geo::KnifePoint> pts;
    for (const glm::dvec3& q : line)
    {
        geo::BvhClosest c;
        if (!bvh.closestPoint(q, c))
            continue;
        if (!pts.empty() && glm::length(pts.back().position - c.point) < 1e-9)
            continue;
        pts.push_back(knifePointAt(mesh, c.face, c.point));
    }
    if (pts.size() < 2)
        return 0;
    return geo::knifeCut(mesh, pts).size();
}
} // namespace

geo::Mesh decalStrips(const Resolved& r)
{
    geo::Mesh out;
    out.materialNames() = {kDecalMaterial};
    for (const ResolvedRun& run : r.runs())
    {
        if (run.output != Output::Decal || run.path.empty())
            continue;
        const Path& p = run.path;
        const double hw = run.width * 0.5, lift = std::max(0.002, run.width * 0.02);
        geo::Mesh strip;
        std::vector<Index> left, right;
        std::vector<double> along;
        for (size_t k = 0; k < p.size(); ++k)
        {
            const glm::dvec3 n = p.normals[k], t = p.tangentAt(p.lengths[k]);
            glm::dvec3 side = glm::cross(n, t);
            if (glm::length(side) < 1e-12)
                continue;
            side = glm::normalize(side);
            left.push_back(strip.addPoint(p.points[k] + side * hw + n * lift));
            right.push_back(strip.addPoint(p.points[k] - side * hw + n * lift));
            along.push_back(p.lengths[k]);
        }
        if (left.size() < 2)
            continue;
        std::vector<glm::vec2> uvs;
        std::vector<Index> faces;
        for (size_t k = 0; k + 1 < left.size(); ++k)
        {
            faces.push_back(strip.addFace({right[k], right[k + 1], left[k + 1], left[k]}));
            // U runs along the line in widths, so a tiling decal keeps its proportions; V crosses it.
            const float u0 = float(along[k] / run.width), u1 = float(along[k + 1] / run.width);
            uvs.insert(uvs.end(), {glm::vec2(u0, 0.0f), glm::vec2(u1, 0.0f), glm::vec2(u1, 1.0f), glm::vec2(u0, 1.0f)});
        }
        std::span<glm::vec2> uv = strip.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        size_t i = 0;
        for (Index f : faces)
            strip.forEachFaceCorner(f, [&](Index c) { uv[c] = uvs[i++]; });
        std::span<int32_t> mat = strip.faceAttrs().getOrAdd<int32_t>(std::string(geo::attr::kMaterial));
        for (Index f : faces)
            mat[f] = 0;
        strip.materialNames() = {kDecalMaterial};
        out.append(strip);
    }
    return out;
}

size_t cutRun(geo::Mesh& mesh, const geo::Mesh& original, const geo::MeshBvh& originalBvh, const ResolvedRun& run, double offset, double reach)
{
    // Cuts are made one at a time, so the lines a run meets may not be cut yet: every end runs on until it leaves its face.
    const double a = reach + 1.5 * faceReach(original, originalBvh, run.path.points.front());
    const double b = reach + 1.5 * faceReach(original, originalBvh, run.path.points.back());
    return cutAlong(mesh, besideLine(run, offset, a, b));
}

double grooveDepth(const Resolved& r, const glm::dvec3& p, uint32_t mask)
{
    double deepest = 0.0;
    for (const LineSample& s : r.linesNear(p, r.reach(), mask))
    {
        const ResolvedRun& run = r.runs()[size_t(s.run)];
        const double hw = run.width * 0.5;
        if (hw <= 0.0 || s.distance >= hw)
            continue;
        bool outside = false;
        const double f = endFactor(run, s, hw, outside);
        if (outside)
            continue;
        deepest = std::max(deepest, run.depth * profileDepth(run.profile, s.distance / hw) * f);
    }
    return deepest;
}

LinesResult applyLines(const Resolved& r)
{
    LILA_ZONE();
    LinesResult out;
    if (!r.mesh())
        return out;
    out.mesh = *r.mesh();
    geo::Mesh& m = out.mesh;

    bool anyGeometry = false, anySculpt = false;
    double narrowestSculpt = std::numeric_limits<double>::max();
    for (const ResolvedRun& run : r.runs())
    {
        anyGeometry |= run.output == Output::Geometry;
        if (run.output == Output::Sculpt)
        {
            anySculpt = true;
            narrowestSculpt = std::min(narrowestSculpt, run.width);
        }
    }
    uint32_t mask = outputBit(Output::Sculpt);
    if (anyGeometry)
    {
        if (size_t(m.numFaces()) * 2 > kMaxCutTriangles)
            out.warnings.push_back(std::format("Geometry lines displaced without cutting: the mesh has {} faces, more than cutting handles quickly. "
                                               "Set the line set to Sculpt to say so, or cut a lower level",
                                               m.numFaces()));
        else
        {
            LILA_ZONE_N("cut");
            const geo::MeshBvh original(*r.mesh());
            for (const ResolvedRun& run : r.runs())
            {
                if (run.output != Output::Geometry)
                    continue;
                const double hw = run.width * 0.5;
                for (double b : profileBreaks(run.profile))
                {
                    if (b <= 0.0)
                        out.cutEdges += cutRun(m, *r.mesh(), original, run, 0.0, hw);
                    else
                        for (double sgn : {1.0, -1.0})
                            out.cutEdges += cutRun(m, *r.mesh(), original, run, sgn * b * hw, hw);
                }
                // Square ends get a cut straight across.
                for (int end = 0; end < 2; ++end)
                {
                    const bool free = end == 0 ? run.freeA : run.freeB;
                    const EndStyle style = end == 0 ? run.endA : run.endB;
                    if (!free || style != EndStyle::Square)
                        continue;
                    const double s = end == 0 ? 0.0 : run.path.length();
                    const glm::dvec3 c = run.path.pointAt(s), t = run.path.tangentAt(s);
                    const glm::dvec3 side = glm::normalize(glm::cross(run.path.normalAt(s), t));
                    const double across = hw + 1.5 * faceReach(*r.mesh(), original, c);
                    out.cutEdges += cutAlong(m, {c - side * across, c, c + side * across});
                }
            }
        }
        mask |= outputBit(Output::Geometry);
    }

    if (anySculpt)
    {
        double edges = 0.0;
        size_t count = 0;
        for (Index e = 0; e < m.edgeCapacity() && count < 20000; ++e)
            if (m.edgeValid(e))
            {
                edges += m.edgeLength(e);
                ++count;
            }
        const double average = count ? edges / double(count) : 0.0;
        if (average > narrowestSculpt * 0.5)
            out.warnings.push_back(std::format("Sculpt lines on a mesh too coarse to show them: edges about {:.3g} cm, lines {:.3g} cm wide. "
                                               "Set the line set to Geometry, or add sculpt levels",
                                               average, narrowestSculpt));
    }

    displace(r, m, mask);
    if (std::any_of(r.runs().begin(), r.runs().end(), [](const ResolvedRun& run) { return run.output == Output::Decal; }))
        m.append(decalStrips(r));
    return out;
}

void displace(const Resolved& r, geo::Mesh& m, uint32_t mask)
{
    LILA_ZONE();
    const std::vector<glm::vec3> normals = geo::pointNormals(m);
    std::vector<glm::dvec3> moved(m.pointCapacity(), glm::dvec3(0.0));
    std::vector<char> hit(m.pointCapacity(), 0);
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p))
            return;
        const double d = grooveDepth(r, m.position(p), mask);
        if (d > 0.0 && p < normals.size())
        {
            moved[p] = m.position(p) - glm::dvec3(normals[p]) * d;
            hit[p] = 1;
        }
    });
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (hit[p])
            m.setPosition(p, moved[p]);
}
} // namespace lila::layout
