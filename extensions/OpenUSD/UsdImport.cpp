#include "UsdImport.h"

#include "UsdAttributes.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/LightingChannels.h>
#include <lila/Lights.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Rig.h>
#include <lila/Tasks.h>

#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/collectionMembershipQuery.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/nurbsCurves.h>
#include <pxr/usd/usdGeom/plane.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>
#include <pxr/usd/usdSkel/skeleton.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <lila/Command.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
using geo::Index;
using geo::kInvalid;
using scene::NodeId;
using scene::kNoNode;

namespace
{
const Usd_PrimFlagsPredicate kChildren = UsdTraverseInstanceProxies(UsdPrimIsActive && UsdPrimIsDefined && !UsdPrimIsAbstract);

bool isShading(const UsdPrim& p) { return p.IsA<UsdShadeMaterial>() || p.IsA<UsdShadeShader>() || p.IsA<UsdShadeNodeGraph>(); }

/** A prim holding only materials (a "Looks" or "Materials" scope): nothing to build. */
bool onlyShading(const UsdPrim& p)
{
    if (isShading(p))
        return true;
    if (!p.IsA<UsdGeomScope>() && p.GetTypeName() != TfToken())
        return false;
    bool any = false;
    for (const UsdPrim& c : p.GetFilteredChildren(kChildren))
    {
        if (!onlyShading(c))
            return false;
        any = true;
    }
    return any;
}

template <class T> T valueOr(const UsdAttribute& a, UsdTimeCode t, T fallback)
{
    T v;
    return a && a.Get(&v, t) ? v : fallback;
}

/** Flattened primvar values cast to T, or empty. */
template <class T> VtArray<T> flattened(const UsdGeomPrimvar& pv, UsdTimeCode t)
{
    VtValue v;
    if (!pv.ComputeFlattened(&v, t))
        return {};
    if (v.IsHolding<VtArray<T>>())
        return v.UncheckedGet<VtArray<T>>();
    if (v.CanCast<VtArray<T>>())
        return v.Cast<VtArray<T>>().template UncheckedGet<VtArray<T>>();
    return {};
}

/** Which of a primvar's values a face vertex reads. */
struct Interp
{
    TfToken how;
    size_t operator()(size_t face, size_t faceVertex, size_t point) const
    {
        if (how == UsdGeomTokens->faceVarying)
            return faceVertex;
        if (how == UsdGeomTokens->uniform)
            return face;
        if (how == UsdGeomTokens->constant)
            return 0;
        return point; // vertex and varying
    }
    size_t needed(size_t faces, size_t faceVertices, size_t points) const
    {
        if (how == UsdGeomTokens->faceVarying)
            return faceVertices;
        if (how == UsdGeomTokens->uniform)
            return faces;
        if (how == UsdGeomTokens->constant)
            return 1;
        return points;
    }
};

struct MeshBuild
{
    geo::Mesh mesh;
    std::vector<uint32_t> pointOf; // USD point -> Lila point
    std::vector<std::string> slots;
    size_t badFaces = 0, holes = 0;
};

/** Hard where the corner normals of an edge's two faces disagree. */
void hardEdgesFromNormals(geo::Mesh& m)
{
    std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e))
            continue;
        Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
        auto at = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
        for (int i = 0; i < 2 && !hard[e]; ++i)
        {
            Index p = m.edgePoint(e, i);
            hard[e] = glm::dot(normals[at(c0, p)], normals[at(c1, p)]) < 0.9999f;
        }
    }
}

/** The order UV primvars become Lila sets: st, st1, st2, ... at their own index, any others after, by name. */
std::vector<std::pair<int, UsdGeomPrimvar>> uvSets(const std::vector<UsdGeomPrimvar>& primvars)
{
    std::vector<std::pair<int, UsdGeomPrimvar>> out;
    std::vector<UsdGeomPrimvar> others;
    std::set<int> used;
    for (const UsdGeomPrimvar& pv : primvars)
    {
        const SdfValueTypeName type = pv.GetTypeName();
        const bool twoFloats = type == SdfValueTypeNames->TexCoord2fArray || type == SdfValueTypeNames->Float2Array ||
                               type == SdfValueTypeNames->TexCoord2dArray || type == SdfValueTypeNames->Double2Array ||
                               type == SdfValueTypeNames->TexCoord2hArray || type == SdfValueTypeNames->Half2Array;
        if (!twoFloats)
            continue;
        const std::string name = pv.GetPrimvarName().GetString();
        const bool texcoord = type.GetRole() == SdfValueRoleNames->TextureCoordinate;
        const int set = uvSetOfPrimvar(name);
        if (set >= 0 && set < 8 && used.insert(set).second)
            out.push_back({set, pv});
        else if (texcoord || name.starts_with("uv") || name.starts_with("UV") || name == "map1")
            others.push_back(pv);
    }
    std::sort(others.begin(), others.end(), [](const UsdGeomPrimvar& a, const UsdGeomPrimvar& b) { return a.GetPrimvarName() < b.GetPrimvarName(); });
    int next = 0;
    for (const UsdGeomPrimvar& pv : others)
    {
        while (used.count(next))
            ++next;
        if (next >= 8)
            break;
        used.insert(next);
        out.push_back({next, pv});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

bool isUvPrimvar(const std::vector<std::pair<int, UsdGeomPrimvar>>& uvs, const UsdGeomPrimvar& pv)
{
    return std::any_of(uvs.begin(), uvs.end(), [&](const auto& u) { return u.second.GetName() == pv.GetName(); });
}

/** Builds a Lila mesh from a UsdGeomMesh at time t: points, faces (n-gons kept), normals, primvars, creases and material slots. */
MeshBuild convertMesh(Importer& im, const UsdGeomMesh& usdMesh, UsdTimeCode t)
{
    LILA_ZONE();

    MeshBuild b;
    geo::Mesh& m = b.mesh;
    const UsdPrim prim = usdMesh.GetPrim();
    const std::string where = prim.GetPath().GetString();
    VtVec3fArray points;
    VtIntArray counts, indices, holes;
    usdMesh.GetPointsAttr().Get(&points, t);
    usdMesh.GetFaceVertexCountsAttr().Get(&counts, t);
    usdMesh.GetFaceVertexIndicesAttr().Get(&indices, t);
    usdMesh.GetHoleIndicesAttr().Get(&holes, t);
    const bool leftHanded = valueOr<TfToken>(usdMesh.GetOrientationAttr(), t, UsdGeomTokens->rightHanded) == UsdGeomTokens->leftHanded;
    const bool flip = leftHanded != (glm::determinant(im.axes.basis) < 0.0);
    const std::set<int> holeSet(holes.begin(), holes.end());

    for (const GfVec3f& p : points)
        m.addPoint(im.axes.pointToLila(glm::dvec3(p[0], p[1], p[2])));

    std::vector<Index> cornerOf(indices.size(), kInvalid);
    std::vector<uint32_t> sourceFace;
    std::vector<Index> pts;
    std::vector<size_t> idx;
    size_t start = 0;
    for (size_t f = 0; f < counts.size(); ++f)
    {
        const size_t n = size_t(std::max(0, counts[f]));
        if (start + n > indices.size())
        {
            im.r.warnings.push_back(std::format("{}: faceVertexIndices is shorter than its counts; the rest is skipped", where));
            break;
        }
        if (holeSet.count(int(f)))
        {
            ++b.holes;
            start += n;
            continue;
        }
        pts.clear();
        idx.clear();
        bool valid = n >= 3;
        for (size_t k = 0; k < n && valid; ++k)
        {
            const size_t i = start + (flip ? n - 1 - k : k);
            valid = indices[i] >= 0 && size_t(indices[i]) < points.size();
            idx.push_back(i);
            pts.push_back(Index(indices[i]));
        }
        const Index lf = valid ? m.addFace(pts) : kInvalid;
        start += n;
        if (lf == kInvalid)
        {
            ++b.badFaces;
            continue;
        }
        const Index first = m.faceCorner(lf);
        for (size_t k = 0; k < idx.size(); ++k)
            cornerOf[idx[k]] = first + Index(k);
        sourceFace.push_back(uint32_t(f));
    }

    // Face vertex i of USD face f: corner cornerOf[i] of Lila face lf.
    auto eachCorner = [&](auto&& fn) {
        size_t s = 0;
        for (size_t f = 0; f < counts.size(); ++f)
        {
            const size_t n = size_t(std::max(0, counts[f]));
            for (size_t k = 0; k < n && s + k < indices.size(); ++k)
                if (cornerOf[s + k] != kInvalid)
                    fn(f, s + k, size_t(indices[s + k]), cornerOf[s + k]);
            s += n;
        }
    };

    UsdGeomPrimvarsAPI pvApi(prim);
    std::vector<UsdGeomPrimvar> primvars = pvApi.GetPrimvarsWithValues();

    // Normals: primvars:normals wins over the normals attribute.
    VtVec3fArray normals;
    Interp nInterp{usdMesh.GetNormalsInterpolation()};
    if (UsdGeomPrimvar pn = pvApi.GetPrimvar(UsdGeomTokens->normals); pn && pn.HasValue())
    {
        normals = flattened<GfVec3f>(pn, t);
        nInterp.how = pn.GetInterpolation();
    }
    else
    {
        usdMesh.GetNormalsAttr().Get(&normals, t);
    }
    if (!normals.empty())
    {
        if (normals.size() < nInterp.needed(counts.size(), indices.size(), points.size()))
            im.r.warnings.push_back(std::format("{}: normals don't match the mesh ({} for {} interpolation); computed instead", where,
                                                normals.size(), nInterp.how.GetString()));
        else
        {
            const glm::dmat3 normalMat = glm::inverse(glm::transpose(glm::dmat3(im.axes.toLila)));
            std::span<glm::vec3> out = m.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
            eachCorner([&](size_t f, size_t fv, size_t p, Index c) {
                const GfVec3f& n = normals[nInterp(f, fv, p)];
                glm::dvec3 v = normalMat * glm::dvec3(n[0], n[1], n[2]);
                const double len = glm::length(v);
                out[c] = glm::vec3(len > 0.0 ? v / len : v);
            });
        }
    }

    // UV sets.
    const auto uvs = uvSets(primvars);
    for (const auto& [set, pv] : uvs)
    {
        VtVec2fArray values = flattened<GfVec2f>(pv, t);
        if (values.empty())
            if (VtVec2dArray d = flattened<GfVec2d>(pv, t); !d.empty())
                for (const GfVec2d& v : d)
                    values.push_back(GfVec2f(float(v[0]), float(v[1])));
        const Interp in{pv.GetInterpolation()};
        if (values.size() < in.needed(counts.size(), indices.size(), points.size()))
        {
            im.r.warnings.push_back(std::format("{}: UV set {} has too few values; skipped", where, pv.GetPrimvarName().GetString()));
            continue;
        }
        std::span<glm::vec2> out = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(set));
        eachCorner([&](size_t f, size_t fv, size_t p, Index c) {
            const GfVec2f& v = values[in(f, fv, p)];
            out[c] = glm::vec2(v[0], v[1]);
        });
    }

    // Colors: displayColor and displayOpacity as one set, and any other color primvar as its own.
    auto colorSet = [&](const std::string& name, const UsdGeomPrimvar& rgb, const UsdGeomPrimvar& alpha) {
        VtVec4fArray rgba = rgb ? flattened<GfVec4f>(rgb, t) : VtVec4fArray();
        VtVec3fArray c3 = rgb && rgba.empty() ? flattened<GfVec3f>(rgb, t) : VtVec3fArray();
        VtFloatArray a = alpha ? flattened<float>(alpha, t) : VtFloatArray();
        const Interp ci{rgb ? rgb.GetInterpolation() : UsdGeomTokens->constant};
        const Interp ai{alpha ? alpha.GetInterpolation() : UsdGeomTokens->constant};
        const size_t need = ci.needed(counts.size(), indices.size(), points.size());
        if (rgb && rgba.size() < need && c3.size() < need)
        {
            im.r.warnings.push_back(std::format("{}: color {} has too few values; skipped", where, name));
            return;
        }
        if (!a.empty() && a.size() < ai.needed(counts.size(), indices.size(), points.size()))
            a.clear();
        std::span<glm::vec4> out = geo::colors::add(m, geo::colors::uniqueName(m, name));
        eachCorner([&](size_t f, size_t fv, size_t p, Index c) {
            glm::vec4 v(1.0f);
            if (!rgba.empty())
            {
                const GfVec4f& x = rgba[ci(f, fv, p)];
                v = glm::vec4(x[0], x[1], x[2], x[3]);
            }
            else if (!c3.empty())
            {
                const GfVec3f& x = c3[ci(f, fv, p)];
                v = glm::vec4(x[0], x[1], x[2], 1.0f);
            }
            if (!a.empty())
                v.a = a[ai(f, fv, p)];
            out[c] = v;
        });
    };
    UsdGeomPrimvar displayColor = pvApi.GetPrimvar(TfToken("displayColor"));
    UsdGeomPrimvar displayOpacity = pvApi.GetPrimvar(TfToken("displayOpacity"));
    const bool hasDisplay = (displayColor && displayColor.HasValue()) || (displayOpacity && displayOpacity.HasValue());
    if (hasDisplay)
    {
        std::string name = "displayColor";
        const VtDictionary d = lilaData(displayColor ? displayColor.GetAttr() : displayOpacity.GetAttr());
        if (auto it = d.find("colorSet"); it != d.end() && it->second.IsHolding<std::string>())
            name = it->second.UncheckedGet<std::string>();
        colorSet(name, displayColor && displayColor.HasValue() ? displayColor : UsdGeomPrimvar(),
                 displayOpacity && displayOpacity.HasValue() ? displayOpacity : UsdGeomPrimvar());
        if (name != "displayColor")
            geo::colors::setCurrent(m, name);
    }

    // Everything else: color primvars as color sets, other numeric ones as mesh attributes, the rest reported.
    for (const UsdGeomPrimvar& pv : primvars)
    {
        const std::string name = pv.GetPrimvarName().GetString();
        if (name == "displayColor" || name == "displayOpacity" || name == "normals" || name.starts_with("skel:") || isUvPrimvar(uvs, pv))
            continue;
        const SdfValueTypeName type = pv.GetTypeName();
        const TfToken interp = pv.GetInterpolation();
        const SdfTupleDimensions dims = type.GetDimensions();
        const size_t components = dims.size == 0 ? 1 : dims.size == 1 ? dims.d[0] : 0;
        if (type.GetRole() == SdfValueRoleNames->Color && (components == 3 || components == 4) && type.IsArray())
        {
            if (interp == UsdGeomTokens->constant)
            {
                im.r.skipped.push_back(std::format("{}: constant color primvar {}", where, name));
                continue;
            }
            colorSet(identifier(name, "colorSet"), pv, UsdGeomPrimvar());
            continue;
        }
        if (interp == UsdGeomTokens->constant || !type.IsArray())
        {
            im.r.skipped.push_back(std::format("{}: constant primvar {} ({})", where, name, type.GetAsToken().GetString()));
            continue;
        }
        geo::Domain domain = interp == UsdGeomTokens->faceVarying ? geo::Domain::Corner
                             : interp == UsdGeomTokens->uniform    ? geo::Domain::Face
                                                                   : geo::Domain::Point;
        std::string attrName = identifier(name, "attr");
        static const std::set<std::string> kReserved{"normal", "tangent", "color", "hard", "seam", "crease", "bevelWeight", "material", "smoothGroup"};
        if (kReserved.count(attrName) || attrName.starts_with("uv"))
            attrName = "usd_" + attrName;
        const Interp in{interp};
        const size_t need = in.needed(counts.size(), indices.size(), points.size());
        geo::AttributeSet& set = m.attributes(domain);
        bool done = false;
        auto fill = [&](auto tag, geo::AttrType at, auto convert) {
            using Src = decltype(tag);
            VtArray<Src> values = flattened<Src>(pv, t);
            if (values.size() < need)
                return;
            auto& layer = set.add(attrName, at);
            using Dst = std::remove_cvref_t<decltype(convert(values[0]))>;
            std::span<Dst> out = layer.template values<Dst>();
            if (domain == geo::Domain::Corner)
                eachCorner([&](size_t f, size_t fv, size_t p, Index c) { out[c] = convert(values[in(f, fv, p)]); });
            else if (domain == geo::Domain::Face)
                for (size_t lf = 0; lf < sourceFace.size(); ++lf)
                    out[lf] = convert(values[sourceFace[lf]]);
            else
                for (size_t p = 0; p < points.size() && p < out.size(); ++p)
                    out[p] = convert(values[p]);
            done = true;
        };
        const auto scalar = type.GetScalarType();
        if (scalar == SdfValueTypeNames->Float || scalar == SdfValueTypeNames->Double || scalar == SdfValueTypeNames->Half)
            fill(float(), geo::AttrType::Float, [](float v) { return v; });
        else if (scalar == SdfValueTypeNames->Int)
            fill(int(), geo::AttrType::Int, [](int v) { return int32_t(v); });
        else if (scalar == SdfValueTypeNames->Bool)
            fill(bool(), geo::AttrType::Bool, [](bool v) { return v; });
        else if (components == 2)
            fill(GfVec2f(), geo::AttrType::Float2, [](const GfVec2f& v) { return glm::vec2(v[0], v[1]); });
        else if (components == 3)
            fill(GfVec3f(), geo::AttrType::Float3, [](const GfVec3f& v) { return glm::vec3(v[0], v[1], v[2]); });
        else if (components == 4)
            fill(GfVec4f(), geo::AttrType::Float4, [](const GfVec4f& v) { return glm::vec4(v[0], v[1], v[2], v[3]); });
        if (!done)
            im.r.skipped.push_back(std::format("{}: primvar {} ({}, {})", where, name, type.GetAsToken().GetString(), interp.GetString()));
    }

    // Creases: chains of points, a sharpness per chain or per edge. Lila keeps edge creases; corners are reported.
    VtIntArray creaseIdx, creaseLen;
    VtFloatArray creaseSharp;
    usdMesh.GetCreaseIndicesAttr().Get(&creaseIdx, t);
    usdMesh.GetCreaseLengthsAttr().Get(&creaseLen, t);
    usdMesh.GetCreaseSharpnessesAttr().Get(&creaseSharp, t);
    if (!creaseIdx.empty() && !creaseLen.empty())
    {
        size_t edges = 0;
        for (int l : creaseLen)
            edges += size_t(std::max(0, l - 1));
        const bool perEdge = creaseSharp.size() == edges && edges != creaseLen.size();
        std::span<float> crease = m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
        size_t at = 0, edge = 0;
        for (size_t c = 0; c < creaseLen.size(); ++c)
        {
            const int len = creaseLen[c];
            for (int k = 0; k + 1 < len && at + size_t(k) + 1 < creaseIdx.size(); ++k, ++edge)
            {
                const float s = perEdge ? (edge < creaseSharp.size() ? creaseSharp[edge] : 0.0f) : (c < creaseSharp.size() ? creaseSharp[c] : 0.0f);
                const int a = creaseIdx[at + size_t(k)], bb = creaseIdx[at + size_t(k) + 1];
                if (a < 0 || bb < 0 || size_t(a) >= points.size() || size_t(bb) >= points.size())
                    continue;
                if (Index e = m.findEdge(Index(a), Index(bb)); e != kInvalid)
                    crease[e] = std::max(0.0f, s);
            }
            at += size_t(std::max(0, len));
        }
    }
    if (VtIntArray corners; usdMesh.GetCornerIndicesAttr().Get(&corners, t) && !corners.empty())
        im.r.warnings.push_back(std::format("{}: {} corner crease(s) not kept; Lila creases edges only", where, corners.size()));

    // Material slots: the mesh's own binding, then a slot per materialBind subset.
    if (im.materials)
    {
        std::vector<int32_t> faceSlot(sourceFace.size(), -1);
        auto slotIndex = [&](const std::string& name) {
            auto it = std::find(b.slots.begin(), b.slots.end(), name);
            if (it != b.slots.end())
                return int32_t(it - b.slots.begin());
            b.slots.push_back(name);
            return int32_t(b.slots.size() - 1);
        };
        std::unordered_map<uint32_t, size_t> lilaFace;
        for (size_t lf = 0; lf < sourceFace.size(); ++lf)
            lilaFace[sourceFace[lf]] = lf;
        bool anySubset = false;
        for (const UsdGeomSubset& subset : UsdGeomSubset::GetAllGeomSubsets(UsdGeomImageable(prim)))
        {
            TfToken family, element;
            subset.GetFamilyNameAttr().Get(&family);
            subset.GetElementTypeAttr().Get(&element);
            if (family != UsdShadeTokens->materialBind)
            {
                im.r.skipped.push_back(std::format("{}: subset {} of family '{}'", where, subset.GetPrim().GetName().GetString(), family.GetString()));
                continue;
            }
            if (element != UsdGeomTokens->face)
                continue;
            UsdShadeMaterial bound = UsdShadeMaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial();
            std::string slot = bound ? im.materials->slot(bound) : identifier(subset.GetPrim().GetName().GetString(), "Material");
            const int32_t id = slotIndex(slot);
            VtIntArray faces;
            subset.GetIndicesAttr().Get(&faces, t);
            for (int f : faces)
                if (auto it = lilaFace.find(uint32_t(f)); it != lilaFace.end())
                    faceSlot[it->second] = id;
            anySubset = true;
        }
        UsdShadeMaterial own = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        const bool uncovered = std::any_of(faceSlot.begin(), faceSlot.end(), [](int32_t s) { return s < 0; });
        if (uncovered && (own || anySubset))
        {
            const int32_t id = slotIndex(own ? im.materials->slot(own) : std::string(scene::materials::kDefaultSlot));
            for (int32_t& s : faceSlot)
                if (s < 0)
                    s = id;
        }
        if (!b.slots.empty())
        {
            m.materialNames() = b.slots;
            std::span<int32_t> out = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
            for (size_t lf = 0; lf < faceSlot.size() && lf < out.size(); ++lf)
                out[lf] = std::max(0, faceSlot[lf]);
            if (valueOr<bool>(usdMesh.GetDoubleSidedAttr(), t, false))
                for (const std::string& s : b.slots)
                    im.materials->setTwoSided(s);
        }
    }

    if (m.cornerAttrs().has(geo::attr::kNormal))
        hardEdgesFromNormals(m);

    m.removeAllLoose();
    b.pointOf.resize(points.size());
    for (size_t p = 0; p < points.size(); ++p)
        b.pointOf[p] = uint32_t(p);
    if (m.hasDeleted())
    {
        geo::MeshRemap remap;
        m.compact(&remap);
        for (uint32_t& p : b.pointOf)
            p = p < remap.points.size() ? remap.points[p] : kInvalid;
    }
    return b;
}

/** USD's intrinsic shapes as Lila meshes, in the prim's own space. */
std::optional<geo::Mesh> tessellate(Importer& im, const UsdPrim& prim, UsdTimeCode t)
{
    auto axisOf = [&](const UsdAttribute& a) { return valueOr<TfToken>(a, t, UsdGeomTokens->z); };
    geo::Mesh m;
    TfToken axis = UsdGeomTokens->y;
    if (UsdGeomCube c(prim); c)
    {
        const double s = valueOr<double>(c.GetSizeAttr(), t, 2.0);
        m = geo::makeCube({.width = s, .height = s, .depth = s});
    }
    else if (UsdGeomSphere sp(prim); sp)
        m = geo::makeSphere({.radius = valueOr<double>(sp.GetRadiusAttr(), t, 1.0), .subdivAxis = 32, .subdivHeight = 16});
    else if (UsdGeomCylinder cy(prim); cy)
    {
        m = geo::makeCylinder({.radius = valueOr<double>(cy.GetRadiusAttr(), t, 1.0), .height = valueOr<double>(cy.GetHeightAttr(), t, 2.0), .subdivAxis = 32});
        axis = axisOf(cy.GetAxisAttr());
    }
    else if (UsdGeomCone co(prim); co)
    {
        m = geo::makeCone({.radius = valueOr<double>(co.GetRadiusAttr(), t, 1.0), .height = valueOr<double>(co.GetHeightAttr(), t, 2.0), .subdivAxis = 32});
        axis = axisOf(co.GetAxisAttr());
    }
    else if (UsdGeomCapsule ca(prim); ca)
    {
        const double r = valueOr<double>(ca.GetRadiusAttr(), t, 0.5);
        m = geo::makeCapsule({.radius = r, .height = valueOr<double>(ca.GetHeightAttr(), t, 1.0) + 2.0 * r, .subdivAxis = 32, .subdivCaps = 8});
        axis = axisOf(ca.GetAxisAttr());
    }
    else if (UsdGeomPlane pl(prim); pl)
    {
        // A USD plane lies across its axis; Lila's lies across Y.
        m = geo::makePlane({.width = valueOr<double>(pl.GetWidthAttr(), t, 2.0), .depth = valueOr<double>(pl.GetLengthAttr(), t, 2.0)});
        axis = axisOf(pl.GetAxisAttr());
    }
    else
        return std::nullopt;
    // Lila primitives are Y-up; turn Y onto the shape's axis, then carry the points into Lila space.
    glm::dmat3 turn(1.0);
    if (axis == UsdGeomTokens->x)
        turn = glm::dmat3(glm::dvec3(0, -1, 0), glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 1));
    else if (axis == UsdGeomTokens->z)
        turn = glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, -1, 0));
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, im.axes.pointToLila(turn * m.position(p)));
    if (m.cornerAttrs().has(geo::attr::kNormal))
        geo::updateCornerNormals(m);
    return m;
}

/** Splines of a BasisCurves or NurbsCurves prim, Lila space. */
curve::Splines convertCurves(Importer& im, const UsdPrim& prim, UsdTimeCode t)
{
    curve::Splines out;
    const std::string where = prim.GetPath().GetString();
    VtVec3fArray points;
    VtIntArray counts;
    auto lila = [&](const GfVec3f& p) { return im.axes.pointToLila(glm::dvec3(p[0], p[1], p[2])); };
    if (UsdGeomNurbsCurves nc(prim); nc)
    {
        VtIntArray order;
        VtDoubleArray knots, weights;
        nc.GetPointsAttr().Get(&points, t);
        nc.GetCurveVertexCountsAttr().Get(&counts, t);
        nc.GetOrderAttr().Get(&order, t);
        nc.GetKnotsAttr().Get(&knots, t);
        nc.GetPointWeightsAttr().Get(&weights, t);
        size_t p0 = 0, k0 = 0;
        for (size_t c = 0; c < counts.size(); ++c)
        {
            const size_t n = size_t(std::max(0, counts[c]));
            const int ord = c < order.size() ? order[c] : 4;
            curve::Spline s;
            s.kind = curve::Kind::Nurbs;
            s.degree = std::clamp(ord - 1, 1, 7);
            for (size_t i = 0; i < n && p0 + i < points.size(); ++i)
            {
                s.points.push_back(lila(points[p0 + i]));
                if (weights.size() == points.size())
                    s.weights.push_back(weights[p0 + i]);
            }
            for (size_t i = 0; i < n + size_t(ord) && k0 + i < knots.size(); ++i)
                s.knots.push_back(knots[k0 + i]);
            p0 += n;
            k0 += n + size_t(ord);
            if (ord - 1 != s.degree)
                im.r.warnings.push_back(std::format("{}: curve {} of order {} clamped to degree {}", where, c, ord, s.degree));
            if (std::string why; curve::isValid(s, &why))
                out.push_back(std::move(s));
            else
                im.r.warnings.push_back(std::format("{}: curve {} skipped: {}", where, c, why));
        }
        return out;
    }
    UsdGeomBasisCurves bc(prim);
    bc.GetPointsAttr().Get(&points, t);
    bc.GetCurveVertexCountsAttr().Get(&counts, t);
    const TfToken type = valueOr<TfToken>(bc.GetTypeAttr(), t, UsdGeomTokens->cubic);
    const TfToken basis = valueOr<TfToken>(bc.GetBasisAttr(), t, UsdGeomTokens->bezier);
    const TfToken wrap = valueOr<TfToken>(bc.GetWrapAttr(), t, UsdGeomTokens->nonperiodic);
    const bool periodic = wrap == UsdGeomTokens->periodic;
    const bool pinned = wrap == UsdGeomTokens->pinned;
    size_t p0 = 0;
    for (size_t c = 0; c < counts.size(); ++c)
    {
        const size_t n = size_t(std::max(0, counts[c]));
        std::vector<glm::dvec3> cv;
        for (size_t i = 0; i < n && p0 + i < points.size(); ++i)
            cv.push_back(lila(points[p0 + i]));
        p0 += n;
        curve::Spline s;
        s.closed = periodic;
        if (type == UsdGeomTokens->linear)
        {
            s.kind = curve::Kind::Poly;
            s.points = cv;
        }
        else if (basis == UsdGeomTokens->bezier)
        {
            // Anchors every third point, the two between them the handles; periodic curves wrap the last segment.
            s.kind = curve::Kind::Bezier;
            if (!periodic && (cv.size() < 4 || (cv.size() - 1) % 3 != 0))
            {
                im.r.warnings.push_back(std::format("{}: bezier curve {} has {} points (not 3n+1); skipped", where, c, cv.size()));
                continue;
            }
            if (periodic && (cv.size() < 3 || cv.size() % 3 != 0))
            {
                im.r.warnings.push_back(std::format("{}: periodic bezier curve {} has {} points (not 3n); skipped", where, c, cv.size()));
                continue;
            }
            const size_t anchors = periodic ? cv.size() / 3 : (cv.size() - 1) / 3 + 1;
            for (size_t a = 0; a < anchors; ++a)
            {
                const glm::dvec3 p = cv[a * 3];
                s.points.push_back(p);
                s.handleR.push_back(a * 3 + 1 < cv.size() ? cv[a * 3 + 1] : p);
                s.handleL.push_back(a > 0 ? cv[a * 3 - 1] : periodic ? cv.back() : p);
                s.typeL.push_back(curve::HandleType::Free);
                s.typeR.push_back(curve::HandleType::Free);
            }
        }
        else if (basis == UsdGeomTokens->bspline)
        {
            s.kind = curve::Kind::Nurbs;
            s.degree = 3;
            if (pinned && cv.size() >= 2)
            {
                cv.insert(cv.begin(), 2.0 * cv[0] - cv[1]);
                cv.push_back(2.0 * cv[cv.size() - 1] - cv[cv.size() - 2]);
            }
            s.points = cv;
            if (!periodic)
                for (size_t k = 0; k < cv.size() + 4; ++k)
                    s.knots.push_back(double(k));
        }
        else if (basis == UsdGeomTokens->catmullRom)
        {
            // Exact as Bezier: each span's handles a sixth of the neighbouring chord along.
            if (pinned && cv.size() >= 2)
            {
                cv.insert(cv.begin(), 2.0 * cv[0] - cv[1]);
                cv.push_back(2.0 * cv[cv.size() - 1] - cv[cv.size() - 2]);
            }
            const size_t m = cv.size();
            if (m < (periodic ? 3u : 4u))
            {
                im.r.warnings.push_back(std::format("{}: catmullRom curve {} is too short; skipped", where, c));
                continue;
            }
            s.kind = curve::Kind::Bezier;
            auto at = [&](long i) { return cv[size_t((i % long(m) + long(m)) % long(m))]; };
            const long first = periodic ? 0 : 1, last = periodic ? long(m) - 1 : long(m) - 2;
            for (long i = first; i <= last; ++i)
            {
                s.points.push_back(at(i));
                s.handleR.push_back(at(i) + (at(i + 1) - at(i - 1)) / 6.0);
                s.handleL.push_back(at(i) - (at(i + 1) - at(i - 1)) / 6.0);
                s.typeL.push_back(curve::HandleType::Aligned);
                s.typeR.push_back(curve::HandleType::Aligned);
            }
        }
        else
        {
            im.r.skipped.push_back(std::format("{}: curve basis {}", where, basis.GetString()));
            continue;
        }
        if (std::string why; curve::isValid(s, &why))
            out.push_back(std::move(s));
        else
            im.r.warnings.push_back(std::format("{}: curve {} skipped: {}", where, c, why));
    }
    if (VtFloatArray widths; bc.GetWidthsAttr().Get(&widths, t) && !widths.empty())
        im.r.skipped.push_back(std::format("{}: curve widths", where));
    return out;
}

void applyCamera(Importer& im, NodeId id, const UsdGeomCamera& cam, UsdTimeCode t)
{
    using scene::Camera;
    auto set = [&](int attr, scene::PlugValue v) { im.g.setValue({id, attr, -1}, std::move(v)); };
    // Lens and film back are tenths of a scene unit: millimetres in a centimetre stage.
    const double mmPerValue = im.axes.cmPerUnit;
    set(Camera::kFocalLength, valueOr<float>(cam.GetFocalLengthAttr(), t, 50.0f) * mmPerValue);
    const double h = valueOr<float>(cam.GetHorizontalApertureAttr(), t, 20.955f) * mmPerValue;
    const double v = valueOr<float>(cam.GetVerticalApertureAttr(), t, 15.2908f) * mmPerValue;
    set(Camera::kHorizontalFilmAperture, h / 25.4);
    set(Camera::kVerticalFilmAperture, v / 25.4);
    const GfVec2f clip = valueOr<GfVec2f>(cam.GetClippingRangeAttr(), t, GfVec2f(1.0f, 1000000.0f));
    set(Camera::kNearClipPlane, double(clip[0]) * im.axes.cmPerUnit);
    set(Camera::kFarClipPlane, double(clip[1]) * im.axes.cmPerUnit);
    const bool ortho = valueOr<TfToken>(cam.GetProjectionAttr(), t, UsdGeomTokens->perspective) == UsdGeomTokens->orthographic;
    set(Camera::kOrthographic, ortho);
    if (ortho)
        set(Camera::kOrthographicWidth, h / 10.0);
    const VtDictionary d = lilaData(cam.GetPrim());
    if (auto it = d.find("filmFit"); it != d.end() && it->second.IsHolding<int>())
        set(Camera::kFilmFit, int64_t(it->second.UncheckedGet<int>()));
    if (valueOr<float>(cam.GetFStopAttr(), t, 0.0f) > 0.0f)
        im.r.skipped.push_back(std::format("{}: depth of field (fStop, focusDistance)", cam.GetPath().GetString()));
}

/** Keys the camera's animated lens, film back and clipping. */
void keyCamera(Importer& im, NodeId id, const UsdGeomCamera& cam)
{
    using scene::Camera;
    const double k = im.axes.cmPerUnit;
    struct Channel
    {
        UsdAttribute attr;
        int plug;
        int component; // clippingRange's near (0) or far (1)
        double scale;
    };
    const Channel channels[] = {{cam.GetFocalLengthAttr(), Camera::kFocalLength, -1, k},
                                {cam.GetHorizontalApertureAttr(), Camera::kHorizontalFilmAperture, -1, k / 25.4},
                                {cam.GetVerticalApertureAttr(), Camera::kVerticalFilmAperture, -1, k / 25.4},
                                {cam.GetClippingRangeAttr(), Camera::kNearClipPlane, 0, k},
                                {cam.GetClippingRangeAttr(), Camera::kFarClipPlane, 1, k}};
    for (const Channel& c : channels)
    {
        std::vector<double> samples;
        if (!c.attr || !c.attr.GetTimeSamples(&samples) || samples.size() < 2)
            continue;
        std::vector<double> times, values;
        for (double s : samples)
        {
            double v = 0.0;
            if (c.component < 0)
                v = valueOr<float>(c.attr, UsdTimeCode(s), 0.0f);
            else
                v = valueOr<GfVec2f>(c.attr, UsdTimeCode(s), GfVec2f(0.0f))[c.component];
            times.push_back(im.seconds(s));
            values.push_back(v * c.scale);
        }
        keyPlug(im, {id, c.plug, -1}, times, values);
    }
}

/** Lila channels of a prim's local transform at t, through the node's camera frames. */
struct LocalChannels
{
    glm::dvec3 t{0.0}, r{0.0}, s{1.0}, pivot{0.0};
    scene::RotateOrder order = scene::RotateOrder::XYZ;
    bool exact = false; // straight from compatible xformOps, no decomposition
    bool sheared = false;
};

LocalChannels localChannels(Importer& im, const UsdGeomXformable& xf, NodeId node, UsdTimeCode t, const glm::dvec3& ref, bool allowExact)
{
    LocalChannels out;
    const Importer::Frame frame = im.frames.count(node) ? im.frames.at(node) : Importer::Frame{};
    const bool plainFrame = nearMatrix(frame.before, glm::dmat4(1.0)) && nearMatrix(frame.after, glm::dmat4(1.0));
    if (allowExact && plainFrame)
    {
        UsdGeomXformCommonAPI api(xf.GetPrim());
        GfVec3d tr;
        GfVec3f rot, sc, piv;
        UsdGeomXformCommonAPI::RotationOrder ro;
        if (api && api.GetXformVectors(&tr, &rot, &sc, &piv, &ro, t))
        {
            static const scene::RotateOrder kOrder[] = {scene::RotateOrder::XYZ, scene::RotateOrder::XZY, scene::RotateOrder::YXZ,
                                                        scene::RotateOrder::YZX, scene::RotateOrder::ZXY, scene::RotateOrder::ZYX};
            out.order = im.axes.orderToLila(kOrder[int(ro)]);
            out.t = im.axes.translateToLila(glm::dvec3(tr[0], tr[1], tr[2]));
            out.r = im.axes.rotateToLila(glm::dvec3(rot[0], rot[1], rot[2]));
            out.s = im.axes.scaleToLila(glm::dvec3(sc[0], sc[1], sc[2]));
            out.pivot = im.axes.translateToLila(glm::dvec3(piv[0], piv[1], piv[2]));
            out.exact = true;
            return out;
        }
    }
    GfMatrix4d m(1.0);
    bool resets = false;
    xf.GetLocalTransformation(&m, &resets, t);
    const glm::dmat4 local = frame.before * im.axes.matrixToLila(toGlm(m)) * frame.after;
    out.order = scene::RotateOrder(im.g.node(node)->as<int64_t>(scene::Transform::kRotateOrder));
    Trs trs = decompose(local, out.order, ref);
    out.t = trs.t;
    out.r = trs.r;
    out.s = trs.s;
    out.sheared = trs.sheared;
    return out;
}

/** The UsdLux lights Lila converts; all shine down their local -Z, as Lila's do. */
bool isLuxLight(const UsdPrim& p)
{
    return p.IsA<UsdLuxSphereLight>() || p.IsA<UsdLuxRectLight>() || p.IsA<UsdLuxDiskLight>() || p.IsA<UsdLuxDistantLight>() ||
           p.IsA<UsdLuxDomeLight>();
}

/**
 * A UsdLux light's settings: sphere lights are points (spots with ShapingAPI), rect, disk, distant and dome lights
 * their Lila kinds, intensity with exposure folded in, sizes in scene units. Lights Lila wrote also carry its own
 * settings in custom data, which win.
 */
void applyLight(Importer& im, NodeId id, const UsdPrim& prim, UsdTimeCode t)
{
    using scene::lights::Kind;
    namespace lights = scene::lights;
    lights::Settings s;
    UsdLuxLightAPI light(prim);
    const double k = im.axes.cmPerUnit;
    if (prim.IsA<UsdLuxSphereLight>())
    {
        UsdLuxSphereLight sphere(prim);
        s.kind = Kind::Point;
        if (!valueOr<bool>(sphere.GetTreatAsPointAttr(), t, false))
            s.sourceRadius = valueOr<float>(sphere.GetRadiusAttr(), t, 0.5f) * k;
        if (prim.HasAPI<UsdLuxShapingAPI>())
        {
            UsdLuxShapingAPI shaping(prim);
            const double cone = valueOr<float>(shaping.GetShapingConeAngleAttr(), t, 90.0f);
            if (cone < 90.0)
            {
                s.kind = Kind::Spot;
                s.outerConeAngle = cone;
                s.innerConeAngle = cone * (1.0 - std::clamp(double(valueOr<float>(shaping.GetShapingConeSoftnessAttr(), t, 0.0f)), 0.0, 1.0));
            }
            if (const std::string ies = valueOr<SdfAssetPath>(shaping.GetShapingIesFileAttr(), t, SdfAssetPath()).GetAssetPath(); !ies.empty())
                s.texture = ies;
        }
    }
    else if (prim.IsA<UsdLuxRectLight>())
    {
        UsdLuxRectLight rect(prim);
        s.kind = Kind::Area;
        s.width = valueOr<float>(rect.GetWidthAttr(), t, 1.0f) * k;
        s.height = valueOr<float>(rect.GetHeightAttr(), t, 1.0f) * k;
    }
    else if (prim.IsA<UsdLuxDiskLight>())
    {
        s.kind = Kind::Disc;
        s.width = s.height = 2.0 * valueOr<float>(UsdLuxDiskLight(prim).GetRadiusAttr(), t, 0.5f) * k;
    }
    else if (prim.IsA<UsdLuxDistantLight>())
        s.kind = Kind::Directional;
    else
    {
        s.kind = Kind::Dome;
        s.texture = valueOr<SdfAssetPath>(UsdLuxDomeLight(prim).GetTextureFileAttr(), t, SdfAssetPath()).GetAssetPath();
    }
    s.sourceFormat = "usd:" + prim.GetTypeName().GetString();
    const GfVec3f c = valueOr<GfVec3f>(light.GetColorAttr(), t, GfVec3f(1.0f));
    s.color = glm::dvec3(c[0], c[1], c[2]);
    s.intensity = valueOr<float>(light.GetIntensityAttr(), t, 1.0f) * std::exp2(double(valueOr<float>(light.GetExposureAttr(), t, 0.0f)));
    s.units = lights::Units::Nits;
    s.useTemperature = valueOr<bool>(light.GetEnableColorTemperatureAttr(), t, false);
    s.temperature = valueOr<float>(light.GetColorTemperatureAttr(), t, 6500.0f);
    if (prim.HasAPI<UsdLuxShadowAPI>())
        s.castShadows = valueOr<bool>(UsdLuxShadowAPI(prim).GetShadowEnableAttr(), t, true);
    const VtDictionary d = lilaData(prim);
    if (auto it = d.find("light"); it != d.end() && it->second.IsHolding<std::string>())
    {
        const nlohmann::json j = nlohmann::json::parse(it->second.UncheckedGet<std::string>(), nullptr, false);
        if (j.is_object())
        {
            s.kind = Kind(std::clamp(j.value("kind", int(s.kind)), 0, int(Kind::Dome)));
            s.units = lights::Units(std::clamp(j.value("units", int(s.units)), 0, int(lights::Units::Watts)));
            s.intensity = j.value("intensity", s.intensity);
            s.range = j.value("range", 0.0) * k;
            s.innerConeAngle = j.value("innerConeAngle", s.innerConeAngle);
            s.outerConeAngle = j.value("outerConeAngle", s.outerConeAngle);
            s.width = j.value("width", s.width / k) * k;
            s.height = j.value("height", s.height / k) * k;
            s.sourceRadius = j.value("sourceRadius", s.sourceRadius / k) * k;
            s.texture = j.value("texture", s.texture);
            s.extra = j.value("extra", std::string());
            s.sourceFormat.clear();
        }
    }
    lights::setSettings(im.g, id, s);
}

const char* attribLabel(const UsdPrim& p)
{
    if (p.IsA<UsdGeomPoints>())
        return "point cloud";
    if (p.HasAPI<UsdLuxLightAPI>())
        return "light";
    return nullptr;
}

struct Builder
{
    Importer& im;
    const scene::NodeType* xformType = scene::types::find("transform");
    const scene::NodeType* meshType = scene::types::find("mesh");
    const scene::NodeType* cameraType = scene::types::find("camera");
    const scene::NodeType* lightType = scene::types::find("light");
    const scene::NodeType* curveType = scene::types::find("curve");
    const scene::NodeType* lodType = scene::types::find("lodGroup");

    bool purposeOk(const UsdPrim& p)
    {
        if (!p.IsA<UsdGeomImageable>())
            return true;
        TfToken purpose;
        UsdGeomImageable(p).GetPurposeAttr().Get(&purpose);
        if (purpose.IsEmpty() || im.opts.purposes.count(purpose.GetString()))
            return true;
        im.r.skipped.push_back(std::format("{} (purpose {})", p.GetPath().GetString(), purpose.GetString()));
        return false;
    }

    NodeId makeNode(const UsdPrim& prim, const scene::NodeType& type, NodeId parent, const char* fallback)
    {
        NodeId replaced = kNoNode;
        NodeId id = takeOver(prim, type, parent, replaced);
        if (!id)
        {
            // Lila's exporter keeps a node's own name when the prim had to be named otherwise (LOD levels, clashes).
            std::string name = identifier(prim.GetName().GetString(), fallback);
            const VtDictionary d = lilaData(prim);
            if (auto it = d.find("name"); it != d.end() && it->second.IsHolding<std::string>() && scene::Graph::validName(it->second.UncheckedGet<std::string>()))
                name = it->second.UncheckedGet<std::string>();
            if (replaced)
                name = im.g.node(replaced)->name();
            id = im.g.createNode(type, name, parent, false)->id();
            if (replaced)
            {
                for (NodeId c : std::vector<NodeId>(im.g.children(replaced)))
                    im.g.setParent(c, id, false);
                deleteWithInputs(im.g, replaced);
                im.g.rename(id, name);
            }
        }
        importComponents(im.g, id, prim, im.restTime, unkeptAttrs);
        im.r.nodes.push_back(id);
        if (parent == im.opts.parent)
            im.r.roots.push_back(id);
        if (im.inCopy)
            im.r.copies.push_back({id, prim.GetPath()});
        else
            im.nodeOf[im.linkPath(prim.GetPath())] = id;
        return id;
    }

    /**
     * opts.reuse: the existing node for prim, back under parent with its transform reset and its curves gone, ready to
     * fill again. A mesh fed by a stack keeps it (stacked). kNoNode when there is none, or when it can't be reused
     * (another type); then replaced is the node to swap out.
     */
    NodeId takeOver(const UsdPrim& prim, const scene::NodeType& type, NodeId parent, NodeId& replaced)
    {
        if (!im.opts.reuse || im.inCopy)
            return kNoNode;
        auto it = im.opts.reuse->find(im.linkPath(prim.GetPath()));
        if (it == im.opts.reuse->end())
            return kNoNode;
        const NodeId id = it->second;
        im.opts.reuse->erase(it);
        const scene::Node* n = im.g.node(id);
        if (!n)
            return kNoNode;
        if (&n->type().registered() != &type)
        {
            replaced = id;
            return kNoNode;
        }
        if (type.isA("mesh") && im.g.source({id, scene::MeshNode::kInMesh, -1}))
            im.stacked.insert(id);
        using scene::Transform;
        if (im.g.parent(id) != parent)
            im.g.setParent(id, parent, false);
        std::set<NodeId> curves;
        for (int a = 0; a < int(n->type().attrs.size()); ++a)
            for (int c = -1; c < 3; ++c)
                if (auto src = im.g.source({id, a, c}); src && im.g.node(src->node)->type().name.starts_with("animCurve"))
                    curves.insert(src->node);
        for (NodeId c : curves)
            im.g.deleteNode(c);
        set(id, Transform::kRotateOrder, int64_t(0));
        set(id, Transform::kRotatePivot, glm::dvec3(0.0));
        set(id, Transform::kScalePivot, glm::dvec3(0.0));
        set(id, Transform::kVisibility, true);
        return id;
    }

    /** A value on a node; a reused node may have the plug driven or locked by the user, which then keeps it. */
    void set(NodeId node, int attr, scene::PlugValue v)
    {
        if (!im.opts.reuse)
        {
            im.g.setValue({node, attr, -1}, std::move(v));
            return;
        }
        try
        {
            im.g.setValue({node, attr, -1}, std::move(v));
        }
        catch (const std::exception& e)
        {
            im.r.warnings.push_back(std::format("{}: {}", im.g.node(node)->name(), e.what()));
        }
    }

    /** The node's rest transform, and whether it animates. */
    void place(const UsdPrim& prim, NodeId node, NodeId parent)
    {
        if (!prim.IsA<UsdGeomXformable>())
            return;
        UsdGeomXformable xf(prim);
        if (auto above = im.frames.find(parent); above != im.frames.end() && !nearMatrix(above->second.after, glm::dmat4(1.0)))
            im.frames[node].before = glm::transpose(above->second.after);
        const bool camera = prim.IsA<UsdGeomCamera>() || isLuxLight(prim);
        if (camera)
            im.frames[node].after = glm::dmat4(glm::transpose(im.axes.basis));
        GfMatrix4d m;
        bool resets = false;
        xf.GetLocalTransformation(&m, &resets, im.restTime);
        LocalChannels c;
        if (resets)
        {
            im.r.warnings.push_back(std::format("{}: resets the transform stack; placed by its world transform", prim.GetPath().GetString()));
            im.g.evaluate();
            const glm::dmat4 parentWorld = parent != kNoNode ? im.g.node(parent)->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
            const Importer::Frame f = im.frames.count(node) ? im.frames.at(node) : Importer::Frame{};
            const glm::dmat4 world = im.axes.matrixToLila(toGlm(xf.ComputeLocalToWorldTransform(im.restTime))) * f.after;
            Trs trs = decompose(glm::inverse(parentWorld) * world, scene::RotateOrder::XYZ, glm::dvec3(0.0));
            c.t = trs.t, c.r = trs.r, c.s = trs.s, c.sheared = trs.sheared;
        }
        else
            c = localChannels(im, xf, node, im.restTime, glm::dvec3(0.0), true);
        if (c.sheared)
            im.r.warnings.push_back(std::format("{}: shear dropped from its transform", prim.GetPath().GetString()));
        using scene::Transform;
        set(node, Transform::kTranslate, c.t);
        set(node, Transform::kRotate, c.r);
        set(node, Transform::kScale, c.s);
        if (c.order != scene::RotateOrder::XYZ)
            set(node, Transform::kRotateOrder, int64_t(c.order));
        if (c.pivot != glm::dvec3(0.0))
        {
            set(node, Transform::kRotatePivot, c.pivot);
            set(node, Transform::kScalePivot, c.pivot);
        }
        if (im.opts.animation && !resets && xf.TransformMightBeTimeVarying())
            im.animated.push_back({prim.GetPath(), node});

        UsdGeomImageable img(prim);
        if (valueOr<TfToken>(img.GetVisibilityAttr(), im.restTime, UsdGeomTokens->inherited) == UsdGeomTokens->invisible)
            set(node, Transform::kVisibility, false);
        if (std::vector<double> vis; img.GetVisibilityAttr().GetTimeSamples(&vis) && vis.size() > 1)
            im.r.warnings.push_back(std::format("{}: animated visibility kept at its first value", prim.GetPath().GetString()));
    }

    /** Model kinds, which Lila has nowhere to keep, and attributes of a type no Lila attribute holds: for the report. */
    int kinds = 0;
    std::vector<std::string> unkeptAttrs;
    void noteUnkept(const UsdPrim& prim)
    {
        TfToken kind;
        if (!lilaFlag(prim, "wrapper") && UsdModelAPI(prim).GetKind(&kind) && !kind.IsEmpty())
            ++kinds;
    }
    void reportUnkept()
    {
        if (kinds)
            im.r.skipped.push_back(std::format("model kind on {} prim(s); Lila has no model hierarchy", kinds));
        if (!unkeptAttrs.empty())
        {
            std::string list;
            for (size_t k = 0; k < unkeptAttrs.size() && k < 5; ++k)
                list += (list.empty() ? "" : ", ") + unkeptAttrs[k];
            im.r.skipped.push_back(std::format("attributes Lila has no attribute type for ({}): {}{}", unkeptAttrs.size(), list, unkeptAttrs.size() > 5 ? ", ..." : ""));
        }
    }

    /** By path: an LOD group below switches variants, which recomposes its subtree under any prim handle held across it. */
    void children(const UsdPrim& prim, NodeId node)
    {
        if (im.shallowRoots.count(prim.GetPath()))
            return;
        std::vector<SdfPath> paths;
        for (const UsdPrim& c : prim.GetFilteredChildren(kChildren))
            paths.push_back(c.GetPath());
        for (const SdfPath& p : paths)
            if (UsdPrim c = im.stage->GetPrimAtPath(p))
                build(c, node);
    }

    void lodGroup(const UsdPrim& prim, NodeId parent, const std::vector<std::string>& variants)
    {
        const SdfPath path = prim.GetPath();
        const NodeId id = makeNode(prim, *lodType, parent, "LOD");
        place(prim, id, parent);
        const std::string original = prim.GetVariantSets().GetVariantSet("LOD").GetVariantSelection();
        const SdfPrimSpecHandle sessionSpec = im.stage->GetSessionLayer()->GetPrimAtPath(path);
        const bool sessionHad = sessionSpec && sessionSpec->GetVariantSelections().count("LOD");
        auto select = [&](const std::string& v) {
            UsdEditContext session(im.stage, im.stage->GetSessionLayer());
            im.stage->GetPrimAtPath(path).GetVariantSets().GetVariantSet("LOD").SetVariantSelection(v);
        };
        const SdfPath link = im.linkPath(path);
        for (const std::string& v : variants)
        {
            select(v);
            im.lodScope.push_back({path, link.AppendVariantSelection("LOD", v)});
            if (UsdPrim now = im.stage->GetPrimAtPath(path))
                children(now, id);
            im.lodScope.pop_back();
        }
        if (im.opts.live && !sessionHad)
        {
            UsdEditContext session(im.stage, im.stage->GetSessionLayer());
            im.stage->GetPrimAtPath(path).GetVariantSets().GetVariantSet("LOD").ClearVariantSelection();
        }
        else
            select(original);
        const VtDictionary d = lilaData(im.stage->GetPrimAtPath(path));
        if (auto it = d.find("lodScreenSizes"); it != d.end() && it->second.IsHolding<VtDoubleArray>())
        {
            const VtDoubleArray& sizes = it->second.UncheckedGet<VtDoubleArray>();
            im.g.setValue({id, scene::LodGroup::kScreenSizes, -1}, scene::lod::formatSizes(std::vector<double>(sizes.begin(), sizes.end())));
        }
    }

    NodeId mesh(const UsdPrim& prim, NodeId parent)
    {
        NodeId id = makeNode(prim, *meshType, parent, "Mesh");
        place(prim, id, parent);
        // By link path: LOD levels hold different meshes at the same composed path.
        const SdfPath key = prim.IsInstanceProxy() ? prim.GetPrimInPrototype().GetPath() : im.linkPath(prim.GetPath());
        scene::MeshPtr ptr;
        if (auto it = im.meshCache.find(key); it != im.meshCache.end())
        {
            ptr = it->second;
            im.pointMaps[prim.GetPath()] = im.pointMaps[key];
        }
        else
        {
            MeshBuild b = convertMesh(im, UsdGeomMesh(prim), im.restTime);
            if (b.badFaces)
                im.r.warnings.push_back(std::format("{}: {} degenerate face(s) skipped", prim.GetPath().GetString(), b.badFaces));
            if (b.holes)
                im.r.warnings.push_back(std::format("{}: {} hole face(s) left out", prim.GetPath().GetString(), b.holes));
            ptr = std::make_shared<geo::Mesh>(std::move(b.mesh));
            im.meshCache[key] = ptr;
            im.pointMaps[key] = b.pointOf;
            im.pointMaps[prim.GetPath()] = std::move(b.pointOf);
        }
        if (!im.stacked.count(id))
            im.g.setValue({id, scene::MeshNode::kInMesh, -1}, ptr);
        else if (im.opts.keepShape && im.opts.keepShape->count(im.linkPath(prim.GetPath())))
            im.keptShape.insert(id);
        else
            try
            {
                im.g.setValue(im.basePlug(id), ptr);
            }
            catch (const std::exception& e)
            {
                im.r.warnings.push_back(std::format("{}: its new shape couldn't go under its modifiers: {}", prim.GetPath().GetString(), e.what()));
            }
        UsdGeomMesh usdMesh(prim);
        if (TfToken scheme; usdMesh.GetSubdivisionSchemeAttr().HasAuthoredValue() && usdMesh.GetSubdivisionSchemeAttr().Get(&scheme) &&
                            scheme != UsdGeomTokens->none)
            im.g.setValue({id, scene::MeshNode::kSmoothPreview, -1}, int64_t(2));
        if (std::vector<double> s; usdMesh.GetPointsAttr().GetTimeSamples(&s) && s.size() > 1)
            pointCache(prim, id, *ptr, s);
        if (prim.HasAPI<UsdSkelBindingAPI>())
            im.bound.push_back({prim.GetPath(), id});
        else if (im.stacked.count(id))
            dropDeadSkin(id);
        return id;
    }

    /**
     * Time-sampled points: a meshCache on the mesh holding every sample, through the rest build's point map, so the
     * cache matches the mesh's points. Normals follow the points (the deformer rebuilds them, keeping the hard edges
     * the rest frame's normals gave). Topology that changes over time keeps the rest frame.
     */
    void pointCache(const UsdPrim& prim, NodeId id, const geo::Mesh& rest, const std::vector<double>& times)
    {
        const std::string where = prim.GetPath().GetString();
        if (!im.opts.animation)
        {
            im.r.skipped.push_back(std::format("{}: animated points (animation off)", where));
            return;
        }
        const UsdGeomMesh usdMesh(prim);
        for (const UsdAttribute& a : {usdMesh.GetFaceVertexCountsAttr(), usdMesh.GetFaceVertexIndicesAttr()})
            if (a.GetNumTimeSamples() > 1)
            {
                im.r.warnings.push_back(std::format("{}: the topology changes over time; the animated points are kept at the rest frame", where));
                return;
            }
        // Instances of one prototype share its mesh, and so its samples.
        if (auto it = im.pointCaches.find(&rest); it != im.pointCaches.end())
            return addCache(id, it->second);
        const std::vector<uint32_t>& pointOf = im.pointMaps[prim.GetPath()];
        auto samples = std::make_shared<scene::MeshCacheSamples>();
        std::vector<glm::dvec3> pts(rest.positions().begin(), rest.positions().end());
        VtVec3fArray p;
        for (double t : times)
        {
            usdMesh.GetPointsAttr().Get(&p, UsdTimeCode(t));
            if (p.size() != pointOf.size())
            {
                im.r.warnings.push_back(std::format("{}: the point count changes over time ({} at time {}, {} at rest); the animated points are "
                                                    "kept at the rest frame",
                                                    where, p.size(), t, pointOf.size()));
                return;
            }
            for (size_t k = 0; k < p.size(); ++k)
                if (pointOf[k] != kInvalid && pointOf[k] < pts.size())
                    pts[pointOf[k]] = im.axes.pointToLila(glm::dvec3(p[k][0], p[k][1], p[k][2]));
            samples->add(im.seconds(t), pts);
        }
        im.firstKey = std::min(im.firstKey, samples->times.front());
        im.lastKey = std::max(im.lastKey, samples->times.back());
        im.pointCaches[&rest] = samples;
        addCache(id, std::move(samples));
    }

    /** The mesh's meshCache gets the samples; a mesh rebuilt in place keeps the one it has. */
    void addCache(NodeId id, std::shared_ptr<const scene::MeshCacheSamples> samples)
    {
        if (const NodeId cache = scene::meshCache::find(im.g, id))
            scene::meshCache::setSamples(im.g, cache, std::move(samples));
        else
            im.r.meshCaches.push_back(scene::meshCache::create(im.g, id, std::move(samples)));
    }

    /** A kept stack's skin whose joints are all gone, on a prim no longer skinned: taken out of the stack. */
    void dropDeadSkin(NodeId mesh)
    {
        const NodeId sk = scene::skin::find(im.g, mesh);
        if (!sk)
            return;
        const std::vector<scene::skin::Influence> infs = scene::skin::influences(im.g, sk);
        if (std::any_of(infs.begin(), infs.end(), [](const scene::skin::Influence& i) { return i.joint != kNoNode; }))
            return;
        removeFromStack(im.g, sk);
    }

    void instancer(const UsdPrim& prim, NodeId node)
    {
        UsdGeomPointInstancer pi(prim);
        SdfPathVector protos;
        pi.GetPrototypesRel().GetTargets(&protos);
        VtIntArray protoIndices;
        pi.GetProtoIndicesAttr().Get(&protoIndices, im.restTime);
        VtMatrix4dArray xforms;
        if (!pi.ComputeInstanceTransformsAtTime(&xforms, im.restTime, im.restTime, UsdGeomPointInstancer::ExcludeProtoXform,
                                                UsdGeomPointInstancer::IgnoreMask))
        {
            im.r.warnings.push_back(std::format("{}: instance transforms could not be computed", prim.GetPath().GetString()));
            return;
        }
        const std::vector<bool> mask = pi.ComputeMaskAtTime(im.restTime);
        size_t made = 0;
        for (size_t i = 0; i < xforms.size() && i < protoIndices.size(); ++i)
        {
            if (!mask.empty() && !mask[i])
                continue;
            const int pidx = protoIndices[i];
            if (pidx < 0 || size_t(pidx) >= protos.size())
                continue;
            UsdPrim proto = im.stage->GetPrimAtPath(protos[size_t(pidx)]);
            if (!proto)
                continue;
            scene::Node* holder =
                im.g.createNode(*xformType, identifier(proto.GetName().GetString(), "instance") + "_" + std::to_string(i), node, false);
            im.r.nodes.push_back(holder->id());
            im.r.pointInstances.push_back({holder->id(), prim.GetPath(), i});
            Trs trs = decompose(im.axes.matrixToLila(toGlm(xforms[i])), scene::RotateOrder::XYZ, glm::dvec3(0.0));
            im.g.setValue({holder->id(), scene::Transform::kTranslate, -1}, trs.t);
            im.g.setValue({holder->id(), scene::Transform::kRotate, -1}, trs.r);
            im.g.setValue({holder->id(), scene::Transform::kScale, -1}, trs.s);
            ++im.inCopy;
            build(proto, holder->id());
            --im.inCopy;
            ++made;
        }
        im.r.instances += int(made);
        if (made)
            im.r.warnings.push_back(std::format("{}: {} point instance(s) realized as transforms sharing their prototype's mesh data", prim.GetPath().GetString(), made));
        if (std::vector<double> s; pi.GetPositionsAttr().GetTimeSamples(&s) && s.size() > 1)
            im.r.warnings.push_back(std::format("{}: animated instances kept at the rest frame", prim.GetPath().GetString()));
    }

    void build(const UsdPrim& prim, NodeId parent)
    {
        tasks::checkCancel();
        if (isShading(prim) || prim.IsA<UsdSkelAnimation>() || prim.IsA<UsdSkelBlendShape>() || prim.IsA<UsdGeomSubset>())
            return;
        if (onlyShading(prim))
            return;
        if (!purposeOk(prim))
            return;
        if (prim.HasPayload() && !prim.IsLoaded())
            im.r.skipped.push_back(std::format("{}: payload not loaded", prim.GetPath().GetString()));
        if (prim.IsInstance())
            ++im.r.instances;
        noteUnkept(prim);
        const std::string path = prim.GetPath().GetString();

        // Prims Lila's exporter adds only to satisfy USD (a root, a skeleton holder) dissolve into their parent.
        const bool wrapper = lilaFlag(prim, "wrapper");
        if (wrapper && prim.IsA<UsdGeomXformable>() && !UsdGeomXformable(prim).TransformMightBeTimeVarying())
        {
            GfMatrix4d m;
            bool resets = false;
            UsdGeomXformable(prim).GetLocalTransformation(&m, &resets, im.restTime);
            if (!resets && nearMatrix(toGlm(m), glm::dmat4(1.0)))
            {
                if (prim.IsA<UsdSkelSkeleton>())
                    im.buildSkeleton(prim, parent);
                else
                    children(prim, parent);
                return;
            }
        }

        if (const std::vector<std::string> lods = lodLevels(prim); !lods.empty())
        {
            lodGroup(prim, parent, lods);
            return;
        }
        if (prim.IsA<UsdGeomMesh>())
        {
            children(prim, mesh(prim, parent));
            return;
        }
        if (prim.IsA<UsdGeomCamera>() && cameraType)
        {
            NodeId id = makeNode(prim, *cameraType, parent, "camera");
            im.r.cameras.push_back(id);
            place(prim, id, parent);
            applyCamera(im, id, UsdGeomCamera(prim), im.restTime);
            if (im.opts.animation)
                keyCamera(im, id, UsdGeomCamera(prim));
            children(prim, id);
            return;
        }
        if (isLuxLight(prim) && lightType)
        {
            NodeId id = makeNode(prim, *lightType, parent, "light");
            im.r.lights.push_back(id);
            place(prim, id, parent);
            applyLight(im, id, prim, im.restTime);
            children(prim, id);
            return;
        }
        if ((prim.IsA<UsdGeomBasisCurves>() || prim.IsA<UsdGeomNurbsCurves>()) && curveType)
        {
            curve::Splines s = convertCurves(im, prim, im.restTime);
            NodeId id = makeNode(prim, *curveType, parent, "Curve");
            place(prim, id, parent);
            if (!s.empty())
                im.g.setValue({id, curve::CurveShape::kCreate, -1}, curve::makeCurveData(std::move(s)));
            im.r.curves.push_back(id);
            children(prim, id);
            return;
        }
        if (prim.IsA<UsdGeomPointInstancer>())
        {
            NodeId id = makeNode(prim, *xformType, parent, "Instancer");
            place(prim, id, parent);
            instancer(prim, id);
            return;
        }
        if (prim.IsA<UsdSkelSkeleton>())
        {
            NodeId id = makeNode(prim, *xformType, parent, "Skeleton");
            place(prim, id, parent);
            im.buildSkeleton(prim, id);
            children(prim, id);
            return;
        }
        if (prim.IsA<UsdGeomGprim>() && !prim.IsA<UsdGeomPointBased>())
        {
            if (std::optional<geo::Mesh> shape = tessellate(im, prim, im.restTime))
            {
                NodeId id = makeNode(prim, *meshType, parent, "Mesh");
                place(prim, id, parent);
                if (im.materials)
                    if (UsdShadeMaterial bound = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial())
                        scene::materials::assign(*shape, im.materials->slot(bound), {});
                im.g.setValue({id, scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(*shape))));
                im.r.warnings.push_back(std::format("{}: {} tessellated to a mesh", path, prim.GetTypeName().GetString()));
                children(prim, id);
                return;
            }
        }
        const bool groupLike = prim.IsA<UsdGeomXformable>() || prim.IsA<UsdGeomScope>() || prim.GetTypeName().IsEmpty();
        if (!groupLike)
        {
            im.r.skipped.push_back(std::format("{} ({})", path, prim.GetTypeName().GetString()));
            return;
        }
        if (const char* label = attribLabel(prim))
            im.r.skipped.push_back(std::format("{} '{}' (imported as a transform)", label, path));
        else if (prim.IsA<UsdGeomGprim>())
            im.r.skipped.push_back(std::format("{} '{}' (imported as a transform)", prim.GetTypeName().GetString(), path));
        NodeId id = makeNode(prim, *xformType, parent, "Transform");
        place(prim, id, parent);
        children(prim, id);
    }
};

/** Applies variant selections in the session layer until nothing more changes (a selection can reveal further sets). */
void applyVariants(const UsdStageRefPtr& stage, const ImportOptions& opts, ImportResult& r)
{
    UsdEditContext session(stage, stage->GetSessionLayer());
    std::set<std::string> usedSets;
    for (int pass = 0; pass < 8 && !opts.variants.empty(); ++pass)
    {
        bool changed = false;
        for (const UsdPrim& p : stage->Traverse())
        {
            if (!p.HasVariantSets())
                continue;
            UsdVariantSets sets = p.GetVariantSets();
            for (const auto& [set, sel] : opts.variants)
                if (sets.HasVariantSet(set))
                {
                    usedSets.insert(set);
                    UsdVariantSet vs = sets.GetVariantSet(set);
                    if (vs.GetVariantSelection() == sel)
                        continue;
                    if (!vs.HasAuthoredVariant(sel))
                    {
                        r.warnings.push_back(std::format("{}: variant set {} has no variant {}", p.GetPath().GetString(), set, sel));
                        continue;
                    }
                    vs.SetVariantSelection(sel);
                    changed = true;
                }
        }
        if (!changed)
            break;
    }
    for (const auto& [set, sel] : opts.variants)
        if (!usedSets.count(set))
            r.warnings.push_back(std::format("no prim has a variant set named {}", set));
    for (const auto& [path, sets] : opts.primVariants)
    {
        UsdPrim p = SdfPath::IsValidPathString(path) ? stage->GetPrimAtPath(SdfPath(path)) : UsdPrim();
        if (!p)
        {
            r.warnings.push_back(std::format("variants: no prim at {}", path));
            continue;
        }
        for (const auto& [set, sel] : sets)
        {
            UsdVariantSet vs = p.GetVariantSets().GetVariantSet(set);
            if (!p.GetVariantSets().HasVariantSet(set) || !vs.HasAuthoredVariant(sel))
                r.warnings.push_back(std::format("{}: no variant {} in set {}", path, sel, set));
            else
                vs.SetVariantSelection(sel);
        }
    }
}
} // namespace

std::vector<std::string> lodLevels(const UsdPrim& prim)
{
    if (!scene::types::find("lodGroup") || prim.IsInstanceProxy() || prim.IsA<UsdGeomGprim>() || !prim.GetVariantSets().HasVariantSet("LOD"))
        return {};
    std::vector<std::pair<int, std::string>> found;
    for (const std::string& v : prim.GetVariantSets().GetVariantSet("LOD").GetVariantNames())
        if (v.size() > 3 && v.starts_with("LOD") && std::all_of(v.begin() + 3, v.end(), [](char c) { return c >= '0' && c <= '9'; }))
            found.push_back({std::stoi(v.substr(3)), v});
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (auto& [k, v] : found)
        out.push_back(v);
    return out;
}

SdfPath Importer::linkPath(const SdfPath& composed) const
{
    if (lodScope.empty() || composed == lodScope.back().first || !composed.HasPrefix(lodScope.back().first))
        return composed;
    SdfPath out = lodScope.back().second;
    for (const SdfPath& p : composed.MakeRelativePath(lodScope.back().first).GetPrefixes())
        out = out.AppendChild(p.GetNameToken());
    return out;
}

scene::PlugRef Importer::basePlug(NodeId mesh)
{
    if (stacked.count(mesh))
        return scene::modifiers::editPlug(g, mesh);
    return {mesh, scene::MeshNode::kInMesh, -1};
}

void removeFromStack(scene::Graph& graph, NodeId modifier)
{
    const scene::ModifierNode* mod = scene::modifiers::asModifier(graph.node(modifier));
    if (!mod)
        return;
    const std::optional<scene::PlugRef> below = graph.source({modifier, mod->inAttr, -1});
    for (const scene::PlugRef& d : graph.destinations({modifier, mod->outAttr, -1}))
    {
        graph.disconnect(d);
        if (below)
            graph.connect(*below, d);
    }
    if (below)
        graph.disconnect({modifier, mod->inAttr, -1});
    if (graph.node(modifier)->type().isA("morph"))
        for (NodeId t : scene::morphs::targetNodes(graph, modifier))
            deleteWithInputs(graph, t);
    deleteWithInputs(graph, modifier);
}

namespace
{
/** How far a key may sit from a sample and still count as reproducing it. */
double tolerance(const std::vector<double>& values)
{
    double big = 1.0;
    for (double v : values)
        big = std::max(big, std::abs(v));
    return 1e-6 * big;
}

/** Sample indices kept so that linear (or held) interpolation between them reproduces every sample of every channel. */
std::vector<size_t> thinSamples(const std::vector<double>& times, const std::vector<const std::vector<double>*>& channels, bool held)
{
    const size_t n = times.size();
    std::vector<double> tol;
    for (const std::vector<double>* c : channels)
        tol.push_back(tolerance(*c));
    auto fits = [&](size_t a, size_t b, size_t i) {
        const double u = held ? 0.0 : (times[i] - times[a]) / (times[b] - times[a]);
        for (size_t k = 0; k < channels.size(); ++k)
        {
            const std::vector<double>& v = *channels[k];
            if (std::abs(v[a] + (v[b] - v[a]) * u - v[i]) > tol[k])
                return false;
        }
        return true;
    };
    std::vector<size_t> kept;
    if (n == 0)
        return kept;
    kept.push_back(0);
    for (size_t i = 1; i + 1 < n; ++i)
    {
        const size_t a = kept.back();
        bool ok = true;
        for (size_t j = a + 1; j <= i && ok; ++j)
            ok = fits(a, i + 1, j);
        if (!ok)
            kept.push_back(i);
    }
    if (n > 1)
        kept.push_back(n - 1);
    return kept;
}

/** A scalar curve Lila's exporter recorded beside the samples, when it still reproduces them. */
std::optional<scene::anim::Curve> storedCurve(const json& j, const std::vector<double>& times, const std::vector<double>& values)
{
    using namespace scene::anim;
    if (!j.is_object() || !j.contains("keys"))
        return std::nullopt;
    Curve c;
    try
    {
        c.keys = Curve::keysFromJson(j["keys"]);
        c.pre = infinityFromName(j.value("pre", std::string(infinityName(Infinity::Constant))));
        c.post = infinityFromName(j.value("post", std::string(infinityName(Infinity::Constant))));
        c.weighted = j.value("weighted", false);
        c.loop = j.value("loop", false);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (c.keys.empty())
        return std::nullopt;
    const double tol = tolerance(values);
    for (size_t i = 0; i < times.size() && i < values.size(); ++i)
        if (std::abs(c.evaluate(times[i]) - values[i]) > tol)
            return std::nullopt;
    return c;
}

/** The recorded rotation curve, when it still poses every sample (Euler degrees in order). */
std::optional<scene::anim::RotationCurve> storedRotation(const json& j, const std::vector<double>& times, const std::vector<glm::dvec3>& euler,
                                                         scene::RotateOrder order)
{
    using namespace scene::anim;
    if (!j.is_object() || !j.contains("keys"))
        return std::nullopt;
    RotationCurve rc;
    try
    {
        rc.order = scene::RotateOrder(std::clamp(j.value("order", 0), 0, 5));
        rc.keys = RotationCurve::keysFromJson(j["keys"], rc.order);
        rc.pre = infinityFromName(j.value("pre", std::string(infinityName(Infinity::Constant))));
        rc.post = infinityFromName(j.value("post", std::string(infinityName(Infinity::Constant))));
        rc.weighted = j.value("weighted", false);
        rc.loop = j.value("loop", false);
        rc.interp = rotInterpFromName(j.value("interp", std::string(rotInterpName(rc.interp))));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (rc.keys.empty())
        return std::nullopt;
    for (size_t i = 0; i < times.size() && i < euler.size(); ++i)
    {
        const glm::dquat a = rc.evaluateQuat(times[i]), b = eulerToQuat(euler[i], order);
        if (1.0 - std::abs(glm::dot(a, b)) > 1e-9)
            return std::nullopt;
    }
    if (rc.order != order)
        rc.reorder(order);
    return rc;
}

/**
 * Lighting channels: the names Lila's exporter kept in custom data, else the lights' lightLink collections baked into
 * channels. A light whose collection holds every built geometry prim (or has none authored) keeps default.
 */
void importLightLinks(scene::Graph& g, const UsdStageRefPtr& stage, ImportResult& r)
{
    namespace lc = scene::lightingChannels;
    std::vector<std::pair<NodeId, UsdPrim>> built;
    for (const auto& [path, id] : r.nodeOf)
        if (UsdPrim p = stage->GetPrimAtPath(path))
            built.push_back({id, p});
    std::set<std::string> kept;
    for (const auto& [id, p] : built)
    {
        const VtDictionary d = lilaData(p);
        auto it = d.find("lightingChannels");
        if (it == d.end() || !it->second.IsHolding<VtStringArray>())
            continue;
        const VtStringArray& channels = it->second.UncheckedGet<VtStringArray>();
        lc::set(g, id, std::vector<std::string>(channels.begin(), channels.end()));
        kept.insert(channels.begin(), channels.end());
    }
    if (!kept.empty())
    {
        kept.erase(lc::kDefault);
        r.lightingChannels.assign(kept.begin(), kept.end());
        return;
    }
    std::vector<std::pair<NodeId, SdfPath>> geometry;
    for (const auto& [id, p] : built)
        if (p.IsA<UsdGeomGprim>())
            geometry.push_back({id, p.GetPath()});
    std::vector<std::pair<NodeId, std::vector<NodeId>>> links;
    for (const auto& [id, p] : built)
    {
        if (!isLuxLight(p))
            continue;
        UsdCollectionAPI link(p, TfToken("lightLink"));
        const UsdAttribute includeRoot = link.GetIncludeRootAttr();
        const UsdRelationship includes = link.GetIncludesRel(), excludes = link.GetExcludesRel();
        if (!(includeRoot && includeRoot.HasAuthoredValue()) && !(includes && includes.HasAuthoredTargets()) &&
            !(excludes && excludes.HasAuthoredTargets()))
            continue;
        const UsdCollectionMembershipQuery query = link.ComputeMembershipQuery();
        std::vector<NodeId> lit;
        for (const auto& [object, path] : geometry)
            if (query.IsPathIncluded(path))
                lit.push_back(object);
        if (lit.size() != geometry.size())
            links.push_back({id, std::move(lit)});
    }
    if (!links.empty())
        r.lightingChannels = lc::bakeLinks(g, links);
}
} // namespace

void keyPlug(Importer& im, const scene::PlugRef& plug, const std::vector<double>& times, const std::vector<double>& values)
{
    using namespace scene;
    if (times.empty() || im.g.source(plug))
        return;
    const anim::Tangent tan = im.held ? anim::Tangent::Step : anim::Tangent::Linear;
    anim::Curve c;
    for (size_t i : thinSamples(times, {&values}, im.held))
        c.keys.push_back({.time = times[i], .value = values[i], .in = tan, .out = tan});
    NodeId curve = im.g.createNode(*types::find("animCurveTU"), im.g.node(plug.node)->name() + "_" + im.g.attr(plug).name, kNoNode, false)->id();
    anim::setCurve(im.g, curve, c);
    im.g.connect({curve, anim::CurveNode::kOutput, -1}, plug);
    im.g.setValue(plug, values.front());
    ++im.r.keyedCurves;
    im.firstKey = std::min(im.firstKey, times.front());
    im.lastKey = std::max(im.lastKey, times.back());
}

void keyChannels(Importer& im, NodeId node, const std::vector<double>& times, const std::vector<glm::dvec3>& t,
                 const std::vector<glm::dvec3>& r, const std::vector<glm::dvec3>& s, const json* recorded)
{
    using namespace scene;
    static constexpr const char* kCurveType[3] = {"animCurveTL", "animCurveTA", "animCurveTU"};
    static constexpr const char* kGroup[3] = {"translate", "rotate", "scale"};
    static constexpr int kAttr[3] = {Transform::kTranslate, Transform::kRotate, Transform::kScale};
    if (times.size() < 2)
        return;
    const anim::Tangent tan = im.held ? anim::Tangent::Step : anim::Tangent::Linear;
    Node* n = im.g.node(node);
    const glm::dvec3 rest[3] = {n->as<glm::dvec3>(Transform::kTranslate), n->as<glm::dvec3>(Transform::kRotate), n->as<glm::dvec3>(Transform::kScale)};
    const std::vector<glm::dvec3>* groups[3] = {&t, &r, &s};
    std::array<std::vector<double>, 9> values;
    for (int ch = 0; ch < 9; ++ch)
        for (size_t i = 0; i < times.size(); ++i)
            values[size_t(ch)].push_back((*groups[ch / 3])[i][ch % 3]);
    auto recordedOf = [&](int ch) -> const json* {
        if (!recorded || !recorded->is_object() || !recorded->contains(kGroup[ch / 3]))
            return nullptr;
        const json& g = (*recorded)[kGroup[ch / 3]];
        return g.is_array() && g.size() == 3 ? &g[size_t(ch % 3)] : nullptr;
    };
    auto sampled = [&](const std::vector<size_t>& kept, int ch) {
        anim::Curve c;
        for (size_t i : kept)
            c.keys.push_back({.time = times[i], .value = values[size_t(ch)][i], .in = tan, .out = tan});
        return c;
    };
    std::array<anim::Curve, 9> curves;
    std::array<bool, 9> stored{};
    for (int ch = 0; ch < 9; ++ch)
    {
        const json* rec = recordedOf(ch);
        if (std::optional<anim::Curve> c = rec ? storedCurve(*rec, times, values[size_t(ch)]) : std::nullopt)
        {
            curves[size_t(ch)] = std::move(*c);
            stored[size_t(ch)] = true;
        }
        else
            curves[size_t(ch)] = sampled(thinSamples(times, {&values[size_t(ch)]}, im.held), ch);
    }
    im.firstKey = std::min(im.firstKey, times.front());
    im.lastKey = std::max(im.lastKey, times.back());

    bool rotationCurve = false;
    const PlugRef rotate{node, Transform::kRotate, -1};
    if (!im.held && anim::keysAsRotation(im.g, rotate) && !im.g.source(rotate))
    {
        bool moves = false;
        for (int ch = 3; ch < 6; ++ch)
            for (double v : values[size_t(ch)])
                moves = moves || std::abs(v - rest[1][ch % 3]) >= 1e-6;
        if (moves)
        {
            auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
            std::optional<anim::RotationCurve> rc;
            if (recorded && recorded->is_object() && recorded->contains("rotateQ"))
                rc = storedRotation((*recorded)["rotateQ"], times, r, order);
            if (!rc)
            {
                // The three axes keyed at the same samples, so each rotation key poses all of them.
                std::array<anim::Curve, 3> axes;
                if (stored[3] && stored[4] && stored[5])
                    axes = {curves[3], curves[4], curves[5]};
                else
                {
                    const std::vector<size_t> kept = thinSamples(times, {&values[3], &values[4], &values[5]}, false);
                    axes = {sampled(kept, 3), sampled(kept, 4), sampled(kept, 5)};
                }
                rc = anim::rotationFromEuler({&axes[0], &axes[1], &axes[2]}, rest[1], order, anim::RotInterp::Slerp);
            }
            NodeId curve = im.g.createNode(*types::find("animCurveRQ"), n->name() + "_rotate", kNoNode, false)->id();
            anim::setRotationCurve(im.g, curve, *rc);
            im.g.connect({curve, anim::RotationCurveNode::kOutput, -1}, rotate);
            ++im.r.keyedCurves;
        }
        rotationCurve = true;
    }
    for (int ch = 0; ch < 9; ++ch)
    {
        if (rotationCurve && ch / 3 == 1)
            continue;
        const double v0 = rest[ch / 3][ch % 3];
        if (std::all_of(values[size_t(ch)].begin(), values[size_t(ch)].end(), [&](double v) { return std::abs(v - v0) < 1e-6; }))
            continue;
        const PlugRef plug{node, kAttr[ch / 3], ch % 3};
        if (im.g.source(plug))
            continue;
        std::string name = n->name() + "_" + im.g.attr(plug).childNames[size_t(ch % 3)];
        NodeId curve = im.g.createNode(*types::find(kCurveType[ch / 3]), name, kNoNode, false)->id();
        anim::setCurve(im.g, curve, curves[size_t(ch)]);
        im.g.connect({curve, anim::CurveNode::kOutput, -1}, plug);
        ++im.r.keyedCurves;
    }
}

/**
 * Where the stage says it is on the Earth: the CRS and projected origin Lila wrote in customLayerData, else a Cesium
 * georeference prim's WGS 84 origin taken in the scene's CRS (or the UTM zone around it when the scene has none).
 */
std::optional<json> stageGeoreference(const UsdStageRefPtr& stage)
{
    const VtDictionary layerData = stage->GetRootLayer()->GetCustomLayerData();
    if (auto it = layerData.find("lilaGeoreference"); it != layerData.end() && it->second.IsHolding<VtDictionary>())
    {
        const VtDictionary& d = it->second.UncheckedGet<VtDictionary>();
        auto crs = d.find("crs");
        auto origin = d.find("origin");
        if (crs != d.end() && origin != d.end() && crs->second.IsHolding<std::string>() && origin->second.IsHolding<GfVec3d>())
        {
            const GfVec3d o = origin->second.UncheckedGet<GfVec3d>();
            return json{{"crs", crs->second.UncheckedGet<std::string>()}, {"origin", {o[0], o[1], o[2]}}};
        }
    }
    for (const UsdPrim& p : stage->Traverse())
    {
        if (p.GetTypeName() != TfToken("CesiumGeoreferencePrim"))
            continue;
        double lat = 0.0, lon = 0.0, h = 0.0;
        if (!p.GetAttribute(TfToken("cesium:georeferenceOrigin:latitude")).Get(&lat) ||
            !p.GetAttribute(TfToken("cesium:georeferenceOrigin:longitude")).Get(&lon))
            return std::nullopt;
        p.GetAttribute(TfToken("cesium:georeferenceOrigin:height")).Get(&h);
        std::string crs;
        try
        {
            const json scene = ops::execute("georeference", json::object());
            if (scene.is_object())
                crs = scene.value("authority", scene.value("crs", std::string()));
        }
        catch (const std::exception&)
        {
        }
        if (crs.empty())
        {
            // No scene georeference yet: the UTM zone around the prim's origin, which becomes the scene origin.
            crs = std::format("EPSG:{}", (lat >= 0.0 ? 32600 : 32700) + std::clamp(int(std::floor((lon + 180.0) / 6.0)) + 1, 1, 60));
            ops::execute("setGeoreference", {{"crs", crs}, {"latLonHeight", {lat, lon, h}}});
        }
        const json projected = ops::execute("georeferenceConvert", {{"points", {{lat, lon, h}}}, {"from", "geographic"}, {"to", crs}});
        return json{{"crs", crs}, {"origin", projected[0]}};
    }
    return std::nullopt;
}

void placeByStageGeoreference(scene::Graph& graph, const UsdStageRefPtr& stage, ImportResult& r)
{
    const std::optional<json> geo = stageGeoreference(stage);
    if (!geo)
        return;
    json names = json::array();
    for (scene::NodeId id : r.roots)
        if (const auto* n = graph.node(id))
            names.push_back(n->name());
    try
    {
        ops::execute("georeferencePlace", {{"args", names}, {"crs", (*geo)["crs"]}, {"origin", (*geo)["origin"]}});
    }
    catch (const std::exception& e)
    {
        r.warnings.push_back(std::string("georeference not applied: ") + e.what());
    }
}

ImportResult importUsd(scene::Graph& graph, const fs::path& path, const ImportOptions& opts)
{
    LILA_ZONE();

    ImportResult r;
    tasks::Scope scope("Importing " + utf8(path.filename()), 0.0, progress::Cancellable);
    UsdStageRefPtr stage;
    std::vector<std::string> opening;
    {
        Diagnostics diag;
        stage = UsdStage::Open(utf8(path), opts.loadPayloads ? UsdStage::LoadAll : UsdStage::LoadNone);
        if (!stage)
        {
            std::vector<std::string> errors = diag.errors();
            r.error = "can't open " + utf8(path) + (errors.empty() ? std::string() : ": " + errors.front());
            return r;
        }

        // Take variants written by Lila's exporter.
        UsdPrim defaultPrim = stage->GetDefaultPrim();
        if (defaultPrim && defaultPrim.GetVariantSets().HasVariantSet("take"))
        {
            UsdVariantSet takes = defaultPrim.GetVariantSets().GetVariantSet("take");
            r.takes = takes.GetVariantNames();
            if (!opts.take.empty())
            {
                if (std::find(r.takes.begin(), r.takes.end(), opts.take) == r.takes.end())
                    r.warnings.push_back(std::format("no take named {}; imported {}", opts.take, takes.GetVariantSelection()));
                else
                {
                    UsdEditContext session(stage, stage->GetSessionLayer());
                    takes.SetVariantSelection(opts.take);
                }
            }
            r.take = takes.GetVariantSelection();
            for (const std::string& t : r.takes)
                if (t != r.take)
                    r.skipped.push_back(std::format("take {} (import it with take='{}')", t, t));
        }
        applyVariants(stage, opts, r);
        for (const std::string& e : diag.errors())
            opening.push_back("OpenUSD: " + e);
        for (const std::string& w : diag.warnings())
            opening.push_back("OpenUSD: " + w);
    }
    importStage(graph, stage, path, opts, r);
    if (opts.georeference && r.ok)
        placeByStageGeoreference(graph, stage, r);
    r.warnings.insert(r.warnings.end(), opening.begin(), opening.end());
    return r;
}

void importStage(scene::Graph& graph, const UsdStageRefPtr& stage, const fs::path& path, const ImportOptions& opts, ImportResult& r)
{
    LILA_ZONE();

    Diagnostics diag;
    UsdPrim defaultPrim = stage->GetDefaultPrim();
    const TfToken up = UsdGeomGetStageUpAxis(stage);
    r.upAxis = up.GetString();
    r.metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
    scene::Graph::Batch batch(graph);
    Importer im{graph, opts, r, stage, makeAxes(up != UsdGeomTokens->y, r.metersPerUnit)};
    im.tcps = stage->GetTimeCodesPerSecond() > 0.0 ? stage->GetTimeCodesPerSecond() : 24.0;
    im.held = stage->GetInterpolationType() == UsdInterpolationTypeHeld;
    if (stage->HasAuthoredTimeCodeRange())
        im.restTime = UsdTimeCode(stage->GetStartTimeCode());
    if (opts.materials)
        im.materials = std::make_unique<MaterialReader>(graph, path);

    std::vector<UsdPrim> tops;
    if (!opts.prim.empty())
    {
        UsdPrim p = SdfPath::IsValidPathString(opts.prim) ? stage->GetPrimAtPath(SdfPath(opts.prim)) : UsdPrim();
        if (!p)
        {
            r.error = "no prim at " + opts.prim;
            return;
        }
        tops.push_back(p);
        if (opts.shallow)
            im.shallowRoots.insert(p.GetPath());
    }
    else
        for (const UsdPrim& p : stage->GetPseudoRoot().GetFilteredChildren(kChildren))
            tops.push_back(p);

    Builder b{im};
    std::vector<std::pair<SdfPath, NodeId>> topPaths;
    for (const UsdPrim& p : tops)
        topPaths.push_back({p.GetPath(), opts.parent});
    for (const auto& [extra, parent] : opts.alsoBuild)
        if (SdfPath::IsValidPathString(extra))
        {
            topPaths.push_back({SdfPath(extra), parent});
            im.shallowRoots.insert(SdfPath(extra));
        }
    for (const auto& [p, parent] : topPaths)
        if (UsdPrim prim = stage->GetPrimAtPath(p))
            b.build(prim, parent);
    b.reportUnkept();

    im.bindAll();

    if (opts.animation)
    {
        for (const auto& [primPath, node] : im.animated)
        {
            const UsdPrim prim = stage->GetPrimAtPath(primPath);
            if (!prim)
                continue;
            UsdGeomXformable xf(prim);
            std::vector<double> samples;
            xf.GetTimeSamples(&samples);
            if (samples.size() < 2)
                continue;
            std::vector<double> times;
            std::vector<glm::dvec3> t, rr, s;
            glm::dvec3 ref = graph.node(node)->as<glm::dvec3>(scene::Transform::kRotate);
            bool exact = true, sheared = false;
            // Exact channels only when every sample reads them; otherwise every sample is decomposed alike.
            for (double tc : samples)
            {
                LocalChannels c = localChannels(im, xf, node, UsdTimeCode(tc), ref, true);
                exact = exact && c.exact;
            }
            for (double tc : samples)
            {
                LocalChannels c = localChannels(im, xf, node, UsdTimeCode(tc), ref, exact);
                ref = c.r;
                sheared = sheared || c.sheared;
                times.push_back(im.seconds(tc));
                t.push_back(c.t);
                rr.push_back(c.r);
                s.push_back(c.s);
            }
            if (sheared)
                r.warnings.push_back(std::format("{}: shear dropped from its animation", prim.GetPath().GetString()));
            json recorded;
            const VtDictionary d = lilaData(xf.GetXformOpOrderAttr());
            if (auto it = d.find("curves"); it != d.end() && it->second.IsHolding<std::string>())
                recorded = json::parse(it->second.UncheckedGet<std::string>(), nullptr, false);
            keyChannels(im, node, times, t, rr, s, recorded.is_object() ? &recorded : nullptr);
        }
        im.keySkeletons();
    }

    r.nodeOf = im.nodeOf;
    if (opts.lightingChannels && !opts.live)
        importLightLinks(graph, stage, r);
    if (im.materials)
    {
        r.materialSlots = im.materials->slots();
        r.materials = im.materials->created;
        r.warnings.insert(r.warnings.end(), im.materials->warnings.begin(), im.materials->warnings.end());
        r.skipped.insert(r.skipped.end(), im.materials->skipped.begin(), im.materials->skipped.end());
    }

    // The take's own range from Lila's exporter, else the stage's, else the keys'.
    r.fps = stage->GetFramesPerSecond() > 0.0 ? stage->GetFramesPerSecond() : im.tcps;
    bool ranged = false;
    if (defaultPrim)
    {
        const VtDictionary d = lilaData(defaultPrim);
        if (auto it = d.find("takes"); it != d.end() && it->second.IsHolding<VtDictionary>())
        {
            const VtDictionary& takes = it->second.UncheckedGet<VtDictionary>();
            if (auto t = takes.find(r.take); t != takes.end() && t->second.IsHolding<VtDictionary>())
            {
                const VtDictionary& range = t->second.UncheckedGet<VtDictionary>();
                auto num = [&](const char* k, double fallback) {
                    auto f = range.find(k);
                    return f != range.end() && f->second.IsHolding<double>() ? f->second.UncheckedGet<double>() : fallback;
                };
                r.fps = num("fps", r.fps);
                r.start = num("start", 0.0);
                r.end = num("end", 0.0);
                ranged = true;
            }
        }
    }
    if (!ranged && stage->HasAuthoredTimeCodeRange())
    {
        r.start = im.seconds(stage->GetStartTimeCode());
        r.end = im.seconds(stage->GetEndTimeCode());
        ranged = true;
    }
    if (!ranged && im.firstKey <= im.lastKey)
    {
        r.start = im.firstKey;
        r.end = im.lastKey;
        ranged = true;
    }
    if (opts.setPlayback && ranged && r.end >= r.start)
    {
        scene::anim::Playback& pb = scene::anim::playback();
        pb.fps = r.fps;
        pb.minTime = pb.animStart = r.start;
        pb.maxTime = pb.animEnd = r.end;
        scene::anim::playbackChanged();
    }

    for (const std::string& e : diag.errors())
        r.warnings.push_back("OpenUSD: " + e);
    for (const std::string& w : diag.warnings())
        r.warnings.push_back("OpenUSD: " + w);
    r.ok = true;
}

void deleteWithInputs(scene::Graph& graph, NodeId id)
{
    if (!graph.node(id))
        return;
    std::set<NodeId> doomed{id};
    for (NodeId d : graph.descendants(id))
        doomed.insert(d);
    std::unordered_map<NodeId, std::set<NodeId>> feeds;
    for (const auto& [dst, src] : graph.connections())
        feeds[src.node].insert(dst.node);
    std::vector<NodeId> inputs;
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const auto& [src, dsts] : feeds)
        {
            const scene::Node* n = graph.node(src);
            if (doomed.count(src) || !n || n->type().dag)
                continue;
            if (std::all_of(dsts.begin(), dsts.end(), [&](NodeId d) { return doomed.count(d) > 0; }))
            {
                doomed.insert(src);
                inputs.push_back(src);
                grew = true;
            }
        }
    }
    graph.deleteNode(id);
    for (NodeId n : inputs)
        graph.deleteNode(n);
}
} // namespace lila::usd
