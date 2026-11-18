#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/BoundingShapes.h"
#include "geo/Bvh.h"
#include "geo/Conform.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Reduce.h"
#include "retopo/Retopo.h"

#include <climits>
#include <numeric>
#include <unordered_map>

namespace lila::gn::nodes
{
namespace meshops
{
int clampInt(int64_t v, int lo, int hi) { return int(std::clamp<int64_t>(v, lo, hi)); }

struct UnionFind
{
    std::vector<Index> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), Index(0)); }
    Index find(Index a)
    {
        while (parent[a] != a)
            a = parent[a] = parent[parent[a]];
        return a;
    }
    void unite(Index a, Index b) { parent[find(a)] = find(b); }
};

/** A mesh-only operation as a node: mesh and selection in, mesh out, plus any bool field outputs the op fills. */
void meshOp(const char* type, const char* label, Domain selDomain, std::vector<SocketDecl> extra, std::vector<PropDecl> props,
            std::vector<SocketDecl> fieldOuts, std::function<void(geo::Mesh&, std::vector<Index>&, ExecContext&)> fn, const char* doc)
{
    std::vector<SocketDecl> inputs = {geoIn("mesh"), selectionIn()};
    inputs.insert(inputs.end(), extra.begin(), extra.end());
    std::vector<SocketDecl> outputs = {geoOut("mesh")};
    outputs.insert(outputs.end(), fieldOuts.begin(), fieldOuts.end());
    registerDecl({
        .type = type,
        .label = label,
        .category = "Mesh",
        .doc = doc,
        .props = std::move(props),
        .sockets = {.inputs = std::move(inputs), .outputs = std::move(outputs)},
        .exec =
            [fn, selDomain](ExecContext& c) {
                LILA_ZONE();
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<Index> sel = selected(c.field("selection"), FieldContext::forMesh(m, selDomain));
                                                                      fn(m, sel, c);
                                                                  }}));
                for (size_t k = 1; k < c.sockets.outputs.size(); ++k)
                    c.out(c.sockets.outputs[k].id, attributeField(c.anonymous(c.sockets.outputs[k].id), T::Bool));
            },
    });
}

/** A whole-mesh rebuild as a node: the mesh is replaced by what fn returns. */
void meshRebuild(const char* type, const char* label, std::vector<SocketDecl> extra, std::vector<PropDecl> props,
                 std::function<geo::Mesh(const geo::Mesh&, ExecContext&)> fn, const char* doc)
{
    std::vector<SocketDecl> inputs = {geoIn("mesh")};
    inputs.insert(inputs.end(), extra.begin(), extra.end());
    registerDecl({
        .type = type,
        .label = label,
        .category = "Mesh",
        .doc = doc,
        .props = std::move(props),
        .sockets = {.inputs = std::move(inputs), .outputs = {geoOut("mesh")}},
        .exec =
            [fn](ExecContext& c) {
                LILA_ZONE();
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) { m = fn(m, c); }}));
            },
    });
}

void markFaces(geo::Mesh& m, const std::string& name, std::span<const Index> faces)
{
    auto flags = m.faceAttrs().getOrAdd<bool>(name);
    for (Index f : faces)
        if (f < flags.size())
            flags[f] = true;
}

std::vector<uint8_t> pointMask(const geo::Mesh& m, std::span<const Index> points)
{
    std::vector<uint8_t> mask(m.pointCapacity(), 0);
    for (Index p : points)
        mask[p] = 1;
    return mask;
}

// ------------------------------------------------------------------------------------------------ solidify

geo::Mesh solidify(const geo::Mesh& src, double thickness, double offset, bool rim)
{
    geo::Mesh m = src;
    std::vector<glm::vec3> normals = geo::pointNormals(m);
    const Index points = m.pointCapacity(), faces = m.faceCapacity();
    const double outer = thickness * (1.0 + offset) * 0.5, inner = thickness * (1.0 - offset) * 0.5;
    std::vector<Index> twin(points, geo::kInvalid);
    for (Index p = 0; p < points; ++p)
    {
        if (!m.pointValid(p))
            continue;
        D3 n(normals[p]);
        D3 at = m.position(p);
        m.setPosition(p, at + n * outer);
        twin[p] = m.addPoint(at - n * inner);
        m.pointAttrs().copy(twin[p], p);
    }
    std::vector<Index> boundary;
    for (Index f = 0; f < faces; ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> corners = m.faceCorners(f);
        std::vector<Index> pts, srcCorners;
        for (size_t k = corners.size(); k-- > 0;)
        {
            pts.push_back(twin[m.cornerPoint(corners[k])]);
            srcCorners.push_back(corners[k]);
        }
        m.addFace(pts, f, srcCorners);
        for (Index cc : corners)
            if (m.cornerRadial(cc) == cc)
                boundary.push_back(cc);
    }
    if (rim)
        for (Index cc : boundary)
        {
            Index a = m.cornerPoint(cc), b = m.cornerNextPoint(cc);
            std::array<Index, 4> quad = {b, a, twin[a], twin[b]};
            std::array<Index, 4> srcCorners = {m.cornerNext(cc), cc, cc, m.cornerNext(cc)};
            m.addFace(quad, m.cornerFace(cc), srcCorners);
        }
    m.cornerAttrs().remove(geo::attr::kNormal);
    return m;
}

// ------------------------------------------------------------------------------------------------ dual

geo::Mesh dualMesh(const geo::Mesh& m)
{
    geo::Mesh out;
    std::vector<Index> facePoint(m.faceCapacity(), geo::kInvalid);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            facePoint[f] = out.addPoint(m.faceCentroid(f));
    std::vector<uint8_t> done(m.pointCapacity(), 0);
    for (Index start = 0; start < m.cornerCapacity(); ++start)
    {
        if (!m.cornerValid(start))
            continue;
        Index p = m.cornerPoint(start);
        if (done[p] || m.isBoundaryPoint(p))
            continue;
        done[p] = 1;
        std::vector<Index> ring;
        Index c = start;
        Index limit = m.pointValence(p) + 1;
        bool closed = false;
        for (Index step = 0; step < limit; ++step)
        {
            ring.push_back(facePoint[m.cornerFace(c)]);
            Index in = m.cornerPrev(c);
            Index r = m.cornerRadial(in);
            if (r == in)
                break;
            c = m.cornerPoint(r) == p ? r : m.cornerNext(r);
            if (c == start)
            {
                closed = true;
                break;
            }
        }
        if (closed && ring.size() >= 3)
            out.addFace(ring);
    }
    out.removeAllLoose();
    return out;
}

// ------------------------------------------------------------------------------------------------ split edges

void splitEdges(geo::Mesh& m, std::span<const Index> edges)
{
    std::vector<uint8_t> cut(m.edgeCapacity(), 0);
    for (Index e : edges)
        cut[e] = 1;
    std::vector<Index> cornerPoint(m.cornerCapacity(), geo::kInvalid);
    const Index points = m.pointCapacity();
    struct Group
    {
        Index point;
        std::vector<std::vector<Index>> fans;
    };
    std::vector<Group> groups;
    for (Index p = 0; p < points; ++p)
    {
        if (!m.pointValid(p))
            continue;
        bool touched = false;
        m.forEachPointEdge(p, [&](Index e) { touched = touched || cut[e]; });
        if (!touched)
            continue;
        std::vector<Index> corners;
        m.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
        if (corners.size() < 2)
            continue;
        std::unordered_map<Index, Index> local;
        for (Index k = 0; k < corners.size(); ++k)
            local[corners[k]] = k;
        UnionFind uf(corners.size());
        m.forEachPointEdge(p, [&](Index e) {
            if (cut[e])
                return;
            Index first = geo::kInvalid;
            m.forEachEdgeCorner(e, [&](Index r) {
                Index at = m.cornerPoint(r) == p ? r : m.cornerNext(r);
                auto it = local.find(at);
                if (it == local.end())
                    return;
                if (first == geo::kInvalid)
                    first = it->second;
                else
                    uf.unite(first, it->second);
            });
        });
        std::unordered_map<Index, size_t> fanOf;
        Group g{p, {}};
        for (Index k = 0; k < corners.size(); ++k)
        {
            auto [it, added] = fanOf.try_emplace(uf.find(k), g.fans.size());
            if (added)
                g.fans.emplace_back();
            g.fans[it->second].push_back(corners[k]);
        }
        if (g.fans.size() > 1)
            groups.push_back(std::move(g));
    }
    for (const Group& g : groups)
        for (size_t f = 1; f < g.fans.size(); ++f)
        {
            Index np = m.addPoint(m.position(g.point));
            m.pointAttrs().copy(np, g.point);
            for (Index c : g.fans[f])
                cornerPoint[c] = np;
        }
    std::vector<Index> oldEdges;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> corners = m.faceCorners(f);
        bool changed = false;
        std::vector<Index> pts, before;
        for (Index c : corners)
        {
            changed = changed || cornerPoint[c] != geo::kInvalid;
            pts.push_back(cornerPoint[c] != geo::kInvalid ? cornerPoint[c] : m.cornerPoint(c));
            before.push_back(m.cornerEdge(c));
        }
        if (!changed || !m.setFacePoints(f, pts))
            continue;
        for (size_t k = 0; k < corners.size(); ++k)
        {
            Index e = m.cornerEdge(corners[k]);
            if (e != before[k])
            {
                m.edgeAttrs().copy(e, before[k]);
                oldEdges.push_back(before[k]);
            }
        }
    }
    m.removeLoose(oldEdges, {});
}

// ------------------------------------------------------------------------------------------------ linear subdivide

geo::Mesh subdivideLinear(const geo::Mesh& m)
{
    geo::Mesh out;
    out.pointAttrs().mergeLayout(m.pointAttrs());
    out.edgeAttrs().mergeLayout(m.edgeAttrs());
    out.faceAttrs().mergeLayout(m.faceAttrs());
    out.cornerAttrs().mergeLayout(m.cornerAttrs());
    out.materialNames() = m.materialNames();
    std::vector<Index> pointMap(m.pointCapacity(), geo::kInvalid), edgeMid(m.edgeCapacity(), geo::kInvalid);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            pointMap[p] = out.addPoint(m.position(p));
            out.pointAttrs().copyFrom(m.pointAttrs(), pointMap[p], p);
        }
    const std::array<float, 2> half = {0.5f, 0.5f};
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
            edgeMid[e] = out.addPoint((m.position(a) + m.position(b)) * 0.5);
            std::array<Index, 2> src = {a, b};
            out.pointAttrs().mixFrom(m.pointAttrs(), edgeMid[e], src, half);
        }
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> corners = m.faceCorners(f), pts = m.facePoints(f);
        const size_t n = corners.size();
        Index centre = out.addPoint(m.faceCentroid(f));
        std::vector<float> even(n, float(1.0 / double(n)));
        out.pointAttrs().mixFrom(m.pointAttrs(), centre, pts, even);
        for (size_t k = 0; k < n; ++k)
        {
            Index prev = corners[(k + n - 1) % n], cur = corners[k];
            std::array<Index, 4> quad = {pointMap[m.cornerPoint(cur)], edgeMid[m.cornerEdge(cur)], centre, edgeMid[m.cornerEdge(prev)]};
            Index nf = out.addFace(quad);
            if (nf == geo::kInvalid)
                continue;
            out.faceAttrs().copyFrom(m.faceAttrs(), nf, f);
            Index oc = out.faceCorner(nf);
            std::array<Index, 2> toNext = {cur, m.cornerNext(cur)}, fromPrev = {prev, cur};
            out.cornerAttrs().copyFrom(m.cornerAttrs(), oc, cur);
            out.cornerAttrs().mixFrom(m.cornerAttrs(), out.cornerNext(oc), toNext, half);
            out.cornerAttrs().mixFrom(m.cornerAttrs(), out.cornerNext(out.cornerNext(oc)), corners, even);
            out.cornerAttrs().mixFrom(m.cornerAttrs(), out.cornerPrev(oc), fromPrev, half);
        }
    }
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            for (int s = 0; s < 2; ++s)
            {
                Index half2 = out.addEdge(pointMap[m.edgePoint(e, s)], edgeMid[e]);
                out.edgeAttrs().copyFrom(m.edgeAttrs(), half2, e);
            }
    out.cornerAttrs().remove(geo::attr::kNormal);
    return out;
}

// ------------------------------------------------------------------------------------------------ scale elements

void scaleElements(geo::Mesh& m, ExecContext& c)
{
    Domain d = c.prop("domain") == "edge" ? Domain::Edge : Domain::Face;
    FieldContext ctx = FieldContext::forMesh(m, d);
    std::vector<Index> sel = selected(c.field("selection"), ctx);
    if (sel.empty())
        return;
    std::vector<double> scale = evaluateAll<double>(c.field("scale"), ctx);
    bool customCentre = !c.group.linksInto(c.node.name, "center").empty();
    std::vector<D3> centre = customCentre ? evaluateAll<D3>(c.field("center"), ctx) : std::vector<D3>();
    auto elementPoints = [&](Index el) {
        return d == Domain::Face ? m.facePoints(el) : std::vector<Index>{m.edgePoint(el, 0), m.edgePoint(el, 1)};
    };
    UnionFind uf(m.pointCapacity());
    for (Index el : sel)
    {
        std::vector<Index> pts = elementPoints(el);
        for (size_t k = 1; k < pts.size(); ++k)
            uf.unite(pts[0], pts[k]);
    }
    struct Island
    {
        D3 centre{0.0};
        double scale = 0.0;
        int count = 0;
    };
    std::unordered_map<Index, Island> islands;
    for (Index el : sel)
    {
        std::vector<Index> pts = elementPoints(el);
        Island& is = islands[uf.find(pts[0])];
        D3 at = customCentre ? centre[el] : (d == Domain::Face ? m.faceCentroid(el) : (m.position(pts[0]) + m.position(pts[1])) * 0.5);
        is.centre += at;
        is.scale += scale[el];
        ++is.count;
    }
    std::vector<uint8_t> moving(m.pointCapacity(), 0);
    for (Index el : sel)
        for (Index p : elementPoints(el))
            moving[p] = 1;
    bool single = c.prop("scaleMode") == "singleAxis";
    D3 axis = c.v("axis");
    axis = glm::length(axis) > 1e-12 ? glm::normalize(axis) : D3(1, 0, 0);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!moving[p])
            continue;
        const Island& is = islands[uf.find(p)];
        D3 ctr = is.centre / double(is.count);
        double s = is.scale / double(is.count);
        D3 rel = m.position(p) - ctr;
        m.setPosition(p, single ? m.position(p) + axis * glm::dot(rel, axis) * (s - 1.0) : ctr + rel * s);
    }
    m.cornerAttrs().remove(geo::attr::kNormal);
}

// ------------------------------------------------------------------------------------------------ shapes

std::vector<D3> allPositions(const Geometry& g)
{
    std::vector<D3> pts;
    Geometry real = g.instances ? realize(g) : g;
    for (const auto& m : {real.mesh, real.points})
        if (m)
            for (Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                    pts.push_back(m->position(p));
    if (real.curves)
    {
        CurveSet ev = real.curves->evaluated();
        pts.insert(pts.end(), ev.positions.begin(), ev.positions.end());
    }
    if (real.heightfield)
    {
        Geometry terrain;
        terrain.heightfield = real.heightfield;
        geo::Mesh hm = toOutputMesh(terrain);
        for (Index p = 0; p < hm.pointCapacity(); ++p)
            if (hm.pointValid(p))
                pts.push_back(hm.position(p));
    }
    return pts;
}

glm::dmat4 frameMatrix(const glm::dmat3& axes, const D3& centre)
{
    glm::dmat4 xf(axes);
    xf[3] = glm::dvec4(centre, 1.0);
    return xf;
}

geo::Mesh boundingShape(std::span<const D3> pts, const std::string& shape, int segments)
{
    geo::Mesh m;
    if (pts.empty())
        return m;
    if (shape == "hull")
        return geo::convexHullMesh(pts, INT_MAX);
    if (shape == "box")
    {
        geo::Bounds b;
        for (const D3& p : pts)
            b.extend(p);
        D3 s = glm::max(b.size(), D3(1e-6));
        m = geo::makeCube({s.x, s.y, s.z});
        transformMesh(m, frameMatrix(glm::dmat3(1.0), b.center()));
        return m;
    }
    if (shape == "sphere")
    {
        geo::RoundShape r = geo::minimumSphere(pts);
        m = geo::makeSphere({std::max(r.radius, 1e-6), segments, std::max(2, segments / 2)});
        transformMesh(m, frameMatrix(glm::dmat3(1.0), r.center));
        return m;
    }
    std::optional<geo::OrientedBox> box = geo::minimumVolumeBox(pts);
    glm::dmat3 frame = box ? box->axes : glm::dmat3(1.0);
    if (shape == "capsule" || shape == "cylinder")
    {
        geo::RoundShape r = shape == "capsule" ? geo::fitCapsule(pts, frame) : geo::fitCylinder(pts, frame);
        double radius = std::max(r.radius, 1e-6);
        if (shape == "capsule")
            m = geo::makeCapsule({.radius = radius, .height = std::max(r.height, 2.0 * radius), .subdivAxis = segments});
        else
            m = geo::makeCylinder({.radius = radius, .height = std::max(r.height, 1e-6), .subdivAxis = segments});
        transformMesh(m, frameMatrix(r.axes, r.center));
        return m;
    }
    geo::OrientedBox ob = box ? *box : geo::boxInFrame(pts, frame);
    D3 s = glm::max(ob.size, D3(1e-6));
    m = geo::makeCube({s.x, s.y, s.z});
    transformMesh(m, frameMatrix(ob.axes, ob.center));
    return m;
}

// ------------------------------------------------------------------------------------------------ deformers

/** Columns map the deformer's handle space (deformation along +Y) into object space. */
glm::dmat3 handleFrame(const std::string& axis, const D3& rotation)
{
    glm::dmat3 toAxis(1.0);
    if (axis == "x")
        toAxis = glm::dmat3(0, -1, 0, 1, 0, 0, 0, 0, 1);
    else if (axis == "z")
        toAxis = glm::dmat3(1, 0, 0, 0, 0, 1, 0, -1, 0);
    return eulerToMatrix(rotation) * toAxis;
}

using PointMap = std::function<D3(const D3&)>;

void deformerNode(const char* type, const char* label, std::vector<SocketDecl> params, std::function<PointMap(ExecContext&)> make, const char* doc)
{
    std::vector<SocketDecl> inputs = {geoIn(), selectionIn(), in("center", T::Vector, vec(0, 0, 0), "Origin of the deformer."),
                                      in("rotation", T::Rotation, vec(0, 0, 0), "Turns the deformer's frame after the axis is chosen."),
                                      in("low", T::Float, -50.0, "Start of the deformed range along the axis, from the centre."),
                                      in("high", T::Float, 50.0, "End of the deformed range along the axis, from the centre.")};
    inputs.insert(inputs.end(), params.begin(), params.end());
    registerDecl({
        .type = type,
        .label = label,
        .category = "Deform",
        .doc = doc,
        .props = {prop("axis", {"x", "y", "z"}, "y", "Axis the deformation runs along.")},
        .sockets = {.inputs = std::move(inputs), .outputs = {geoOut()}},
        .exec =
            [make](ExecContext& c) {
                LILA_ZONE();
                PointMap fn = make(c);
                glm::dmat3 frame = handleFrame(c.prop("axis"), c.v("rotation"));
                glm::dmat3 inv = glm::transpose(frame);
                D3 centre = c.v("center");
                auto apply = [&](const D3& p) { return centre + frame * fn(inv * (p - centre)); };
                auto deformPoints = [&](geo::Mesh& m, const FieldContext& ctx) {
                    std::vector<Index> sel = selected(c.field("selection"), ctx);
                    tbb::parallel_for(size_t(0), sel.size(), [&](size_t k) { m.setPosition(sel[k], apply(m.position(sel[k]))); });
                    m.cornerAttrs().remove(geo::attr::kNormal);
                };
                c.out("geometry", editComponents(c.geometry("geometry"), {.mesh = [&](geo::Mesh& m) { deformPoints(m, FieldContext::forMesh(m, Domain::Point)); },
                                                                .points = [&](geo::Mesh& m) { deformPoints(m, FieldContext::forPoints(m)); },
                                                                .curves =
                                                                    [&](CurveSet& cs) {
                                                                        std::vector<Index> sel = selected(c.field("selection"), FieldContext::forCurves(cs, Domain::Point));
                                                                        auto left = cs.pointAttrs.get<glm::vec3>("handle_left");
                                                                        auto right = cs.pointAttrs.get<glm::vec3>("handle_right");
                                                                        for (Index p : sel)
                                                                        {
                                                                            cs.positions[p] = apply(cs.positions[p]);
                                                                            if (p < left.size())
                                                                                left[p] = glm::vec3(apply(D3(left[p])));
                                                                            if (p < right.size())
                                                                                right[p] = glm::vec3(apply(D3(right[p])));
                                                                        }
                                                                    }}));
            },
    });
}

// ------------------------------------------------------------------------------------------------ registration

void topologyOps()
{
    meshOp(
        "Bevel", "Bevel", Domain::Edge,
        {bounded(in("offset", T::Float, 5.0, "Width from each edge to the new edges."), 0.0), bounded(in("segments", T::Int, 1), 1, 100),
         bounded(in("profile", T::Float, 0.5, "0.5 round, 0.25 straight, towards 1 square."), 0.0, 1.0), in("clamp", T::Bool, true, "Keep offsets from overlapping."),
         in("smoothingAngle", T::Float, 30.0, "New edges sharper than this many degrees are hard; negative leaves them soft.")},
        {}, {fout("bevel", T::Bool, "The new bevel faces.")},
        [](geo::Mesh& m, std::vector<Index>& sel, ExecContext& c) {
            geo::BevelOptions o;
            o.offset = std::max(0.0, c.f("offset"));
            o.segments = clampInt(c.i("segments"), 1, 100);
            o.profile = std::clamp(c.f("profile"), 0.0, 1.0);
            o.clamp = c.b("clamp");
            o.smoothingAngle = c.f("smoothingAngle");
            std::vector<Index> faces = geo::bevelEdges(m, sel, o);
            markFaces(m, c.anonymous("bevel"), faces);
        },
        "Bevels the selected manifold edges.");

    meshOp(
        "InsetFaces", "Inset Faces", Domain::Face,
        {bounded(in("thickness", T::Float, 5.0, "Inward distance in the face plane."), 0.0), in("depth", T::Float, 0.0, "Distance along the normals after the inset."),
         in("individual", T::Bool, false, "Each face separately instead of connected regions."), in("clamp", T::Bool, true)},
        {}, {fout("inner", T::Bool, "The inset faces."), fout("rim", T::Bool, "The new faces round them.")},
        [](geo::Mesh& m, std::vector<Index>& sel, ExecContext& c) {
            geo::InsetOptions o;
            o.thickness = std::max(0.0, c.f("thickness"));
            o.depth = c.f("depth");
            o.individual = c.b("individual");
            o.clamp = c.b("clamp");
            geo::ExtrudeResult r = geo::insetFaces(m, sel, o);
            markFaces(m, c.anonymous("inner"), r.faces);
            markFaces(m, c.anonymous("rim"), r.sides);
        },
        "Insets the selected faces, leaving a ring of faces round each region.");

    meshRebuild(
        "Solidify", "Solidify",
        {in("thickness", T::Float, 5.0), bounded(in("offset", T::Float, -1.0, "-1 grows inward from the surface, 1 outward, 0 both ways."), -1.0, 1.0),
         in("fillRim", T::Bool, true, "Close open borders between the two shells.")},
        {}, [](const geo::Mesh& m, ExecContext& c) { return solidify(m, c.f("thickness"), std::clamp(c.f("offset"), -1.0, 1.0), c.b("fillRim")); },
        "Gives the surface thickness: an inner shell along the point normals, joined at open borders.");

    meshOp(
        "BridgeEdgeLoops", "Bridge Edge Loops", Domain::Edge,
        {bounded(in("divisions", T::Int, 0, "Extra rows of faces between the loops; -1 picks them from the gap and the open ends."), -1, 1000),
         bounded(in("smoothness", T::Float, 1.0, "0 straightens the curve, 1 is the full curve."), 0.0, 1.0),
         in("tensionSource", T::Float, 1.0), in("tensionTarget", T::Float, 1.0),
         in("offset", T::Int, 0, "Steps the pairing round closed loops."), in("twist", T::Float, 0.0, "Degrees from source to target."),
         in("taper", T::Float, 1.0, "Scale of the middle rows."), bounded(in("bias", T::Float, 0.5, "Where the taper peaks."), 0.0, 1.0),
         in("profileFactor", T::Float, 0.0, "Bulge (positive) or pinch of the middle rows."),
         in("weldEnds", T::Bool, true, "Open bridges land their end rows on the border between the sides' ends."),
         bounded(in("relax", T::Int, 0, "Smoothing passes over the new points."), 0, 100),
         in("merge", T::Bool, false, "Merge the loops instead of bridging."), bounded(in("mergeFactor", T::Float, 0.5), 0.0, 1.0)},
        {prop("curve", {"linear", "blend", "auto"}, "linear", "Linear rows, a blend square to each border, or a fit to the faces around them."),
         prop("profileShape", {"smooth", "sphere", "root", "sharp", "linear", "inverseSquare", "constant"}, "smooth"),
         prop("transition", {"quads", "triangles"}, "quads", "How borders of different lengths make up the difference."),
         prop("connect", {"pairs", "chain", "chainClosed"}, "pairs", "Pairs, or one run through every loop in order.")},
        {fout("bridge", T::Bool, "The new faces.")},
        [](geo::Mesh& m, std::vector<Index>& sel, ExecContext& c) {
            geo::BridgeOptions o;
            o.divisions = clampInt(c.i("divisions"), -1, 1000);
            const std::string curve = c.prop("curve"), shape = c.prop("profileShape"), connect = c.prop("connect");
            o.curve = curve == "auto" ? geo::BridgeCurve::Auto : curve == "blend" ? geo::BridgeCurve::Blend : geo::BridgeCurve::Linear;
            o.smoothness = std::clamp(c.f("smoothness"), 0.0, 1.0);
            o.tensionSource = c.f("tensionSource");
            o.tensionTarget = c.f("tensionTarget");
            o.offset = int(c.i("offset"));
            o.twist = c.f("twist");
            o.taper = c.f("taper");
            o.bias = std::clamp(c.f("bias"), 0.0, 1.0);
            o.profileFactor = c.f("profileFactor");
            const char* shapes[] = {"smooth", "sphere", "root", "sharp", "linear", "inverseSquare", "constant"};
            for (size_t k = 0; k < std::size(shapes); ++k)
                if (shape == shapes[k])
                    o.profileShape = geo::BridgeProfile(k);
            o.transition = c.prop("transition") == "triangles" ? geo::BridgeTransition::Triangles : geo::BridgeTransition::Quads;
            o.connect = connect == "chain" ? geo::BridgeConnect::Chain : connect == "chainClosed" ? geo::BridgeConnect::ChainClosed : geo::BridgeConnect::Pairs;
            o.weldEnds = c.b("weldEnds");
            o.relax = clampInt(c.i("relax"), 0, 100);
            o.merge = c.b("merge");
            o.mergeFactor = std::clamp(c.f("mergeFactor"), 0.0, 1.0);
            markFaces(m, c.anonymous("bridge"), geo::bridgeEdges(m, sel, o));
        },
        "Joins pairs of open border loops among the selected edges with faces; loops of different lengths make up the difference with quads.");

    meshOp(
        "MirrorMesh", "Mirror Mesh", Domain::Point,
        {in("offset", T::Float, 0.0, "Where the plane lies along its axis."), in("weld", T::Bool, true, "Merge points on the plane with their mirror images."),
         bounded(in("tolerance", T::Float, 0.001), 0.0)},
        {prop("axis", {"x", "y", "z"}, "x"), prop("direction", {"both", "positive", "negative"}, "both", "Positive keeps the side the axis points to and rebuilds the other.")},
        {},
        [](geo::Mesh& m, std::vector<Index>&, ExecContext& c) {
            geo::MirrorOptions o;
            std::string axis = c.prop("axis"), dir = c.prop("direction");
            o.plane = geo::axisPlane(axis == "y" ? 1 : axis == "z" ? 2 : 0, c.f("offset"));
            o.direction = dir == "positive" ? geo::MirrorDirection::Positive : dir == "negative" ? geo::MirrorDirection::Negative : geo::MirrorDirection::Both;
            o.weld = c.b("weld");
            o.tolerance = std::max(0.0, c.f("tolerance"));
            geo::mirrorMesh(m, o);
        },
        "Adds a mirrored copy across an axis plane, welded on the plane.");

    meshRebuild(
        "Decimate", "Decimate",
        {bounded(in("ratio", T::Float, 0.5, "Fraction of triangles to keep."), 0.0, 1.0), bounded(in("targetTriangles", T::Int, 0, "Positive overrides the ratio."), 0, INT_MAX),
         in("keepBorders", T::Bool, true), in("keepHardEdges", T::Bool, true), in("quads", T::Bool, false, "Join triangle pairs back into quads.")},
        {prop("symmetry", {"none", "x", "y", "z"}, "none")},
        [](const geo::Mesh& m, ExecContext& c) {
            geo::ReduceOptions o;
            o.ratio = std::clamp(c.f("ratio"), 0.0, 1.0);
            o.targetTriangles = Index(std::max<int64_t>(0, c.i("targetTriangles")));
            o.keepBorders = c.b("keepBorders");
            o.keepHardEdges = c.b("keepHardEdges");
            o.quads = c.b("quads");
            std::string s = c.prop("symmetry");
            o.symmetryAxis = s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : -1;
            return geo::reduceMesh(m, o).mesh;
        },
        "Quadric edge-collapse reduction that keeps uvs, borders and hard edges.");

    meshRebuild(
        "QuadRemesh", "Quad Remesh",
        {bounded(in("targetFaces", T::Int, 500), 1, 10000000), bounded(in("targetEdgeLength", T::Float, 0.0, "Positive overrides the face count."), 0.0),
         bounded(in("adaptivity", T::Float, 0.25, "0 uniform, 1 denser where curved."), 0.0, 1.0),
         in("sharpAngle", T::Float, 40.0, "Edges sharper than this many degrees steer the flow; negative off."), in("pureQuads", T::Bool, true)},
        {prop("symmetry", {"none", "x", "y", "z"}, "none")},
        [](const geo::Mesh& m, ExecContext& c) {
            retopo::QuadRemeshOptions o;
            o.targetFaces = clampInt(c.i("targetFaces"), 1, 10000000);
            o.targetEdgeLength = std::max(0.0, c.f("targetEdgeLength"));
            o.adaptivity = std::clamp(c.f("adaptivity"), 0.0, 1.0);
            o.sharpAngle = c.f("sharpAngle");
            o.pureQuads = c.b("pureQuads");
            std::string s = c.prop("symmetry");
            o.symmetryAxis = s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : -1;
            retopo::QuadRemeshResult r = retopo::quadRemesh(m, o);
            if (!r.ok())
                throw ExecError(c.node.name + ": " + r.error);
            return std::move(r.mesh);
        },
        "Field-aligned quad remesh following curvature and sharp edges.");

    meshRebuild(
        "VoxelRemesh", "Voxel Remesh",
        {bounded(in("resolution", T::Int, 128, "Voxels along the longest side."), 4, 2048), in("project", T::Bool, true, "Pull the result onto the input surface.")}, {},
        [](const geo::Mesh& m, ExecContext& c) {
            retopo::VoxelOptions o;
            o.resolution = clampInt(c.i("resolution"), 4, 2048);
            o.project = c.b("project");
            return retopo::voxelRemesh(m, o);
        },
        "Rebuilds the mesh as one closed quad surface, closing holes and merging overlaps.");

    meshOp(
        "SmoothMesh", "Smooth Mesh", Domain::Point,
        {bounded(in("iterations", T::Int, 5), 0, 1000), bounded(in("strength", T::Float, 0.5), 0.0, 1.0), in("pinBorders", T::Bool, true)},
        {prop("mode", {"relax", "laplacian"}, "relax", "Relax evens the points out on the surface; laplacian smooths the shape, which shrinks it.")}, {},
        [](geo::Mesh& m, std::vector<Index>& sel, ExecContext& c) {
            retopo::RelaxOptions o;
            o.iterations = clampInt(c.i("iterations"), 0, 1000);
            o.strength = std::clamp(c.f("strength"), 0.0, 1.0);
            o.pinBoundary = c.b("pinBorders");
            o.active = pointMask(m, sel);
            if (c.prop("mode") == "laplacian")
            {
                o.tangential = false;
                o.project = false;
                o.sharpAngle = -1.0;
                o.cornerAngle = -1.0;
                o.hardEdgesAreCreases = false;
            }
            retopo::relaxMesh(m, nullptr, o);
            m.cornerAttrs().remove(geo::attr::kNormal);
        },
        "Smooths the selected points.");

    registerDecl({
        .type = "Shrinkwrap",
        .label = "Shrinkwrap",
        .category = "Mesh",
        .doc = "Moves the selected points onto the target's surface.",
        .props = {prop("mode", {"nearestSurface", "normal", "direction", "nearestVertex"}, "nearestSurface")},
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), geoIn("target", "Mesh to wrap onto."), in("offset", T::Float, 0.0, "Distance to stay off the target, outward."),
                               in("direction", T::Vector, vec(0, -1, 0), "Projection direction for direction mode."),
                               in("maxDistance", T::Float, -1.0, "Points farther than this stay put; negative is unlimited.")},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr tg = c.geometry("target");
                Geometry real = tg->instances ? realize(*tg) : *tg;
                if (!real.mesh || real.mesh->numFaces() == 0)
                {
                    c.out("mesh", c.geometry("mesh"));
                    return;
                }
                const geo::Mesh& target = *real.mesh;
                geo::MeshBvh bvh(target);
                std::string mode = c.prop("mode");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<Index> sel = selected(c.field("selection"), FieldContext::forMesh(m, Domain::Point));
                                                                      if (mode == "nearestVertex")
                                                                      {
                                                                          geo::ShapeSelection shape;
                                                                          shape.loose = sel;
                                                                          geo::ConformOptions o;
                                                                          o.mode = geo::ConformMode::Vertex;
                                                                          for (const geo::PointMove& mv : geo::conformToMesh(m, shape, target, o))
                                                                              m.setPosition(mv.point, mv.position);
                                                                      }
                                                                      else
                                                                      {
                                                                          retopo::ProjectOptions o;
                                                                          o.method = mode == "normal"      ? retopo::ProjectMethod::Normal
                                                                                     : mode == "direction" ? retopo::ProjectMethod::Direction
                                                                                                           : retopo::ProjectMethod::Closest;
                                                                          o.direction = c.v("direction");
                                                                          o.offset = c.f("offset");
                                                                          o.maxDistance = c.f("maxDistance");
                                                                          o.active = pointMask(m, sel);
                                                                          retopo::projectMesh(m, target, bvh, o);
                                                                      }
                                                                      m.cornerAttrs().remove(geo::attr::kNormal);
                                                                  }}));
            },
    });

    meshRebuild("DualMesh", "Dual Mesh", {}, {}, [](const geo::Mesh& m, ExecContext&) { return dualMesh(m); },
                "Swaps faces and points: a point at each face centre, a face round each interior point. Open borders are dropped.");

    meshOp("SplitEdges", "Split Edges", Domain::Edge, {}, {}, {}, [](geo::Mesh& m, std::vector<Index>& sel, ExecContext&) { splitEdges(m, sel); },
           "Disconnects the faces on either side of the selected edges.");

    registerDecl({
        .type = "ScaleElements",
        .label = "Scale Elements",
        .category = "Mesh",
        .doc = "Scales each connected group of selected faces or edges about its centre.",
        .props = {prop("domain", {"face", "edge"}, "face"), prop("scaleMode", {"uniform", "singleAxis"}, "uniform")},
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), fin("scale", T::Float, 1.0),
                               fin("center", T::Vector, vec(0, 0, 0), Implicit::None, "Per-element centre, averaged per group. Unlinked: the elements' own centres."),
                               in("axis", T::Vector, vec(1, 0, 0), "Direction for single-axis scaling.")},
                    .outputs = {geoOut("mesh")}},
        .exec = [](ExecContext& c) { c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) { scaleElements(m, c); }})); },
    });

    meshRebuild(
        "SubdivideMesh", "Subdivide Mesh", {bounded(in("level", T::Int, 1), 0, 6)}, {},
        [](const geo::Mesh& m, ExecContext& c) {
            geo::Mesh out = m;
            for (int k = clampInt(c.i("level"), 0, 6); k > 0; --k)
                out = subdivideLinear(out);
            return out;
        },
        "Splits every face into quads without smoothing. Uvs and colours are interpolated.");

    registerDecl({
        .type = "GridFill",
        .label = "Grid Fill",
        .category = "Mesh",
        .doc = "Fills holes, or replaces disc patches of faces such as a pole's fan, with a quad grid inside a ring of quads.",
        .props = {prop("mode", {"holes", "faces"}, "holes", "Holes bordered by the selected edges, or patches of selected faces."),
                  prop("style", {"grid", "radial"}, "grid", "One ring round a grid, or rings following the border inward to a pole of kite quads.")},
        .sockets = {.inputs = {geoIn("mesh"), selectionIn(), bounded(in("gridSize", T::Float, 0.4, "The grid's size against the hole. With radial, the pole's size: smaller adds rings."), 0.0, 1.0),
                               in("rotate", T::Int, 0, "Steps the grid's corners round the border.")},
                    .outputs = {geoOut("mesh"), fout("fill", T::Bool, "The new faces.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                bool faces = c.prop("mode") == "faces";
                std::string name = c.anonymous("fill");
                c.out("mesh", editComponents(c.geometry("mesh"), {.mesh = [&](geo::Mesh& m) {
                                                                      std::vector<Index> sel = selected(c.field("selection"), FieldContext::forMesh(m, faces ? Domain::Face : Domain::Edge));
                                                                      geo::GridFillOptions o;
                                                                      o.style = c.prop("style") == "radial" ? geo::GridFillOptions::Style::Radial : geo::GridFillOptions::Style::Grid;
                                                                      o.gridSize = std::clamp(c.f("gridSize"), 0.0, 1.0);
                                                                      o.rotate = int(c.i("rotate"));
                                                                      markFaces(m, name, faces ? geo::gridFillFaces(m, sel, o) : geo::gridFillHoles(m, sel, o));
                                                                  }}));
                c.out("fill", attributeField(name, T::Bool));
            },
    });
}

void shapeNodes()
{
    registerDecl({
        .type = "ConvexHull",
        .label = "Convex Hull",
        .category = "Geometry",
        .doc = "The smallest convex mesh round every point of the geometry.",
        .sockets = {.inputs = {geoIn(), bounded(in("maxPoints", T::Int, 0, "Above this many points the hull is built from extreme points only; 0 uses all."), 0, INT_MAX)},
                    .outputs = {geoOut("convexHull")}},
        .exec =
            [](ExecContext& c) {
                std::vector<D3> pts = allPositions(*c.geometry("geometry"));
                int maxPoints = int(c.i("maxPoints"));
                c.out("convexHull", pts.size() < 4 ? makeGeometry({}) : meshGeometry(geo::convexHullMesh(pts, maxPoints > 0 ? maxPoints : INT_MAX)));
            },
    });

    registerDecl({
        .type = "BoundingShape",
        .label = "Bounding Shape",
        .category = "Geometry",
        .doc = "A fitted shape containing every point: axis box, minimum oriented box, sphere, cylinder, capsule or convex hull.",
        .props = {prop("shape", {"box", "orientedBox", "sphere", "cylinder", "capsule", "hull"}, "orientedBox")},
        .sockets = {.inputs = {geoIn(), bounded(in("segments", T::Int, 16, "Sides of round shapes."), 3, 256)},
                    .outputs = {geoOut("mesh"), out("center", T::Vector), out("size", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                std::vector<D3> pts = allPositions(*c.geometry("geometry"));
                geo::Mesh m = boundingShape(pts, c.prop("shape"), clampInt(c.i("segments"), 3, 256));
                geo::Bounds b = m.bounds();
                c.out("center", constantVector(b.valid() ? b.center() : D3(0.0)));
                c.out("size", constantVector(b.size()));
                c.out("mesh", m.empty() ? makeGeometry({}) : meshGeometry(std::move(m)));
            },
    });
}

void deformers()
{
    deformerNode(
        "Bend", "Bend", {in("angle", T::Float, 90.0, "Degrees bent across the range.")},
        [](ExecContext& c) -> PointMap { return [o = deform::BendParams{c.f("angle"), c.f("low"), c.f("high")}](const D3& p) { return deform::bend(p, o); }; },
        "Bends geometry round an arc across a range of the axis; past the range it carries on straight.");
    deformerNode(
        "Twist", "Twist", {in("startAngle", T::Float, 0.0, "Degrees at the low end."), in("endAngle", T::Float, 90.0, "Degrees at the high end.")},
        [](ExecContext& c) -> PointMap {
            return [o = deform::TwistParams{c.f("startAngle"), c.f("endAngle"), c.f("low"), c.f("high")}](const D3& p) { return deform::twist(p, o); };
        },
        "Twists geometry about the axis, the angle blending from low to high.");
    deformerNode(
        "Taper", "Taper",
        {in("startScale", T::Float, 1.0), in("endScale", T::Float, 0.5), in("curve", T::Float, 0.0, "Bulge added in the middle, in scale units.")},
        [](ExecContext& c) -> PointMap {
            return [o = deform::TaperParams{c.f("startScale"), c.f("endScale"), c.f("curve"), c.f("low"), c.f("high")}](const D3& p) { return deform::taper(p, o); };
        },
        "Scales geometry across the axis, blending from the start scale to the end scale.");
}
} // namespace meshops

void registerMeshOpsNodes()
{
    meshops::topologyOps();
    meshops::shapeNodes();
    meshops::deformers();
}
} // namespace lila::gn::nodes
