#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Triangulate.h"

#include <map>
#include <unordered_map>

namespace lila::gn::nodes
{
namespace
{
int clampInt(int64_t v, int lo, int hi) { return int(std::clamp<int64_t>(v, lo, hi)); }

geo::Mesh icoSphere(double radius, int subdiv)
{
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    std::vector<D3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                         {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<std::array<Index, 3>> f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                           {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                           {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int level = 1; level < subdiv; ++level)
    {
        std::map<std::pair<Index, Index>, Index> mid;
        auto midpoint = [&](Index a, Index b) {
            auto key = std::minmax(a, b);
            auto it = mid.find(key);
            if (it != mid.end())
                return it->second;
            v.push_back((v[a] + v[b]) * 0.5);
            return mid[key] = Index(v.size() - 1);
        };
        std::vector<std::array<Index, 3>> next;
        for (auto& tri : f)
        {
            Index a = midpoint(tri[0], tri[1]), b = midpoint(tri[1], tri[2]), c = midpoint(tri[2], tri[0]);
            next.push_back({tri[0], a, c});
            next.push_back({tri[1], b, a});
            next.push_back({tri[2], c, b});
            next.push_back({a, b, c});
        }
        f.swap(next);
    }
    for (D3& p : v)
        p = glm::normalize(p) * radius;
    std::vector<Index> sizes(f.size(), 3), idx;
    for (auto& tri : f)
        idx.insert(idx.end(), tri.begin(), tri.end());
    return geo::Mesh::fromPolygons(v, sizes, idx);
}

geo::Mesh circleMesh(int vertices, double radius, const std::string& fill)
{
    geo::Mesh m;
    std::vector<Index> pts;
    for (int k = 0; k < vertices; ++k)
    {
        double a = 2.0 * kPi * k / vertices;
        // Counter-clockwise seen from +Y, so a filled circle faces up.
        pts.push_back(m.addPoint({std::cos(a) * radius, 0.0, -std::sin(a) * radius}));
    }
    if (fill == "ngon")
        m.addFace(pts);
    else if (fill == "triangleFan")
    {
        Index center = m.addPoint(D3(0.0));
        for (int k = 0; k < vertices; ++k)
            m.addFace({center, pts[k], pts[(k + 1) % vertices]});
    }
    else
        for (int k = 0; k < vertices; ++k)
            m.addEdge(pts[k], pts[(k + 1) % vertices]);
    return m;
}

void primitive(const char* type, const char* label, std::vector<SocketDecl> inputs, std::vector<PropDecl> props, std::function<geo::Mesh(ExecContext&)> build,
               const char* doc)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Mesh Primitives",
        .doc = doc,
        .props = std::move(props),
        .sockets = {.inputs = std::move(inputs), .outputs = {geoOut("mesh")}},
        .exec = [build](ExecContext& c) { c.out("mesh", meshGeometry(build(c))); },
    });
}

SocketDecl heightFraction() { return bounded(in("fraction", T::Float, 1.0, "Portion of the height kept, from the top: 1 is whole, 0.5 a dome open at the bottom."), 0.01, 1.0); }
SocketDecl sweepFraction() { return bounded(in("fraction", T::Float, 1.0, "Portion of a full turn swept around the up axis: 1 is whole, 0.5 half."), 0.01, 1.0); }
SocketDecl cutCaps() { return in("cutCaps", T::Bool, true, "Close the flat ends a partial sweep leaves."); }
double fraction(ExecContext& c) { return std::clamp(c.f("fraction"), 0.01, 1.0); }

void meshPrimitives()
{
    primitive(
        "MeshCube", "Cube",
        {in("size", T::Vector, vec(100, 100, 100)), bounded(in("verticesX", T::Int, 2), 2, 1001), bounded(in("verticesY", T::Int, 2), 2, 1001), bounded(in("verticesZ", T::Int, 2), 2, 1001)}, {},
        [](ExecContext& c) {
            D3 s = c.v("size");
            return geo::makeCube({s.x, s.y, s.z, clampInt(c.i("verticesX") - 1, 1, 1000), clampInt(c.i("verticesY") - 1, 1, 1000),
                                  clampInt(c.i("verticesZ") - 1, 1, 1000)});
        },
        "A box centred on the origin, with uvs.");
    primitive(
        "MeshGrid", "Grid", {bounded(in("sizeX", T::Float, 100.0), 0.0), bounded(in("sizeY", T::Float, 100.0), 0.0), bounded(in("verticesX", T::Int, 3), 2, 10001),
         bounded(in("verticesY", T::Int, 3), 2, 10001)}, {},
        [](ExecContext& c) {
            return geo::makePlane({c.f("sizeX"), c.f("sizeY"), clampInt(c.i("verticesX") - 1, 1, 10000), clampInt(c.i("verticesY") - 1, 1, 10000)});
        },
        "A flat grid in the ground plane, facing up. sizeY runs along depth.");
    primitive(
        "MeshUVSphere", "UV Sphere", {bounded(in("segments", T::Int, 32), 3, 1000), bounded(in("rings", T::Int, 16), 2, 1000), bounded(in("radius", T::Float, 50.0), 0.0), heightFraction()}, {},
        [](ExecContext& c) {
            return geo::makeSphere({c.f("radius"), clampInt(c.i("segments"), 3, 1000), clampInt(c.i("rings"), 2, 1000), false, fraction(c)});
        },
        "Latitude-longitude sphere.");
    primitive(
        "MeshIcoSphere", "Ico Sphere", {bounded(in("radius", T::Float, 50.0), 0.0), bounded(in("subdivisions", T::Int, 2), 1, 7), heightFraction()}, {},
        [](ExecContext& c) {
            const int subdiv = clampInt(c.i("subdivisions"), 1, 7);
            if (fraction(c) < 1.0)
                return geo::makeIcoSphere({c.f("radius"), subdiv, false, fraction(c)});
            return icoSphere(c.f("radius"), subdiv);
        },
        "Evenly triangulated sphere.");
    primitive(
        "MeshQuadSphere", "Quad Sphere", {bounded(in("radius", T::Float, 50.0), 0.0), bounded(in("subdivisions", T::Int, 3), 1, 200), heightFraction()}, {},
        [](ExecContext& c) { return geo::makeQuadSphere({c.f("radius"), clampInt(c.i("subdivisions"), 1, 200), fraction(c)}); },
        "All-quad sphere, no poles.");
    primitive(
        "MeshCylinder", "Cylinder",
        {bounded(in("vertices", T::Int, 32), 3, 1000), bounded(in("sideSegments", T::Int, 1), 1, 1000), bounded(in("fillSegments", T::Int, 0), 0, 1000),
         bounded(in("radius", T::Float, 50.0), 0.0), bounded(in("depth", T::Float, 100.0), 0.0),
         sweepFraction(), cutCaps()},
        {prop("fillType", {"ngon", "none"}, "ngon")},
        [](ExecContext& c) {
            return geo::makeCylinder({c.f("radius"), c.f("depth"), clampInt(c.i("vertices"), 3, 1000), clampInt(c.i("sideSegments"), 1, 1000),
                                      clampInt(c.i("fillSegments"), 0, 1000), c.prop("fillType") != "none", false, fraction(c), c.b("cutCaps")});
        },
        "Upright cylinder centred on the origin.");
    primitive(
        "MeshCone", "Cone",
        {bounded(in("vertices", T::Int, 32), 3, 1000), bounded(in("sideSegments", T::Int, 1), 1, 1000), bounded(in("fillSegments", T::Int, 0), 0, 1000),
         bounded(in("radius", T::Float, 50.0), 0.0), bounded(in("depth", T::Float, 100.0), 0.0),
         sweepFraction(), cutCaps()},
        {},
        [](ExecContext& c) {
            return geo::makeCone({c.f("radius"), c.f("depth"), clampInt(c.i("vertices"), 3, 1000), clampInt(c.i("sideSegments"), 1, 1000),
                                  clampInt(c.i("fillSegments"), 0, 1000), false, fraction(c), c.b("cutCaps")});
        },
        "Upright cone centred on the origin.");
    primitive(
        "MeshTorus", "Torus",
        {bounded(in("radius", T::Float, 50.0), 0.0), bounded(in("sectionRadius", T::Float, 20.0), 0.0), bounded(in("segments", T::Int, 32), 3, 1000),
         bounded(in("sectionSegments", T::Int, 16), 3, 1000),
         sweepFraction(), cutCaps()},
        {},
        [](ExecContext& c) {
            return geo::makeTorus({c.f("radius"), c.f("sectionRadius"), clampInt(c.i("segments"), 3, 1000), clampInt(c.i("sectionSegments"), 3, 1000),
                                   fraction(c), c.b("cutCaps")});
        },
        "Ring in the ground plane.");
    primitive(
        "MeshCircle", "Mesh Circle", {bounded(in("vertices", T::Int, 32), 3, 100000), bounded(in("radius", T::Float, 50.0), 0.0)}, {prop("fillType", {"none", "ngon", "triangleFan"}, "none")},
        [](ExecContext& c) { return circleMesh(clampInt(c.i("vertices"), 3, 100000), c.f("radius"), c.prop("fillType")); },
        "A ring of points in the ground plane, as edges or filled.");
    primitive(
        "MeshLine", "Mesh Line",
        {bounded(in("count", T::Int, 10), 1, 1000000), in("start", T::Vector, vec(0, 0, 0)), in("offset", T::Vector, vec(0, 10, 0), "Step between points (offset mode) or end point.")},
        {prop("mode", {"offset", "endPoints"}, "offset")},
        [](ExecContext& c) {
            int n = clampInt(c.i("count"), 1, 1000000);
            D3 start = c.v("start"), off = c.v("offset");
            D3 step = c.prop("mode") == "endPoints" ? (n > 1 ? (off - start) / double(n - 1) : D3(0.0)) : off;
            geo::Mesh m;
            for (int k = 0; k < n; ++k)
                m.addPoint(start + step * double(k));
            for (int k = 0; k + 1 < n; ++k)
                m.addEdge(Index(k), Index(k + 1));
            return m;
        },
        "Points joined by edges in a straight line.");
}

/** Wraps a mesh-only operation as a node: geometry and selection in, geometry out. */
void meshOp(const char* type, const char* label, Domain selDomain, std::vector<SocketDecl> extra, std::vector<PropDecl> props,
            std::function<void(geo::Mesh&, std::vector<Index>&, ExecContext&)> fn, const char* doc)
{
    std::vector<SocketDecl> inputs = {geoIn("mesh"), selectionIn()};
    inputs.insert(inputs.end(), extra.begin(), extra.end());
    registerDecl({
        .type = type,
        .label = label,
        .category = "Mesh",
        .doc = doc,
        .props = std::move(props),
        .sockets = {.inputs = std::move(inputs), .outputs = {geoOut("mesh")}},
        .exec =
            [fn, selDomain](ExecContext& c) {
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<Index> sel = selected(c.field("selection"), FieldContext::forMesh(m, selDomain));
                                                                      fn(m, sel, c);
                                                                  }}));
            },
    });
}

/** Merge for point clouds and loose vertices: each point within distance of an earlier kept one is dropped. */
geo::Mesh weldLoosePoints(const geo::Mesh& m, const std::vector<Index>& sel, double dist)
{
    std::vector<uint8_t> selected(m.pointCapacity(), 0), keep(m.pointCapacity(), 1);
    for (Index p : sel)
        selected[p] = 1;
    double cell = std::max(dist, 1e-9);
    std::unordered_map<uint64_t, std::vector<Index>> grid;
    auto key = [](const glm::i64vec3& c) { return (uint64_t(c.x & 0x1fffff) << 42) | (uint64_t(c.y & 0x1fffff) << 21) | uint64_t(c.z & 0x1fffff); };
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!selected[p])
            continue;
        glm::i64vec3 c(glm::floor(m.position(p) / cell));
        bool merged = false;
        for (int dx = -1; dx <= 1 && !merged; ++dx)
            for (int dy = -1; dy <= 1 && !merged; ++dy)
                for (int dz = -1; dz <= 1 && !merged; ++dz)
                {
                    auto it = grid.find(key(c + glm::i64vec3(dx, dy, dz)));
                    if (it != grid.end())
                        for (Index q : it->second)
                            if (glm::distance(m.position(p), m.position(q)) <= dist)
                            {
                                merged = true;
                                break;
                            }
                }
        if (merged)
            keep[p] = 0;
        else
            grid[key(c)].push_back(p);
    }
    geo::Mesh out;
    out.pointAttrs().mergeLayout(m.pointAttrs());
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (keep[p])
            out.pointAttrs().copyFrom(m.pointAttrs(), out.addPoint(m.position(p)), p);
    return out;
}

void meshOps()
{
    registerDecl({
        .type = "SubdivisionSurface",
        .label = "Subdivision Surface",
        .category = "Mesh",
        .doc = "Catmull-Clark subdivision. Edge \"crease\" attributes keep edges sharp.",
        .props = {prop("uvs", {"fitSurface", "blend"}, "fitSurface", "Island interior uvs: spread evenly over the smoothed surface, or smoothed like the points."),
                  prop("uvCorners", {"keep", "smooth"}, "keep", "Island corner uvs: kept where the input has them, or smoothed with the border.")},
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("level", T::Int, 1), 0, 6)}, .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                int level = clampInt(c.i("level"), 0, 6);
                GeometryPtr g = c.geometry("mesh");
                if (level == 0 || !g->mesh)
                {
                    c.out("mesh", g);
                    return;
                }
                Geometry out = *g;
                geo::SubdivideOptions o;
                o.levels = level;
                o.uvs = c.prop("uvs") == "blend" ? geo::SubdivideUvs::Blend : geo::SubdivideUvs::FitSurface;
                o.uvCorners = c.prop("uvCorners") == "smooth" ? geo::SubdivideUvCorners::Smooth : geo::SubdivideUvCorners::Keep;
                geo::Mesh m = geo::subdivide(*g->mesh, o);
                m.compact();
                out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
                c.out("mesh", makeGeometry(std::move(out)));
            },
    });

    registerDecl({
        .type = "ExtrudeMesh",
        .label = "Extrude Mesh",
        .category = "Mesh",
        .doc = "Extrudes faces by an offset (a field, the face normal by default) times the scale.",
        .props = {prop("individual", {"true", "false"}, "true", "Each face separately, or connected regions together.")},
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), fin("offset", T::Vector, nullptr, Implicit::Normal), fin("offsetScale", T::Float, 10.0)},
                    .outputs = {geoOut("mesh"), fout("top", T::Bool), fout("side", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::string topName = c.anonymous("top"), sideName = c.anonymous("side");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      FieldContext ctx = FieldContext::forMesh(m, Domain::Face);
                                                                      std::vector<Index> sel = selected(c.field("selection"), ctx);
                                                                      std::vector<D3> off = evaluateAll<D3>(c.field("offset"), ctx);
                                                                      std::vector<double> scale = evaluateAll<double>(c.field("offsetScale"), ctx);
                                                                      geo::ExtrudeOptions o;
                                                                      o.individual = c.prop("individual") != "false";
                                                                      o.keepBack = false;
                                                                      geo::ExtrudeResult r = geo::extrudeFaces(m, sel, o);
                                                                      std::unordered_map<Index, std::pair<D3, int>> moves;
                                                                      for (Index f : r.faces)
                                                                      {
                                                                          D3 d = f < off.size() ? off[f] * scale[f] : D3(0.0);
                                                                          m.forEachFaceCorner(f, [&](Index cc) {
                                                                              auto& mv = moves[m.cornerPoint(cc)];
                                                                              mv.first += d;
                                                                              ++mv.second;
                                                                          });
                                                                      }
                                                                      for (auto& [p, mv] : moves)
                                                                          m.setPosition(p, m.position(p) + mv.first / double(mv.second));
                                                                      auto top = m.faceAttrs().getOrAdd<bool>(topName);
                                                                      for (Index f : r.faces)
                                                                          top[f] = true;
                                                                      auto side = m.faceAttrs().getOrAdd<bool>(sideName);
                                                                      for (Index f : r.sides)
                                                                          side[f] = true;
                                                                  }}));
                c.out("top", attributeField(topName, T::Bool));
                c.out("side", attributeField(sideName, T::Bool));
            },
    });

    meshOp("FlipFaces", "Flip Faces", Domain::Face, {}, {}, [](geo::Mesh& m, std::vector<Index>& sel, ExecContext&) { geo::flipNormals(m, sel); },
           "Reverses the winding of the selected faces.");
    meshOp(
        "Triangulate", "Triangulate", Domain::Face, {}, {prop("quadMethod", {"shortestDiagonal", "fixed"}, "shortestDiagonal")},
        [](geo::Mesh& m, std::vector<Index>& sel, ExecContext& c) {
            geo::TriangulateOptions o;
            o.quads = c.prop("quadMethod") == "fixed" ? geo::QuadSplit::FirstCorner : geo::QuadSplit::ShortestDiagonal;
            geo::triangulateFaces(m, sel, o);
        },
        "Splits the selected faces into triangles.");
    meshOp(
        "SetShadeSmooth", "Set Shade Smooth", Domain::Face, {fin("shadeSmooth", T::Bool, true)}, {prop("domain", {"face", "edge"}, "face")},
        [](geo::Mesh& m, std::vector<Index>&, ExecContext& c) {
            bool edges = c.prop("domain") == "edge";
            FieldContext ctx = FieldContext::forMesh(m, edges ? Domain::Edge : Domain::Face);
            std::vector<uint8_t> sel = expand<uint8_t>(evaluate(c.field("selection"), ctx, T::Bool), ctx.size);
            std::vector<uint8_t> smooth = expand<uint8_t>(evaluate(c.field("shadeSmooth"), ctx, T::Bool), ctx.size);
            m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
            auto hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
            if (edges)
            {
                for (Index e = 0; e < m.edgeCapacity(); ++e)
                    if (sel[e])
                        hard[e] = !smooth[e];
                m.cornerAttrs().remove(geo::attr::kNormal);
                return;
            }
            // A flat face hardens its edges; a smooth one softens them unless a flat neighbour keeps them hard.
            for (Index f = 0; f < m.faceCapacity(); ++f)
                if (sel[f] && smooth[f])
                    m.forEachFaceCorner(f, [&](Index cc) { hard[m.cornerEdge(cc)] = false; });
            for (Index f = 0; f < m.faceCapacity(); ++f)
                if (sel[f] && !smooth[f])
                    m.forEachFaceCorner(f, [&](Index cc) { hard[m.cornerEdge(cc)] = true; });
            m.cornerAttrs().remove(geo::attr::kNormal);
        },
        "Smooth or flat shading through hard edges: flat faces harden their edges.");

    registerDecl({
        .type = "MergeByDistance",
        .label = "Merge by Distance",
        .category = "Geometry",
        .doc = "Welds selected points closer than the distance.",
        .sockets = {.inputs = {geoIn(), selectionIn(), bounded(in("distance", T::Float, 0.1), 0.0)}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                double dist = std::max(0.0, c.f("distance"));
                auto merge = [&](geo::Mesh& m, const FieldContext& ctx) {
                    std::vector<Index> sel = selected(c.field("selection"), ctx);
                    if (sel.empty())
                        return;
                    if (m.numEdges() == 0)
                    {
                        m = weldLoosePoints(m, sel, dist);
                        return;
                    }
                    geo::MergeOptions o;
                    o.distance = dist;
                    geo::mergeByDistance(m, sel, o);
                    m.compact();
                };
                c.out("geometry", editComponents(c.geometry("geometry"), {.mesh = [&](geo::Mesh& m) { merge(m, FieldContext::forMesh(m, Domain::Point)); },
                                                                         .points = [&](geo::Mesh& m) { merge(m, FieldContext::forPoints(m)); }}));
            },
    });
}

void pointConversions()
{
    registerDecl({
        .type = "MeshToPoints",
        .label = "Mesh to Points",
        .category = "Point",
        .doc = "A point cloud from mesh vertices, edge or face centres, or corners. Vertices keep their point attributes.",
        .props = {prop("mode", {"vertices", "edges", "faces", "corners"}, "vertices")},
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), fin("position", T::Vector, nullptr, Implicit::Position), bounded(fin("radius", T::Float, 5.0), 0.0)},
                    .outputs = {geoOut("points")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("mesh");
                if (!g->mesh)
                {
                    c.out("points", makeGeometry({}));
                    return;
                }
                const geo::Mesh& m = *g->mesh;
                std::string mode = c.prop("mode");
                Domain d = mode == "edges" ? Domain::Edge : mode == "faces" ? Domain::Face : mode == "corners" ? Domain::Corner : Domain::Point;
                FieldContext ctx = FieldContext::forMesh(m, d);
                std::vector<Index> sel = selected(c.field("selection"), ctx);
                std::vector<D3> pos = evaluateAll<D3>(c.field("position"), ctx);
                std::vector<double> rad = evaluateAll<double>(c.field("radius"), ctx);
                geo::Mesh pts;
                if (d == Domain::Point)
                    pts.pointAttrs().mergeLayout(m.pointAttrs());
                std::vector<double> radii;
                for (Index k : sel)
                {
                    Index p = pts.addPoint(pos[k]);
                    if (d == Domain::Point)
                        pts.pointAttrs().copyFrom(m.pointAttrs(), p, k);
                    radii.push_back(rad[k]);
                }
                writeAttribute(pts, "radius", Domain::Point, Column::of(std::move(radii)), T::Float);
                Geometry out;
                out.points = std::make_shared<const geo::Mesh>(std::move(pts));
                c.out("points", makeGeometry(std::move(out)));
            },
    });
    registerDecl({
        .type = "PointsToVertices",
        .label = "Points to Vertices",
        .category = "Point",
        .doc = "Turns a point cloud into a mesh of loose vertices.",
        .sockets = {.inputs = {geoIn("points"), selectionIn()}, .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("points");
                if (!g->points)
                {
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                std::vector<uint8_t> keep = expand<uint8_t>(evaluate(c.field("selection"), FieldContext::forPoints(*g->points), T::Bool),
                                                           g->points->pointCapacity());
                geo::Mesh m;
                m.pointAttrs().mergeLayout(g->points->pointAttrs());
                for (Index p = 0; p < g->points->pointCapacity(); ++p)
                    if (keep[p])
                        m.pointAttrs().copyFrom(g->points->pointAttrs(), m.addPoint(g->points->position(p)), p);
                c.out("mesh", meshGeometry(std::move(m)));
            },
    });
}

/** Uniform grid for the Poisson-disk rejection test. */
struct PointGrid
{
    double cell;
    std::unordered_map<uint64_t, std::vector<D3>> cells;

    uint64_t key(int64_t x, int64_t y, int64_t z) const { return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff); }
    glm::i64vec3 coord(const D3& p) const { return glm::i64vec3(glm::floor(p / cell)); }
    bool near(const D3& p, double dist) const
    {
        glm::i64vec3 c = coord(p);
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    auto it = cells.find(key(c.x + dx, c.y + dy, c.z + dz));
                    if (it == cells.end())
                        continue;
                    for (const D3& q : it->second)
                        if (glm::distance(p, q) < dist)
                            return true;
                }
        return false;
    }
    void add(const D3& p)
    {
        glm::i64vec3 c = coord(p);
        cells[key(c.x, c.y, c.z)].push_back(p);
    }
};

void distribute()
{
    registerDecl({
        .type = "DistributePointsOnFaces",
        .label = "Distribute Points on Faces",
        .category = "Point",
        .doc = "Scatters points over the surface. Density is points per square metre (10000 cm2). Each point gets a stable \"id\", "
               "so Random Value stays put as the surface changes.",
        .props = {prop("distributeMethod", {"random", "poissonDisk"}, "random")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                s.inputs = {geoIn("mesh"), selectionIn()};
                if (findDecl("DistributePointsOnFaces")->propValue(n, "distributeMethod") == "poissonDisk")
                {
                    s.inputs.push_back(bounded(in("distanceMin", T::Float, 10.0, "Minimum spacing, cm."), 0.0));
                    s.inputs.push_back(bounded(in("densityMax", T::Float, 100.0, "Candidate density before spacing, per m2."), 0.0));
                    s.inputs.push_back(bounded(fin("densityFactor", T::Float, 1.0), 0.0, 1.0));
                }
                else
                    s.inputs.push_back(bounded(fin("density", T::Float, 10.0), 0.0));
                s.inputs.push_back(in("seed", T::Int, 0));
                s.outputs = {geoOut("points"), fout("normal", T::Vector), fout("rotation", T::Rotation)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::string normalName = c.anonymous("normal"), rotName = c.anonymous("rotation");
                c.out("normal", attributeField(normalName, T::Vector));
                c.out("rotation", attributeField(rotName, T::Rotation));
                GeometryPtr g = c.geometry("mesh");
                if (!g->mesh)
                {
                    c.out("points", makeGeometry({}));
                    return;
                }
                const geo::Mesh& m = *g->mesh;
                bool poisson = c.prop("distributeMethod") == "poissonDisk";
                FieldContext fctx = FieldContext::forMesh(m, Domain::Face);
                std::vector<uint8_t> sel = expand<uint8_t>(evaluate(c.field("selection"), fctx, T::Bool), fctx.size);
                std::vector<double> density = poisson ? std::vector<double>(fctx.size, c.f("densityMax")) : evaluateAll<double>(c.field("density"), fctx);
                std::vector<double> factor = poisson ? evaluateAll<double>(c.field("densityFactor"), fctx) : std::vector<double>();
                uint32_t seed = uint32_t(c.i("seed"));
                geo::Triangulation tris = geo::triangulate(m);
                size_t nt = tris.faces.size();

                std::vector<uint32_t> count(nt, 0);
                tbb::parallel_for(size_t(0), nt, [&](size_t t) {
                    Index f = tris.faces[t];
                    if (!sel[f] || density[f] <= 0.0)
                        return;
                    D3 a = m.position(m.cornerPoint(tris.corners[t * 3])), b = m.position(m.cornerPoint(tris.corners[t * 3 + 1])),
                       cc = m.position(m.cornerPoint(tris.corners[t * 3 + 2]));
                    double expected = 0.5 * glm::length(glm::cross(b - a, cc - a)) * density[f] / 10000.0;
                    double whole = std::floor(expected);
                    count[t] = uint32_t(whole) + (hashToUnit(hash32(uint32_t(t), seed, 0x51)) < expected - whole ? 1 : 0);
                });
                std::vector<size_t> first(nt + 1, 0);
                for (size_t t = 0; t < nt; ++t)
                    first[t + 1] = first[t] + count[t];
                size_t total = first[nt];
                std::vector<D3> pos(total), nrm(total);
                std::vector<int32_t> ids(total);
                std::vector<Index> faceOf(total);
                tbb::parallel_for(size_t(0), nt, [&](size_t t) {
                    if (!count[t])
                        return;
                    Index f = tris.faces[t];
                    D3 a = m.position(m.cornerPoint(tris.corners[t * 3])), b = m.position(m.cornerPoint(tris.corners[t * 3 + 1])),
                       cc = m.position(m.cornerPoint(tris.corners[t * 3 + 2]));
                    D3 n = m.faceNormal(f);
                    for (uint32_t k = 0; k < count[t]; ++k)
                    {
                        uint32_t h = hash32(uint32_t(t), k, seed);
                        double s = std::sqrt(hashToUnit(hash32(h, 1))), u = hashToUnit(hash32(h, 2));
                        size_t i = first[t] + k;
                        pos[i] = a * (1.0 - s) + b * (s * (1.0 - u)) + cc * (s * u);
                        nrm[i] = n;
                        ids[i] = int32_t(h & 0x7fffffff);
                        faceOf[i] = f;
                    }
                });

                std::vector<uint8_t> keep(total, 1);
                if (poisson)
                {
                    double dmin = std::max(1e-6, c.f("distanceMin"));
                    PointGrid grid{dmin, {}};
                    for (size_t i = 0; i < total; ++i)
                    {
                        if (grid.near(pos[i], dmin) || hashToUnit(hash32(uint32_t(ids[i]), seed, 0x77)) >= factor[faceOf[i]])
                        {
                            keep[i] = 0;
                            continue;
                        }
                        grid.add(pos[i]);
                    }
                }

                geo::Mesh pts;
                std::vector<D3> outN, outR;
                std::vector<int64_t> outId;
                for (size_t i = 0; i < total; ++i)
                {
                    if (!keep[i])
                        continue;
                    pts.addPoint(pos[i]);
                    outN.push_back(nrm[i]);
                    outR.push_back(matrixToEuler(rotationBetween(D3(0, 1, 0), nrm[i])));
                    outId.push_back(ids[i]);
                }
                writeAttribute(pts, "id", Domain::Point, Column::of(std::move(outId)), T::Int);
                writeAttribute(pts, normalName, Domain::Point, Column::of(std::move(outN)), T::Vector);
                writeAttribute(pts, rotName, Domain::Point, Column::of(std::move(outR)), T::Rotation);
                Geometry out;
                out.points = std::make_shared<const geo::Mesh>(std::move(pts));
                c.out("points", makeGeometry(std::move(out)));
            },
    });
}
} // namespace

void registerMeshNodes()
{
    meshPrimitives();
    meshOps();
    pointConversions();
    distribute();
}
} // namespace lila::gn::nodes
