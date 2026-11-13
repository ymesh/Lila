#include "UsdExport.h"

#include "UsdAttributes.h"
#include "UsdMaterials.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/LightingChannels.h>
#include <lila/Lights.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Rig.h>

#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/zipFile.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/nurbsCurves.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>
#include <pxr/usd/usdSkel/inbetweenShape.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
namespace
{
using fbx::ExportNode;
using fbx::ExportScene;
using fbx::ExportTake;
using geo::Index;

/**
 * The scene's place on the Earth: a CesiumGeoreferencePrim (Cesium for Omniverse's schema, read by Cesium's USD tools) at
 * the WGS 84 origin, and the CRS and projected origin in the layer's customLayerData under lilaGeoreference.
 */
void writeGeoreference(const UsdStageRefPtr& stage, const SdfLayerRefPtr& rootLayer, const SdfLayerRefPtr& geoLayer)
{
    json geo;
    try
    {
        geo = ops::execute("georeference", json::object());
    }
    catch (const std::exception&)
    {
        return;
    }
    if (!geo.is_object())
        return;
    VtDictionary d;
    d["crs"] = VtValue(geo.value("authority", geo.value("crs", std::string())));
    if (const json& o = geo["origin"]; o.is_array() && o.size() == 3)
        d["origin"] = VtValue(GfVec3d(o[0].get<double>(), o[1].get<double>(), o[2].get<double>()));
    d["unitToMetre"] = VtValue(geo.value("unitToMetre", 1.0));
    d["zUp"] = VtValue(geo.value("zUp", false));
    VtDictionary layerData = rootLayer->GetCustomLayerData();
    layerData["lilaGeoreference"] = VtValue(d);
    rootLayer->SetCustomLayerData(layerData);
    if (!geo.contains("wgs84"))
        return;
    std::optional<UsdEditContext> ctx;
    if (geoLayer)
        ctx.emplace(stage, geoLayer);
    UsdPrim p = stage->DefinePrim(SdfPath("/CesiumGeoreference"), TfToken("CesiumGeoreferencePrim"));
    const json& w = geo["wgs84"];
    p.CreateAttribute(TfToken("cesium:georeferenceOrigin:latitude"), SdfValueTypeNames->Double).Set(w["latitude"].get<double>());
    p.CreateAttribute(TfToken("cesium:georeferenceOrigin:longitude"), SdfValueTypeNames->Double).Set(w["longitude"].get<double>());
    p.CreateAttribute(TfToken("cesium:georeferenceOrigin:height"), SdfValueTypeNames->Double).Set(w["height"].get<double>());
}

template <class T> struct BitsHash
{
    size_t operator()(const T& v) const
    {
        uint64_t h = 1469598103934665603ull;
        const auto* b = reinterpret_cast<const unsigned char*>(&v);
        for (size_t i = 0; i < sizeof(T); ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return size_t(h);
    }
};
template <class T> struct BitsEq
{
    bool operator()(const T& a, const T& b) const { return std::memcmp(&a, &b, sizeof(T)) == 0; }
};

/** Values and indices of an indexed primvar, equal values shared. */
template <class Out, class In> std::pair<VtArray<Out>, VtIntArray> indexed(const std::vector<In>& values, auto&& convert)
{
    std::unordered_map<In, int, BitsHash<In>, BitsEq<In>> ids;
    VtArray<Out> out;
    VtIntArray idx;
    idx.reserve(values.size());
    for (const In& v : values)
    {
        auto [it, added] = ids.emplace(v, int(ids.size()));
        if (added)
            out.push_back(convert(v));
        idx.push_back(it->second);
    }
    return {out, idx};
}

GfVec3f vec3f(const glm::dvec3& v) { return GfVec3f(float(v.x), float(v.y), float(v.z)); }

/** A morph target with its in-betweens: entries index the ExportMorphs targets, weights the shapes' (1 first). */
struct MorphGroup
{
    std::string name;
    std::vector<size_t> entries;
    std::vector<double> weights;
};

std::vector<double> shapeWeights(const scene::Node& t)
{
    std::vector<double> out{1.0};
    const scene::DataPtr& d = t.as<scene::DataPtr>(scene::morphs::kInBetweens);
    if (d && std::string_view(d->kind()) == "morphInBetweens")
        for (const auto& s : static_cast<const scene::MorphInBetweens*>(d.get())->shapes)
            out.push_back(s.weight);
    return out;
}

class Writer
{
public:
    Writer(scene::Graph& graph, const ExportOptions& opts, ExportResult& res, const fs::path& path)
        : g(graph), opts(opts), res(res), path(path), axes(makeAxes(opts.zUp, opts.metersPerUnit))
    {
    }

    bool run(const std::vector<scene::NodeId>& roots);

private:
    scene::Graph& g;
    const ExportOptions& opts;
    ExportResult& res;
    fs::path path;
    Axes axes;
    ExportScene snap;
    std::vector<const ExportTake*> takes;
    double tcps = 30.0;
    bool package = false;
    fs::path stagingDir; // usdz: where the layers are written before packaging

    UsdStageRefPtr stage;
    SdfLayerRefPtr geoLayer, matLayer, animLayer;
    SdfPath rootPath;
    std::unique_ptr<MaterialWriter> mats;

    // Per exported node.
    std::vector<int> usdParent;   // node index whose prim this node's prim sits under, -1 for the root
    std::vector<SdfPath> prims;   // empty for joints and dissolved nodes
    std::vector<int> skelOf;      // joints: their skeleton; meshes: the skeleton they bind to
    std::vector<char> lodLevel;   // inside an LOD variant
    struct Ops
    {
        bool matrix = false;
        scene::RotateOrder order = scene::RotateOrder::XYZ;
        UsdGeomXformOp t, r, s, m;
    };
    std::vector<Ops> ops;
    std::vector<char> animatedAnywhere; // its prim's transform moves in some take
    std::map<SdfPath, std::set<std::string>> taken;

    struct Skel
    {
        int anchor = -1; // node the Skeleton prim sits under (-1 the root)
        std::vector<int> joints;
        VtTokenArray tokens;
        SdfPath path;
        UsdSkelAnimation anim;
        bool synthetic = false;
        std::vector<int> meshes;
        struct Channel
        {
            std::string token;
            int mesh;
            MorphGroup group;
        };
        std::vector<Channel> channels;
    };
    std::vector<Skel> skels;

    std::string uniqueChild(const SdfPath& parent, const std::string& want)
    {
        std::set<std::string>& used = taken[parent];
        std::string name = want;
        for (int k = 1; used.count(name); ++k)
            name = want + std::to_string(k);
        used.insert(name);
        return name;
    }

    SdfPath parentPath(int i) const { return usdParent[size_t(i)] >= 0 ? prims[size_t(usdParent[size_t(i)])] : rootPath; }

    /** The node's local matrix in a take's frame (or at rest), relative to its exported parent. */
    glm::dmat4 exportedLocal(size_t i, const ExportTake* take, size_t f) const
    {
        if (take && i < take->locals.size() && !take->locals[i].empty())
            return take->locals[i][std::min(f, take->locals[i].size() - 1)];
        return snap.nodes[i].local;
    }
    /** Relative to the prim it is written under, which skips joints and anything else that isn't a prim. */
    glm::dmat4 usdLocal(size_t i, const ExportTake* take, size_t f) const
    {
        glm::dmat4 m = exportedLocal(i, take, f);
        for (int p = snap.nodes[i].parent; p >= 0 && p != usdParent[i]; p = snap.nodes[size_t(p)].parent)
            m = exportedLocal(size_t(p), take, f) * m;
        return m;
    }
    bool animatedIn(size_t i, const ExportTake* take) const
    {
        if (!take)
            return false;
        for (int p = int(i); p >= 0; p = snap.nodes[size_t(p)].parent)
        {
            if (size_t(p) < take->locals.size() && !take->locals[size_t(p)].empty())
                return true;
            if (p != int(i) && p == usdParent[i])
                break;
        }
        return false;
    }
    /** Cameras and lights, whose look down -Z is turned between their Lila and file frames. */
    static bool turned(const ExportNode& e) { return e.camera || e.light; }
    /** File-space local of a prim, with cameras and lights turned between their Lila and file frames. */
    glm::dmat4 fileLocal(size_t i, const glm::dmat4& lila) const
    {
        glm::dmat4 m = axes.matrixToFile(lila);
        const int p = usdParent[i];
        if (p >= 0 && turned(snap.nodes[size_t(p)]))
            m = glm::dmat4(glm::transpose(axes.basis)) * m;
        if (turned(snap.nodes[i]))
            m = m * glm::dmat4(axes.basis);
        return m;
    }

    std::vector<char> cameraAnimated, skelAnimated, weightsAnimated, pointsAnimated;
    std::vector<glm::dvec3> prevRot; // per node, the last sample's Euler angles

    std::string skeletonName(const Skel& s) const
    {
        return s.synthetic ? identifier(snap.nodes[size_t(s.meshes.front())].name, "Mesh") + "_Skeleton" : std::string("Skeleton");
    }
    std::vector<MorphGroup> morphGroups(size_t i) const;
    void planHierarchy();
    void planSkeletons();
    void planPaths(size_t i, const SdfPath& parent, const std::string& name);
    bool lodWritable(size_t i) const;
    void writeNode(size_t i);
    void writeXform(size_t i, const UsdPrim& prim);
    void setXform(size_t i, const ExportTake* take, size_t f, UsdTimeCode t);
    void writeMesh(size_t i, const UsdPrim& prim);
    /** Points laid out like node i's mesh, in the file's order and axes, as writeMesh writes them. */
    VtVec3fArray filePoints(size_t i, std::span<const glm::dvec3> points) const;
    /** Whether node i is written as a mesh prim of its own. */
    bool meshPrim(size_t i) const { return !prims[i].IsEmpty() && !lodLevel[i] && snap.nodes[i].mesh && snap.nodes[i].mesh->numFaces() > 0; }
    void writeCamera(size_t i, const UsdPrim& prim);
    UsdPrim defineLight(size_t i, const SdfPath& at);
    void writeCurves(size_t i, const SdfPath& at);
    void writeSkeleton(size_t k);
    void writeSkelRoots();
    void writeLightLinks();
    void writeTake(const ExportTake* take, bool restForStill);
    void recordCurves(size_t i);
    void writeAnimatedRest();
    UsdTimeCode code(double seconds) const { return UsdTimeCode(seconds * tcps); }
    bool save();

    /** Inside a variant the edit target already points into the right layer's variant. */
    int variantDepth = 0;
    template <class F> void inLayer(const SdfLayerRefPtr& layer, F&& fn)
    {
        if (!layer || variantDepth > 0)
        {
            fn();
            return;
        }
        UsdEditContext ctx(stage, layer);
        fn();
    }
};

std::vector<MorphGroup> Writer::morphGroups(size_t i) const
{
    std::vector<MorphGroup> out;
    const ExportNode& e = snap.nodes[i];
    if (!e.morphs)
        return out;
    const auto& targets = e.morphs->targets;
    const scene::NodeId bs = scene::morphs::find(g, e.source);
    size_t k = 0;
    if (bs != scene::kNoNode)
        for (scene::NodeId t : scene::morphs::targetNodes(g, bs))
        {
            MorphGroup grp;
            grp.weights = shapeWeights(*g.node(t));
            for (size_t s = 0; s < grp.weights.size(); ++s)
                grp.entries.push_back(k + s);
            k += grp.weights.size();
            if (k > targets.size())
                break;
            grp.name = targets[grp.entries[0]].name;
            out.push_back(std::move(grp));
        }
    if (k != targets.size())
    {
        out.clear();
        for (size_t t = 0; t < targets.size(); ++t)
            out.push_back({targets[t].name, {t}, {1.0}});
    }
    return out;
}

/** Where every node's prim goes: joints become a Skeleton's joints, and what hangs off a joint moves up beside it. */
void Writer::planHierarchy()
{
    const size_t n = snap.nodes.size();
    usdParent.assign(n, -1);
    prims.assign(n, SdfPath());
    skelOf.assign(n, -1);
    lodLevel.assign(n, 0);
    ops.assign(n, {});
    animatedAnywhere.assign(n, 0);
    std::set<std::string> movedOff;
    for (size_t i = 0; i < n; ++i)
    {
        int p = snap.nodes[i].parent;
        bool viaJoint = false;
        while (p >= 0 && snap.nodes[size_t(p)].joint)
        {
            viaJoint = true;
            p = snap.nodes[size_t(p)].parent;
        }
        usdParent[i] = p;
        if (viaJoint && !snap.nodes[i].joint)
            movedOff.insert(snap.nodes[i].name);
    }
    if (!movedOff.empty())
    {
        std::string list;
        for (const std::string& s : movedOff)
            list += (list.empty() ? "" : ", ") + s;
        res.warnings.push_back(std::format("USD joints can't have children: {} written beside the skeleton, following its joint's "
                                           "animation baked",
                                           list));
    }
}

void Writer::planSkeletons()
{
    const size_t n = snap.nodes.size();
    std::vector<std::vector<int>> kids(n);
    for (size_t i = 0; i < n; ++i)
        if (snap.nodes[i].parent >= 0)
            kids[size_t(snap.nodes[i].parent)].push_back(int(i));
    for (size_t i = 0; i < n; ++i)
    {
        const ExportNode& e = snap.nodes[i];
        if (!e.joint || (e.parent >= 0 && snap.nodes[size_t(e.parent)].joint))
            continue;
        Skel s;
        s.anchor = usdParent[i];
        std::vector<std::pair<int, std::string>> stack{{int(i), identifier(e.name, "joint")}};
        while (!stack.empty())
        {
            auto [j, token] = stack.back();
            stack.pop_back();
            skelOf[size_t(j)] = int(skels.size());
            s.joints.push_back(j);
            s.tokens.push_back(TfToken(token));
            const auto& c = kids[size_t(j)];
            for (auto it = c.rbegin(); it != c.rend(); ++it)
                if (snap.nodes[size_t(*it)].joint)
                    stack.push_back({*it, token + "/" + identifier(snap.nodes[size_t(*it)].name, "joint")});
        }
        skels.push_back(std::move(s));
    }
    // Skinned meshes bind to the skeleton of their first influence; morph-only meshes get a one-joint skeleton of their own.
    for (size_t i = 0; i < n; ++i)
    {
        const ExportNode& e = snap.nodes[i];
        if (!e.mesh || e.joint)
            continue;
        if (e.skin && !e.skin->clusters.empty())
        {
            int sk = -1;
            for (const auto& c : e.skin->clusters)
                if (c.joint >= 0 && skelOf[size_t(c.joint)] >= 0)
                {
                    if (sk < 0)
                        sk = skelOf[size_t(c.joint)];
                    else if (sk != skelOf[size_t(c.joint)])
                    {
                        res.warnings.push_back(std::format("{}: skinned to joints of more than one skeleton; bound to the first", e.name));
                        break;
                    }
                }
            if (sk >= 0)
            {
                skelOf[i] = sk;
                skels[size_t(sk)].meshes.push_back(int(i));
                continue;
            }
        }
        if (e.morphs && !e.morphs->targets.empty())
        {
            Skel s;
            s.anchor = usdParent[i];
            s.synthetic = true;
            s.tokens.push_back(TfToken("root"));
            s.meshes.push_back(int(i));
            skelOf[i] = int(skels.size());
            skels.push_back(std::move(s));
        }
    }
}

/** LOD levels go into variants only when they are plain meshes: no skin, morphs, animation or children. */
bool Writer::lodWritable(size_t i) const
{
    if (!opts.lods || !snap.nodes[i].lodGroup)
        return false;
    bool any = false;
    for (size_t c = 0; c < snap.nodes.size(); ++c)
    {
        if (snap.nodes[c].parent != int(i))
            continue;
        const ExportNode& lv = snap.nodes[c];
        if (!lv.mesh || lv.skin || lv.morphs || lv.camera || lv.joint)
            return false;
        for (const ExportTake* t : takes)
            if (animatedIn(c, t))
                return false;
        for (size_t d = 0; d < snap.nodes.size(); ++d)
            if (snap.nodes[d].parent == int(c))
                return false;
        any = true;
    }
    return any;
}

void Writer::writeXform(size_t i, const UsdPrim& prim)
{
    const ExportNode& e = snap.nodes[i];
    UsdGeomXformable xf(prim);
    Ops& o = ops[i];
    bool sheared = false;
    auto check = [&](const glm::dmat4& m) {
        Trs t = decompose(m, scene::RotateOrder::XYZ, glm::dvec3(0.0));
        sheared = sheared || t.sheared;
    };
    check(fileLocal(i, usdLocal(i, nullptr, 0)));
    for (const ExportTake* t : takes)
        if (animatedIn(i, t))
        {
            animatedAnywhere[i] = 1;
            for (size_t f = 0; f < t->times.size() && !sheared; ++f)
                check(fileLocal(i, usdLocal(i, t, f)));
        }
    o.matrix = sheared;
    const bool channels = e.plainTrs && usdParent[i] == e.parent && !turned(e) && (e.parent < 0 || !turned(snap.nodes[size_t(e.parent)])) &&
                          nearMatrix(e.pre, glm::dmat4(1.0)) && nearMatrix(e.post, glm::dmat4(1.0));
    o.order = channels ? axes.map.order(e.rotateOrder) : scene::RotateOrder::XYZ;
    if (o.matrix)
        o.m = xf.AddTransformOp();
    else
    {
        o.t = xf.AddTranslateOp(UsdGeomXformOp::PrecisionDouble);
        switch (o.order)
        {
        case scene::RotateOrder::XYZ: o.r = xf.AddRotateXYZOp(); break;
        case scene::RotateOrder::YZX: o.r = xf.AddRotateYZXOp(); break;
        case scene::RotateOrder::ZXY: o.r = xf.AddRotateZXYOp(); break;
        case scene::RotateOrder::XZY: o.r = xf.AddRotateXZYOp(); break;
        case scene::RotateOrder::YXZ: o.r = xf.AddRotateYXZOp(); break;
        case scene::RotateOrder::ZYX: o.r = xf.AddRotateZYXOp(); break;
        }
        o.s = xf.AddScaleOp();
    }
    if (!animatedAnywhere[i] || takes.size() <= 1)
        setXform(i, nullptr, 0, UsdTimeCode::Default());
}

/** Writes the prim's transform at frame f of take (rest when null) at time t. */
void Writer::setXform(size_t i, const ExportTake* take, size_t f, UsdTimeCode t)
{
    const ExportNode& e = snap.nodes[i];
    Ops& o = ops[i];
    const glm::dmat4 m = fileLocal(i, usdLocal(i, take, f));
    if (o.matrix)
    {
        o.m.Set(toGf(m), t);
        return;
    }
    glm::dvec3 tr, rot, sc;
    if (!take && e.plainTrs && o.order == axes.map.order(e.rotateOrder) && usdParent[i] == e.parent && !turned(e))
    {
        tr = axes.map.translate(e.translate) / axes.cmPerUnit;
        rot = axes.map.rotate(e.rotate);
        sc = axes.map.scale(e.scale);
    }
    else
    {
        // Euler angles near the rest channels (and the previous frame), so samples neither flip nor wrap.
        const glm::dvec3 ref = take && f > 0 && i < prevRot.size() ? prevRot[i] : axes.map.rotate(e.rotate);
        Trs trs = decompose(m, o.order, ref);
        if (i < prevRot.size())
            prevRot[i] = trs.r;
        tr = trs.t, rot = trs.r, sc = trs.s;
    }
    o.t.Set(GfVec3d(tr.x, tr.y, tr.z), t);
    o.r.Set(vec3f(rot), t);
    o.s.Set(vec3f(sc), t);
}

VtVec3fArray Writer::filePoints(size_t i, std::span<const glm::dvec3> points) const
{
    const ExportNode& e = snap.nodes[i];
    const glm::dmat4 toFile = axes.toFile * e.bake;
    VtVec3fArray out;
    out.reserve(points.size());
    for (Index p = 0; p < e.mesh->pointCapacity() && p < points.size(); ++p)
        if (e.mesh->pointValid(p))
            out.push_back(vec3f(glm::dvec3(toFile * glm::dvec4(points[p], 1.0))));
    return out;
}

void Writer::writeMesh(size_t i, const UsdPrim& prim)
{
    LILA_ZONE();

    const ExportNode& e = snap.nodes[i];
    const geo::Mesh& m = *e.mesh;
    UsdGeomMesh mesh(prim);
    const glm::dmat4 toFile = axes.toFile * e.bake;
    const glm::dmat3 lin(toFile);
    const glm::dmat3 normalMat = glm::inverse(glm::transpose(lin));
    const bool mirrored = glm::determinant(lin) < 0.0;

    std::vector<int> pointIndex(m.pointCapacity(), -1);
    VtVec3fArray points;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            pointIndex[p] = int(points.size());
            points.push_back(vec3f(glm::dvec3(toFile * glm::dvec4(m.position(p), 1.0))));
        }

    // Polygons as runs of corners, triangulated as the viewport splits them when asked.
    std::vector<Index> corners, polyFace;
    std::vector<uint32_t> polyStart;
    if (opts.triangulate)
    {
        geo::Triangulation tris = geo::triangulate(m);
        corners = tris.corners;
        polyFace = tris.faces;
        for (Index t = 0; t < tris.triangleCount(); ++t)
            polyStart.push_back(t * 3);
    }
    else
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            polyStart.push_back(uint32_t(corners.size()));
            polyFace.push_back(f);
            m.forEachFaceCorner(f, [&](Index c) { corners.push_back(c); });
        }
    polyStart.push_back(uint32_t(corners.size()));
    if (mirrored)
        for (size_t k = 0; k + 1 < polyStart.size(); ++k)
            std::reverse(corners.begin() + polyStart[k], corners.begin() + polyStart[k + 1]);
    const size_t polyCount = polyStart.size() - 1;

    VtIntArray counts(polyCount), indices(corners.size());
    for (size_t k = 0; k < polyCount; ++k)
        counts[k] = int(polyStart[k + 1] - polyStart[k]);
    for (size_t k = 0; k < corners.size(); ++k)
        indices[k] = pointIndex[m.cornerPoint(corners[k])];
    mesh.CreatePointsAttr().Set(points);
    mesh.CreateFaceVertexCountsAttr().Set(counts);
    mesh.CreateFaceVertexIndicesAttr().Set(indices);
    VtVec3fArray extent;
    if (UsdGeomPointBased::ComputeExtent(points, &extent))
        mesh.CreateExtentAttr().Set(extent);
    const scene::Node* node = g.node(e.source);
    const bool smooth = node && node->type().isA("mesh") && node->as<int64_t>(scene::MeshNode::kSmoothPreview) > 0;
    mesh.CreateSubdivisionSchemeAttr().Set(smooth ? UsdGeomTokens->catmullClark : UsdGeomTokens->none);

    if (opts.normals)
    {
        std::vector<glm::vec3> computed;
        std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        if (normals.size() != m.cornerCapacity())
        {
            computed = geo::cornerNormals(m);
            normals = computed;
        }
        VtVec3fArray out(corners.size());
        for (size_t k = 0; k < corners.size(); ++k)
        {
            glm::dvec3 n = normalMat * glm::dvec3(normals[corners[k]]);
            const double len = glm::length(n);
            out[k] = vec3f(len > 0.0 ? n / len : n);
        }
        mesh.CreateNormalsAttr().Set(out);
        mesh.SetNormalsInterpolation(UsdGeomTokens->faceVarying);
    }

    UsdGeomPrimvarsAPI pv(prim);
    for (int set = 0; set < 8; ++set)
    {
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(set));
        if (uv.size() != m.cornerCapacity())
            continue;
        std::vector<glm::vec2> values;
        values.reserve(corners.size());
        for (Index c : corners)
            values.push_back(uv[c]);
        auto [vals, idx] = indexed<GfVec2f>(values, [](const glm::vec2& v) { return GfVec2f(v.x, v.y); });
        UsdGeomPrimvar p = pv.CreatePrimvar(TfToken(uvPrimvarName(set)), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying);
        p.Set(vals);
        p.SetIndices(idx);
    }

    const std::string current = geo::colors::current(m);
    for (const std::string& set : geo::colors::sets(m))
    {
        std::span<const glm::vec4> col = geo::colors::values(m, set);
        if (col.size() != m.cornerCapacity())
            continue;
        std::vector<glm::vec4> values;
        values.reserve(corners.size());
        for (Index c : corners)
            values.push_back(col[c]);
        if (set == current || (current.empty() && set == geo::colors::sets(m).front()))
        {
            // Unreal reads displayColor and displayOpacity as the vertex colors.
            auto [rgb, ri] = indexed<GfVec3f>(values, [](const glm::vec4& v) { return GfVec3f(v.r, v.g, v.b); });
            UsdGeomPrimvar dc = pv.CreatePrimvar(TfToken("displayColor"), SdfValueTypeNames->Color3fArray, UsdGeomTokens->faceVarying);
            dc.Set(rgb);
            dc.SetIndices(ri);
            setLilaData(dc.GetAttr(), "colorSet", VtValue(set));
            auto [a, ai] = indexed<float>(values, [](const glm::vec4& v) { return v.a; });
            if (!(a.size() == 1 && a[0] == 1.0f))
            {
                UsdGeomPrimvar dop = pv.CreatePrimvar(TfToken("displayOpacity"), SdfValueTypeNames->FloatArray, UsdGeomTokens->faceVarying);
                dop.Set(a);
                dop.SetIndices(ai);
            }
            continue;
        }
        auto [vals, idx] = indexed<GfVec4f>(values, [](const glm::vec4& v) { return GfVec4f(v.r, v.g, v.b, v.a); });
        UsdGeomPrimvar p = pv.CreatePrimvar(TfToken(identifier(set, "colorSet")), SdfValueTypeNames->Color4fArray, UsdGeomTokens->faceVarying);
        p.Set(vals);
        p.SetIndices(idx);
    }

    if (std::span<const float> crease = m.edgeAttrs().get<float>(geo::attr::kCrease); crease.size() == m.edgeCapacity())
    {
        VtIntArray ci, cl;
        VtFloatArray cs;
        for (Index ed = 0; ed < m.edgeCapacity(); ++ed)
            if (m.edgeValid(ed) && crease[ed] > 0.0f)
            {
                ci.push_back(pointIndex[m.edgePoint(ed, 0)]);
                ci.push_back(pointIndex[m.edgePoint(ed, 1)]);
                cl.push_back(2);
                cs.push_back(crease[ed]);
            }
        if (!cl.empty())
        {
            mesh.CreateCreaseIndicesAttr().Set(ci);
            mesh.CreateCreaseLengthsAttr().Set(cl);
            mesh.CreateCreaseSharpnessesAttr().Set(cs);
        }
    }

    if (mats)
        inLayer(matLayer, [&] {
            std::span<const int32_t> faceMat = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
            std::map<std::string, VtIntArray> facesOf;
            std::vector<std::string> order;
            for (size_t k = 0; k < polyCount; ++k)
            {
                const int32_t id = faceMat.size() == m.faceCapacity() ? faceMat[polyFace[k]] : 0;
                const std::string slot = m.materialNames().empty() && id == 0 ? std::string(scene::materials::kDefaultSlot)
                                                                               : scene::materials::slotName(m, id);
                if (!facesOf.count(slot))
                    order.push_back(slot);
                facesOf[slot].push_back(int(k));
            }
            bool twoSided = false;
            if (order.size() == 1)
            {
                UsdShadeMaterialBindingAPI::Apply(prim).Bind(mats->material(order[0]));
                twoSided = mats->twoSided(order[0]);
            }
            else
            {
                UsdShadeMaterialBindingAPI binding = UsdShadeMaterialBindingAPI::Apply(prim);
                std::set<std::string> names;
                for (const std::string& slot : order)
                {
                    std::string name = identifier(slot, "Material");
                    for (int k = 1; names.count(name); ++k)
                        name = identifier(slot, "Material") + std::to_string(k);
                    names.insert(name);
                    UsdGeomSubset subset = binding.CreateMaterialBindSubset(TfToken(name), facesOf[slot]);
                    UsdShadeMaterialBindingAPI::Apply(subset.GetPrim()).Bind(mats->material(slot));
                    twoSided = twoSided || mats->twoSided(slot);
                }
                binding.SetMaterialBindSubsetsFamilyType(UsdGeomTokens->partition);
            }
            if (twoSided)
                mesh.CreateDoubleSidedAttr().Set(true);
        });

    // Skin: per point joint indices and weights into the skeleton, the mesh's bind placement in skeleton space.
    if (skelOf[i] < 0)
        return;
    const Skel& sk = skels[size_t(skelOf[i])];
    UsdSkelBindingAPI binding = UsdSkelBindingAPI::Apply(prim);
    binding.CreateSkeletonRel().SetTargets({sk.path});
    const glm::dmat4 anchorBind = sk.anchor >= 0 ? snap.nodes[size_t(sk.anchor)].bindWorld : glm::dmat4(1.0);
    if (e.skin && !sk.synthetic)
    {
        std::vector<std::vector<std::pair<int, float>>> perPoint(points.size());
        std::unordered_map<int, int> jointIndex;
        for (size_t k = 0; k < sk.joints.size(); ++k)
            jointIndex[sk.joints[k]] = int(k);
        for (const auto& c : e.skin->clusters)
        {
            auto it = jointIndex.find(c.joint);
            if (it == jointIndex.end())
                continue;
            for (size_t k = 0; k < c.vertices.size(); ++k)
                if (size_t(c.vertices[k]) < perPoint.size())
                    perPoint[size_t(c.vertices[k])].push_back({it->second, float(c.weights[k])});
        }
        int per = 1;
        for (const auto& p : perPoint)
            per = std::max(per, int(p.size()));
        VtIntArray ji(points.size() * size_t(per), 0);
        VtFloatArray jw(points.size() * size_t(per), 0.0f);
        for (size_t p = 0; p < perPoint.size(); ++p)
        {
            auto& list = perPoint[p];
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            float sum = 0.0f;
            for (const auto& [j, w] : list)
                sum += w;
            for (size_t k = 0; k < list.size(); ++k)
            {
                ji[p * size_t(per) + k] = list[k].first;
                jw[p * size_t(per) + k] = sum > 0.0f ? list[k].second / sum : 0.0f;
            }
        }
        binding.CreateJointIndicesPrimvar(false, per).Set(ji);
        binding.CreateJointWeightsPrimvar(false, per).Set(jw);
        binding.CreateGeomBindTransformAttr().Set(toGf(axes.matrixToFile(glm::inverse(anchorBind) * e.skin->meshBindWorld)));
        if (e.skin->dualQuaternion)
        {
            binding.CreateSkinningMethodAttr().Set(UsdSkelTokens->dualQuaternion);
        }
    }
    else
    {
        binding.CreateJointIndicesPrimvar(true, 1).Set(VtIntArray{0});
        binding.CreateJointWeightsPrimvar(true, 1).Set(VtFloatArray{1.0f});
        binding.CreateGeomBindTransformAttr().Set(toGf(axes.matrixToFile(glm::inverse(anchorBind) * e.bindWorld)));
    }

    // Blend shapes: one per morph target, its in-betweens on it; their offsets share one set of point indices.
    if (!e.morphs)
        return;
    VtTokenArray names;
    SdfPathVector targets;
    std::set<std::string> used;
    Skel& skw = skels[size_t(skelOf[i])];
    for (const MorphGroup& grp : morphGroups(i))
    {
        std::string base = identifier(grp.name, "Shape");
        std::string name = base;
        for (int k = 1; used.count(name); ++k)
            name = base + std::to_string(k);
        used.insert(name);
        std::map<int32_t, size_t> slot;
        for (size_t entry : grp.entries)
            for (int32_t v : e.morphs->targets[entry].vertices)
                slot.emplace(v, 0);
        VtIntArray pointIdx;
        for (auto& [v, k] : slot)
        {
            k = size_t(pointIdx.size());
            pointIdx.push_back(int(v));
        }
        auto offsetsOf = [&](size_t entry) {
            VtVec3fArray off(pointIdx.size(), GfVec3f(0.0f));
            const auto& tg = e.morphs->targets[entry];
            for (size_t k = 0; k < tg.vertices.size(); ++k)
                off[slot[tg.vertices[k]]] = vec3f(lin * tg.deltas[k]);
            return off;
        };
        const SdfPath shapePath = prim.GetPath().AppendChild(TfToken(uniqueChild(prim.GetPath(), name)));
        UsdSkelBlendShape bs = UsdSkelBlendShape::Define(stage, shapePath);
        bs.CreateOffsetsAttr().Set(offsetsOf(grp.entries[0]));
        bs.CreatePointIndicesAttr().Set(pointIdx);
        for (size_t s = 1; s < grp.entries.size(); ++s)
        {
            UsdSkelInbetweenShape ib = bs.CreateInbetween(TfToken(std::format("w{}", int(std::lround(grp.weights[s] * 1000.0)))));
            ib.SetWeight(float(grp.weights[s]));
            ib.SetOffsets(offsetsOf(grp.entries[s]));
        }
        // The animation drives channels by name; a name another mesh of this skeleton already uses gets the mesh's prefix.
        std::string token = grp.name;
        for (const auto& c : skw.channels)
            if (c.token == token)
            {
                token = e.name + "_" + grp.name;
                break;
            }
        names.push_back(TfToken(token));
        targets.push_back(shapePath);
        skw.channels.push_back({token, int(i), grp});
    }
    binding.CreateBlendShapesAttr().Set(names);
    binding.CreateBlendShapeTargetsRel().SetTargets(targets);
}

void Writer::writeCamera(size_t i, const UsdPrim& prim)
{
    const fbx::ExportCamera& c = *snap.nodes[i].camera;
    UsdGeomCamera cam(prim);
    // Lens and film back in tenths of a scene unit: millimetres in a centimetre stage.
    const double perMm = 1.0 / axes.cmPerUnit;
    cam.CreateProjectionAttr().Set(c.ortho ? UsdGeomTokens->orthographic : UsdGeomTokens->perspective);
    cam.CreateFocalLengthAttr().Set(float(c.focalLength * perMm));
    if (c.ortho)
    {
        const double aspect = c.filmWidth > 0.0 ? c.filmHeight / c.filmWidth : 1.0;
        cam.CreateHorizontalApertureAttr().Set(float(c.orthoWidth * 10.0 / axes.cmPerUnit));
        cam.CreateVerticalApertureAttr().Set(float(c.orthoWidth * aspect * 10.0 / axes.cmPerUnit));
    }
    else
    {
        cam.CreateHorizontalApertureAttr().Set(float(c.filmWidth * 25.4 * perMm));
        cam.CreateVerticalApertureAttr().Set(float(c.filmHeight * 25.4 * perMm));
    }
    cam.CreateClippingRangeAttr().Set(GfVec2f(float(c.nearClip / axes.cmPerUnit), float(c.farClip / axes.cmPerUnit)));
    setLilaData(prim, "filmFit", VtValue(c.filmFit));
}

/**
 * A UsdLux light: points and spots as sphere lights (spots shaped with ShapingAPI), areas as rect lights, discs as
 * disk lights, directional as distant and dome as dome lights. Lila's own settings go in custom data for a round trip.
 */
UsdPrim Writer::defineLight(size_t i, const SdfPath& at)
{
    using scene::lights::Kind;
    const scene::lights::Settings& l = *snap.nodes[i].light;
    const double perCm = 1.0 / axes.cmPerUnit;
    UsdPrim prim;
    switch (l.kind)
    {
    case Kind::Point:
    case Kind::Spot:
    {
        UsdLuxSphereLight sphere = UsdLuxSphereLight::Define(stage, at);
        sphere.CreateRadiusAttr().Set(float(l.sourceRadius * perCm));
        sphere.CreateTreatAsPointAttr().Set(l.sourceRadius <= 0.0);
        prim = sphere.GetPrim();
        if (l.kind == Kind::Spot)
        {
            UsdLuxShapingAPI shaping = UsdLuxShapingAPI::Apply(prim);
            shaping.CreateShapingConeAngleAttr().Set(float(l.outerConeAngle));
            shaping.CreateShapingConeSoftnessAttr().Set(float(l.outerConeAngle > 0.0 ? 1.0 - l.innerConeAngle / l.outerConeAngle : 0.0));
        }
        if (!l.texture.empty())
            UsdLuxShapingAPI::Apply(prim).CreateShapingIesFileAttr().Set(SdfAssetPath(l.texture));
        break;
    }
    case Kind::Directional: prim = UsdLuxDistantLight::Define(stage, at).GetPrim(); break;
    case Kind::Area:
    {
        UsdLuxRectLight rect = UsdLuxRectLight::Define(stage, at);
        rect.CreateWidthAttr().Set(float(l.width * perCm));
        rect.CreateHeightAttr().Set(float(l.height * perCm));
        prim = rect.GetPrim();
        break;
    }
    case Kind::Disc:
    {
        UsdLuxDiskLight disk = UsdLuxDiskLight::Define(stage, at);
        disk.CreateRadiusAttr().Set(float(l.width * 0.5 * perCm));
        prim = disk.GetPrim();
        break;
    }
    case Kind::Dome:
    {
        UsdLuxDomeLight dome = UsdLuxDomeLight::Define(stage, at);
        if (!l.texture.empty())
            dome.CreateTextureFileAttr().Set(SdfAssetPath(l.texture));
        prim = dome.GetPrim();
        break;
    }
    }
    UsdLuxLightAPI light(prim);
    light.CreateIntensityAttr().Set(float(l.intensity));
    light.CreateColorAttr().Set(GfVec3f(float(l.color.x), float(l.color.y), float(l.color.z)));
    light.CreateEnableColorTemperatureAttr().Set(l.useTemperature);
    light.CreateColorTemperatureAttr().Set(float(l.temperature));
    UsdLuxShadowAPI::Apply(prim).CreateShadowEnableAttr().Set(l.castShadows);
    const nlohmann::json j = {{"kind", int(l.kind)},
                              {"units", int(l.units)},
                              {"intensity", l.intensity},
                              {"range", l.range * perCm},
                              {"innerConeAngle", l.innerConeAngle},
                              {"outerConeAngle", l.outerConeAngle},
                              {"width", l.width * perCm},
                              {"height", l.height * perCm},
                              {"sourceRadius", l.sourceRadius * perCm},
                              {"texture", l.texture},
                              {"extra", l.extra}};
    setLilaData(prim, "light", VtValue(j.dump()));
    return prim;
}

void Writer::writeCurves(size_t i, const SdfPath& at)
{
    const curve::Splines& splines = curve::shapeSplines(g, snap.nodes[i].source);
    auto file = [&](const glm::dvec3& p) { return vec3f(axes.pointToFile(p)); };
    bool allPoly = !splines.empty(), allBezier = !splines.empty();
    const bool closed = !splines.empty() && splines.front().closed;
    for (const curve::Spline& s : splines)
    {
        allPoly = allPoly && s.kind == curve::Kind::Poly && s.closed == closed;
        allBezier = allBezier && s.kind == curve::Kind::Bezier && s.closed == closed;
    }
    VtVec3fArray points;
    VtIntArray counts;
    if (allPoly || allBezier)
    {
        UsdGeomBasisCurves bc = UsdGeomBasisCurves::Define(stage, at);
        bc.CreateTypeAttr().Set(allPoly ? UsdGeomTokens->linear : UsdGeomTokens->cubic);
        if (allBezier)
            bc.CreateBasisAttr().Set(UsdGeomTokens->bezier);
        bc.CreateWrapAttr().Set(closed ? UsdGeomTokens->periodic : UsdGeomTokens->nonperiodic);
        for (curve::Spline s : splines)
        {
            const size_t before = points.size();
            if (allPoly)
                for (const glm::dvec3& p : s.points)
                    points.push_back(file(p));
            else
            {
                s.normalize();
                for (size_t a = 0; a < s.points.size(); ++a)
                {
                    points.push_back(file(s.points[a]));
                    const bool last = a + 1 == s.points.size();
                    if (last && !closed)
                        break;
                    points.push_back(file(s.handleR[a]));
                    points.push_back(file(s.handleL[last ? 0 : a + 1]));
                }
            }
            counts.push_back(int(points.size() - before));
        }
        bc.CreatePointsAttr().Set(points);
        bc.CreateCurveVertexCountsAttr().Set(counts);
        VtVec3fArray extent;
        if (UsdGeomPointBased::ComputeExtent(points, &extent))
            bc.CreateExtentAttr().Set(extent);
        return;
    }
    // Anything else (NURBS, or kinds mixed in one node) as NURBS, exact; closed ones unrolled to their periodic knots.
    UsdGeomNurbsCurves nc = UsdGeomNurbsCurves::Define(stage, at);
    VtIntArray order;
    VtDoubleArray knots, weights;
    bool rational = false;
    for (const curve::Spline& src : splines)
    {
        const curve::Spline s = src.kind == curve::Kind::Nurbs ? src : curve::toNurbs(src);
        const std::vector<glm::dvec4> cv = curve::controlPoints(s);
        const std::vector<double> kv = curve::knotVector(s);
        for (const glm::dvec4& h : cv)
        {
            points.push_back(file(glm::dvec3(h) / h.w));
            weights.push_back(h.w);
            rational = rational || std::abs(h.w - 1.0) > 1e-12;
        }
        knots.insert(knots.end(), kv.begin(), kv.end());
        counts.push_back(int(cv.size()));
        order.push_back(s.degree + 1);
    }
    nc.CreatePointsAttr().Set(points);
    nc.CreateCurveVertexCountsAttr().Set(counts);
    nc.CreateOrderAttr().Set(order);
    nc.CreateKnotsAttr().Set(knots);
    if (rational)
        nc.CreatePointWeightsAttr().Set(weights);
    VtVec3fArray extent;
    if (UsdGeomPointBased::ComputeExtent(points, &extent))
        nc.CreateExtentAttr().Set(extent);
}


/** Every prim's path, before anything is written, so bindings can name prims that come later. */
void Writer::planPaths(size_t i, const SdfPath& parent, const std::string& name)
{
    const SdfPath at = parent.AppendChild(TfToken(name));
    prims[i] = at;
    for (Skel& s : skels)
        if (s.anchor == int(i))
            s.path = at.AppendChild(TfToken(uniqueChild(at, skeletonName(s))));
    std::vector<size_t> kids;
    for (size_t c = 0; c < snap.nodes.size(); ++c)
        if (usdParent[c] == int(i) && !snap.nodes[c].joint)
            kids.push_back(c);
    const bool lod = lodWritable(i);
    int level = 0;
    for (size_t c : kids)
    {
        if (lod && snap.nodes[c].parent == int(i))
        {
            lodLevel[c] = 1;
            planPaths(c, at, "LOD" + std::to_string(level++));
        }
        else
            planPaths(c, at, uniqueChild(at, identifier(snap.nodes[c].name, "node")));
    }
}

void Writer::writeNode(size_t i)
{
    const ExportNode& e = snap.nodes[i];
    const SdfPath at = prims[i];
    const scene::Node* src = g.node(e.source);
    const bool isMesh = e.mesh && e.mesh->numFaces() > 0;
    const bool isCurve = !isMesh && src && src->type().isA("curve") && !curve::shapeSplines(g, e.source).empty();
    UsdPrim prim;
    inLayer(geoLayer, [&] {
        if (isMesh)
            prim = UsdGeomMesh::Define(stage, at).GetPrim();
        else if (e.camera)
            prim = UsdGeomCamera::Define(stage, at).GetPrim();
        else if (e.light)
            prim = defineLight(i, at);
        else if (isCurve)
        {
            writeCurves(i, at);
            prim = stage->GetPrimAtPath(at);
        }
        else
            prim = UsdGeomXform::Define(stage, at).GetPrim();
        if (prim.GetName().GetString() != e.name)
            setLilaData(prim, "name", VtValue(e.name));
        writeXform(i, prim);
        if (src && !src->as<bool>(scene::Transform::kVisibility))
            UsdGeomImageable(prim).CreateVisibilityAttr().Set(UsdGeomTokens->invisible);
        if (e.camera)
            writeCamera(i, prim);
        exportComponents(g, e.source, prim);
    });
    for (size_t k = 0; k < skels.size(); ++k)
        if (skels[k].anchor == int(i))
            writeSkeleton(k);
    if (isMesh)
        inLayer(geoLayer, [&] { writeMesh(i, prim); });

    std::vector<size_t> kids, levels;
    for (size_t c = 0; c < snap.nodes.size(); ++c)
        if (usdParent[c] == int(i) && !snap.nodes[c].joint)
            (lodLevel[c] && snap.nodes[c].parent == int(i) ? levels : kids).push_back(c);
    if (!levels.empty())
    {
        // Materials live outside the variants; making them first keeps them out of the variant edit target.
        if (mats)
            inLayer(matLayer, [&] {
                for (size_t lv : levels)
                    for (const auto& [slot, count] : scene::materials::slotUsage(*snap.nodes[lv].mesh))
                        mats->material(slot);
                for (size_t lv : levels)
                    if (snap.nodes[lv].mesh->materialNames().empty())
                        mats->material(scene::materials::kDefaultSlot);
            });
        // Unreal's LOD convention: a variant set named LOD, variant LODn holding one mesh prim named LODn.
        inLayer(geoLayer, [&] {
            UsdVariantSet vs = prim.GetVariantSets().AddVariantSet("LOD");
            for (size_t k = 0; k < levels.size(); ++k)
            {
                const std::string variant = "LOD" + std::to_string(k);
                vs.AddVariant(variant);
                vs.SetVariantSelection(variant);
                UsdEditContext ctx(vs.GetVariantEditContext());
                ++variantDepth;
                writeNode(levels[k]);
                --variantDepth;
            }
            vs.SetVariantSelection("LOD0");
            if (!e.lodScreenSizes.empty())
                setLilaData(prim, "lodScreenSizes", VtValue(VtDoubleArray(e.lodScreenSizes.begin(), e.lodScreenSizes.end())));
        });
    }
    else if (e.lodGroup && opts.lods)
        res.warnings.push_back(std::format("{}: LOD levels with skins, morphs, animation or children are written as plain children", e.name));
    for (size_t c : kids)
        writeNode(c);
}

void Writer::writeSkeleton(size_t k)
{
    Skel& s = skels[k];
    inLayer(geoLayer, [&] {
        UsdSkelSkeleton sk = UsdSkelSkeleton::Define(stage, s.path);
        setLilaData(sk.GetPrim(), "wrapper", VtValue(true));
        if (s.synthetic)
            setLilaData(sk.GetPrim(), "synthetic", VtValue(true));
        sk.CreateJointsAttr().Set(s.tokens);
        const glm::dmat4 anchorBind = s.anchor >= 0 ? snap.nodes[size_t(s.anchor)].bindWorld : glm::dmat4(1.0);
        VtMatrix4dArray rest, bind;
        if (s.synthetic)
        {
            rest.assign(1, GfMatrix4d(1.0));
            bind.assign(1, GfMatrix4d(1.0));
        }
        else
            for (int j : s.joints)
            {
                rest.push_back(toGf(axes.matrixToFile(snap.nodes[size_t(j)].local)));
                bind.push_back(toGf(axes.matrixToFile(glm::inverse(anchorBind) * snap.nodes[size_t(j)].bindWorld)));
            }
        sk.CreateRestTransformsAttr().Set(rest);
        sk.CreateBindTransformsAttr().Set(bind);
        if (!s.synthetic)
        {
            VtTokenArray names;
            for (int j : s.joints)
                names.push_back(TfToken(snap.nodes[size_t(j)].name));
            sk.CreateJointNamesAttr().Set(names);
        }
        // An animation only when something drives it: joints in a take, or blend shapes on its meshes.
        bool shapes = false;
        for (int m : s.meshes)
            shapes = shapes || (snap.nodes[size_t(m)].morphs && !snap.nodes[size_t(m)].morphs->targets.empty());
        if (!skelAnimated[k] && !shapes)
            return;
        s.anim = UsdSkelAnimation::Define(stage, s.path.AppendChild(TfToken("Animation")));
        if (!s.synthetic && skelAnimated[k])
            s.anim.CreateJointsAttr().Set(s.tokens);
        UsdSkelBindingAPI::Apply(sk.GetPrim()).CreateAnimationSourceRel().SetTargets({s.anim.GetPath()});
    });
}

/**
 * Lighting channels as UsdLux light links: each light's lightLink collection includes the mesh prims sharing a channel
 * with it, or the whole stage (includeRoot) when it lights every one. The channel names go in custom data for Lila.
 */
void Writer::writeLightLinks()
{
    namespace lc = scene::lightingChannels;
    std::vector<scene::NodeId> sources;
    for (const ExportNode& e : snap.nodes)
        sources.push_back(e.source);
    res.lightingChannels = lc::usage(g, sources);
    if (!opts.lightingChannels || res.lightingChannels.empty())
        return;
    std::vector<size_t> lights, meshes;
    for (size_t i = 0; i < snap.nodes.size(); ++i)
    {
        const scene::Node* n = g.node(snap.nodes[i].source);
        if (!n || prims[i].IsEmpty() || lodLevel[i])
            continue;
        if (snap.nodes[i].light)
            lights.push_back(i);
        else if (meshPrim(i))
            meshes.push_back(i);
        if (n->type().attrIndex(lc::kAttr) >= 0)
        {
            const std::vector<std::string> channels = lc::of(*n);
            setLilaData(stage->GetPrimAtPath(prims[i]), "lightingChannels", VtValue(VtStringArray(channels.begin(), channels.end())));
        }
    }
    for (size_t l : lights)
    {
        const scene::Node& light = *g.node(snap.nodes[l].source);
        std::vector<SdfPath> lit;
        for (size_t m : meshes)
            if (lc::lights(light, *g.node(snap.nodes[m].source)))
                lit.push_back(prims[m]);
        UsdCollectionAPI link = UsdCollectionAPI::Apply(stage->GetPrimAtPath(prims[l]), TfToken("lightLink"));
        const bool all = lit.size() == meshes.size();
        link.CreateIncludeRootAttr(VtValue(all));
        if (!all)
        {
            UsdRelationship includes = link.CreateIncludesRel();
            for (const SdfPath& p : lit)
                includes.AddTarget(p);
        }
    }
}

/**
 * Each skeleton and the meshes bound to it need a SkelRoot above them all: their closest common Xform, or the file's
 * root prim. Nested ones fold into the outermost.
 */
void Writer::writeSkelRoots()
{
    std::set<SdfPath> roots;
    for (const Skel& s : skels)
    {
        SdfPath common = s.path.GetParentPath();
        for (int m : s.meshes)
            if (!prims[size_t(m)].IsEmpty() && !lodLevel[size_t(m)])
                common = common.GetCommonPrefix(prims[size_t(m)].GetParentPath());
        while (common != rootPath && common.HasPrefix(rootPath))
        {
            UsdPrim p = stage->GetPrimAtPath(common);
            if (p && p.GetTypeName() == TfToken("Xform"))
                break;
            common = common.GetParentPath();
        }
        if (!common.HasPrefix(rootPath))
            common = rootPath;
        roots.insert(common);
    }
    inLayer(geoLayer, [&] {
        for (const SdfPath& r : roots)
        {
            bool nested = false;
            for (const SdfPath& other : roots)
                nested = nested || (other != r && r.HasPrefix(other));
            if (!nested)
                stage->GetPrimAtPath(r).SetTypeName(TfToken("SkelRoot"));
        }
    });
}

/**
 * A take's samples: transforms, cameras, joints and blend shape weights. restForStill (inside a take variant) also
 * writes the rest value of whatever another take animates and this one doesn't.
 */
void Writer::writeTake(const ExportTake* take, bool restForStill)
{
    LILA_ZONE();

    const size_t n = snap.nodes.size();
    const size_t frames = take ? take->times.size() : 0;
    prevRot.assign(n, glm::dvec3(0.0));
    for (size_t i = 0; i < n; ++i)
    {
        if (prims[i].IsEmpty() || lodLevel[i] || (!ops[i].t && !ops[i].m))
            continue;
        if (take && animatedIn(i, take))
        {
            for (size_t f = 0; f < frames; ++f)
                setXform(i, take, f, code(take->times[f]));
            if (takes.size() == 1)
                recordCurves(i);
        }
        else if (restForStill && animatedAnywhere[i])
            setXform(i, nullptr, 0, UsdTimeCode::Default());
    }
    // Cameras: lens and clipping per frame.
    for (size_t i = 0; i < n; ++i)
    {
        if (!snap.nodes[i].camera || prims[i].IsEmpty() || lodLevel[i])
            continue;
        UsdGeomCamera cam(stage->GetPrimAtPath(prims[i]));
        const bool moves = take && i < take->cameras.size() && !take->cameras[i].empty();
        if (!moves && !(restForStill && cameraAnimated[i]))
            continue;
        auto set = [&](const fbx::ExportCamera& c, UsdTimeCode t) {
            cam.GetFocalLengthAttr().Set(float(c.focalLength / axes.cmPerUnit), t);
            if (!c.ortho)
            {
                cam.GetHorizontalApertureAttr().Set(float(c.filmWidth * 25.4 / axes.cmPerUnit), t);
                cam.GetVerticalApertureAttr().Set(float(c.filmHeight * 25.4 / axes.cmPerUnit), t);
            }
            cam.GetClippingRangeAttr().Set(GfVec2f(float(c.nearClip / axes.cmPerUnit), float(c.farClip / axes.cmPerUnit)), t);
        };
        if (moves)
            for (size_t f = 0; f < frames && f < take->cameras[i].size(); ++f)
                set(take->cameras[i][f], code(take->times[f]));
        else
            set(*snap.nodes[i].camera, UsdTimeCode::Default());
    }
    // Point caches: the mesh's points (and extent) per frame.
    for (size_t i = 0; i < n; ++i)
    {
        if (!meshPrim(i))
            continue;
        const bool moves = take && i < take->meshPoints.size() && !take->meshPoints[i].empty();
        if (!moves && !(restForStill && pointsAnimated[i]))
            continue;
        UsdGeomMesh mesh(stage->GetPrimAtPath(prims[i]));
        auto set = [&](std::span<const glm::dvec3> pts, UsdTimeCode t) {
            const VtVec3fArray points = filePoints(i, pts);
            mesh.GetPointsAttr().Set(points, t);
            if (VtVec3fArray extent; UsdGeomPointBased::ComputeExtent(points, &extent))
                mesh.CreateExtentAttr().Set(extent, t);
        };
        if (moves)
            for (size_t f = 0; f < frames && f < take->meshPoints[i].size(); ++f)
                set(take->meshPoints[i][f], code(take->times[f]));
        else
            set(snap.nodes[i].mesh->positions(), UsdTimeCode::Default());
    }
    // Skeletons: joint transforms per frame, and the blend shape weights their meshes read.
    for (size_t k = 0; k < skels.size(); ++k)
    {
        Skel& s = skels[k];
        if (!s.anim)
            continue;
        bool jointsMove = false;
        for (int j : s.joints)
            jointsMove = jointsMove || (take && size_t(j) < take->locals.size() && !take->locals[size_t(j)].empty());
        auto joints = [&](const ExportTake* t, size_t f) {
            VtMatrix4dArray xf;
            for (int j : s.joints)
                xf.push_back(toGf(axes.matrixToFile(exportedLocal(size_t(j), t, f))));
            return xf;
        };
        if (!s.synthetic && jointsMove)
            for (size_t f = 0; f < frames; ++f)
                s.anim.SetTransforms(joints(take, f), code(take->times[f]));
        else if (!s.synthetic && restForStill && skelAnimated[k])
            s.anim.SetTransforms(joints(nullptr, 0), UsdTimeCode::Default());

        if (s.channels.empty())
            continue;
        auto weightsAt = [&](const ExportTake* t, size_t f) {
            VtFloatArray w;
            for (const Skel::Channel& c : s.channels)
            {
                const ExportNode& mesh = snap.nodes[size_t(c.mesh)];
                const auto* perTarget = t && size_t(c.mesh) < t->morphWeights.size() && !t->morphWeights[size_t(c.mesh)].empty()
                                            ? &t->morphWeights[size_t(c.mesh)]
                                            : nullptr;
                // A slider's weight from its shapes' coefficients: the shapes sit at their weights, interpolated linearly.
                double v = 0.0;
                for (size_t sh = 0; sh < c.group.entries.size(); ++sh)
                {
                    const size_t entry = c.group.entries[sh];
                    const double coef = perTarget && entry < perTarget->size() && f < (*perTarget)[entry].size() ? (*perTarget)[entry][f]
                                                                                                             : mesh.morphs->targets[entry].weight;
                    v += coef * c.group.weights[sh];
                }
                w.push_back(float(v));
            }
            return w;
        };
        bool weightsMove = false;
        for (const Skel::Channel& c : s.channels)
            weightsMove = weightsMove || (take && size_t(c.mesh) < take->morphWeights.size() && !take->morphWeights[size_t(c.mesh)].empty());
        if (weightsMove)
            for (size_t f = 0; f < frames; ++f)
                s.anim.GetBlendShapeWeightsAttr().Set(weightsAt(take, f), code(take->times[f]));
        else if (restForStill && weightsAnimated[k])
            s.anim.GetBlendShapeWeightsAttr().Set(weightsAt(nullptr, 0), UsdTimeCode::Default());
    }
}

/**
 * The node's own curves beside its samples (lila:curves on xformOpOrder), so building it again from the samples gets the
 * same keys back. Only where the prim's channels are the node's: the importer checks them against the samples anyway.
 */
void Writer::recordCurves(size_t i)
{
    using namespace scene;
    const ExportNode& e = snap.nodes[i];
    if (ops[i].matrix || !e.plainTrs || usdParent[i] != e.parent || turned(e) || !g.node(e.source))
        return;
    static constexpr const char* kGroup[3] = {"translate", "rotate", "scale"};
    static constexpr int kAttr[3] = {Transform::kTranslate, Transform::kRotate, Transform::kScale};
    json out = json::object();
    for (int a = 0; a < 3; ++a)
    {
        json channels = json::array();
        bool any = false;
        for (int c = 0; c < 3; ++c)
        {
            json entry;
            if (auto src = g.source({e.source, kAttr[a], c}))
                if (const Node* cn = g.node(src->node); cn && anim::isCurve(*cn))
                {
                    const anim::Curve cv = anim::curveOf(*cn);
                    entry = {{"keys", cv.keysToJson()}, {"pre", anim::infinityName(cv.pre)}, {"post", anim::infinityName(cv.post)},
                             {"weighted", cv.weighted}, {"loop", cv.loop}};
                    any = true;
                }
            channels.push_back(std::move(entry));
        }
        if (any)
            out[kGroup[a]] = std::move(channels);
    }
    if (auto src = g.source({e.source, Transform::kRotate, -1}))
        if (const Node* cn = g.node(src->node); cn && anim::isRotationCurve(*cn))
        {
            const anim::RotationCurve rc = anim::rotationCurveOf(*cn);
            out["rotateQ"] = {{"keys", rc.keysToJson()},
                              {"pre", anim::infinityName(rc.pre)},
                              {"post", anim::infinityName(rc.post)},
                              {"weighted", rc.weighted},
                              {"loop", rc.loop},
                              {"interp", anim::rotInterpName(rc.interp)},
                              {"order", int(rc.order)}};
        }
    if (!out.empty())
        setLilaData(UsdGeomXformable(stage->GetPrimAtPath(prims[i])).GetXformOpOrderAttr(), "curves", VtValue(out.dump()));
}

/** The rest values of what some take animates, when there is no take variant to hold them. */
void Writer::writeAnimatedRest()
{
    for (size_t i = 0; i < snap.nodes.size(); ++i)
        if (!prims[i].IsEmpty() && !lodLevel[i] && animatedAnywhere[i])
            setXform(i, nullptr, 0, UsdTimeCode::Default());
    for (size_t k = 0; k < skels.size(); ++k)
    {
        Skel& s = skels[k];
        if (!s.anim)
            continue;
        if (!s.synthetic && skelAnimated[k])
        {
            VtMatrix4dArray rest;
            for (int j : s.joints)
                rest.push_back(toGf(axes.matrixToFile(snap.nodes[size_t(j)].local)));
            s.anim.SetTransforms(rest, UsdTimeCode::Default());
        }
        if (!s.channels.empty())
        {
            VtFloatArray w;
            for (const Skel::Channel& c : s.channels)
            {
                double v = 0.0;
                for (size_t sh = 0; sh < c.group.entries.size(); ++sh)
                    v += snap.nodes[size_t(c.mesh)].morphs->targets[c.group.entries[sh]].weight * c.group.weights[sh];
                w.push_back(float(v));
            }
            s.anim.GetBlendShapeWeightsAttr().Set(w, UsdTimeCode::Default());
        }
    }
}

bool Writer::save()
{
    std::string ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const fs::path outDir = package ? stagingDir : path.parent_path();
    const std::string stem = utf8(path.stem());
    SdfLayer::FileFormatArguments args;
    if (ext == ".usd")
        args["format"] = opts.format == Format::Usda ? "usda" : "usdc";
    const fs::path rootFile = package ? stagingDir / utf8Path(stem + ".usdc") : path;
    std::vector<std::pair<fs::path, std::string>> written; // file, its path inside a package
    SdfLayerHandle root = stage->GetRootLayer();
    if (geoLayer)
    {
        const bool text = ext == ".usda" || (ext == ".usd" && opts.format == Format::Usda);
        const std::string subExt = text ? ".usda" : ".usdc";
        std::vector<std::string> subs;
        const std::pair<SdfLayerRefPtr, const char*> parts[] = {{animLayer, "anim"}, {matLayer, "materials"}, {geoLayer, "geometry"}};
        for (const auto& [layer, part] : parts)
        {
            const std::string name = stem + "." + part + subExt;
            const fs::path file = outDir / utf8Path(name);
            if (!layer->Export(utf8(file)))
            {
                res.error = "can't write " + utf8(file);
                return false;
            }
            written.push_back({file, name});
            subs.push_back("./" + name);
        }
        root->SetSubLayerPaths(subs);
    }
    if (!root->Export(utf8(rootFile), std::string(), args))
    {
        res.error = "can't write " + utf8(rootFile);
        return false;
    }
    written.insert(written.begin(), {rootFile, stem + ".usdc"});
    if (!package)
    {
        for (const auto& [file, inner] : written)
            res.files.push_back(utf8(file));
        return true;
    }
    // usdz: the root layer first, then its sublayers and textures, stored uncompressed and aligned as the format requires.
    SdfZipFileWriter zip = SdfZipFileWriter::CreateNew(utf8(path));
    if (!zip)
    {
        res.error = "can't write " + utf8(path);
        return false;
    }
    for (const auto& [file, inner] : written)
        if (zip.AddFile(utf8(file), inner).empty())
        {
            res.error = "can't add " + inner + " to the package";
            return false;
        }
    if (mats)
        for (const auto& [file, inner] : mats->packaged())
            if (zip.AddFile(utf8(file), inner).empty())
                res.warnings.push_back(std::format("texture {} could not be packaged", utf8(file)));
    if (!zip.Save())
    {
        res.error = "can't write " + utf8(path);
        return false;
    }
    res.files.push_back(utf8(path));
    return true;
}

bool Writer::run(const std::vector<scene::NodeId>& roots)
{
    LILA_ZONE();

    fbx::ExportOptions f;
    f.kind = opts.worldSpace ? fbx::ExportKind::Static : fbx::ExportKind::Skeletal;
    f.maxInfluences = opts.maxInfluences;
    f.animation = opts.animation;
    f.bakeAnimation = true;
    f.start = opts.start;
    f.end = opts.end;
    f.takes = opts.takes;
    f.triangulate = false;
    f.tangents = false;
    f.worldSpace = opts.worldSpace;
    f.atOrigin = opts.atOrigin;
    f.children = opts.children;
    f.deformOnly = opts.deformOnly;
    f.relativeTo = opts.relativeTo;
    f.childless = opts.childless;
    f.localRoots = opts.localRoots;
    snap = fbx::gatherExport(g, roots, f);
    for (const std::string& w : snap.warnings)
        if (w.find("worldSpace is ignored") == std::string::npos)
            res.warnings.push_back(w);
    if (snap.nodes.empty())
    {
        res.error = "nothing to export";
        return false;
    }
    if (opts.worldSpace && opts.animation)
        res.warnings.push_back("worldSpace writes the current pose: no skins, morphs or animation");
    if (snap.take)
        takes.push_back(&*snap.take);
    for (const ExportTake& t : snap.extraTakes)
        takes.push_back(&t);
    tcps = !takes.empty() && takes[0]->fps > 0.0 ? takes[0]->fps : (snap.fps > 0.0 ? snap.fps : 30.0);
    if (opts.timeCodesPerSecond > 0.0)
        tcps = opts.timeCodesPerSecond;

    std::string ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    package = ext == ".usdz";
    if (package)
    {
        std::error_code ec;
        stagingDir = fs::temp_directory_path(ec) / utf8Path("lila_usdz_" + std::to_string(std::hash<std::string>()(utf8(path))));
        fs::remove_all(stagingDir, ec);
        fs::create_directories(stagingDir, ec);
    }
    SdfLayerRefPtr rootLayer = SdfLayer::CreateAnonymous("root.usda");
    if (opts.splitLayers)
    {
        geoLayer = SdfLayer::CreateAnonymous("geometry.usda");
        matLayer = SdfLayer::CreateAnonymous("materials.usda");
        animLayer = SdfLayer::CreateAnonymous("anim.usda");
        rootLayer->SetSubLayerPaths({animLayer->GetIdentifier(), matLayer->GetIdentifier(), geoLayer->GetIdentifier()});
    }
    stage = UsdStage::Open(rootLayer);
    if (!stage)
    {
        res.error = "can't create a stage";
        return false;
    }
    UsdGeomSetStageUpAxis(stage, opts.zUp ? UsdGeomTokens->z : UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, opts.metersPerUnit);
    stage->SetTimeCodesPerSecond(tcps);
    stage->SetFramesPerSecond(tcps);
    rootLayer->SetDocumentation("Exported from Lila");

    rootPath = SdfPath::AbsoluteRootPath().AppendChild(TfToken(identifier(utf8(path.stem()), "Root")));
    inLayer(geoLayer, [&] {
        UsdPrim root = UsdGeomXform::Define(stage, rootPath).GetPrim();
        setLilaData(root, "wrapper", VtValue(true));
        UsdModelAPI(root).SetKind(TfToken("component"));
    });
    stage->SetDefaultPrim(stage->GetPrimAtPath(rootPath));
    writeGeoreference(stage, rootLayer, geoLayer);
    if (opts.materials)
    {
        TextureMode mode = TextureMode::Relative;
        if (package)
            mode = TextureMode::Package;
        else if (opts.textures == ExportOptions::Textures::Copy)
            mode = TextureMode::Copy;
        else if (opts.textures == ExportOptions::Textures::Absolute)
            mode = TextureMode::Absolute;
        mats = std::make_unique<MaterialWriter>(g, stage, rootPath.AppendChild(TfToken("Materials")), path, mode);
        mats->setShading(opts.previewSurface, opts.materialX);
    }

    planHierarchy();
    planSkeletons();
    taken[rootPath].insert("Materials");
    for (Skel& s : skels)
        if (s.anchor < 0)
            s.path = rootPath.AppendChild(TfToken(uniqueChild(rootPath, skeletonName(s))));
    for (size_t i = 0; i < snap.nodes.size(); ++i)
        if (usdParent[i] < 0 && !snap.nodes[i].joint)
            planPaths(i, rootPath, uniqueChild(rootPath, identifier(snap.nodes[i].name, "node")));

    // What moves in which take decides where values go once there are take variants.
    cameraAnimated.assign(snap.nodes.size(), 0);
    pointsAnimated.assign(snap.nodes.size(), 0);
    skelAnimated.assign(skels.size(), 0);
    weightsAnimated.assign(skels.size(), 0);
    for (const ExportTake* t : takes)
    {
        for (size_t i = 0; i < snap.nodes.size(); ++i)
        {
            if (i < t->cameras.size() && !t->cameras[i].empty())
                cameraAnimated[i] = 1;
            if (i < t->meshPoints.size() && !t->meshPoints[i].empty())
                pointsAnimated[i] = 1;
        }
        for (size_t k = 0; k < skels.size(); ++k)
        {
            for (int j : skels[k].joints)
                if (size_t(j) < t->locals.size() && !t->locals[size_t(j)].empty())
                    skelAnimated[k] = 1;
            for (int m : skels[k].meshes)
                if (size_t(m) < t->morphWeights.size() && !t->morphWeights[size_t(m)].empty())
                    weightsAnimated[k] = 1;
        }
    }

    for (size_t k = 0; k < skels.size(); ++k)
        if (skels[k].anchor < 0)
            writeSkeleton(k);
    for (size_t i = 0; i < snap.nodes.size(); ++i)
        if (usdParent[i] < 0 && !snap.nodes[i].joint)
            writeNode(i);
    inLayer(geoLayer, [&] {
        for (Skel& s : skels)
            if (!s.channels.empty())
            {
                VtTokenArray tokens;
                for (const Skel::Channel& c : s.channels)
                    tokens.push_back(TfToken(c.token));
                s.anim.CreateBlendShapesAttr().Set(tokens);
                s.anim.CreateBlendShapeWeightsAttr();
            }
    });
    writeSkelRoots();
    inLayer(geoLayer, [&] { writeLightLinks(); });
    if (mats)
        inLayer(matLayer, [&] {
            const SdfPath scopePath = rootPath.AppendChild(TfToken("Materials"));
            if (stage->GetPrimAtPath(scopePath))
                setLilaData(UsdGeomScope::Define(stage, scopePath).GetPrim(), "wrapper", VtValue(true));
        });

    // Cameras and point caches animated in some take keep no rest value outside the takes, or it would hide their variants' samples.
    const bool variants = takes.size() > 1;
    if (variants)
        inLayer(geoLayer, [&] {
            for (size_t i = 0; i < snap.nodes.size(); ++i)
                if (cameraAnimated[i] && !prims[i].IsEmpty())
                {
                    UsdGeomCamera cam(stage->GetPrimAtPath(prims[i]));
                    for (const UsdAttribute& a : {cam.GetFocalLengthAttr(), cam.GetHorizontalApertureAttr(), cam.GetVerticalApertureAttr(),
                                                  cam.GetClippingRangeAttr()})
                        a.ClearDefault();
                }
                else if (pointsAnimated[i] && meshPrim(i))
                {
                    UsdGeomMesh mesh(stage->GetPrimAtPath(prims[i]));
                    mesh.GetPointsAttr().ClearDefault();
                    mesh.GetExtentAttr().ClearDefault();
                }
        });
    if (!variants)
        inLayer(animLayer, [&] {
            writeAnimatedRest();
            if (!takes.empty())
                writeTake(takes[0], false);
        });
    else
        inLayer(animLayer, [&] {
            UsdPrim root = stage->GetPrimAtPath(rootPath);
            UsdVariantSet vs = root.GetVariantSets().AddVariantSet("take");
            VtDictionary ranges;
            std::set<std::string> used;
            std::string first;
            for (const ExportTake* t : takes)
            {
                std::string name = identifier(t->name, "take");
                for (int k = 2; used.count(name); ++k)
                    name = identifier(t->name, "take") + std::to_string(k);
                used.insert(name);
                if (first.empty())
                    first = name;
                VtDictionary range;
                range["name"] = VtValue(t->name);
                range["start"] = VtValue(t->start);
                range["end"] = VtValue(t->end);
                range["fps"] = VtValue(t->fps);
                ranges[name] = VtValue(range);
                vs.AddVariant(name);
                vs.SetVariantSelection(name);
                UsdEditContext ctx(vs.GetVariantEditContext());
                ++variantDepth;
                writeTake(t, true);
                --variantDepth;
            }
            vs.SetVariantSelection(first);
            setLilaData(root, "takes", VtValue(ranges));
        });
    if (!takes.empty())
    {
        double start = takes[0]->start, end = takes[0]->end;
        for (const ExportTake* t : takes)
            start = std::min(start, t->start), end = std::max(end, t->end);
        stage->SetStartTimeCode(start * tcps);
        stage->SetEndTimeCode(end * tcps);
    }
    if (mats)
        res.warnings.insert(res.warnings.end(), mats->warnings.begin(), mats->warnings.end());
    if (opts.inMemory)
    {
        res.stage = stage;
        for (size_t i = 0; i < snap.nodes.size(); ++i)
            if (!prims[i].IsEmpty())
                res.prims.push_back({snap.nodes[i].source, prims[i]});
        if (mats)
            for (const auto& [slot, m] : mats->written())
                res.materials[slot] = m.GetPath();
        for (const Skel& sk : skels)
        {
            std::vector<scene::NodeId> joints;
            for (int j : sk.joints)
                joints.push_back(snap.nodes[size_t(j)].source);
            res.skeletons.push_back({sk.path, std::move(joints)});
        }
        return true;
    }
    const bool ok = save();
    if (package)
    {
        std::error_code ec;
        fs::remove_all(stagingDir, ec);
    }
    return ok;
}
} // namespace

ExportResult exportUsd(scene::Graph& graph, const std::vector<scene::NodeId>& roots, const fs::path& path, const ExportOptions& opts)
{
    LILA_ZONE();

    ExportResult res;
    Diagnostics diag;
    Writer w(graph, opts, res, path);
    res.ok = w.run(roots);
    for (const std::string& e : diag.errors())
        res.warnings.push_back("OpenUSD: " + e);
    for (const std::string& wn : diag.warnings())
        res.warnings.push_back("OpenUSD: " + wn);
    return res;
}
} // namespace lila::usd
