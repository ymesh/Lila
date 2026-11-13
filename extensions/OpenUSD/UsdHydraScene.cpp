#include "UsdHydraScene.h"

#include "UsdCommon.h"
#include "UsdMaterials.h"

#include <lila/Interchange.h>
#include <lila/Lights.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/extentSchema.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd::hydra
{
namespace
{
using scene::Graph;
using scene::NodeId;

template <class T> typename HdRetainedTypedSampledDataSource<T>::Handle retained(const T& v)
{
    return HdRetainedTypedSampledDataSource<T>::New(v);
}

HdContainerDataSourceHandle container(const std::vector<std::pair<TfToken, HdDataSourceBaseHandle>>& entries)
{
    std::vector<TfToken> names;
    std::vector<HdDataSourceBaseHandle> values;
    for (const auto& [n, v] : entries)
        if (v)
        {
            names.push_back(n);
            values.push_back(v);
        }
    return HdRetainedContainerDataSource::New(names.size(), names.data(), values.data());
}

HdContainerDataSourceHandle xformSource(const glm::dmat4& m)
{
    return HdXformSchema::Builder().SetMatrix(retained(toGf(m))).SetResetXformStack(retained(true)).Build();
}

HdContainerDataSourceHandle bindingSource(const SdfPath& material)
{
    // Every purpose, since delegates ask for theirs (Storm preview, final-frame renderers full) without falling back.
    const HdDataSourceBaseHandle binding = HdMaterialBindingSchema::Builder().SetPath(retained(material)).Build();
    const TfToken purposes[3] = {HdMaterialBindingsSchemaTokens->allPurpose, HdTokens->preview, HdTokens->full};
    const HdDataSourceBaseHandle bindings[3] = {binding, binding, binding};
    return HdMaterialBindingsSchema::BuildRetained(3, purposes, bindings);
}

HdContainerDataSourceHandle primvar(const VtValue& value, const TfToken& interpolation, const TfToken& role)
{
    return HdPrimvarSchema::Builder()
        .SetPrimvarValue(HdRetainedSampledDataSource::New(value))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
        .SetRole(role.IsEmpty() ? nullptr : HdPrimvarSchema::BuildRoleDataSource(role))
        .Build();
}

size_t mix(size_t h, size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }
size_t hashOf(double v) { return std::hash<double>()(v); }
size_t hashOf(const glm::dvec3& v) { return mix(mix(hashOf(v.x), hashOf(v.y)), hashOf(v.z)); }

/** Whether Hio can read the image at path, remembered until the file changes. */
bool readableImage(const fs::path& path)
{
    static std::map<fs::path, std::pair<fs::file_time_type, bool>> known;
    std::error_code ec;
    const fs::file_time_type written = fs::last_write_time(path, ec);
    if (ec)
        return false;
    auto it = known.find(path);
    if (it == known.end() || it->second.first != written)
        it = known.insert_or_assign(path, std::pair{written, HioImage::OpenForReading(paths::toUtf8(path)) != nullptr}).first;
    return it->second.second;
}

/**
 * A dome image as a path Hydra can open: Lila's own HDRIs are stored by file name, the rest as scene paths. Empty when
 * the file is missing or unreadable, since Storm faults the GPU filtering a dome texture it could not load.
 */
std::string domeTexture(const std::string& stored)
{
    std::error_code ec;
    fs::path path = paths::fromUtf8(stored);
    if (path.is_relative())
        if (fs::path shipped = paths::resourcesDir() / "hdri" / path; fs::exists(shipped, ec))
            path = shipped;
    if (path.is_relative())
        path = paths::resolvePath(stored);
    return fs::is_regular_file(path, ec) && readableImage(path) ? paths::toUtf8(path) : std::string();
}

/** A UsdLux-style light: sphere (points and spots, shaped by a cone), distant, rect, disk or dome. */
std::pair<TfToken, HdContainerDataSourceHandle> lightSource(const scene::Node& n, size_t& hash)
{
    using scene::lights::Kind;
    const scene::lights::Settings l = scene::lights::settings(n);
    hash = mix(mix(mix(size_t(l.kind), hashOf(l.color)), hashOf(l.intensity)), mix(hashOf(l.sourceRadius), hashOf(l.width)));
    hash = mix(mix(hash, hashOf(l.height)), mix(hashOf(l.outerConeAngle), hashOf(l.innerConeAngle)));
    hash = mix(mix(hash, std::hash<std::string>()(l.texture)), mix(size_t(l.castShadows), mix(size_t(l.useTemperature), hashOf(l.temperature))));
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> p = {
        {HdLightTokens->intensity, retained(float(l.intensity))},
        {HdLightTokens->exposure, retained(0.0f)},
        {HdLightTokens->color, retained(GfVec3f(float(l.color.x), float(l.color.y), float(l.color.z)))},
        {HdLightTokens->enableColorTemperature, retained(l.useTemperature)},
        {HdLightTokens->colorTemperature, retained(float(l.temperature))},
        {HdLightTokens->normalize, retained(false)},
        {HdLightTokens->diffuse, retained(1.0f)},
        {HdLightTokens->specular, retained(1.0f)},
        {HdLightTokens->shadowEnable, retained(l.castShadows)},
    };
    TfToken type;
    switch (l.kind)
    {
    case Kind::Point:
    case Kind::Spot:
        type = HdPrimTypeTokens->sphereLight;
        p.push_back({HdLightTokens->radius, retained(float(l.sourceRadius))});
        p.push_back({TfToken("treatAsPoint"), retained(l.sourceRadius <= 0.0)});
        if (l.kind == Kind::Spot)
        {
            p.push_back({HdLightTokens->shapingConeAngle, retained(float(l.outerConeAngle))});
            p.push_back({HdLightTokens->shapingConeSoftness,
                         retained(float(l.outerConeAngle > 0.0 ? 1.0 - l.innerConeAngle / l.outerConeAngle : 0.0))});
        }
        break;
    case Kind::Directional: type = HdPrimTypeTokens->distantLight; break;
    case Kind::Area:
        type = HdPrimTypeTokens->rectLight;
        p.push_back({HdLightTokens->width, retained(float(l.width))});
        p.push_back({HdLightTokens->height, retained(float(l.height))});
        break;
    case Kind::Disc:
        type = HdPrimTypeTokens->diskLight;
        p.push_back({HdLightTokens->radius, retained(float(l.width * 0.5))});
        break;
    case Kind::Dome:
        type = HdPrimTypeTokens->domeLight;
        if (const std::string file = l.texture.empty() ? std::string() : domeTexture(l.texture); !file.empty())
            p.push_back({HdLightTokens->textureFile, retained(SdfAssetPath(file, file))});
        break;
    }
    return {type, container(p)};
}

/** Hydra's apertures and focal length are in scene units: Lila's inches and mm as cm. */
HdContainerDataSourceHandle cameraSource(const scene::Node& n, size_t& hash)
{
    using scene::Camera;
    const bool ortho = n.as<bool>(Camera::kOrthographic);
    const double hfa = n.as<double>(Camera::kHorizontalFilmAperture), vfa = n.as<double>(Camera::kVerticalFilmAperture);
    const double focal = n.as<double>(Camera::kFocalLength), width = n.as<double>(Camera::kOrthographicWidth);
    const double nearClip = n.as<double>(Camera::kNearClipPlane), farClip = n.as<double>(Camera::kFarClipPlane);
    hash = mix(mix(mix(size_t(ortho), hashOf(hfa)), mix(hashOf(vfa), hashOf(focal))), mix(hashOf(width), mix(hashOf(nearClip), hashOf(farClip))));
    const float h = float(ortho ? width : hfa * 2.54);
    const float v = float(ortho ? width * vfa / std::max(hfa, 1e-6) : vfa * 2.54);
    return HdCameraSchema::Builder()
        .SetProjection(HdCameraSchema::BuildProjectionDataSource(ortho ? HdCameraSchemaTokens->orthographic : HdCameraSchemaTokens->perspective))
        .SetHorizontalAperture(retained(h))
        .SetVerticalAperture(retained(v))
        .SetFocalLength(retained(float(focal * 0.1)))
        .SetClippingRange(retained(GfVec2f(float(nearClip), float(farClip))))
        .Build();
}

void addEntry(HdSceneIndexObserver::AddedPrimEntries& out, const SdfPath& p, const TfToken& type) { out.emplace_back(p, type); }
} // namespace

SdfPath stageRoot(NodeId stageNode) { return SdfPath(std::format("/Stages/S{}", stageNode)); }

bool drawnByLila(const scene::Node& n)
{
    if (n.type().isA("mesh"))
    {
        const scene::MeshPtr& m = n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        return m && !m->empty();
    }
    if (n.type().isA("curve"))
        return !curve::splinesOf(n.value(curve::CurveShape::kLocal)).empty();
    return n.type().isA("camera") || n.type().isA("light");
}

// ---------------------------------------------------------------- LilaSceneIndex

struct LilaSceneIndex::MeshData
{
    struct FaceVarying
    {
        TfToken name, role;
        VtValue value;
    };
    VtVec3fArray points;
    std::vector<FaceVarying> faceVarying;
    HdContainerDataSourceHandle extent;
    VtIntArray counts, indices;
    /** Faces per section, when there are several; each section's material slot. */
    std::vector<VtIntArray> subsets;
    std::vector<std::string> slots;
    /** Smooth preview level: Catmull-Clark refined this many times, 0 for the cage as it is. */
    int level = 0;
    /** The cage face of each face Hydra gets, for isolate select's face masks. */
    std::vector<geo::Index> cageFace;
    glm::dvec3 centre{0.0};

    bool sameTopology(const MeshData& o) const
    {
        return level == o.level && counts == o.counts && indices == o.indices && subsets == o.subsets && slots == o.slots;
    }

    /** What a node's prim gets: the mesh less the faces isolate select hides (left out, since Storm ignores holes). */
    struct Shape
    {
        HdContainerDataSourceHandle mesh, primvars;
        std::vector<VtIntArray> subsets;
    };
    Shape shape(const VtIntArray& hidden) const
    {
        Shape out;
        VtIntArray c = counts, i = indices;
        std::vector<FaceVarying> fv = faceVarying;
        out.subsets = subsets;
        if (!hidden.empty())
        {
            std::vector<char> drop(counts.size(), 0);
            for (int f : hidden)
                drop[size_t(f)] = 1;
            std::vector<int> newIndex(counts.size(), -1);
            std::vector<size_t> keptCorners;
            c.clear();
            i.clear();
            size_t corner = 0;
            for (size_t f = 0; f < counts.size(); ++f)
            {
                const size_t n = size_t(counts[f]);
                if (!drop[f])
                {
                    newIndex[f] = int(c.size());
                    c.push_back(int(n));
                    for (size_t k = 0; k < n; ++k)
                    {
                        i.push_back(indices[corner + k]);
                        keptCorners.push_back(corner + k);
                    }
                }
                corner += n;
            }
            auto keep = [&](const auto& in) {
                std::decay_t<decltype(in)> o(keptCorners.size());
                for (size_t k = 0; k < keptCorners.size(); ++k)
                    o[k] = in[keptCorners[k]];
                return o;
            };
            for (FaceVarying& v : fv)
            {
                if (v.value.IsHolding<VtVec3fArray>())
                    v.value = VtValue(keep(v.value.UncheckedGet<VtVec3fArray>()));
                else if (v.value.IsHolding<VtVec2fArray>())
                    v.value = VtValue(keep(v.value.UncheckedGet<VtVec2fArray>()));
                else if (v.value.IsHolding<VtFloatArray>())
                    v.value = VtValue(keep(v.value.UncheckedGet<VtFloatArray>()));
            }
            for (VtIntArray& sub : out.subsets)
            {
                VtIntArray kept;
                for (int f : sub)
                    if (newIndex[size_t(f)] >= 0)
                        kept.push_back(newIndex[size_t(f)]);
                sub = kept;
            }
        }
        out.mesh = HdMeshSchema::Builder()
                       .SetTopology(HdMeshTopologySchema::Builder()
                                        .SetFaceVertexCounts(retained(c))
                                        .SetFaceVertexIndices(retained(i))
                                        .SetOrientation(HdMeshTopologySchema::BuildOrientationDataSource(HdMeshTopologySchemaTokens->rightHanded))
                                        .Build())
                       .SetSubdivisionScheme(retained(PxOsdOpenSubdivTokens->none))
                       .SetDoubleSided(retained(true))
                       .Build();
        std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> pv = {
            {HdTokens->points, primvar(VtValue(points), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point)}};
        for (const FaceVarying& v : fv)
            pv.push_back({v.name, primvar(v.value, HdPrimvarSchemaTokens->faceVarying, v.role)});
        out.primvars = container(pv);
        return out;
    }
};

struct LilaSceneIndex::CurveData
{
    const void* source = nullptr;
    int64_t mode = -1, segments = -1;
    VtIntArray counts;
    VtVec3fArray points;
    HdContainerDataSourceHandle curves, extent;
    glm::dvec3 centre{0.0};
};

std::shared_ptr<const LilaSceneIndex::MeshData> LilaSceneIndex::meshData(const scene::MeshPtr& mesh, int level)
{
    if (auto it = mMeshes.find({mesh.get(), level}); it != mMeshes.end())
        return it->second.second;
    LILA_ZONE();
    auto d = std::make_shared<MeshData>();
    d->level = level;
    // Subdivided here as Lila's smooth preview does it, so every delegate draws the same smooth shaded surface.
    geo::Mesh refined;
    geo::SubdivideMap map;
    if (level > 0)
    {
        geo::SubdivideOptions so;
        so.levels = level;
        so.uvs = geo::SubdivideUvs::Blend;
        refined = geo::subdivide(*mesh, so, &map);
    }
    const geo::Mesh& m = level > 0 ? refined : *mesh;
    geo::RenderMeshOptions o;
    o.wireframe = false;
    const geo::RenderMesh rm = geo::buildRenderMesh(m, o);

    std::vector<int> pointIndex(m.pointCapacity(), -1);
    VtVec3fArray points;
    points.reserve(m.numPoints());
    GfRange3d bounds;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            pointIndex[p] = int(points.size());
            const glm::dvec3& v = m.position(p);
            points.push_back(GfVec3f(float(v.x), float(v.y), float(v.z)));
            bounds.UnionWith(GfVec3d(v.x, v.y, v.z));
        }

    // Faces in index order; values per corner come from the render mesh's vertex for that corner, which already holds the
    // split normals, UV seams and colours Lila's viewport draws with.
    std::vector<int> faceIndex(m.faceCapacity(), -1);
    std::vector<geo::Index> vertexOfCorner;
    vertexOfCorner.reserve(m.numCorners());
    d->counts.reserve(m.numFaces());
    d->indices.reserve(m.numCorners());
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        faceIndex[f] = int(d->counts.size());
        d->cageFace.push_back(level > 0 ? (f < map.faces.size() ? map.faces[f] : geo::kInvalid) : f);
        d->counts.push_back(int(m.faceSize(f)));
        m.forEachFaceCorner(f, [&](geo::Index c) {
            d->indices.push_back(pointIndex[m.cornerPoint(c)]);
            vertexOfCorner.push_back(c < rm.cornerVertex.size() ? rm.cornerVertex[c] : geo::kInvalid);
        });
    }
    const size_t corners = vertexOfCorner.size();
    auto perCorner = [&](auto&& value) {
        using T = std::decay_t<decltype(value(geo::Index(0)))>;
        VtArray<T> out(corners);
        for (size_t i = 0; i < corners; ++i)
            out[i] = vertexOfCorner[i] == geo::kInvalid ? T() : value(vertexOfCorner[i]);
        return out;
    };

    d->points = std::move(points);
    auto& pv = d->faceVarying;
    if (rm.normals.size() == rm.positions.size())
        pv.push_back({HdTokens->normals, HdPrimvarSchemaTokens->normal,
                      VtValue(perCorner([&](geo::Index v) { return GfVec3f(rm.normals[v].x, rm.normals[v].y, rm.normals[v].z); }))});
    for (size_t s = 0; s < rm.uvs.size(); ++s)
        pv.push_back({TfToken(uvPrimvarName(int(s))), HdPrimvarSchemaTokens->textureCoordinate,
                      VtValue(perCorner([&](geo::Index v) { return GfVec2f(rm.uvs[s][v].x, rm.uvs[s][v].y); }))});
    if (rm.colors.size() == rm.positions.size() && !rm.colors.empty())
    {
        pv.push_back({HdTokens->displayColor, HdPrimvarSchemaTokens->color,
                      VtValue(perCorner([&](geo::Index v) { return GfVec3f(rm.colors[v].r, rm.colors[v].g, rm.colors[v].b); }))});
        pv.push_back({HdTokens->displayOpacity, TfToken(), VtValue(perCorner([&](geo::Index v) { return rm.colors[v].a; }))});
    }

    for (const geo::RenderSection& s : rm.sections)
        d->slots.push_back(scene::materials::slotName(m, s.material));
    if (d->slots.empty())
        d->slots.push_back(scene::materials::slotName(m, 0));
    if (rm.sections.size() > 1)
        for (const geo::RenderSection& s : rm.sections)
        {
            std::set<int> faces;
            for (geo::Index t = s.firstIndex / 3; t < (s.firstIndex + s.indexCount) / 3; ++t)
                if (const geo::Index f = rm.triangleFaces[t]; f < faceIndex.size() && faceIndex[f] >= 0)
                    faces.insert(faceIndex[f]);
            d->subsets.emplace_back(faces.begin(), faces.end());
        }

    if (!bounds.IsEmpty())
    {
        d->extent = HdExtentSchema::Builder().SetMin(retained(bounds.GetMin())).SetMax(retained(bounds.GetMax())).Build();
        const GfVec3d c = bounds.GetMidpoint();
        d->centre = {c[0], c[1], c[2]};
    }
    mMeshes[{mesh.get(), level}] = {mesh, d};
    return d;
}

std::shared_ptr<const LilaSceneIndex::CurveData> LilaSceneIndex::curveData(const scene::Node& n)
{
    using curve::CurveShape;
    const scene::PlugValue& v = n.value(CurveShape::kLocal);
    const scene::DataPtr* data = std::get_if<scene::DataPtr>(&v);
    const void* source = data ? data->get() : nullptr;
    const int64_t mode = n.as<int64_t>(CurveShape::kTessMode), segments = n.as<int64_t>(CurveShape::kTessSegments);
    if (auto it = mNodes.find(n.id()); it != mNodes.end() && it->second.curve && it->second.curve->source == source &&
                                       it->second.curve->mode == mode && it->second.curve->segments == segments)
        return it->second.curve;
    // Tessellated as the viewport's curve drawer does; a closed line repeats its first point.
    curve::TessOptions o;
    o.mode = curve::TessMode(std::clamp<int64_t>(mode, 0, 2));
    o.perSpan = int(std::max<int64_t>(1, segments));
    o.chordError = 0.01;
    o.maxAngle = 0.07;
    auto d = std::make_shared<CurveData>();
    d->source = source;
    d->mode = mode;
    d->segments = segments;
    GfRange3d bounds;
    for (const curve::Spline& s : curve::splinesOf(v))
    {
        const curve::Polyline pl = curve::tessellate(s, o);
        if (pl.size() < 2)
            continue;
        const size_t count = pl.size() + (pl.closed ? 1 : 0);
        d->counts.push_back(int(count));
        for (size_t i = 0; i < count; ++i)
        {
            const glm::dvec3& p = pl.points[i % pl.size()];
            d->points.push_back(GfVec3f(float(p.x), float(p.y), float(p.z)));
            bounds.UnionWith(GfVec3d(p.x, p.y, p.z));
        }
    }
    if (d->counts.empty())
        return nullptr;
    d->curves = HdBasisCurvesSchema::Builder()
                    .SetTopology(HdBasisCurvesTopologySchema::Builder()
                                     .SetCurveVertexCounts(retained(d->counts))
                                     .SetBasis(retained(HdTokens->bezier))
                                     .SetType(retained(HdTokens->linear))
                                     .SetWrap(retained(HdTokens->nonperiodic))
                                     .Build())
                    .Build();
    d->extent = HdExtentSchema::Builder().SetMin(retained(bounds.GetMin())).SetMax(retained(bounds.GetMax())).Build();
    const GfVec3d c = bounds.GetMidpoint();
    d->centre = {c[0], c[1], c[2]};
    return d;
}

HdContainerDataSourceHandle LilaSceneIndex::nodeSource(const Node& n) const
{
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> e = {
        {HdXformSchema::GetSchemaToken(), xformSource(glm::translate(glm::dmat4(1.0), -mOrigin) * n.world)},
        {HdVisibilitySchema::GetSchemaToken(), HdVisibilitySchema::BuildRetained(retained(n.visible))},
    };
    if (n.mesh)
    {
        e.push_back({HdMeshSchema::GetSchemaToken(), n.shape.mesh});
        e.push_back({HdPrimvarsSchema::GetSchemaToken(), n.shape.primvars});
        e.push_back({HdExtentSchema::GetSchemaToken(), n.mesh->extent});
        if (n.bindings.size() == 1)
            e.push_back({HdMaterialBindingsSchema::GetSchemaToken(), bindingSource(n.bindings[0])});
    }
    else if (n.curve)
    {
        e.push_back({HdBasisCurvesSchema::GetSchemaToken(), n.curve->curves});
        e.push_back({HdExtentSchema::GetSchemaToken(), n.curve->extent});
        e.push_back({HdPrimvarsSchema::GetSchemaToken(),
                     container({{HdTokens->points, primvar(VtValue(n.curve->points), HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point)},
                                {HdTokens->widths, primvar(VtValue(VtFloatArray(1, n.width)), HdPrimvarSchemaTokens->constant, TfToken())},
                                {HdTokens->displayColor, n.own}})});
    }
    else if (n.type == HdPrimTypeTokens->camera)
        e.push_back({HdCameraSchema::GetSchemaToken(), n.own});
    else
        e.push_back({HdLightSchemaTokens->light, n.own});
    return container(e);
}

void LilaSceneIndex::put(const SdfPath& path, Prim prim)
{
    mPrims[path] = std::move(prim);
    for (SdfPath p = path; !p.IsAbsoluteRootPath(); p = p.GetParentPath())
        if (!mChildren[p.GetParentPath()].insert(p).second)
            break;
}

void LilaSceneIndex::drop(const SdfPath& path)
{
    for (auto it = mPrims.lower_bound(path); it != mPrims.end() && it->first.HasPrefix(path);)
        it = mPrims.erase(it);
    for (auto it = mChildren.lower_bound(path); it != mChildren.end() && it->first.HasPrefix(path);)
        it = mChildren.erase(it);
    if (auto parent = mChildren.find(path.GetParentPath()); parent != mChildren.end())
        parent->second.erase(path);
}

HdSceneIndexPrim LilaSceneIndex::GetPrim(const SdfPath& path) const
{
    if (auto it = mPrims.find(path); it != mPrims.end())
        return {it->second.type, it->second.ds};
    return {TfToken(), nullptr};
}

SdfPathVector LilaSceneIndex::GetChildPrimPaths(const SdfPath& path) const
{
    if (auto it = mChildren.find(path); it != mChildren.end())
        return SdfPathVector(it->second.begin(), it->second.end());
    return {};
}

float LilaSceneIndex::curveWidth(const Node& n, const ViewContext& view, float was)
{
    // Lila draws curves a few pixels wide whatever their distance; Hydra's widths are in scene units.
    const double perPixel = view.unitsPerPixel ? view.unitsPerPixel(glm::dvec3(n.world * glm::dvec4(n.curve->centre, 1.0))) : 0.1;
    const float width = float(std::max(perPixel, 1e-4) * view.curvePixels);
    return was > 0.0f && std::abs(width / was - 1.0f) < 0.15f ? was : width;
}

void LilaSceneIndex::setOrigin(const glm::dvec3& origin)
{
    if (origin == mOrigin)
        return;
    LILA_ZONE();
    mOrigin = origin;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    HdDataSourceLocatorSet locators;
    locators.insert(HdXformSchema::GetDefaultLocator());
    for (auto& [id, n] : mNodes)
    {
        put(n.path, {n.type, nodeSource(n)});
        dirtied.emplace_back(n.path, locators);
    }
    if (!dirtied.empty())
        _SendPrimsDirtied(dirtied);
}

void LilaSceneIndex::syncView(const ViewContext& view)
{
    LILA_ZONE();
    mCounts = {};
    setOrigin(view.origin);
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    for (auto& [id, n] : mNodes)
    {
        if (!n.curve)
            continue;
        const float width = curveWidth(n, view, n.width);
        if (width == n.width)
            continue;
        n.width = width;
        put(n.path, {n.type, nodeSource(n)});
        HdDataSourceLocatorSet locators;
        locators.insert(HdPrimvarsSchema::GetDefaultLocator());
        locators.insert(HdExtentSchema::GetDefaultLocator());
        dirtied.emplace_back(n.path, locators);
        ++mCounts.dirtied;
    }
    if (!dirtied.empty())
        _SendPrimsDirtied(dirtied);
}

void LilaSceneIndex::sync(Graph& g, const ViewContext& view, const std::function<SdfPath(const std::string&)>& bind)
{
    LILA_ZONE();
    mCounts = {};
    setOrigin(view.origin);
    HdSceneIndexObserver::AddedPrimEntries added;
    HdSceneIndexObserver::RemovedPrimEntries removed;
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    std::unordered_map<NodeId, Node> next;
    std::set<std::pair<const void*, int>> usedMeshes;
    mSlots.clear();

    if (!mPrims.count(nodesRoot()))
    {
        put(nodesRoot(), {TfToken(), nullptr});
        addEntry(added, nodesRoot(), TfToken());
    }
    const HdContainerDataSourceHandle curveColor =
        primvar(VtValue(VtVec3fArray(1, GfVec3f(view.curveColor.r, view.curveColor.g, view.curveColor.b))), HdPrimvarSchemaTokens->constant,
                HdPrimvarSchemaTokens->color);

    for (NodeId id : g.nodes())
    {
        const scene::Node* sn = g.node(id);
        if (!sn || !drawnByLila(*sn))
            continue;
        Node n;
        n.path = nodesRoot().AppendChild(TfToken(std::format("N{}", id)));
        n.world = sn->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        n.visible = scene::effectivelyVisible(g, id) && (!view.isolation || view.isolation->shows(id));
        auto was = mNodes.find(id);
        if (sn->type().isA("mesh"))
        {
            const scene::MeshPtr& m = sn->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            n.type = HdPrimTypeTokens->mesh;
            n.mesh = meshData(m, int(std::clamp<int64_t>(sn->as<int64_t>(scene::MeshNode::kSmoothPreview), 0, 4)));
            usedMeshes.insert({m.get(), n.mesh->level});
            for (const std::string& slot : n.mesh->slots)
            {
                mSlots.insert(slot);
                n.bindings.push_back(bind(slot));
            }
            if (view.isolation && view.isolation->isPartial(id))
            {
                const std::vector<uint8_t> shown = view.isolation->shownFaces(id, *m);
                for (size_t i = 0; i < n.mesh->cageFace.size(); ++i)
                    if (const geo::Index f = n.mesh->cageFace[i]; f < shown.size() && !shown[f])
                        n.holes.push_back(int(i));
            }
            if (was != mNodes.end() && was->second.mesh == n.mesh && was->second.holes == n.holes)
                n.shape = was->second.shape;
            else
            {
                MeshData::Shape sh = n.mesh->shape(n.holes);
                n.shape.mesh = sh.mesh;
                n.shape.primvars = sh.primvars;
                n.shape.subsets = std::move(sh.subsets);
            }
        }
        else if (sn->type().isA("curve"))
        {
            n.type = HdPrimTypeTokens->basisCurves;
            n.curve = curveData(*sn);
            if (!n.curve)
                continue;
            n.own = curveColor;
            n.width = curveWidth(n, view, was != mNodes.end() ? was->second.width : 0.0f);
        }
        else if (sn->type().isA("camera"))
        {
            n.type = HdPrimTypeTokens->camera;
            n.own = cameraSource(*sn, n.settings);
        }
        else
            std::tie(n.type, n.own) = lightSource(*sn, n.settings);

        const bool fresh = was == mNodes.end() || was->second.type != n.type ||
                           (n.mesh && was->second.mesh != n.mesh && !was->second.mesh->sameTopology(*n.mesh)) ||
                           (n.curve && was->second.curve != n.curve && was->second.curve->counts != n.curve->counts) ||
                           was->second.bindings != n.bindings || was->second.holes != n.holes;
        HdDataSourceLocatorSet locators;
        if (!fresh)
        {
            const Node& o = was->second;
            if (o.world != n.world)
                locators.insert(HdXformSchema::GetDefaultLocator());
            if (o.visible != n.visible)
                locators.insert(HdVisibilitySchema::GetDefaultLocator());
            if (o.mesh != n.mesh || o.curve != n.curve || o.width != n.width)
            {
                locators.insert(HdPrimvarsSchema::GetDefaultLocator());
                locators.insert(HdExtentSchema::GetDefaultLocator());
            }
            if (o.settings != n.settings)
                locators.insert(n.type == HdPrimTypeTokens->camera ? HdCameraSchema::GetDefaultLocator()
                                                                    : HdDataSourceLocator(HdLightSchemaTokens->light));
            if (locators.IsEmpty())
            {
                n.own = o.own;
                next.emplace(id, std::move(n));
                continue;
            }
        }
        else if (was != mNodes.end())
        {
            // Re-added from scratch: its old geometry subsets go first.
            drop(was->second.path);
            removed.emplace_back(was->second.path);
        }
        put(n.path, {n.type, nodeSource(n)});
        if (fresh)
        {
            addEntry(added, n.path, n.type);
            ++mCounts.added;
            if (n.mesh && n.shape.subsets.size() > 1)
                for (size_t s = 0; s < n.shape.subsets.size(); ++s)
                {
                    const SdfPath sub = n.path.AppendChild(TfToken(std::format("S{}", s)));
                    put(sub, {HdPrimTypeTokens->geomSubset,
                              container({{HdGeomSubsetSchema::GetSchemaToken(),
                                          HdGeomSubsetSchema::Builder()
                                              .SetType(HdGeomSubsetSchema::BuildTypeDataSource(HdGeomSubsetSchemaTokens->typeFaceSet))
                                              .SetIndices(retained(n.shape.subsets[s]))
                                              .Build()},
                                         {HdMaterialBindingsSchema::GetSchemaToken(), bindingSource(n.bindings[s])}})});
                    addEntry(added, sub, HdPrimTypeTokens->geomSubset);
                }
        }
        else
        {
            dirtied.emplace_back(n.path, locators);
            ++mCounts.dirtied;
        }
        next.emplace(id, std::move(n));
    }

    for (auto& [id, n] : mNodes)
        if (!next.count(id))
        {
            drop(n.path);
            removed.emplace_back(n.path);
            ++mCounts.removed;
        }
    mNodes = std::move(next);
    for (auto it = mMeshes.begin(); it != mMeshes.end();)
        it = usedMeshes.count(it->first) ? std::next(it) : mMeshes.erase(it);

    if (!removed.empty())
        _SendPrimsRemoved(removed);
    if (!added.empty())
        _SendPrimsAdded(added);
    if (!dirtied.empty())
        _SendPrimsDirtied(dirtied);
}

// ---------------------------------------------------------------- MaterialLibrary

MaterialLibrary::MaterialLibrary() : mStage(UsdStage::CreateInMemory("lilaMaterials.usda")) {}

SdfPath MaterialLibrary::bind(const std::string& slot) const
{
    return materialsRoot().AppendChild(TfToken("Materials")).AppendChild(TfToken(identifier(slot, "Material")));
}

void MaterialLibrary::changed(Graph& g, scene::Change c, NodeId id)
{
    using scene::Change;
    if (c == Change::Reset || c == Change::Cleared)
    {
        mAllStale = true;
        return;
    }
    const scene::Node* n = g.node(id);
    if (c == Change::Deleted || c == Change::Renamed || c == Change::Created)
    {
        if (!n || n->type().isA("material"))
            mAllStale = true;
        return;
    }
    if (n && n->type().isA("material") && (c == Change::Value || c == Change::Connection || c == Change::Attrs))
        mStale.insert(n->name());
}

void MaterialLibrary::sync(Graph& g, const std::set<std::string>& used)
{
    LILA_ZONE();
    std::vector<std::string> write;
    for (const std::string& slot : used)
        if (mAllStale || !mWritten.count(slot) || mStale.count(slot))
            write.push_back(slot);
    mAllStale = false;
    mStale.clear();
    if (write.empty())
        return;
    const SdfPath scope("/Materials");
    MaterialWriter w(g, mStage, scope, fs::temp_directory_path() / "lilaHydraMaterials.usda", TextureMode::Absolute);
    for (const std::string& slot : write)
    {
        mStage->RemovePrim(scope.AppendChild(TfToken(identifier(slot, "Material"))));
        w.material(slot);
        mWritten.insert(slot);
    }
}

// ---------------------------------------------------------------- StageOverrides

bool StageOverrides::insidePruned(const SdfPath& path) const
{
    for (const SdfPath& p : mPruned)
        if (path != p && path.HasPrefix(p))
            return true;
    return false;
}

HdSceneIndexPrim StageOverrides::GetPrim(const SdfPath& path) const
{
    if (insidePruned(path))
        return {TfToken(), nullptr};
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    if (!mBlank.count(path) && !mPruned.count(path))
        return prim;
    // Left as a transform: no geometry, instancing or anything else the stage gave it.
    const HdContainerDataSourceHandle& in = prim.dataSource;
    return {TfToken(), container({{HdXformSchema::GetSchemaToken(), in ? in->Get(HdXformSchema::GetSchemaToken()) : nullptr},
                                  {HdVisibilitySchema::GetSchemaToken(), in ? in->Get(HdVisibilitySchema::GetSchemaToken()) : nullptr}})};
}

SdfPathVector StageOverrides::GetChildPrimPaths(const SdfPath& path) const
{
    if (mPruned.count(path) || insidePruned(path))
        return {};
    return _GetInputSceneIndex()->GetChildPrimPaths(path);
}

void StageOverrides::_PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    HdSceneIndexObserver::AddedPrimEntries out;
    out.reserve(entries.size());
    for (const auto& e : entries)
        if (!insidePruned(e.primPath))
            out.emplace_back(e.primPath, mBlank.count(e.primPath) || mPruned.count(e.primPath) ? TfToken() : e.primType);
    if (!out.empty())
        _SendPrimsAdded(out);
}

void StageOverrides::_PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) { _SendPrimsRemoved(entries); }

void StageOverrides::_PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    if (mPruned.empty())
    {
        _SendPrimsDirtied(entries);
        return;
    }
    HdSceneIndexObserver::DirtiedPrimEntries out;
    for (const auto& e : entries)
        if (!insidePruned(e.primPath))
            out.push_back(e);
    if (!out.empty())
        _SendPrimsDirtied(out);
}

void StageOverrides::update(std::set<SdfPath> blank, std::set<SdfPath> pruned)
{
    if (blank == mBlank && pruned == mPruned)
        return;
    HdSceneIndexObserver::RemovedPrimEntries removed;
    std::set<SdfPath> readd, subtrees;
    for (const SdfPath& p : pruned)
        if (!mPruned.count(p))
        {
            for (const SdfPath& c : GetChildPrimPaths(p))
                removed.emplace_back(c);
            readd.insert(p);
        }
    for (const SdfPath& p : mPruned)
        if (!pruned.count(p))
            subtrees.insert(p);
    std::set_symmetric_difference(blank.begin(), blank.end(), mBlank.begin(), mBlank.end(), std::inserter(readd, readd.end()));

    mBlank = std::move(blank);
    mPruned = std::move(pruned);

    HdSceneIndexObserver::AddedPrimEntries added;
    for (const SdfPath& p : readd)
        if (!insidePruned(p))
            added.emplace_back(p, GetPrim(p).primType);
    std::function<void(const SdfPath&)> subtree = [&](const SdfPath& p) {
        added.emplace_back(p, GetPrim(p).primType);
        for (const SdfPath& c : GetChildPrimPaths(p))
            subtree(c);
    };
    for (const SdfPath& root : subtrees)
        if (!insidePruned(root))
            subtree(root);
    if (!removed.empty())
        _SendPrimsRemoved(removed);
    if (!added.empty())
        _SendPrimsAdded(added);
}

// ---------------------------------------------------------------- StagePlacement

glm::dmat4 StagePlacement::flattened(const SdfPath& path) const
{
    const HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    if (prim.dataSource)
        if (HdXformSchema xf = HdXformSchema::GetFromParent(prim.dataSource))
            if (HdMatrixDataSourceHandle ds = xf.GetMatrix())
                return toGlm(ds->GetTypedValue(0.0f));
    return glm::dmat4(1.0);
}

HdSceneIndexPrim StagePlacement::GetPrim(const SdfPath& path) const
{
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    if (!prim.dataSource)
        return prim;
    // The nearest prim at or above this one that a Lila node places carries it: kept where it sits relative to that prim.
    const Placed* by = nullptr;
    SdfPath at;
    for (SdfPath p = path; !p.IsEmpty() && !p.IsAbsoluteRootPath(); p = p.GetParentPath())
        if (auto it = mPlaced.find(p); it != mPlaced.end())
        {
            by = &it->second;
            at = p;
            break;
        }
    glm::dmat4 m = flattened(path);
    std::vector<std::pair<TfToken, HdDataSourceBaseHandle>> e;
    if (by)
    {
        m = at == path ? by->matrix : by->matrix * glm::inverse(flattened(at)) * m;
        if (!by->visible || at == path)
            e.push_back({HdVisibilitySchema::GetSchemaToken(), HdVisibilitySchema::BuildRetained(retained(by->visible))});
    }
    e.push_back({HdXformSchema::GetSchemaToken(), xformSource(mToWorld * m)});
    prim.dataSource = HdOverlayContainerDataSource::New(container(e), prim.dataSource);
    return prim;
}

SdfPathVector StagePlacement::GetChildPrimPaths(const SdfPath& path) const { return _GetInputSceneIndex()->GetChildPrimPaths(path); }

void StagePlacement::_PrimsAdded(const HdSceneIndexBase&, const HdSceneIndexObserver::AddedPrimEntries& entries) { _SendPrimsAdded(entries); }
void StagePlacement::_PrimsRemoved(const HdSceneIndexBase&, const HdSceneIndexObserver::RemovedPrimEntries& entries) { _SendPrimsRemoved(entries); }
void StagePlacement::_PrimsDirtied(const HdSceneIndexBase&, const HdSceneIndexObserver::DirtiedPrimEntries& entries) { _SendPrimsDirtied(entries); }

void StagePlacement::update(const glm::dmat4& toWorld, std::map<SdfPath, Placed> placed)
{
    const bool all = toWorld != mToWorld;
    std::set<SdfPath> moved;
    if (!all)
    {
        for (const auto& [p, o] : placed)
            if (auto was = mPlaced.find(p); was == mPlaced.end() || !(was->second == o))
                moved.insert(p);
        for (const auto& [p, o] : mPlaced)
            if (!placed.count(p))
                moved.insert(p);
    }
    mToWorld = toWorld;
    mPlaced = std::move(placed);
    if (!all && moved.empty())
        return;
    static const HdDataSourceLocatorSet placement{HdXformSchema::GetDefaultLocator(), HdVisibilitySchema::GetDefaultLocator()};
    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
    std::set<SdfPath> done;
    std::function<void(const SdfPath&)> subtree = [&](const SdfPath& p) {
        if (!done.insert(p).second)
            return;
        dirtied.emplace_back(p, placement);
        for (const SdfPath& c : _GetInputSceneIndex()->GetChildPrimPaths(p))
            subtree(c);
    };
    if (all)
        subtree(SdfPath::AbsoluteRootPath());
    else
        for (const SdfPath& p : moved)
            subtree(p);
    if (!dirtied.empty())
        _SendPrimsDirtied(dirtied);
}
} // namespace lila::usd::hydra
