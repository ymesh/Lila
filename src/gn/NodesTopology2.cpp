#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "curve/Cdt.h"
#include "geo/Bvh.h"
#include "geo/Ops.h"
#include "geo/Topology.h"
#include "retopo/Retopo.h"
#include "uv/Seams.h"

#include <algorithm>
#include <map>
#include <numeric>

namespace lila::gn::nodes
{
namespace topo2
{
std::vector<uint8_t> boolsOn(const FieldPtr& f, const geo::Mesh& m, Domain d = Domain::Edge) { return evaluateAll<uint8_t>(f, FieldContext::forMesh(m, d)); }

double meshScale(const geo::Mesh& m)
{
    geo::Bounds b = m.bounds();
    return b.valid() ? std::max(1.0, glm::length(b.size())) : 1.0;
}

/** Two unit vectors that, with n, make a right-handed frame: cross(u, v) == n. */
void planeBasis(const D3& n, D3& u, D3& v)
{
    u = std::abs(n.x) < 0.9 ? glm::normalize(glm::cross(n, D3(1, 0, 0))) : glm::normalize(glm::cross(n, D3(0, 1, 0)));
    v = glm::cross(n, u);
}

std::vector<geo::Attribute*> uvLayers(geo::Mesh& m)
{
    std::vector<geo::Attribute*> out;
    for (size_t k = 0; k < m.cornerAttrs().layerCount(); ++k)
    {
        geo::Attribute& a = m.cornerAttrs().layer(k);
        if (a.type() == geo::AttrType::Float2 && a.name().rfind("uv", 0) == 0)
            out.push_back(&a);
    }
    return out;
}

/**
 * New faces get uvs flattened in the plane with normal n, at the uv scale of the faces bordering them and centred on
 * those faces' uvs where they meet, so the cap lands next to its surroundings at a matching texel density.
 */
void planarUvs(geo::Mesh& m, std::span<const Index> faces, const D3& n)
{
    if (faces.empty())
        return;
    std::vector<uint8_t> isNew = geo::makeMask(m.faceCapacity(), faces);
    D3 u, v;
    planeBasis(n, u, v);
    for (geo::Attribute* layer : uvLayers(m))
    {
        std::span<glm::vec2> uv = layer->values<glm::vec2>();
        std::vector<double> ratios;
        glm::dvec2 uvSum(0.0);
        D3 posSum(0.0);
        size_t count = 0;
        for (Index f : faces)
            m.forEachFaceCorner(f, [&](Index k) {
                Index e = m.cornerEdge(k);
                m.forEachEdgeCorner(e, [&](Index o) {
                    if (isNew[m.cornerFace(o)])
                        return;
                    Index o2 = m.cornerNext(o);
                    double len = glm::length(m.position(m.cornerPoint(o2)) - m.position(m.cornerPoint(o)));
                    if (len > 1e-12)
                        ratios.push_back(glm::length(glm::dvec2(uv[o2] - uv[o])) / len);
                    uvSum += glm::dvec2(uv[o]);
                    posSum += m.position(m.cornerPoint(o));
                    ++count;
                });
            });
        if (count == 0)
            continue;
        std::nth_element(ratios.begin(), ratios.begin() + std::ptrdiff_t(ratios.size() / 2), ratios.end());
        double scale = ratios.empty() || ratios[ratios.size() / 2] <= 0.0 ? 0.01 : ratios[ratios.size() / 2];
        glm::dvec2 uvCentre = uvSum / double(count);
        D3 centre = posSum / double(count);
        for (Index f : faces)
            m.forEachFaceCorner(f, [&](Index k) {
                D3 d = m.position(m.cornerPoint(k)) - centre;
                uv[k] = glm::vec2(uvCentre + scale * glm::dvec2(glm::dot(d, u), glm::dot(d, v)));
            });
    }
}

/** Triangulates the region the loops bound in the plane through origin, facing capNormal, holes respected. Returns the new faces. */
std::vector<Index> capPlanarLoops(geo::Mesh& m, std::span<const geo::EdgePath> loops, const D3& origin, const D3& capNormal)
{
    D3 u, v;
    planeBasis(capNormal, u, v);
    std::vector<glm::dvec2> pts;
    std::vector<Index> ids;
    std::vector<std::pair<int, int>> segs;
    for (const geo::EdgePath& loop : loops)
    {
        int first = int(pts.size());
        for (Index p : loop.points)
        {
            D3 d = m.position(p) - origin;
            pts.push_back({glm::dot(d, u), glm::dot(d, v)});
            ids.push_back(p);
        }
        int n = int(loop.points.size());
        for (int i = 0; i < n; ++i)
            segs.push_back({first + i, first + (i + 1) % n});
    }
    std::vector<Index> out;
    curve::CdtResult r = curve::constrainedDelaunay(pts, segs, true);
    for (const auto& t : r.triangles)
    {
        Index f = m.addFace({ids[size_t(t[0])], ids[size_t(t[1])], ids[size_t(t[2])]});
        if (f != geo::kInvalid)
            out.push_back(f);
    }
    return out;
}

void markFaceFlag(geo::Mesh& m, const std::string& name, std::span<const Index> faces)
{
    auto flags = m.faceAttrs().getOrAdd<bool>(name);
    for (Index f : faces)
        if (f < flags.size())
            flags[f] = true;
}

/** One side of the mesh cut by the plane: keep +1 above (along the normal), -1 below. */
geo::Mesh clipHalf(const geo::Mesh& src, const D3& origin, const D3& normal, int keep, bool cap, const std::string& capName, const std::string& cutName)
{
    geo::Mesh m = src;
    const double tol = 1e-6 * meshScale(m);
    geo::cutAlongPlane(m, origin, normal, tol);
    std::vector<Index> drop;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        double d = glm::dot(m.faceCentroid(f) - origin, normal) * double(keep);
        if (d <= tol)
            drop.push_back(f);
    }
    geo::deleteFaces(m, drop);
    auto onPlane = [&](Index p) { return std::abs(glm::dot(m.position(p) - origin, normal)) <= tol * 4.0; };
    if (cap)
    {
        std::vector<geo::EdgePath> loops;
        for (geo::EdgePath& loop : geo::boundaryLoops(m))
            if (loop.closed && loop.points.size() >= 3 && std::all_of(loop.points.begin(), loop.points.end(), onPlane))
                loops.push_back(std::move(loop));
        std::vector<Index> faces = capPlanarLoops(m, loops, origin, normal * double(-keep));
        planarUvs(m, faces, normal * double(-keep));
        markFaceFlag(m, capName, faces);
    }
    auto cut = m.edgeAttrs().getOrAdd<bool>(cutName);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeCorner(e) != geo::kInvalid && onPlane(m.edgePoint(e, 0)) && onPlane(m.edgePoint(e, 1)))
            cut[e] = true;
    return m;
}

/** Appends the parts of every curve on the kept side (sign * distance >= 0), cut where they cross the plane. */
void clipCurveSide(const CurveSet& in, const D3& origin, const D3& normal, int sign, CurveSet& out)
{
    auto addPoint = [&](size_t a, size_t b, double t) {
        size_t dst = out.positions.size();
        out.positions.push_back(glm::mix(in.positions[a], in.positions[b], t));
        out.pointAttrs.resize(out.positions.size());
        Index srcs[2] = {Index(a), Index(b)};
        float w[2] = {float(1.0 - t), float(t)};
        out.pointAttrs.mixFrom(in.pointAttrs, dst, srcs, w);
    };
    auto endCurve = [&](size_t c, size_t start, bool cyclic) {
        if (out.positions.size() - start < 2)
        {
            out.positions.resize(start);
            out.pointAttrs.resize(start);
            return;
        }
        out.offsets.push_back(Index(out.positions.size()));
        out.cyclic.push_back(cyclic ? 1 : 0);
        out.curveAttrs.resize(out.cyclic.size());
        out.curveAttrs.copyFrom(in.curveAttrs, out.cyclic.size() - 1, c);
    };
    for (size_t c = 0; c < in.numCurves(); ++c)
    {
        size_t o = in.offsets[c], n = in.curveSize(c);
        bool cyclic = in.cyclic[c] != 0;
        std::vector<double> d(n);
        for (size_t i = 0; i < n; ++i)
            d[i] = glm::dot(in.positions[o + i] - origin, normal) * double(sign);
        if (std::all_of(d.begin(), d.end(), [](double x) { return x >= 0.0; }))
        {
            size_t start = out.positions.size();
            for (size_t i = 0; i < n; ++i)
                addPoint(o + i, o + i, 0.0);
            endCurve(c, start, cyclic);
            continue;
        }
        // A cyclic curve is walked from a dropped point so no kept run wraps past the end.
        size_t first = 0;
        if (cyclic)
            while (d[first] >= 0.0)
                ++first;
        size_t segments = cyclic ? n : n - 1;
        size_t start = out.positions.size();
        bool open = false;
        if (d[first] >= 0.0)
        {
            addPoint(o + first, o + first, 0.0);
            open = true;
        }
        for (size_t s = 0; s < segments; ++s)
        {
            size_t i = (first + s) % n, j = (first + s + 1) % n;
            bool ki = d[i] >= 0.0, kj = d[j] >= 0.0;
            if (ki != kj)
            {
                double t = d[i] / (d[i] - d[j]);
                if (ki)
                {
                    if (d[i] > 0.0)
                        addPoint(o + i, o + j, t);
                    endCurve(c, start, false);
                    open = false;
                }
                else
                {
                    start = out.positions.size();
                    if (d[j] > 0.0)
                        addPoint(o + i, o + j, t);
                    open = true;
                }
            }
            if (kj && open)
                addPoint(o + j, o + j, 0.0);
        }
        if (open)
            endCurve(c, start, false);
    }
}

CurveSet clipCurves(const CurveSet& src, const D3& origin, const D3& normal, int keep)
{
    CurveSet in = src.evaluated();
    CurveSet out;
    out.pointAttrs.mergeLayout(in.pointAttrs);
    out.curveAttrs.mergeLayout(in.curveAttrs);
    out.pointAttrs.resize(0);
    out.curveAttrs.resize(0);
    if (keep >= 0)
        clipCurveSide(in, origin, normal, 1, out);
    if (keep <= 0)
        clipCurveSide(in, origin, normal, -1, out);
    out.syncSizes();
    return out;
}

void clipNode()
{
    registerDecl({
        .type = "Clip",
        .label = "Clip",
        .category = "Mesh",
        .doc = "Cuts meshes, curves and point clouds with a plane. Above keeps the side the normal points to, below the other, both keeps the two halves "
               "as separate pieces. Cap closes the cut with faces, holes respected, uvs matched to the surrounding texel density.",
        .props = {prop("keep", {"above", "below", "both"}, "above")},
        .sockets = {.inputs = {geoIn(), in("origin", T::Vector, vec(0, 0, 0), "A point on the plane."), in("normal", T::Vector, vec(0, 1, 0)),
                               in("cap", T::Bool, true, "Close the cut on meshes.")},
                    .outputs = {geoOut(), fout("cut", T::Bool, "Mesh edges on the cut."), fout("capFaces", T::Bool, "The cap faces.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                D3 origin = c.v("origin"), normal = c.v("normal");
                if (glm::length(normal) < 1e-12)
                    throw ExecError(c.node.name + ": the plane normal is zero");
                normal = glm::normalize(normal);
                std::string k = c.prop("keep");
                int keep = k == "below" ? -1 : k == "both" ? 0 : 1;
                bool cap = c.b("cap");
                std::string capName = c.anonymous("capFaces"), cutName = c.anonymous("cut");
                GeometryPtr g = c.geometry("geometry");
                Geometry out = *g;
                if (g->mesh)
                {
                    geo::Mesh m = clipHalf(*g->mesh, origin, normal, keep == 0 ? 1 : keep, cap, capName, cutName);
                    if (keep == 0)
                        m.append(clipHalf(*g->mesh, origin, normal, -1, cap, capName, cutName));
                    m.compact();
                    out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
                }
                if (g->curves)
                    out.curves = std::make_shared<const CurveSet>(clipCurves(*g->curves, origin, normal, keep));
                if (g->points && keep != 0)
                {
                    geo::Mesh p = *g->points;
                    std::vector<Index> drop;
                    for (Index i = 0; i < p.pointCapacity(); ++i)
                        if (p.pointValid(i) && glm::dot(p.position(i) - origin, normal) * keep < 0.0)
                            drop.push_back(i);
                    geo::deletePoints(p, drop);
                    p.compact();
                    out.points = std::make_shared<const geo::Mesh>(std::move(p));
                }
                c.out("geometry", makeGeometry(std::move(out)));
                c.out("cut", attributeField(cutName, T::Bool));
                c.out("capFaces", attributeField(capName, T::Bool));
            },
    });
}

double loopPerimeter(const geo::Mesh& m, const geo::EdgePath& loop)
{
    double p = 0.0;
    for (Index e : loop.edges)
        p += m.edgeLength(e);
    return p;
}

void fillHolesNode()
{
    registerDecl({
        .type = "FillHoles",
        .label = "Fill Holes",
        .category = "Mesh",
        .doc = "Caps open border loops. Polygon makes one face per hole, triangles cuts it into triangles, fan adds a centre point, grid fills with "
               "quads. Border uvs copies the neighbouring uvs onto the cap; planar flattens it at the surrounding texel density.",
        .props = {prop("method", {"polygon", "triangles", "fan", "grid"}, "polygon"), prop("uvs", {"border", "planar"}, "planar")},
        .sockets = {.inputs = {geoIn("mesh"), fin("selection", T::Bool, true, Implicit::None, "Holes with any selected border edge are filled."),
                               bounded(in("maxSides", T::Int, 0, "Holes with more border edges stay open; 0 fills any."), 0),
                               bounded(in("maxPerimeter", T::Float, 0.0, "Holes longer round than this stay open; 0 fills any."), 0.0)},
                    .outputs = {geoOut("mesh"), fout("fill", T::Bool, "The new faces."), out("holesFilled", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                std::string method = c.prop("method"), name = c.anonymous("fill");
                bool planar = c.prop("uvs") == "planar";
                int64_t maxSides = std::max<int64_t>(0, c.i("maxSides"));
                double maxPerimeter = std::max(0.0, c.f("maxPerimeter"));
                int64_t filled = 0;
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<uint8_t> sel = boolsOn(c.field("selection"), m);
                                                                      std::vector<std::vector<Index>> holes;
                                                                      for (const geo::EdgePath& loop : geo::boundaryLoops(m))
                                                                      {
                                                                          if (!loop.closed || loop.points.size() < 3)
                                                                              continue;
                                                                          if (maxSides > 0 && int64_t(loop.edges.size()) > maxSides)
                                                                              continue;
                                                                          if (maxPerimeter > 0.0 && loopPerimeter(m, loop) > maxPerimeter)
                                                                              continue;
                                                                          if (std::none_of(loop.edges.begin(), loop.edges.end(), [&](Index e) { return sel[e] != 0; }))
                                                                              continue;
                                                                          holes.push_back(loop.edges);
                                                                      }
                                                                      std::vector<Index> faces;
                                                                      for (const std::vector<Index>& edges : holes)
                                                                      {
                                                                          std::vector<Index> made;
                                                                          if (method == "grid")
                                                                          {
                                                                              made = geo::gridFillHoles(m, edges);
                                                                              if (planar && !made.empty())
                                                                              {
                                                                                  D3 n(0.0);
                                                                                  for (Index f : made)
                                                                                      n += m.faceAreaVector(f);
                                                                                  planarUvs(m, made, glm::length(n) > 1e-12 ? glm::normalize(n) : D3(0, 1, 0));
                                                                              }
                                                                          }
                                                                          else
                                                                          {
                                                                              made = geo::fillHoles(m, edges);
                                                                              if (planar)
                                                                                  for (Index f : made)
                                                                                      planarUvs(m, std::span<const Index>(&f, 1), m.faceNormal(f));
                                                                              if (method == "triangles")
                                                                                  made = geo::triangulateFaces(m, made);
                                                                              else if (method == "fan")
                                                                                  made = geo::pokeFaces(m, made);
                                                                          }
                                                                          if (!made.empty())
                                                                              ++filled;
                                                                          faces.insert(faces.end(), made.begin(), made.end());
                                                                      }
                                                                      markFaceFlag(m, name, faces);
                                                                  }}));
                c.out("fill", attributeField(name, T::Bool));
                c.out("holesFilled", constantInt(filled));
            },
    });
}

void cleanNode()
{
    using retopo::MeshIssue;
    registerDecl({
        .type = "CleanMesh",
        .label = "Clean Mesh",
        .category = "Mesh",
        .doc = "Finds and fixes common mesh faults: coincident points are welded, degenerate edges and faces collapsed, duplicate faces dropped, "
               "non-manifold edges and points split apart, winding made consistent, unused points and edges removed. Report only leaves the "
               "mesh as it is. The selections mark what was found, on elements that survive the fix.",
        .sockets = {.inputs = {geoIn("mesh"), in("reportOnly", T::Bool, false), in("mergePoints", T::Bool, true), in("degenerate", T::Bool, true),
                               in("duplicates", T::Bool, true), in("nonManifold", T::Bool, true), in("winding", T::Bool, true),
                               in("removeLoose", T::Bool, true),
                               in("tolerance", T::Float, -1.0, "Distance under which points coincide and faces count as degenerate; negative scales with the mesh.")},
                    .outputs = {geoOut("mesh"), fout("coincident", T::Bool, "Points."), fout("degenerateFaces", T::Bool, "Zero-area faces and faces on zero-length edges."),
                                fout("duplicateFaces", T::Bool), fout("nonManifoldEdges", T::Bool), fout("loosePoints", T::Bool, "Points no face uses."),
                                out("issueCount", T::Int), out("report", T::String)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                retopo::RepairOptions o;
                o.checks.fill(false);
                o.set(MeshIssue::Coincident, c.b("mergePoints"));
                o.set(MeshIssue::ZeroLengthEdge, c.b("degenerate"));
                o.set(MeshIssue::ZeroAreaFace, c.b("degenerate"));
                o.set(MeshIssue::Lamina, c.b("duplicates"));
                o.set(MeshIssue::NonManifoldEdge, c.b("nonManifold"));
                o.set(MeshIssue::NonManifoldPoint, c.b("nonManifold"));
                o.set(MeshIssue::Flipped, c.b("winding"));
                o.removeLoose = c.b("removeLoose");
                o.tolerance = c.f("tolerance");
                const bool reportOnly = c.b("reportOnly");
                const char* outs[] = {"coincident", "degenerateFaces", "duplicateFaces", "nonManifoldEdges", "loosePoints"};
                int64_t total = 0;
                std::string report = "no issues";
                GeometryPtr g = c.geometry("mesh");
                if (g->mesh)
                {
                    geo::Mesh m = *g->mesh;
                    retopo::MeshIssues found = retopo::findIssues(m, o);
                    auto flag = [&](const char* id, geo::Domain d, std::span<const Index> ids) {
                        auto a = m.attributes(d).getOrAdd<bool>(c.anonymous(id));
                        for (Index i : ids)
                            if (i < a.size())
                                a[i] = true;
                    };
                    flag("coincident", geo::Domain::Point, found.of(MeshIssue::Coincident));
                    std::vector<Index> degenerate = found.of(MeshIssue::ZeroAreaFace);
                    for (Index f : geo::facesOfEdges(m, found.of(MeshIssue::ZeroLengthEdge)))
                        degenerate.push_back(f);
                    flag("degenerateFaces", geo::Domain::Face, degenerate);
                    flag("duplicateFaces", geo::Domain::Face, found.of(MeshIssue::Lamina));
                    flag("nonManifoldEdges", geo::Domain::Edge, found.of(MeshIssue::NonManifoldEdge));
                    std::vector<Index> loose;
                    for (Index p = 0; p < m.pointCapacity(); ++p)
                    {
                        if (!m.pointValid(p))
                            continue;
                        bool used = false;
                        m.forEachPointCorner(p, [&](Index) { used = true; });
                        if (!used)
                            loose.push_back(p);
                    }
                    flag("loosePoints", geo::Domain::Point, loose);
                    total = int64_t(found.total());
                    if (o.removeLoose)
                        total += int64_t(loose.size());
                    report = found.summary();
                    if (o.removeLoose && !loose.empty())
                        report = (found.total() ? report + ", " : std::string()) + std::to_string(loose.size()) + " loose points";
                    if (!reportOnly && total > 0)
                    {
                        retopo::repairMesh(m, o);
                        if (o.removeLoose)
                            m.removeAllLoose();
                    }
                    m.compact();
                    Geometry keep = *g;
                    keep.mesh = std::make_shared<const geo::Mesh>(std::move(m));
                    g = makeGeometry(std::move(keep));
                }
                c.out("mesh", g);
                for (const char* id : outs)
                    c.out(id, attributeField(c.anonymous(id), T::Bool));
                c.out("issueCount", constantInt(total));
                c.outString("report", report);
            },
    });
}

double edgeBend(const geo::Mesh& m, Index e)
{
    Index k = m.edgeCorner(e);
    return std::acos(std::clamp(glm::dot(m.faceNormal(m.cornerFace(k)), m.faceNormal(m.cornerFace(m.cornerRadial(k)))), -1.0, 1.0));
}

/** True where the uvs of a layer differ on the two sides of a manifold edge. */
bool uvSplitAt(const geo::Mesh& m, std::span<const glm::vec2> uv, Index e)
{
    Index k1 = m.edgeCorner(e), k2 = m.cornerRadial(k1);
    auto same = [](glm::vec2 a, glm::vec2 b) { return glm::length(a - b) <= 1e-6f; };
    if (m.cornerPoint(k1) == m.cornerPoint(k2))
        return !same(uv[k1], uv[k2]) || !same(uv[m.cornerNext(k1)], uv[m.cornerNext(k2)]);
    return !same(uv[k1], uv[m.cornerNext(k2)]) || !same(uv[m.cornerNext(k1)], uv[k2]);
}

void dissolveNode()
{
    registerDecl({
        .type = "DissolveFlatEdges",
        .label = "Dissolve Flat Edges",
        .category = "Mesh",
        .doc = "Merges faces across edges that bend less than the angle, and removes points left in the middle of straight runs.",
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), bounded(in("angle", T::Float, 5.0, "Degrees of bend under which an edge goes."), 0.0, 180.0),
                               in("keepBorders", T::Bool, true, "Edges touching an open border stay."),
                               in("keepSeams", T::Bool, true, "Uv seams stay: marked seams and edges where the uvs split."),
                               in("keepHardEdges", T::Bool, true), in("keepMaterials", T::Bool, true, "Edges between materials stay.")},
                    .outputs = {geoOut("mesh"), out("dissolved", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                double limit = glm::radians(std::clamp(c.f("angle"), 0.0, 180.0));
                bool borders = c.b("keepBorders"), seams = c.b("keepSeams"), hard = c.b("keepHardEdges"), materials = c.b("keepMaterials");
                int64_t count = 0;
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<uint8_t> sel = boolsOn(c.field("selection"), m);
                                                                      auto seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
                                                                      auto hardE = m.edgeAttrs().get<bool>(geo::attr::kHard);
                                                                      auto mat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
                                                                      auto uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
                                                                      std::vector<Index> edges;
                                                                      for (Index e = 0; e < m.edgeCapacity(); ++e)
                                                                      {
                                                                          if (!m.edgeValid(e) || !sel[e] || m.edgeFaceCount(e) != 2 || edgeBend(m, e) >= limit)
                                                                              continue;
                                                                          if (borders && (m.isBoundaryPoint(m.edgePoint(e, 0)) || m.isBoundaryPoint(m.edgePoint(e, 1))))
                                                                              continue;
                                                                          if (seams && ((!seam.empty() && seam[e]) || (!uv.empty() && uvSplitAt(m, uv, e))))
                                                                              continue;
                                                                          if (hard && !hardE.empty() && hardE[e])
                                                                              continue;
                                                                          Index k = m.edgeCorner(e);
                                                                          if (materials && !mat.empty() && mat[m.cornerFace(k)] != mat[m.cornerFace(m.cornerRadial(k))])
                                                                              continue;
                                                                          edges.push_back(e);
                                                                      }
                                                                      Index before = m.numEdges();
                                                                      // Interior points whose every edge goes are merged first: joining edge by edge would wrap a face round them.
                                                                      std::vector<uint8_t> going = geo::makeMask(m.edgeCapacity(), edges);
                                                                      std::vector<Index> points;
                                                                      for (Index p = 0; p < m.pointCapacity(); ++p)
                                                                      {
                                                                          if (!m.pointValid(p) || m.isBoundaryPoint(p) || m.pointEdge(p) == geo::kInvalid)
                                                                              continue;
                                                                          bool all = true;
                                                                          m.forEachPointEdge(p, [&](Index e) { all &= going[e] != 0; });
                                                                          if (all)
                                                                              points.push_back(p);
                                                                      }
                                                                      geo::dissolvePoints(m, points);
                                                                      std::erase_if(edges, [&](Index e) { return !m.edgeValid(e); });
                                                                      geo::dissolveEdges(m, edges, true);
                                                                      count = int64_t(before) - int64_t(m.numEdges());
                                                                  }}));
                c.out("dissolved", constantInt(count));
            },
    });
}

/** Splits face f between each consecutive pair of the listed points, picking the piece that holds both. Marks the new edges. */
void splitFaceAt(geo::Mesh& m, Index f, const std::vector<Index>& points, const std::string& mark)
{
    std::vector<Index> pieces = {f};
    for (size_t i = 0; i + 1 < points.size(); i += 2)
        for (Index piece : pieces)
        {
            Index a = m.findCorner(piece, points[i]), b = m.findCorner(piece, points[i + 1]);
            if (a == geo::kInvalid || b == geo::kInvalid || m.cornerNext(a) == b || m.cornerNext(b) == a)
                continue;
            Index nf = m.splitFace(piece, a, b);
            if (nf == geo::kInvalid)
                break;
            m.edgeAttrs().get<bool>(mark)[m.lastSplitEdge()] = true;
            pieces.push_back(nf);
            break;
        }
}

void splitByAttributeNode()
{
    registerDecl({
        .type = "SplitByPointAttribute",
        .label = "Split by Point Attribute",
        .category = "Mesh",
        .doc = "Cuts faces along a line through a point value. Threshold inserts points where the value crosses it, attributes interpolated; change "
               "inserts a point in the middle of every edge whose ends differ, so faces split between regions of different ids.",
        .props = {prop("mode", {"threshold", "change"}, "threshold")},
        .sockets = {.inputs = {geoIn("mesh"), fin("value", T::Float, 0.0, Implicit::None, "Read per point."), in("threshold", T::Float, 0.5)},
                    .outputs = {geoOut("mesh"), fout("split", T::Bool, "The new edges along the line.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                bool change = c.prop("mode") == "change";
                double th = c.f("threshold");
                std::string name = c.anonymous("split");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<double> v = evaluateAll<double>(c.field("value"), FieldContext::forMesh(m, Domain::Point));
                                                                      auto side = [&](Index p) { return v[p] > th ? 1 : v[p] < th ? -1 : 0; };
                                                                      std::vector<uint8_t> onLine(m.pointCapacity(), 0);
                                                                      if (!change)
                                                                          for (Index p = 0; p < m.pointCapacity(); ++p)
                                                                              onLine[p] = m.pointValid(p) && side(p) == 0 ? 1 : 0;
                                                                      std::vector<std::pair<Index, double>> cuts;
                                                                      for (Index e = 0; e < m.edgeCapacity(); ++e)
                                                                      {
                                                                          if (!m.edgeValid(e))
                                                                              continue;
                                                                          Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
                                                                          if (change ? std::abs(v[a] - v[b]) > 1e-9 : side(a) * side(b) < 0)
                                                                              cuts.push_back({e, change ? 0.5 : (th - v[a]) / (v[b] - v[a])});
                                                                      }
                                                                      // Region labels per point: originals keep theirs, new points sit on the line.
                                                                      std::vector<double> label = v;
                                                                      for (auto [e, t] : cuts)
                                                                      {
                                                                          Index p = m.splitEdge(e, t);
                                                                          onLine.resize(m.pointCapacity(), 0);
                                                                          label.resize(m.pointCapacity(), th);
                                                                          onLine[p] = 1;
                                                                      }
                                                                      m.edgeAttrs().getOrAdd<bool>(name);
                                                                      const Index faces = m.faceCapacity();
                                                                      for (Index f = 0; f < faces; ++f)
                                                                      {
                                                                          if (!m.faceValid(f))
                                                                              continue;
                                                                          std::vector<Index> corners = m.faceCorners(f);
                                                                          std::vector<size_t> line;
                                                                          bool above = false, below = false;
                                                                          double firstLabel = 0.0;
                                                                          bool mixed = false, haveLabel = false;
                                                                          for (size_t i = 0; i < corners.size(); ++i)
                                                                          {
                                                                              Index p = m.cornerPoint(corners[i]);
                                                                              if (onLine[p])
                                                                              {
                                                                                  line.push_back(i);
                                                                                  continue;
                                                                              }
                                                                              above |= label[p] > th;
                                                                              below |= label[p] < th;
                                                                              if (haveLabel && std::abs(label[p] - firstLabel) > 1e-9)
                                                                                  mixed = true;
                                                                              firstLabel = haveLabel ? firstLabel : label[p];
                                                                              haveLabel = true;
                                                                          }
                                                                          if (line.size() < 2 || !(change ? mixed : above && below))
                                                                              continue;
                                                                          // Pair the line corners in winding order, starting so that each pair spans corners off the line.
                                                                          const size_t n = corners.size(), L = line.size();
                                                                          auto spans = [&](size_t r) {
                                                                              for (size_t k = 0; k + 1 < L; k += 2)
                                                                              {
                                                                                  size_t a = line[(r + k) % L], b = line[(r + k + 1) % L];
                                                                                  size_t gap = (b + n - a) % n;
                                                                                  if (gap < 2 || n - gap < 2)
                                                                                      return false;
                                                                              }
                                                                              return true;
                                                                          };
                                                                          size_t r = spans(0) ? 0 : spans(1) ? 1 : L;
                                                                          if (r == L)
                                                                              continue;
                                                                          std::vector<Index> pts;
                                                                          for (size_t k = 0; k + 1 < L; k += 2)
                                                                          {
                                                                              pts.push_back(m.cornerPoint(corners[line[(r + k) % L]]));
                                                                              pts.push_back(m.cornerPoint(corners[line[(r + k + 1) % L]]));
                                                                          }
                                                                          splitFaceAt(m, f, pts, name);
                                                                      }
                                                                  }}));
                c.out("split", attributeField(name, T::Bool));
            },
    });
}

void cornersOfEdgeNode()
{
    registerDecl({
        .type = "CornersOfEdge",
        .label = "Corners of Edge",
        .category = "Mesh Topology",
        .doc = "One of the corners that start an edge, one per face using it, chosen by sorting them by weight. With equal weights they come in "
               "index order.",
        .sockets = {.inputs = {fin("edgeIndex", T::Int, nullptr, Implicit::Index, "The edge to look along; defaults to the index."),
                               fin("weights", T::Float, 0.0, Implicit::None, "Values the choices are sorted by, lowest first."),
                               fin("sortIndex", T::Int, 0, Implicit::None, "Which of the sorted choices to output; wraps around.")},
                    .outputs = {fout("cornerIndex", T::Int), fout("total", T::Int, "How many choices there are.")}},
        .exec =
            [](ExecContext& c) {
                FieldPtr index = c.field("edgeIndex"), weights = c.field("weights"), sortIndex = c.field("sortIndex");
                class EdgeCorners final : public Field
                {
                public:
                    EdgeCorners(FieldPtr i, FieldPtr w, FieldPtr s, bool total) : Field(T::Int), mI(std::move(i)), mW(std::move(w)), mS(std::move(s)), mTotal(total) {}
                    Column evaluate(const FieldContext& ctx) const override
                    {
                        if (ctx.component != Component::Mesh || !ctx.mesh)
                            return Column::of(int64_t(0));
                        const geo::Mesh& m = *ctx.mesh;
                        std::vector<int64_t> ei = evaluateAll<int64_t>(mI, ctx), si = evaluateAll<int64_t>(mS, ctx);
                        std::vector<double> w = evaluateAll<double>(mW, FieldContext::forMesh(m, Domain::Corner));
                        std::vector<int64_t> out(ctx.size, 0);
                        std::vector<Index> ids;
                        for (size_t k = 0; k < ctx.size; ++k)
                        {
                            if (ei[k] < 0 || ei[k] >= int64_t(m.edgeCapacity()) || !m.edgeValid(Index(ei[k])) || m.edgeCorner(Index(ei[k])) == geo::kInvalid)
                                continue;
                            ids.clear();
                            m.forEachEdgeCorner(Index(ei[k]), [&](Index x) { ids.push_back(x); });
                            if (mTotal)
                            {
                                out[k] = int64_t(ids.size());
                                continue;
                            }
                            std::sort(ids.begin(), ids.end());
                            std::stable_sort(ids.begin(), ids.end(), [&](Index a, Index b) { return w[a] < w[b]; });
                            int64_t n = int64_t(ids.size());
                            out[k] = ids[size_t(((si[k] % n) + n) % n)];
                        }
                        return Column::of(std::move(out));
                    }

                private:
                    FieldPtr mI, mW, mS;
                    bool mTotal;
                };
                c.out("cornerIndex", std::make_shared<EdgeCorners>(index, weights, sortIndex, false));
                c.out("total", std::make_shared<EdgeCorners>(index, weights, sortIndex, true));
            },
    });
}

void autoSeamsNode()
{
    registerDecl({
        .type = "AutoUVSeams",
        .label = "Auto UV Seams",
        .category = "UV",
        .doc = "Marks uv seams on edges: sharp edges first, then each island is cut open along short, creased and hidden paths and split where it "
               "would stretch. Writes the seam edge attribute that unwrapping reads.",
        .props = {prop("method", {"split", "optimise"}, "split", "Split cuts and merges islands; optimise trades seam length against stretch over the whole mesh.")},
        .sockets = {.inputs = {geoIn("mesh"), fin("selection", T::Bool, true, Implicit::None, "Faces to cut."),
                               bounded(in("hardAngle", T::Float, 60.0, "Edges sharper than this many degrees are always seams."), 0.0, 180.0),
                               in("useHardEdges", T::Bool, false, "Edges marked hard are seams too."), in("keepSeams", T::Bool, false, "Existing seams stay."),
                               bounded(in("maxDistortion", T::Float, 0.08, "Islands stretching more than this are split."), 0.0),
                               in("hideSeams", T::Bool, true, "Prefer occluded, concave and downward-facing edges."),
                               in("merge", T::Bool, true, "Join neighbouring islands back while they stay under the distortion.")},
                    .outputs = {geoOut("mesh"), fout("seam", T::Bool), out("islands", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                uv::AutoSeamOptions o;
                o.method = c.prop("method") == "optimise" ? uv::SeamMethod::OptCuts : uv::SeamMethod::Split;
                o.hardAngle = std::clamp(c.f("hardAngle"), 0.0, 180.0);
                o.useHardEdges = c.b("useHardEdges");
                o.keepSeams = c.b("keepSeams");
                o.maxDistortion = std::max(0.0, c.f("maxDistortion"));
                o.visibility = c.b("hideSeams");
                o.merge = c.b("merge");
                int64_t islands = 0;
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<Index> faces = selected(c.field("selection"), FieldContext::forMesh(m, Domain::Face));
                                                                      if (faces.empty())
                                                                          return;
                                                                      if (faces.size() == m.numFaces())
                                                                          faces.clear();
                                                                      islands = int64_t(uv::autoSeams(m, faces, o).charts.size());
                                                                  }}));
                c.out("seam", attributeField(std::string(geo::attr::kSeam), T::Bool));
                c.out("islands", constantInt(islands));
            },
    });
}

void edgeRelaxNode()
{
    registerDecl({
        .type = "EdgeRelax",
        .label = "Edge Relax",
        .category = "Mesh",
        .doc = "Moves the selected points so each edge takes the length it has in the reference, a copy of the mesh with the same points and edges "
               "such as its rest shape. Without a reference, edges even out towards the mean length.",
        .sockets = {.inputs = {geoIn("mesh"), fin("selection", T::Bool, true, Implicit::None, "Points free to move."), geoIn("reference"),
                               bounded(in("iterations", T::Int, 20), 0, 10000), bounded(in("strength", T::Float, 0.5), 0.0, 1.0),
                               in("keepOnSurface", T::Bool, false, "Pull moved points back onto the input surface.")},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr refG = c.geometry("reference");
                std::shared_ptr<const geo::Mesh> ref = refG->mesh;
                int iterations = int(std::clamp<int64_t>(c.i("iterations"), 0, 10000));
                double strength = std::clamp(c.f("strength"), 0.0, 1.0);
                bool surface = c.b("keepOnSurface");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<double> rest(m.edgeCapacity(), 0.0);
                                                                      if (ref)
                                                                      {
                                                                          if (ref->pointCapacity() != m.pointCapacity())
                                                                              throw ExecError(c.node.name + ": the reference has " + std::to_string(ref->numPoints()) +
                                                                                              " points and the mesh " + std::to_string(m.numPoints()) + "; they must match");
                                                                          for (Index e = 0; e < m.edgeCapacity(); ++e)
                                                                              if (m.edgeValid(e))
                                                                              {
                                                                                  Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
                                                                                  rest[e] = glm::length(ref->position(a) - ref->position(b));
                                                                              }
                                                                      }
                                                                      else
                                                                      {
                                                                          double sum = 0.0;
                                                                          for (Index e = 0; e < m.edgeCapacity(); ++e)
                                                                              if (m.edgeValid(e))
                                                                                  sum += m.edgeLength(e);
                                                                          std::fill(rest.begin(), rest.end(), m.numEdges() ? sum / m.numEdges() : 0.0);
                                                                      }
                                                                      std::vector<uint8_t> free = boolsOn(c.field("selection"), m, Domain::Point);
                                                                      std::unique_ptr<geo::MeshBvh> bvh;
                                                                      if (surface && m.numFaces())
                                                                          bvh = std::make_unique<geo::MeshBvh>(geo::Mesh(m));
                                                                      std::vector<D3> move(m.pointCapacity());
                                                                      std::vector<double> weight(m.pointCapacity());
                                                                      for (int it = 0; it < iterations; ++it)
                                                                      {
                                                                          std::fill(move.begin(), move.end(), D3(0.0));
                                                                          std::fill(weight.begin(), weight.end(), 0.0);
                                                                          for (Index e = 0; e < m.edgeCapacity(); ++e)
                                                                          {
                                                                              if (!m.edgeValid(e))
                                                                                  continue;
                                                                              Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
                                                                              bool fa = free[a] != 0, fb = free[b] != 0;
                                                                              if (!fa && !fb)
                                                                                  continue;
                                                                              D3 d = m.position(b) - m.position(a);
                                                                              double len = glm::length(d);
                                                                              if (len < 1e-12)
                                                                                  continue;
                                                                              D3 corr = d * ((len - rest[e]) / len);
                                                                              double share = fa && fb ? 0.5 : 1.0;
                                                                              if (fa)
                                                                              {
                                                                                  move[a] += corr * share;
                                                                                  weight[a] += 1.0;
                                                                              }
                                                                              if (fb)
                                                                              {
                                                                                  move[b] -= corr * share;
                                                                                  weight[b] += 1.0;
                                                                              }
                                                                          }
                                                                          for (Index p = 0; p < m.pointCapacity(); ++p)
                                                                          {
                                                                              if (weight[p] <= 0.0)
                                                                                  continue;
                                                                              D3 q = m.position(p) + move[p] * (strength / weight[p]);
                                                                              geo::BvhClosest hit;
                                                                              if (bvh && bvh->closestPoint(q, hit))
                                                                                  q = hit.point;
                                                                              m.setPosition(p, q);
                                                                          }
                                                                      }
                                                                      m.cornerAttrs().remove(geo::attr::kNormal);
                                                                  }}));
            },
    });
}
} // namespace topo2

void registerTopology2Nodes()
{
    topo2::clipNode();
    topo2::fillHolesNode();
    topo2::cleanNode();
    topo2::dissolveNode();
    topo2::splitByAttributeNode();
    topo2::cornersOfEdgeNode();
    topo2::autoSeamsNode();
    topo2::edgeRelaxNode();
}
} // namespace lila::gn::nodes
