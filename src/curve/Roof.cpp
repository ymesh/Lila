#include "curve/Roof.h"

#include "core/Profile.h"
#include "curve/CapFill.h"
#include "curve/CurveMath.h"
#include "curve/Skeleton.h"
#include "geo/Attribute.h"
#include "geo/Normals.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lila::curve
{
namespace
{
using geo::Index;

/** Closed curves as regions of clean 2D loops on their plane. */
struct Regions
{
    PlaneFrame plane;
    std::vector<std::vector<std::vector<D2>>> loops; // per region: outer, then holes
    std::vector<std::string> warnings;
    double extent = 0.0;
};

Regions regionsOf(const Splines& curves, const glm::dvec3& normal, double tolerance)
{
    Regions r;
    double deviation = 0.0;
    r.plane = fitPlane(curves, normal, &deviation);
    TessOptions tess;
    tess.chordError = std::max(tolerance, 1e-5);
    Loops2 loops = loopsOf(curves, r.plane, tess, &r.warnings);
    for (const Loop2& l : loops)
        for (const D2& p : l.points)
            r.extent = std::max({r.extent, std::abs(p.x), std::abs(p.y)});
    if (deviation > std::max(0.01, r.extent * 1e-4))
        r.warnings.push_back("the curves are not planar (up to " + std::to_string(deviation) + " cm off their plane); they are flattened onto it");
    if (loopsCross(loops))
        r.warnings.push_back("the curves cross each other; the result may be wrong where they do");
    double tol = std::max(r.extent * 1e-9, 1e-12);
    for (Region2& reg : nestLoops(std::move(loops)))
    {
        std::vector<std::vector<D2>> polys;
        std::vector<D2> outer = cleanLoop(reg.outer.points, tol);
        if (outer.size() < 3)
            continue;
        polys.push_back(std::move(outer));
        for (Loop2& h : reg.holes)
            if (std::vector<D2> hole = cleanLoop(h.points, tol); hole.size() >= 3)
                polys.push_back(std::move(hole));
        r.loops.push_back(std::move(polys));
    }
    if (r.loops.empty())
        r.warnings.push_back("no closed curves enclosing an area");
    return r;
}

/** Each loop pushed out by d along its edges' normals, corners mitred (clamped at four times d). */
std::vector<D2> grow(const std::vector<D2>& loop, double d)
{
    size_t n = loop.size();
    std::vector<D2> out(n);
    for (size_t i = 0; i < n; ++i)
    {
        D2 a = glm::normalize(loop[i] - loop[(i + n - 1) % n]), b = glm::normalize(loop[(i + 1) % n] - loop[i]);
        D2 na(a.y, -a.x), nb(b.y, -b.x); // outward: right of travel
        D2 m = na + nb;
        double ml = glm::length(m);
        if (ml < 1e-9)
        {
            out[i] = loop[i] + na * d;
            continue;
        }
        m /= ml;
        double c = std::max(glm::dot(m, na), 0.25);
        out[i] = loop[i] + m * (d / c);
    }
    return out;
}

/** Edge flags for the gables: listed edges (as point pairs) and, with ends, every short edge between two convex corners. */
std::vector<std::vector<double>> roofWeights(const std::vector<std::vector<D2>>& polys, const std::set<std::pair<std::pair<double, double>, std::pair<double, double>>>& listed,
                                             bool ends)
{
    std::vector<std::vector<double>> w;
    for (const std::vector<D2>& l : polys)
    {
        size_t n = l.size();
        std::vector<double> lw(n, 1.0);
        for (size_t i = 0; i < n; ++i)
        {
            D2 a = l[i], b = l[(i + 1) % n];
            if (listed.count({{a.x, a.y}, {b.x, b.y}}) || listed.count({{b.x, b.y}, {a.x, a.y}}))
                lw[i] = 0.0;
            if (!ends)
                continue;
            D2 prev = l[i] - l[(i + n - 1) % n], cur = b - a, next = l[(i + 2) % n] - b;
            double lp = glm::length(prev), lc = glm::length(cur), ln = glm::length(next);
            if (cross2(prev, cur) > 0.0 && cross2(cur, next) > 0.0 && lc < lp * (1.0 - 1e-9) && lc < ln * (1.0 - 1e-9))
                lw[i] = 0.0;
        }
        w.push_back(std::move(lw));
    }
    return w;
}
} // namespace

RoofResult roof(const Splines& footprint, const RoofOptions& o)
{
    LILA_ZONE();

    RoofResult res;
    Regions regs = regionsOf(footprint, o.normal, o.tolerance);
    res.warnings = regs.warnings;
    // Gable edges by their end points, so the numbering follows the curves as drawn.
    std::set<std::pair<std::pair<double, double>, std::pair<double, double>>> listed;
    if (!o.gables.empty())
    {
        TessOptions tess;
        tess.chordError = std::max(o.tolerance, 1e-5);
        int base = 0;
        for (const Loop2& l : loopsOf(footprint, regs.plane, tess))
        {
            size_t n = l.points.size();
            for (int g : o.gables)
                if (g >= base && g < base + int(n))
                {
                    D2 a = l.points[size_t(g - base)], b = l.points[(size_t(g - base) + 1) % n];
                    listed.insert({{a.x, a.y}, {b.x, b.y}});
                }
            base += int(n);
        }
    }
    double pitch = glm::radians(std::clamp(o.pitch, 1.0, 89.0));
    double slope = std::tan(pitch);
    double overhang = std::max(0.0, o.overhang);
    double uv = o.uvScale > 0.0 ? o.uvScale : 100.0;
    geo::Mesh& m = res.mesh;
    auto parts = [&](Index f, int part) {
        auto pa = m.faceAttrs().getOrAdd<int32_t>("part");
        if (f != geo::kInvalid && f < pa.size())
            pa[f] = part;
    };
    auto setUvs = [&](Index f, const std::vector<glm::vec2>& uvs) {
        if (f == geo::kInvalid)
            return;
        auto attr = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        size_t i = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            if (i < uvs.size())
                attr[c] = uvs[i];
            ++i;
        });
    };
    for (const auto& polys0 : regs.loops)
    {
        std::vector<std::vector<D2>> polys;
        for (const std::vector<D2>& l : polys0)
            polys.push_back(overhang > 0.0 ? grow(l, overhang) : l);
        if (overhang > 0.0)
        {
            Loops2 check;
            for (const auto& l : polys)
                check.push_back({l, std::vector<double>(l.size(), 0.0), std::vector<uint8_t>(l.size(), 0), 0});
            if (loopsCross(check))
                res.warnings.push_back("the overhang is wider than the footprint allows; the eaves overlap");
        }
        SkeletonOptions so;
        so.weights = roofWeights(polys0, listed, o.gableEnds);
        StraightSkeleton sk = straightSkeleton(polys, so);
        if (!sk.ok())
            res.warnings.push_back("roof: " + sk.error);
        double rise = slope;
        if (o.height > 0.0)
        {
            double top = 0.0;
            for (double t : sk.times)
                top = std::max(top, t);
            rise = top > overhang ? o.height / (top - overhang) : slope;
        }
        std::vector<Index> top(sk.nodes.size(), geo::kInvalid), bottom(sk.nodes.size(), geo::kInvalid);
        double drop = o.thickness > 0.0 ? o.thickness * std::sqrt(1.0 + rise * rise) : 0.0;
        auto pointOf = [&](std::vector<Index>& ids, int n, double shift) {
            if (ids[size_t(n)] == geo::kInvalid)
                ids[size_t(n)] = m.addPoint(regs.plane.to3(sk.nodes[size_t(n)], (sk.times[size_t(n)] - overhang) * rise - shift));
            return ids[size_t(n)];
        };
        std::vector<double> edgeWeight;
        for (const std::vector<double>& lw : so.weights)
            edgeWeight.insert(edgeWeight.end(), lw.begin(), lw.end());
        std::map<std::pair<Index, Index>, int> topEdges;
        std::vector<Index> roofFaces;
        for (size_t e = 0; e < sk.faces.size(); ++e)
        {
            const std::vector<int>& face = sk.faces[e];
            if (face.size() < 3)
                continue;
            bool gable = e < edgeWeight.size() && edgeWeight[e] == 0.0;
            D2 s0 = sk.nodes[size_t(sk.edges[e][0])], d = sk.nodes[size_t(sk.edges[e][1])] - s0;
            double len = glm::length(d);
            if (len <= 0.0)
                continue;
            d /= len;
            D2 in(-d.y, d.x);
            std::vector<Index> pts;
            std::vector<glm::vec2> uvs;
            for (int n : face)
            {
                // Under a thick roof a gable is a wall of its own, hung below the slab.
                Index id = gable && drop > 0.0 ? m.addPoint(regs.plane.to3(sk.nodes[size_t(n)], (sk.times[size_t(n)] - overhang) * rise - drop))
                                               : pointOf(top, n, 0.0);
                if (!pts.empty() && pts.back() == id)
                    continue;
                pts.push_back(id);
                D2 q = sk.nodes[size_t(n)] - s0;
                double z = (sk.times[size_t(n)] - overhang) * rise;
                uvs.push_back(glm::vec2(float(glm::dot(q, d) / uv), float((gable ? z : glm::dot(q, in) * std::sqrt(1.0 + rise * rise)) / uv)));
            }
            while (pts.size() > 1 && pts.front() == pts.back())
            {
                pts.pop_back();
                uvs.pop_back();
            }
            if (pts.size() < 3)
                continue;
            Index f = m.addFace(pts);
            if (f == geo::kInvalid || m.faceArea(f) < 1e-12)
            {
                if (f != geo::kInvalid)
                    m.removeFace(f);
                continue;
            }
            setUvs(f, uvs);
            parts(f, gable ? 1 : 0);
            if (!gable)
            {
                roofFaces.push_back(f);
                for (size_t i = 0; i < pts.size(); ++i)
                    ++topEdges[{pts[i], pts[(i + 1) % pts.size()]}];
            }
        }
        if (drop <= 0.0)
            continue;
        // The underside, and a fascia round every open edge of the roof surface.
        std::map<Index, int> nodeOf;
        for (size_t n = 0; n < top.size(); ++n)
            if (top[n] != geo::kInvalid)
                nodeOf[top[n]] = int(n);
        for (Index f : roofFaces)
        {
            std::vector<Index> pts = m.facePoints(f), under;
            for (auto it = pts.rbegin(); it != pts.rend(); ++it)
                under.push_back(pointOf(bottom, nodeOf[*it], drop));
            Index g = m.addFace(under);
            parts(g, 2);
        }
        for (auto& [e, count] : topEdges)
        {
            if (topEdges.count({e.second, e.first}))
                continue;
            Index a = e.first, b = e.second;
            Index ab = pointOf(bottom, nodeOf[a], drop), bb = pointOf(bottom, nodeOf[b], drop);
            Index f = m.addFace({b, a, ab, bb});
            parts(f, 3);
            float l = float(glm::length(m.position(b) - m.position(a)) / uv), h = float(drop / uv);
            setUvs(f, {{l, h}, {0.0f, h}, {0.0f, 0.0f}, {l, 0.0f}});
        }
    }
    m.removeAllLoose();
    geo::setHardEdgesByAngle(m, 5.0);
    return res;
}

SkeletonCurvesResult insetCurves(const Splines& curves, double distance, int steps, const glm::dvec3& normal, double tolerance)
{
    LILA_ZONE();

    SkeletonCurvesResult res;
    Regions regs = regionsOf(curves, normal, tolerance);
    res.warnings = regs.warnings;
    steps = std::clamp(steps, 1, 1000);
    if (!(distance > 0.0))
    {
        res.warnings.push_back("the inset distance must be above 0");
        return res;
    }
    for (const auto& polys : regs.loops)
        for (int k = 1; k <= steps; ++k)
        {
            SkeletonOptions so;
            so.maxTime = distance * k / steps;
            StraightSkeleton sk = straightSkeleton(polys, so);
            if (!sk.ok())
                res.warnings.push_back(sk.error);
            for (const std::vector<int>& loop : sk.front)
            {
                Spline s;
                s.kind = Kind::Poly;
                s.closed = true;
                for (int n : loop)
                    s.points.push_back(regs.plane.to3(sk.nodes[size_t(n)]));
                s.normalize();
                if (isValid(s))
                    res.curves.push_back(std::move(s));
            }
        }
    if (res.curves.empty() && !regs.loops.empty())
        res.warnings.push_back("nothing is left at that inset");
    return res;
}

SkeletonCurvesResult centerLines(const Splines& curves, const glm::dvec3& normal, double tolerance)
{
    LILA_ZONE();

    SkeletonCurvesResult res;
    Regions regs = regionsOf(curves, normal, tolerance);
    res.warnings = regs.warnings;
    for (const auto& polys : regs.loops)
    {
        StraightSkeleton sk = straightSkeleton(polys);
        if (!sk.ok())
            res.warnings.push_back(sk.error);
        for (const std::vector<D2>& line : skeletonCenterLines(sk))
        {
            if (line.size() < 2)
                continue;
            Spline s;
            s.kind = Kind::Poly;
            for (const D2& p : line)
                s.points.push_back(regs.plane.to3(p));
            s.normalize();
            if (isValid(s))
                res.curves.push_back(std::move(s));
        }
    }
    return res;
}

SkeletonCurvesResult skeletonArcs(const Splines& curves, double rise, const glm::dvec3& normal, double tolerance)
{
    LILA_ZONE();

    SkeletonCurvesResult res;
    Regions regs = regionsOf(curves, normal, tolerance);
    res.warnings = regs.warnings;
    for (const auto& polys : regs.loops)
    {
        StraightSkeleton sk = straightSkeleton(polys);
        if (!sk.ok())
            res.warnings.push_back(sk.error);
        for (const SkeletonArc& a : sk.arcs)
        {
            if (a.a < 0 || a.b < 0 || a.a == a.b)
                continue;
            Spline s;
            s.kind = Kind::Poly;
            for (int n : {a.a, a.b})
                s.points.push_back(regs.plane.to3(sk.nodes[size_t(n)], sk.times[size_t(n)] * rise));
            if (glm::distance(s.points[0], s.points[1]) <= 1e-9)
                continue;
            s.normalize();
            res.curves.push_back(std::move(s));
        }
    }
    return res;
}

namespace
{
/** Least-squares affine map from plane coordinates to uv0 over the region's corners; false without uvs. */
bool fitUv(const geo::Mesh& m, const std::vector<Index>& faces, const PlaneFrame& plane, glm::dmat3x2& out)
{
    auto uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    if (uv.empty())
        return false;
    glm::dmat3 ata(0.0);
    glm::dvec3 atu(0.0), atv(0.0);
    for (Index f : faces)
        m.forEachFaceCorner(f, [&](Index c) {
            D2 q = plane.to2(m.position(m.cornerPoint(c)));
            glm::dvec3 r(q.x, q.y, 1.0);
            ata += glm::outerProduct(r, r);
            atu += r * double(uv[c].x);
            atv += r * double(uv[c].y);
        });
    if (std::abs(glm::determinant(ata)) < 1e-18)
        return false;
    glm::dmat3 inv = glm::inverse(ata);
    glm::dvec3 a = inv * atu, b = inv * atv;
    out = glm::dmat3x2(a.x, b.x, a.y, b.y, a.z, b.z);
    return true;
}
} // namespace

RegionInsetResult insetRegion(geo::Mesh& m, std::span<const Index> faces, const RegionInsetOptions& o)
{
    LILA_ZONE();

    RegionInsetResult res;
    if (!(o.distance > 0.0))
    {
        res.warnings.push_back("the inset distance must be above 0");
        return res;
    }
    std::set<Index> sel;
    for (Index f : faces)
        if (m.faceValid(f))
            sel.insert(f);
    std::set<Index> seen;
    for (Index seed : sel)
    {
        if (seen.count(seed))
            continue;
        // The region: selected faces joined by edges.
        std::vector<Index> patch = {seed};
        seen.insert(seed);
        for (size_t k = 0; k < patch.size(); ++k)
            m.forEachFaceCorner(patch[k], [&](Index c) {
                Index e = m.cornerEdge(c);
                m.forEachEdgeCorner(e, [&](Index d) {
                    Index g = m.cornerFace(d);
                    if (sel.count(g) && !seen.count(g))
                    {
                        seen.insert(g);
                        patch.push_back(g);
                    }
                });
            });
        D3 n(0.0), centre(0.0);
        for (Index f : patch)
        {
            n += m.faceAreaVector(f);
            centre += m.faceCentroid(f);
        }
        if (glm::length(n) < 1e-12)
        {
            res.warnings.push_back("a region has no area");
            continue;
        }
        n = glm::normalize(n);
        double cosLimit = std::cos(glm::radians(std::clamp(o.planarAngle, 0.0, 89.0)));
        bool flat = std::ranges::all_of(patch, [&](Index f) { return glm::dot(m.faceNormal(f), n) >= cosLimit - 1e-12; });
        if (!flat)
        {
            res.warnings.push_back("a region of " + std::to_string(patch.size()) + " faces is not flat; it was left alone");
            continue;
        }
        PlaneFrame plane = planeFrame(centre / double(patch.size()), n);
        // Its outline: directed edges with no selected face on the other side, chained into loops.
        std::set<Index> inPatch(patch.begin(), patch.end());
        std::map<Index, Index> next;
        bool pinched = false;
        for (Index f : patch)
            m.forEachFaceCorner(f, [&](Index c) {
                Index e = m.cornerEdge(c);
                bool shared = false;
                m.forEachEdgeCorner(e, [&](Index d) { shared = shared || (d != c && inPatch.count(m.cornerFace(d))); });
                if (shared)
                    return;
                Index a = m.cornerPoint(c), b = m.cornerNextPoint(c);
                if (next.count(a))
                    pinched = true;
                next[a] = b;
            });
        if (pinched || next.empty())
        {
            res.warnings.push_back("a region's outline touches itself at a point; it was left alone");
            continue;
        }
        std::vector<std::vector<Index>> loops;
        std::set<Index> used;
        for (auto& [a, b0] : next)
        {
            if (used.count(a))
                continue;
            std::vector<Index> loop;
            for (Index p = a; !used.count(p) && next.count(p); p = next[p])
            {
                used.insert(p);
                loop.push_back(p);
            }
            if (loop.size() >= 3)
                loops.push_back(std::move(loop));
        }
        std::vector<std::vector<D2>> polys;
        for (const auto& loop : loops)
        {
            polys.emplace_back();
            for (Index p : loop)
                polys.back().push_back(plane.to2(m.position(p)));
        }
        SkeletonOptions so;
        so.maxTime = o.distance;
        StraightSkeleton sk = straightSkeleton(polys, so);
        if (!sk.ok())
        {
            res.warnings.push_back("inset: " + sk.error + "; the region was left alone");
            continue;
        }
        glm::dmat3x2 uvMap(0.0);
        bool hasUv = fitUv(m, patch, plane, uvMap);
        Index src = patch.front();
        Index srcCorner = m.faceCorner(src);
        std::vector<Index> edgesBefore, pointsBefore;
        for (Index f : patch)
            m.forEachFaceCorner(f, [&](Index c) {
                edgesBefore.push_back(m.cornerEdge(c));
                pointsBefore.push_back(m.cornerPoint(c));
            });
        double height = 0.0;
        size_t outline = 0;
        for (const auto& loop : loops)
            for (Index p : loop)
            {
                height += plane.height(m.position(p));
                ++outline;
            }
        height /= double(std::max<size_t>(1, outline));
        for (Index f : patch)
            m.removeFace(f);

        std::vector<Index> ids(sk.nodes.size(), geo::kInvalid);
        size_t k = 0;
        for (const auto& loop : loops)
            for (Index p : loop)
                ids[k++] = p;
        auto pointOf = [&](int nd) {
            if (ids[size_t(nd)] == geo::kInvalid)
                ids[size_t(nd)] = m.addPoint(plane.to3(sk.nodes[size_t(nd)], height + o.depth * sk.times[size_t(nd)] / o.distance));
            return ids[size_t(nd)];
        };
        auto addFace = [&](std::vector<Index> pts, std::vector<Index>& into) {
            for (size_t i = 0; i < pts.size() && pts.size() >= 3;)
                if (pts[i] == pts[(i + 1) % pts.size()])
                    pts.erase(pts.begin() + std::ptrdiff_t(i));
                else
                    ++i;
            if (pts.size() < 3)
                return;
            std::vector<Index> srcCorners(pts.size(), srcCorner);
            Index f = m.addFace(pts, src, srcCorners);
            if (f == geo::kInvalid)
                return;
            if (hasUv)
            {
                auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
                m.forEachFaceCorner(f, [&](Index c) {
                    D2 q = plane.to2(m.position(m.cornerPoint(c)));
                    uv[c] = glm::vec2(uvMap * glm::dvec3(q.x, q.y, 1.0));
                });
            }
            into.push_back(f);
        };
        for (const std::vector<int>& face : sk.faces)
        {
            std::vector<Index> pts;
            for (int nd : face)
                pts.push_back(pointOf(nd));
            addFace(std::move(pts), res.band);
        }
        // The inside: each front loop's region, holes nested by containment.
        Loops2 fronts;
        for (const std::vector<int>& loop : sk.front)
        {
            Loop2 l;
            for (int nd : loop)
            {
                l.points.push_back(sk.nodes[size_t(nd)]);
                l.params.push_back(double(nd));
                l.kink.push_back(0);
            }
            fronts.push_back(std::move(l));
        }
        for (const Region2& r : nestLoops(fronts))
        {
            auto idsOf = [&](const Loop2& l) {
                std::vector<Index> out;
                for (double nd : l.params)
                    out.push_back(pointOf(int(nd)));
                return out;
            };
            if (r.holes.empty())
            {
                addFace(idsOf(r.outer), res.inner);
                continue;
            }
            std::vector<std::vector<D2>> fill = {r.outer.points};
            std::vector<Index> boundary = idsOf(r.outer);
            for (const Loop2& h : r.holes)
            {
                fill.push_back(h.points);
                for (Index p : idsOf(h))
                    boundary.push_back(p);
            }
            CapFillResult cf = capFill(fill);
            if (!cf.ok())
            {
                res.warnings.push_back("inset: " + cf.error);
                continue;
            }
            std::vector<Index> all(cf.points.size());
            for (size_t i = 0; i < cf.points.size(); ++i)
                all[i] = i < boundary.size() ? boundary[i] : m.addPoint(plane.to3(cf.points[i], height + o.depth));
            for (const std::vector<int>& f : cf.faces)
            {
                std::vector<Index> pts;
                for (int v : f)
                    pts.push_back(all[size_t(v)]);
                addFace(std::move(pts), res.inner);
            }
        }
        m.removeLoose(edgesBefore, pointsBefore);
    }
    return res;
}
} // namespace lila::curve
