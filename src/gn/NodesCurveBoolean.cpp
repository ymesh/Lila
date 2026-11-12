#include "gn/NodesCommon.h"

#include "core/Profile.h"

#include <manifold/cross_section.h>

#include <algorithm>
#include <map>

namespace lila::gn::nodes::curvebool
{
/** A place on a poly curve: segment seg, t of the way along it. */
struct CurveCut
{
    Index seg = 0;
    double t = 0.0;

    double at() const { return double(seg) + t; }
};

struct Segment
{
    size_t curve = 0;
    Index seg = 0;
    D3 a, b;
    double lo = 0.0, hi = 0.0; // sweep extent along X
};

/** Closest points of segments p0p1 and q0q1 as parameters s and t (Ericson). */
void closestOnSegments(const D3& p0, const D3& p1, const D3& q0, const D3& q1, double& s, double& t)
{
    D3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    if (a <= 1e-18 && e <= 1e-18)
    {
        s = t = 0.0;
        return;
    }
    if (a <= 1e-18)
    {
        s = 0.0;
        t = std::clamp(f / e, 0.0, 1.0);
        return;
    }
    double cc = glm::dot(d1, r);
    if (e <= 1e-18)
    {
        t = 0.0;
        s = std::clamp(-cc / a, 0.0, 1.0);
        return;
    }
    double b = glm::dot(d1, d2), denom = a * e - b * b;
    s = denom > 1e-18 ? std::clamp((b * f - cc * e) / denom, 0.0, 1.0) : 0.0;
    t = (b * s + f) / e;
    if (t < 0.0)
    {
        t = 0.0;
        s = std::clamp(-cc / a, 0.0, 1.0);
    }
    else if (t > 1.0)
    {
        t = 1.0;
        s = std::clamp((b - cc) / a, 0.0, 1.0);
    }
}

/** Where segments cross seen down the Y axis (on the ground plane), as parameters along each. */
bool crossOnGround(const D3& p0, const D3& p1, const D3& q0, const D3& q1, double& s, double& t)
{
    glm::dvec2 a(p0.x, p0.z), b(p1.x, p1.z), c(q0.x, q0.z), d(q1.x, q1.z);
    glm::dvec2 r = b - a, q = d - c;
    double den = r.x * q.y - r.y * q.x;
    if (std::abs(den) < 1e-14)
        return false;
    glm::dvec2 w = c - a;
    s = (w.x * q.y - w.y * q.x) / den;
    t = (w.x * r.y - w.y * r.x) / den;
    return s >= -1e-9 && s <= 1.0 + 1e-9 && t >= -1e-9 && t <= 1.0 + 1e-9;
}

/** Splits every curve at its cuts; a closed curve with cuts opens into pieces from cut to cut. Point attributes are blended. */
CurveSet splitAtCuts(const CurveSet& cs, std::vector<std::vector<CurveCut>>& cuts)
{
    CurveSet out;
    out.pointAttrs.mergeLayout(cs.pointAttrs);
    out.curveAttrs.mergeLayout(cs.curveAttrs);
    for (size_t cu = 0; cu < cs.numCurves(); ++cu)
    {
        const size_t n = cs.curveSize(cu);
        const bool cyclic = cs.cyclic[cu] != 0;
        const Index p0 = cs.offsets[cu];
        const double end = double(cyclic ? n : n - 1);
        std::vector<double> at;
        for (const CurveCut& k : cuts[cu])
            at.push_back(std::clamp(k.at(), 0.0, end));
        std::sort(at.begin(), at.end());
        at.erase(std::unique(at.begin(), at.end(), [](double x, double y) { return std::abs(x - y) < 1e-7; }), at.end());
        if (!cyclic)
            std::erase_if(at, [&](double x) { return x < 1e-7 || x > end - 1e-7; });
        else if (!at.empty() && at.back() > end - 1e-7 && at.front() < 1e-7)
            at.pop_back();
        // A sample at parameter x: the blend of the points around it.
        auto add = [&](std::vector<std::pair<Index, Index>>& src, std::vector<float>& w, std::vector<D3>& pts, double x) {
            double fx = std::floor(x);
            size_t a = size_t(fx) % n, b = (a + 1) % n;
            double t = x - fx;
            if (!cyclic && a == n - 1)
            {
                b = a;
                t = 0.0;
            }
            pts.push_back(glm::mix(cs.positions[p0 + a], cs.positions[p0 + b], t));
            src.push_back({p0 + Index(a), p0 + Index(b)});
            w.push_back(float(t));
        };
        auto emit = [&](double from, double to, bool closed) {
            std::vector<std::pair<Index, Index>> src;
            std::vector<float> w;
            std::vector<D3> pts;
            add(src, w, pts, from);
            for (double v = std::floor(from) + 1.0; v < to - 1e-7; v += 1.0)
                if (v > from + 1e-7)
                    add(src, w, pts, v);
            if (!closed)
                add(src, w, pts, to);
            size_t q0 = out.numPoints();
            out.addCurve(pts, closed);
            out.curveAttrs.copyFrom(cs.curveAttrs, out.numCurves() - 1, cu);
            for (size_t k = 0; k < pts.size(); ++k)
            {
                Index s[2] = {src[k].first, src[k].second};
                float wt[2] = {1.0f - w[k], w[k]};
                out.pointAttrs.mixFrom(cs.pointAttrs, q0 + k, s, wt);
            }
        };
        if (n < 2 || at.empty())
        {
            emit(0.0, end, cyclic);
            continue;
        }
        if (!cyclic)
        {
            double prev = 0.0;
            for (double x : at)
            {
                emit(prev, x, false);
                prev = x;
            }
            emit(prev, end, false);
            continue;
        }
        for (size_t k = 0; k < at.size(); ++k)
        {
            double from = at[k], to = k + 1 < at.size() ? at[k + 1] : at[0] + end;
            // Unroll the loop past its end so a piece can wrap through the start.
            std::vector<std::pair<Index, Index>> src;
            std::vector<float> w;
            std::vector<D3> pts;
            auto wrapped = [&](double x) { return x >= end ? x - end : x; };
            add(src, w, pts, wrapped(from));
            for (double v = std::floor(from) + 1.0; v < to - 1e-7; v += 1.0)
                if (v > from + 1e-7)
                    add(src, w, pts, wrapped(v));
            add(src, w, pts, wrapped(to));
            size_t q0 = out.numPoints();
            out.addCurve(pts, false);
            out.curveAttrs.copyFrom(cs.curveAttrs, out.numCurves() - 1, cu);
            for (size_t j = 0; j < pts.size(); ++j)
            {
                Index s[2] = {src[j].first, src[j].second};
                float wt[2] = {1.0f - w[j], w[j]};
                out.pointAttrs.mixFrom(cs.pointAttrs, q0 + j, s, wt);
            }
        }
    }
    return out;
}

void curveIntersect()
{
    registerDecl({
        .type = "CurveIntersect",
        .label = "Curve Intersect",
        .category = "Curve",
        .doc = "Finds where curves cross: in 3D where they pass within distance of each other, or on the ground plane as seen from "
               "above. Gives a point at each crossing (\"curveA\", \"curveB\" name the curves) and, with split, the curves cut "
               "there. Bezier and NURBS curves are resolved to poly first.",
        .props = {prop("mode", {"distance", "ground"}, "distance")},
        .sockets = {.inputs = {geoIn("curves"), bounded(in("distance", T::Float, 0.1, "cm. Distance mode."), 0.0), in("selfIntersect", T::Bool, true),
                               in("split", T::Bool, true)},
                    .outputs = {geoOut("curves"), geoOut("points")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("curves");
                if (!g->curves || g->curves->numCurves() == 0)
                {
                    c.out("curves", g);
                    c.out("points", pointGeometry({}));
                    return;
                }
                const CurveSet ev = g->curves->evaluated();
                const bool ground = c.prop("mode") == "ground", self = c.b("selfIntersect");
                const double tol = std::max(c.f("distance"), 1e-9);
                std::vector<Segment> segs;
                for (size_t cu = 0; cu < ev.numCurves(); ++cu)
                {
                    size_t n = ev.curveSize(cu);
                    size_t count = n < 2 ? 0 : (ev.cyclic[cu] ? n : n - 1);
                    for (size_t k = 0; k < count; ++k)
                    {
                        Segment s{cu, Index(k), ev.positions[ev.offsets[cu] + k], ev.positions[ev.offsets[cu] + (k + 1) % n]};
                        double pad = ground ? 1e-9 : tol;
                        s.lo = std::min(s.a.x, s.b.x) - pad;
                        s.hi = std::max(s.a.x, s.b.x) + pad;
                        segs.push_back(s);
                    }
                }
                std::sort(segs.begin(), segs.end(), [](const Segment& a, const Segment& b) { return a.lo < b.lo; });
                std::vector<std::vector<CurveCut>> cuts(ev.numCurves());
                geo::Mesh pts;
                std::vector<int64_t> curveA, curveB;
                for (size_t i = 0; i < segs.size(); ++i)
                    for (size_t j = i + 1; j < segs.size() && segs[j].lo <= segs[i].hi; ++j)
                    {
                        const Segment &p = segs[i], &q = segs[j];
                        if (p.curve == q.curve)
                        {
                            if (!self)
                                continue;
                            size_t n = ev.curveSize(p.curve);
                            Index d = p.seg > q.seg ? p.seg - q.seg : q.seg - p.seg;
                            if (d <= 1 || (ev.cyclic[p.curve] && d == Index(n - 1)))
                                continue;
                        }
                        double s = 0.0, t = 0.0;
                        D3 at;
                        if (ground)
                        {
                            if (!crossOnGround(p.a, p.b, q.a, q.b, s, t))
                                continue;
                            s = std::clamp(s, 0.0, 1.0);
                            t = std::clamp(t, 0.0, 1.0);
                            at = 0.5 * (glm::mix(p.a, p.b, s) + glm::mix(q.a, q.b, t));
                        }
                        else
                        {
                            closestOnSegments(p.a, p.b, q.a, q.b, s, t);
                            D3 x = glm::mix(p.a, p.b, s), y = glm::mix(q.a, q.b, t);
                            if (glm::distance(x, y) > tol)
                                continue;
                            at = 0.5 * (x + y);
                        }
                        bool seen = false;
                        for (Index k = 0; k < pts.numPoints() && !seen; ++k)
                            seen = glm::distance(pts.position(k), at) < 1e-6;
                        cuts[p.curve].push_back({p.seg, s});
                        cuts[q.curve].push_back({q.seg, t});
                        if (seen)
                            continue;
                        pts.addPoint(at);
                        curveA.push_back(int64_t(std::min(p.curve, q.curve)));
                        curveB.push_back(int64_t(std::max(p.curve, q.curve)));
                    }
                writeAttribute(pts, "curveA", Domain::Point, Column::of(std::move(curveA)), T::Int);
                writeAttribute(pts, "curveB", Domain::Point, Column::of(std::move(curveB)), T::Int);
                c.out("points", pointGeometry(std::move(pts)));
                if (!c.b("split"))
                {
                    c.out("curves", g);
                    return;
                }
                Geometry out = *g;
                out.curves = std::make_shared<const CurveSet>(splitAtCuts(ev, cuts));
                c.out("curves", makeGeometry(std::move(out)));
            },
    });
}

/** Axes of the plane booleans work in, and the axis dropped. */
struct Plane
{
    int u = 0, v = 2, w = 1;
    double flipV = -1.0;
};

Plane planeOf(const std::string& s)
{
    if (s == "xy")
        return {0, 1, 2, 1.0};
    if (s == "yz")
        return {1, 2, 0, 1.0};
    return {};
}

/** The closed curves of a geometry as polygons in the plane, adding their dropped coordinates to depth. */
manifold::Polygons polygonsOf(const GeometryPtr& g, const Plane& pl, double& depth, size_t& depthCount)
{
    manifold::Polygons polys;
    if (!g->curves)
        return polys;
    CurveSet ev = g->curves->evaluated();
    for (size_t cu = 0; cu < ev.numCurves(); ++cu)
    {
        if (!ev.cyclic[cu] || ev.curveSize(cu) < 3)
            continue;
        manifold::SimplePolygon poly;
        for (const D3& p : ev.curvePoints(cu))
        {
            poly.push_back({p[pl.u], pl.flipV * p[pl.v]});
            depth += p[pl.w];
            ++depthCount;
        }
        polys.push_back(std::move(poly));
    }
    return polys;
}

void curveBoolean2d()
{
    registerDecl({
        .type = "CurveBoolean2D",
        .label = "Curve Boolean 2D",
        .category = "Curve",
        .doc = "Combines the areas inside closed curves lying in a plane: union, difference (a minus every b), intersect or xor. "
               "Curves inside curves of the same input are holes. Open curves are ignored. The result is closed poly curves at the "
               "inputs' average height above the plane.",
        .props = {prop("operation", {"union", "difference", "intersect", "xor"}, "union"), prop("plane", {"xz", "xy", "yz"}, "xz", "xz is the ground.")},
        .sockets = {.inputs = {geoIn("a"), multiIn("b")}, .outputs = {geoOut("curves")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                const Plane pl = planeOf(c.prop("plane"));
                const std::string op = c.prop("operation");
                double depth = 0.0;
                size_t depthCount = 0;
                using CS = manifold::CrossSection;
                CS a(polygonsOf(c.geometry("a"), pl, depth, depthCount), CS::FillRule::EvenOdd);
                std::vector<CS> bs;
                for (const GeometryPtr& g : c.geometries("b"))
                    bs.emplace_back(polygonsOf(g, pl, depth, depthCount), CS::FillRule::EvenOdd);
                CS b = bs.empty() ? CS() : CS::BatchBoolean(bs, manifold::OpType::Add);
                CS r;
                if (op == "union")
                    r = a.Boolean(b, manifold::OpType::Add);
                else if (op == "difference")
                    r = a.Boolean(b, manifold::OpType::Subtract);
                else if (op == "intersect")
                    r = a.Boolean(b, manifold::OpType::Intersect);
                else
                    r = a.Boolean(b, manifold::OpType::Add).Boolean(a.Boolean(b, manifold::OpType::Intersect), manifold::OpType::Subtract);
                const double h = depthCount ? depth / double(depthCount) : 0.0;
                CurveSet out;
                for (const manifold::SimplePolygon& poly : r.ToPolygons())
                {
                    std::vector<D3> pts;
                    for (const manifold::vec2& p : poly)
                    {
                        D3 q(0.0);
                        q[pl.u] = p.x;
                        q[pl.v] = pl.flipV * p.y;
                        q[pl.w] = h;
                        pts.push_back(q);
                    }
                    if (pts.size() >= 3)
                        out.addCurve(pts, true);
                }
                c.out("curves", curveGeometry(std::move(out)));
            },
    });
}
} // namespace lila::gn::nodes::curvebool

namespace lila::gn::nodes
{
void registerCurveBooleanNodes()
{
    curvebool::curveIntersect();
    curvebool::curveBoolean2d();
}
} // namespace lila::gn::nodes
