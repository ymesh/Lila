#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Triangulate.h"



namespace lila::gn::nodes::curvesurface
{
/** A mesh's triangles laid out in a uv map, for finding what lies under a uv. */
struct UvLookup
{
    std::vector<Index> corners; // 3 mesh corners per triangle
    geo::MeshBvh tree;
};

std::shared_ptr<const UvLookup> uvLookup(const geo::Mesh& m, const std::vector<D3>& cornerUv)
{
    auto l = std::make_shared<UvLookup>();
    geo::Triangulation tris = geo::triangulate(m);
    l->corners = std::move(tris.corners);
    std::vector<glm::vec3> soup(l->corners.size());
    for (size_t k = 0; k < soup.size(); ++k)
        soup[k] = glm::vec3(float(cornerUv[l->corners[k]].x), float(cornerUv[l->corners[k]].y), 0.0f);
    if (!soup.empty())
        l->tree.buildSoup(soup);
    return l;
}

/** Weights of a triangle's three corners at a uv, or none when no triangle covers it. */
struct UvHit
{
    bool valid = false;
    Index tri = 0;
    D3 bary{0.0};
};

UvHit findUv(const UvLookup& l, const D3& uv)
{
    UvHit h;
    if (l.tree.empty())
        return h;
    geo::BvhClosest c;
    if (!l.tree.closestPoint(D3(uv.x, uv.y, 0.0), c, 1e-4))
        return h;
    h.valid = true;
    h.tri = c.triangle;
    h.bary = c.bary;
    return h;
}

template <class V> V blend(const std::vector<V>& v, const Index* c, const D3& w)
{
    if constexpr (std::is_same_v<V, int64_t> || std::is_same_v<V, uint8_t>)
    {
        int best = w.x >= w.y && w.x >= w.z ? 0 : (w.y >= w.z ? 1 : 2);
        return v[c[best]];
    }
    else if constexpr (std::is_same_v<V, glm::dmat4>)
        return v[c[0]];
    else
        return v[c[0]] * w.x + v[c[1]] * w.y + v[c[2]] * w.z;
}

class UvSampleField final : public Field
{
public:
    UvSampleField(SocketType t, std::shared_ptr<const UvLookup> lookup, Column cornerValues, FieldPtr uv, bool valid)
        : Field(valid ? SocketType::Bool : t), mLookup(std::move(lookup)), mValues(std::move(cornerValues)), mUv(std::move(uv)), mValid(valid)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        std::vector<D3> uv = evaluateAll<D3>(mUv, ctx);
        std::vector<UvHit> hits(uv.size());
        for (size_t k = 0; k < uv.size(); ++k)
            hits[k] = findUv(*mLookup, uv[k]);
        if (mValid)
        {
            std::vector<uint8_t> ok(hits.size());
            for (size_t k = 0; k < hits.size(); ++k)
                ok[k] = hits[k].valid;
            return Column::of(std::move(ok));
        }
        return std::visit(
            [&](const auto& vals) -> Column {
                using V = typename std::decay_t<decltype(vals)>::value_type;
                std::vector<V> out(hits.size(), V(0));
                for (size_t k = 0; k < hits.size(); ++k)
                    if (hits[k].valid)
                        out[k] = blend(vals, &mLookup->corners[size_t(hits[k].tri) * 3], hits[k].bary);
                return Column::of(std::move(out));
            },
            mValues.data);
    }

private:
    std::shared_ptr<const UvLookup> mLookup;
    Column mValues;
    FieldPtr mUv;
    bool mValid;
};

void sampleUvSurface()
{
    registerDecl({
        .type = "SampleUVSurface",
        .label = "Sample UV Surface",
        .category = "Geometry",
        .doc = "Reads a mesh's value at a uv: the triangle under the uv in the source uv map (uv0 when unlinked), blended across it. "
               "Integers and booleans take the nearest corner. Is valid is false where no face covers the uv.",
        .props = {prop("dataType", dataTypes(), "float")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("SampleUVSurface")->propValue(n, "dataType"));
                return Sockets{{geoIn("mesh"), fin("value", t, zeroJson(t), Implicit::None, "Evaluated on the mesh's corners."),
                                fin("sourceUvMap", T::Vector, vec(0, 0, 0), Implicit::None, "The mesh's uvs, on its corners."),
                                fin("sampleUv", T::Vector, vec(0, 0, 0), Implicit::None, "Where to read, on the geometry being evaluated.")},
                               {fout("value", t), fout("isValid", T::Bool)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                GeometryPtr g = c.geometry("mesh");
                if (!g->mesh || g->mesh->numFaces() == 0)
                {
                    c.out("value", constantField(t, convert(Column::of(0.0), t)));
                    c.out("isValid", constantBool(false));
                    return;
                }
                const geo::Mesh& m = *g->mesh;
                FieldContext corners = FieldContext::forMesh(m, Domain::Corner);
                const bool linked = !c.group.linksInto(c.node.name, "sourceUvMap").empty() || c.node.values.contains("sourceUvMap");
                FieldPtr uvSource = linked ? c.field("sourceUvMap") : attributeField(geo::attr::uvName(0), T::Vector);
                std::shared_ptr<const UvLookup> lookup = uvLookup(m, evaluateAll<D3>(uvSource, corners));
                Column values = evaluate(c.field("value"), corners, t);
                values = std::visit([&](const auto& v) -> Column { return Column::of(expand<typename std::decay_t<decltype(v)>::value_type>(values, corners.size)); }, values.data);
                c.out("value", std::make_shared<UvSampleField>(t, lookup, values, c.field("sampleUv"), false));
                c.out("isValid", std::make_shared<UvSampleField>(t, lookup, Column{}, c.field("sampleUv"), true));
            },
    });
}

/** An orthonormal frame on a triangle at a point of it: origin, then the edge direction, normal and their cross. */
glm::dmat4 triangleFrame(const D3& a, const D3& b, const D3& cc, const D3& bary)
{
    D3 o = a * bary.x + b * bary.y + cc * bary.z;
    D3 x = b - a;
    D3 n = glm::cross(b - a, cc - a);
    if (glm::length(x) < 1e-12 || glm::length(n) < 1e-12)
    {
        glm::dmat4 m(1.0);
        m[3] = glm::dvec4(o, 1.0);
        return m;
    }
    x = glm::normalize(x);
    n = glm::normalize(n);
    D3 z = glm::cross(x, n);
    return glm::dmat4(glm::dvec4(x, 0.0), glm::dvec4(n, 0.0), glm::dvec4(z, 0.0), glm::dvec4(o, 1.0));
}

void deformCurvesOnSurface()
{
    registerDecl({
        .type = "DeformCurvesOnSurface",
        .label = "Deform Curves on Surface",
        .category = "Curve",
        .doc = "Moves each curve with the surface under its first point, as hair follows a deforming head. Where the root sits is found on "
               "the rest surface (the rest input, or the surface's \"rest_position\" point attribute when unlinked); the curve then turns "
               "and moves with that triangle on the surface. Rest and surface must share their topology.",
        .sockets = {.inputs = {geoIn("curves"), geoIn("surface", "The deformed mesh."), geoIn("rest", "The mesh at rest.")}, .outputs = {geoOut("curves")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr surf = c.geometry("surface"), rest = c.geometry("rest");
                GeometryPtr curves = c.geometry("curves");
                if (!curves->curves || !surf->mesh || surf->mesh->numFaces() == 0)
                {
                    c.out("curves", curves);
                    return;
                }
                const geo::Mesh& deformed = *surf->mesh;
                geo::Mesh restMesh;
                if (rest->mesh)
                {
                    restMesh = *rest->mesh;
                    if (restMesh.numPoints() != deformed.numPoints() || restMesh.numFaces() != deformed.numFaces())
                        throw ExecError(c.node.name + ": rest and surface have different topology");
                }
                else
                {
                    std::span<const glm::vec3> rp = deformed.pointAttrs().get<glm::vec3>("rest_position");
                    if (rp.empty())
                        throw ExecError(c.node.name + ": link a rest mesh or store rest_position on the surface");
                    restMesh = deformed;
                    for (Index p = 0; p < restMesh.numPoints(); ++p)
                        restMesh.setPosition(p, D3(rp[p]));
                }
                geo::MeshBvh tree(restMesh);
                const geo::Triangulation& tris = tree.triangles();
                c.out("curves", editComponents(curves, {.curves = [&](CurveSet& cs) {
                                                            std::vector<glm::dmat4> xf(cs.numCurves(), glm::dmat4(1.0));
                                                            for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                            {
                                                                if (cs.curveSize(cu) == 0)
                                                                    continue;
                                                                geo::BvhClosest hit;
                                                                if (!tree.closestPoint(cs.positions[cs.offsets[cu]], hit))
                                                                    continue;
                                                                const Index* cn = &tris.corners[size_t(hit.triangle) * 3];
                                                                Index p[3] = {restMesh.cornerPoint(cn[0]), restMesh.cornerPoint(cn[1]), restMesh.cornerPoint(cn[2])};
                                                                glm::dmat4 from = triangleFrame(restMesh.position(p[0]), restMesh.position(p[1]), restMesh.position(p[2]), hit.bary);
                                                                glm::dmat4 to = triangleFrame(deformed.position(p[0]), deformed.position(p[1]), deformed.position(p[2]), hit.bary);
                                                                xf[cu] = to * glm::inverse(from);
                                                            }
                                                            std::span<glm::vec3> hl = cs.pointAttrs.get<glm::vec3>("handle_left");
                                                            std::span<glm::vec3> hr = cs.pointAttrs.get<glm::vec3>("handle_right");
                                                            for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                for (Index q = cs.offsets[cu]; q < cs.offsets[cu + 1]; ++q)
                                                                {
                                                                    cs.positions[q] = D3(xf[cu] * glm::dvec4(cs.positions[q], 1.0));
                                                                    if (!hl.empty())
                                                                        hl[q] = glm::vec3(xf[cu] * glm::dvec4(D3(hl[q]), 1.0));
                                                                    if (!hr.empty())
                                                                        hr[q] = glm::vec3(xf[cu] * glm::dvec4(D3(hr[q]), 1.0));
                                                                }
                                                        }}));
            },
    });
}
} // namespace lila::gn::nodes::curvesurface

namespace lila::gn::nodes
{
void registerCurveSurfaceNodes()
{
    curvesurface::sampleUvSurface();
    curvesurface::deformCurvesOnSurface();
}
} // namespace lila::gn::nodes
