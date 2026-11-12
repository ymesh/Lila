#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "curve/Along.h"
#include "curve/Cable.h"
#include "curve/CurveBoolean.h"
#include "curve/Roof.h"
#include "curve/Sweep.h"
#include "geo/BoundingShapes.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "gn/Texture.h"

#include <tbb/parallel_for.h>

/** Surface and shape builders: revolve, loft, straight skeleton roofs, 2D offsets, tiling, cables and rocks. */
namespace lila::gn::nodes
{
namespace build
{
curve::Splines splinesOf(const GeometryPtr& g)
{
    curve::Splines out;
    if (!g->curves)
        return out;
    for (size_t c = 0; c < g->curves->numCurves(); ++c)
    {
        curve::Spline s = g->curves->spline(c);
        if (curve::isValid(s))
            out.push_back(std::move(s));
    }
    return out;
}

CurveSet curveSetOf(const curve::Splines& splines)
{
    CurveSet cs;
    for (const curve::Spline& s : splines)
        if (curve::isValid(s))
            cs.addSpline(s);
    return cs;
}

GeometryPtr meshOrEmpty(geo::Mesh m) { return m.empty() ? makeGeometry({}) : meshGeometry(std::move(m)); }
GeometryPtr curvesOrEmpty(CurveSet cs) { return cs.numCurves() == 0 ? makeGeometry({}) : curveGeometry(std::move(cs)); }

double radians(double degrees) { return degrees * kPi / 180.0; }

// ------------------------------------------------------------------------------------------------ revolve, loft

void surfaceNodes()
{
    registerDecl({
        .type = "Revolve",
        .label = "Revolve",
        .category = "Curve",
        .doc = "Spins each profile curve around an axis into a quad surface with UVs: U around, V along the profile. Profile points on "
               "the axis close into poles. A partial turn of a closed profile can be capped.",
        .sockets = {.inputs = {geoIn("curve", "Profile curves."), in("origin", T::Vector, vec(0, 0, 0), "A point on the axis."),
                               in("axis", T::Vector, vec(0, 1, 0)), in("startAngle", T::Float, 0.0, "Degrees."),
                               in("angle", T::Float, 360.0, "Degrees turned from the start."), bounded(in("segments", T::Int, 16), 1, 4096),
                               in("caps", T::Bool, true, "Close the open ends of a partial turn of a closed profile."), in("flip", T::Bool, false)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                curve::RevolveOptions o;
                o.origin = c.v("origin");
                o.axis = glm::length(c.v("axis")) > 1e-12 ? glm::normalize(c.v("axis")) : D3(0, 1, 0);
                o.startAngle = radians(c.f("startAngle"));
                o.endAngle = o.startAngle + radians(std::clamp(c.f("angle"), -360.0, 360.0));
                o.segments = int(std::clamp<int64_t>(c.i("segments"), 1, 4096));
                o.caps = c.b("caps");
                o.flip = c.b("flip");
                geo::Mesh m;
                for (const curve::Spline& s : splinesOf(c.geometry("curve")))
                    if (geo::Mesh part = curve::revolve(curve::tessellate(s), o); !part.empty())
                        m.append(part);
                c.out("mesh", meshOrEmpty(std::move(m)));
            },
    });

    registerDecl({
        .type = "Loft",
        .label = "Loft",
        .category = "Curve",
        .doc = "A quad surface through the curves in their order, with UVs: U across each curve, V from first to last. Curves are "
               "resampled to match; closed curves are turned to line up. Closed joins the last curve back to the first.",
        .sockets = {.inputs = {geoIn("curve", "Sections, in order."), bounded(in("samples", T::Int, 0, "Points across each curve; 0 keeps their own."), 0, 100000),
                               bounded(in("spansBetween", T::Int, 4, "Rows between neighbouring curves."), 1, 1000),
                               in("smooth", T::Bool, true, "Curved through the sections instead of straight between them."), in("closed", T::Bool, false),
                               in("caps", T::Bool, false, "Fill the open ends when the curves are closed."), in("autoAlign", T::Bool, true),
                               in("flip", T::Bool, false)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                curve::Splines sections = splinesOf(c.geometry("curve"));
                if (sections.size() < 2)
                {
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                curve::LoftOptions o;
                o.samples = int(std::clamp<int64_t>(c.i("samples"), 0, 100000));
                o.spansBetween = int(std::clamp<int64_t>(c.i("spansBetween"), 1, 1000));
                o.smooth = c.b("smooth");
                o.closeLoft = c.b("closed");
                o.caps = c.b("caps");
                o.autoAlign = c.b("autoAlign");
                o.flip = c.b("flip");
                geo::Mesh m;
                try
                {
                    m = curve::loft(sections, o);
                }
                catch (const std::invalid_argument& e)
                {
                    throw ExecError(std::string("Loft: ") + e.what());
                }
                c.out("mesh", meshOrEmpty(std::move(m)));
            },
    });
}

// ------------------------------------------------------------------------------------------------ skeleton, offset

void planarNodes()
{
    registerDecl({
        .type = "StraightSkeleton",
        .label = "Straight Skeleton",
        .category = "Curve",
        .doc = "The straight skeleton of the region closed planar curves enclose (inner loops are holes), as two-point curves, and a "
               "roof over it: hip slopes every side at the pitch, gable stands the short ends upright. Lift raises the skeleton onto a "
               "hip roof. The roof mesh has UVs along each slope and a face \"part\" (0 roof, 1 gable, 2 underside, 3 fascia).",
        .props = {prop("roof", {"hip", "gable"}, "hip")},
        .sockets = {.inputs = {geoIn("curve"), bounded(in("pitch", T::Float, 30.0, "Roof slope, degrees."), 0.0, 89.0),
                               bounded(in("overhang", T::Float, 0.0, "How far the eaves reach past the outline, cm."), 0.0),
                               bounded(in("thickness", T::Float, 0.0, "Roof slab thickness, cm; 0 is a single surface."), 0.0),
                               bounded(in("uvScale", T::Float, 100.0, "cm per UV unit."), 0.001), in("lift", T::Bool, false)},
                    .outputs = {geoOut("skeleton"), geoOut("roof")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                curve::Splines footprint = splinesOf(c.geometry("curve"));
                if (footprint.empty())
                {
                    c.out("skeleton", makeGeometry({}));
                    c.out("roof", makeGeometry({}));
                    return;
                }
                double pitch = std::clamp(c.f("pitch"), 0.0, 89.0);
                const double tol = 0.01;
                curve::SkeletonCurvesResult arcs = curve::skeletonArcs(footprint, c.b("lift") ? std::tan(radians(pitch)) : 0.0, D3(0.0), tol);
                c.out("skeleton", curvesOrEmpty(curveSetOf(arcs.curves)));

                curve::RoofOptions o;
                o.pitch = pitch;
                o.overhang = std::max(0.0, c.f("overhang"));
                o.thickness = std::max(0.0, c.f("thickness"));
                o.uvScale = std::max(0.001, c.f("uvScale"));
                o.gableEnds = c.prop("roof") == "gable";
                o.tolerance = tol;
                c.out("roof", meshOrEmpty(curve::roof(footprint, o).mesh));
            },
    });

    registerDecl({
        .type = "PolygonOffset",
        .label = "Polygon Offset 2D",
        .category = "Curve",
        .doc = "Grows or shrinks the region closed planar curves enclose (holes by containment) within their plane. Loops that run "
               "into each other merge, parts that pinch apart split and parts that vanish are dropped. Straight runs stay straight; "
               "corners follow the join.",
        .props = {prop("join", {"miter", "round", "square"}, "miter")},
        .sockets = {.inputs = {geoIn("curve"), in("distance", T::Float, 10.0, "cm; negative insets."),
                               bounded(in("miterLimit", T::Float, 4.0, "Miter corners longer than this many times the distance are squared off."), 1.0),
                               bounded(in("steps", T::Int, 1, "Evenly spaced offsets out to the distance."), 1, 1000)},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                curve::Splines curves = splinesOf(c.geometry("curve"));
                std::erase_if(curves, [](const curve::Spline& s) { return !s.closed; });
                curve::RegionOffsetOptions o;
                curve::parseJoinType(c.prop("join"), &o.join);
                o.miterLimit = std::max(1.0, c.f("miterLimit"));
                int steps = int(std::clamp<int64_t>(c.i("steps"), 1, 1000));
                CurveSet out;
                if (!curves.empty())
                    for (int k = 1; k <= steps; ++k)
                    {
                        o.distance = c.f("distance") * k / steps;
                        if (o.distance == 0.0)
                        {
                            out.append(curveSetOf(curves));
                            continue;
                        }
                        out.append(curveSetOf(curve::regionOffset(curves, o).curves));
                    }
                c.out("curve", curvesOrEmpty(std::move(out)));
            },
    });
}

// ------------------------------------------------------------------------------------------------ mesh tiler

/** Module bounds as a footprint: x and z across the tile, y up from its lowest point. */
struct TileFrame
{
    geo::Bounds b;
    double sx = 1.0, sz = 1.0;
};

/** One copy of the module laid over a target face: its footprint spread bilinearly over the face, height along the blended normal. */
void tileOntoFace(geo::Mesh& out, const geo::Mesh& module, const TileFrame& tf, const std::array<D3, 4>& p, const std::array<D3, 4>& n, double heightScale)
{
    Index first = out.append(module);
    for (Index k = 0; k < module.pointCapacity(); ++k)
    {
        if (!module.pointValid(k))
            continue;
        D3 q = module.position(k);
        double u = (q.x - tf.b.min.x) / tf.sx, v = (q.z - tf.b.min.z) / tf.sz, h = (q.y - tf.b.min.y) * heightScale;
        double w[4] = {(1 - u) * (1 - v), u * (1 - v), u * v, (1 - u) * v};
        D3 at(0.0), nn(0.0);
        for (int i = 0; i < 4; ++i)
        {
            at += p[i] * w[i];
            nn += n[i] * w[i];
        }
        double len = glm::length(nn);
        out.setPosition(first + k, at + (len > 1e-12 ? nn / len : D3(0, 1, 0)) * h);
    }
}

geo::Mesh tileSurface(const geo::Mesh& module, const geo::Mesh& target, const std::string& fit)
{
    TileFrame tf;
    tf.b = module.bounds();
    tf.sx = std::max(tf.b.max.x - tf.b.min.x, 1e-9);
    tf.sz = std::max(tf.b.max.z - tf.b.min.z, 1e-9);
    std::vector<glm::vec3> normals = geo::pointNormals(target);
    geo::Mesh out;
    for (Index f = 0; f < target.faceCapacity(); ++f)
    {
        if (!target.faceValid(f))
            continue;
        std::vector<Index> pts = target.facePoints(f);
        if (pts.size() != 3 && pts.size() != 4)
            continue;
        if (pts.size() == 3)
            pts.push_back(pts[2]);
        std::array<D3, 4> p, n;
        for (int i = 0; i < 4; ++i)
        {
            p[i] = target.position(pts[i]);
            n[i] = D3(normals[pts[i]]);
        }
        double heightScale = 1.0;
        if (fit == "fit")
            heightScale = std::sqrt(std::max(target.faceArea(f), 1e-12) / (tf.sx * tf.sz));
        tileOntoFace(out, module, tf, p, n, heightScale);
    }
    out.cornerAttrs().remove(geo::attr::kNormal);
    return out;
}

geo::Mesh tileCurves(const geo::Mesh& module, const CurveSet& curves, const std::string& fit, int count, const std::string& axis)
{
    curve::ArrayAlongOptions o;
    o.deform = true;
    o.weld = true;
    o.forward = axis == "z" ? curve::AlongAxis::PosZ : curve::AlongAxis::PosX;
    if (fit == "stretch")
    {
        o.fill = curve::ArrayFill::Count;
        o.count = count;
    }
    else
    {
        o.fill = curve::ArrayFill::Spacing;
        o.spacing = 0.0;
        o.fitToLength = fit == "fit";
    }
    geo::Mesh out;
    for (size_t c = 0; c < curves.numCurves(); ++c)
    {
        curve::Rail rail(curves.spline(c));
        if (!rail.valid())
            continue;
        try
        {
            out.append(curve::arrayAlongCurve(module, nullptr, nullptr, rail, o));
        }
        catch (const std::invalid_argument&)
        {
            // A curve too short for one module adds nothing.
        }
    }
    return out;
}

void tilerNode()
{
    registerDecl({
        .type = "MeshTiler",
        .label = "Mesh Tiler",
        .category = "Mesh",
        .doc = "Tiles a module mesh along each curve of the target, or across every quad of a target mesh (a grid). Along curves the "
               "module's forward axis follows the curve and each copy bends with it: stretch fits count copies to the length, fit "
               "rounds to whole copies and evens them out, repeat keeps their own length. On a surface the module's X and Z footprint "
               "fills each quad and its Y rises along the normal; fit scales the height with the quad, stretch keeps it in cm.",
        .props = {prop("fit", {"fit", "stretch", "repeat"}, "fit"), prop("forwardAxis", {"x", "z"}, "x", "Module axis laid along curves.")},
        .sockets = {.inputs = {geoIn("module"), geoIn("target", "Curves, or a mesh of quads."), bounded(in("count", T::Int, 4, "Stretch along curves: copies per curve."), 1, 100000)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr mod = c.geometry("module"), target = c.geometry("target");
                if (!mod->mesh || mod->mesh->empty())
                {
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                std::string fit = c.prop("fit");
                geo::Mesh out;
                if (target->curves && target->curves->numCurves())
                    out = tileCurves(*mod->mesh, *target->curves, fit, int(std::clamp<int64_t>(c.i("count"), 1, 100000)), c.prop("forwardAxis"));
                else if (target->mesh)
                    out = tileSurface(*mod->mesh, *target->mesh, fit);
                c.out("mesh", meshOrEmpty(std::move(out)));
            },
    });
}

// ------------------------------------------------------------------------------------------------ cable

void cableNode()
{
    registerDecl({
        .type = "Cable",
        .label = "Cable",
        .category = "Curve",
        .doc = "Hanging cables between anchors: the points of a point cloud in order, or each curve's points (a closed curve wraps). "
               "Each span is a true catenary with slack (extra length as a fraction of the span) or a sag depth in cm. Count above 1 "
               "makes a loose bundle. Tube also sweeps a round mesh along them.",
        .props = {prop("mode", {"slack", "sag"}, "slack")},
        .sockets = {.inputs = {geoIn("anchors", "Points or curves."), bounded(in("slack", T::Float, 0.1), 0.0), bounded(in("sag", T::Float, 20.0, "cm."), 0.0),
                               in("gravity", T::Vector, vec(0, -1, 0)), bounded(in("segments", T::Int, 32, "Points per span."), 2, 4096),
                               bounded(in("count", T::Int, 1), 1, 64), bounded(in("spread", T::Float, 2.0, "Bundle: how far apart the ends sit, cm."), 0.0),
                               in("seed", T::Int, 1), in("tube", T::Bool, true), bounded(in("radius", T::Float, 1.0, "Tube radius, cm."), 0.001),
                               bounded(in("sides", T::Int, 8), 3, 256)},
                    .outputs = {geoOut("curve"), geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("anchors");
                curve::CableOptions o;
                o.mode = c.prop("mode") == "sag" ? curve::SagMode::Depth : curve::SagMode::Slack;
                o.slack = std::max(0.0, c.f("slack"));
                o.sag = std::max(0.0, c.f("sag"));
                o.gravity = glm::length(c.v("gravity")) > 1e-12 ? c.v("gravity") : D3(0, -1, 0);
                o.samples = int(std::clamp<int64_t>(c.i("segments"), 2, 4096));
                o.count = int(std::clamp<int64_t>(c.i("count"), 1, 64));
                o.spread = std::max(0.0, c.f("spread"));
                o.seed = uint32_t(c.i("seed"));
                curve::Splines cables;
                auto addChain = [&](const std::vector<D3>& anchors, bool closed) {
                    if (anchors.size() < 2)
                        return;
                    curve::CableResult r = curve::makeCables(anchors, closed, o);
                    cables.insert(cables.end(), r.cables.begin(), r.cables.end());
                };
                if (g->curves)
                    for (size_t cu = 0; cu < g->curves->numCurves(); ++cu)
                    {
                        std::span<const D3> pts = g->curves->curvePoints(cu);
                        addChain({pts.begin(), pts.end()}, g->curves->cyclic[cu] != 0);
                    }
                if (g->points)
                    addChain({g->points->positions().begin(), g->points->positions().end()}, false);

                c.out("curve", curvesOrEmpty(curveSetOf(cables)));
                geo::Mesh tube;
                if (c.b("tube"))
                {
                    curve::Profile profile = curve::circleProfile(int(std::clamp<int64_t>(c.i("sides"), 3, 256)), std::max(0.001, c.f("radius")));
                    for (const curve::Spline& s : cables)
                        tube.append(curve::sweep(profile, curve::pathFrom(s)).mesh);
                }
                c.out("mesh", meshOrEmpty(std::move(tube)));
            },
    });
}

// ------------------------------------------------------------------------------------------------ rock

double rockRandom(uint32_t seed, uint32_t k, uint32_t salt) { return hashToUnit(hash32(seed, k, salt)); }

double rockNoise(const D3& p, double seedOffset, int octaves)
{
    double sum = 0.0, amp = 0.5, freq = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * tex::gradientNoise4(glm::dvec4(p * freq, seedOffset + o * 7.31));
        amp *= 0.5;
        freq *= 2.03;
    }
    return sum;
}

/** Distance from the origin along unit dir to the boundary of a convex mesh of outward triangles around it. */
double hullReach(const std::vector<std::pair<D3, double>>& planes, const D3& dir)
{
    double best = std::numeric_limits<double>::max();
    for (const auto& [n, d] : planes)
    {
        double den = glm::dot(n, dir);
        if (den > 1e-12)
            best = std::min(best, d / den);
    }
    return best == std::numeric_limits<double>::max() ? 0.0 : best;
}

/** Box-projected uv0 per corner, by each face's dominant normal axis. */
void rockUvs(geo::Mesh& m, double cmPerUnit)
{
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        D3 n = glm::abs(m.faceNormal(f));
        int axis = n.x > n.y ? (n.x > n.z ? 0 : 2) : (n.y > n.z ? 1 : 2);
        m.forEachFaceCorner(f, [&](Index c) {
            D3 p = m.position(m.cornerPoint(c)) / cmPerUnit;
            uv[c] = axis == 0 ? glm::vec2(p.z, p.y) : axis == 1 ? glm::vec2(p.x, p.z) : glm::vec2(p.x, p.y);
        });
    }
}

geo::Mesh makeRock(uint32_t seed, D3 size, double faceting, int cuts, double cutDepth, double erosion, double erosionScale, int detail)
{
    size = glm::max(size, D3(1e-3));
    const D3 half = size * 0.5;
    int count = int(std::lround(200.0 - 188.0 * std::clamp(faceting, 0.0, 1.0)));
    std::vector<D3> pts;
    pts.reserve(count);
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int k = 0; k < count; ++k)
    {
        double y = 1.0 - 2.0 * (k + 0.5) / count, r = std::sqrt(std::max(0.0, 1.0 - y * y));
        double a = golden * k + rockRandom(seed, uint32_t(k), 1) * 0.6;
        D3 d(std::cos(a) * r, y, std::sin(a) * r);
        pts.push_back(d * half * (0.8 + 0.3 * rockRandom(seed, uint32_t(k), 2)));
    }
    for (int k = 0; k < cuts; ++k)
    {
        double z = 2.0 * rockRandom(seed, uint32_t(k), 3) - 1.0, a = 2.0 * kPi * rockRandom(seed, uint32_t(k), 4);
        double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        D3 n(std::cos(a) * r, z, std::sin(a) * r);
        double reach = glm::length(n * half);
        double plane = reach * (1.0 - cutDepth * (0.4 + 0.6 * rockRandom(seed, uint32_t(k), 5)));
        for (D3& p : pts)
            if (double over = glm::dot(p, n) - plane; over > 0.0)
                p -= n * over;
    }
    geo::Mesh hull = geo::convexHullMesh(pts, int(pts.size()) + 1);
    D3 centre(0.0);
    for (const D3& p : pts)
        centre += p;
    centre /= double(pts.size());
    std::vector<std::pair<D3, double>> planes;
    for (Index f = 0; f < hull.faceCapacity(); ++f)
        if (hull.faceValid(f))
        {
            D3 n = hull.faceNormal(f);
            planes.push_back({n, glm::dot(n, hull.position(hull.facePoints(f)[0]) - centre)});
        }

    geo::IcoSphereOptions io;
    io.radius = 1.0;
    io.subdivisions = std::clamp(detail, 1, 7);
    geo::Mesh m = geo::makeIcoSphere(io);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            D3 d = glm::normalize(m.position(p));
            m.setPosition(p, centre + d * hullReach(planes, d));
        }
    if (erosion > 0.0)
    {
        std::vector<glm::vec3> normals = geo::pointNormals(m);
        double depth = erosion * glm::length(half) * 0.15, offset = rockRandom(seed, 0, 6) * 100.0;
        std::vector<D3> moved(m.pointCapacity());
        tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
            if (!m.pointValid(p))
                return;
            D3 q = m.position(p);
            double pits = std::abs(rockNoise(q / std::max(erosionScale, 1e-3), offset, 4));
            moved[p] = q - D3(normals[p]) * depth * pits;
        });
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                m.setPosition(p, moved[p]);
    }
    geo::Bounds b = m.bounds();
    D3 scale = size / glm::max(b.size(), D3(1e-9));
    D3 base((b.min.x + b.max.x) * 0.5, b.min.y, (b.min.z + b.max.z) * 0.5);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, (m.position(p) - base) * scale);
    geo::setHardEdgesByAngle(m, 35.0);
    rockUvs(m, 100.0);
    return m;
}

void rockNode()
{
    registerDecl({
        .type = "Rock",
        .label = "Rock",
        .category = "Mesh",
        .doc = "A procedural rock from a seed: a hull of jittered points, cut by random planes into flat faces, then pitted by noise. "
               "Faceting toward 1 gives fewer, larger facets. It fills Size exactly, sitting on the ground plane, centred, with box-projected UVs (1 per m) "
               "and hard edges at its sharp facets.",
        .sockets = {.inputs = {in("seed", T::Int, 0), in("size", T::Vector, vec(100, 70, 80), "cm."), bounded(in("faceting", T::Float, 0.6), 0.0, 1.0),
                               bounded(in("cuts", T::Int, 6, "Planes sliced off."), 0, 64), bounded(in("cutDepth", T::Float, 0.25, "How deep each cut reaches, against the size."), 0.0, 0.9),
                               bounded(in("erosion", T::Float, 0.3), 0.0, 2.0), bounded(in("erosionScale", T::Float, 25.0, "Size of the pits, cm."), 0.01),
                               bounded(in("detail", T::Int, 4, "Mesh resolution."), 1, 7)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                c.out("mesh", meshGeometry(makeRock(uint32_t(c.i("seed")), glm::abs(c.v("size")), c.f("faceting"), int(std::clamp<int64_t>(c.i("cuts"), 0, 64)),
                                                    std::clamp(c.f("cutDepth"), 0.0, 0.9), std::clamp(c.f("erosion"), 0.0, 2.0), c.f("erosionScale"),
                                                    int(std::clamp<int64_t>(c.i("detail"), 1, 7)))));
            },
    });
}
} // namespace build

void registerBuildNodes()
{
    build::surfaceNodes();
    build::planarNodes();
    build::tilerNode();
    build::cableNode();
    build::rockNode();
}
} // namespace lila::gn::nodes
