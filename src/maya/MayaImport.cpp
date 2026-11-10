#include "maya/Maya.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/Fbx.h"
#include "fbx/FbxAxes.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "ops/Undo.h"
#include "io/References.h"
#include "io/SceneFormats.h"
#include "maya/MayaFile.h"
#include "maya/MayaIndex.h"
#include "maya/MayaRig.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/Camera.h"
#include "scene/Deformers.h"
#include "scene/DisplayLayers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/Light.h"
#include "scene/LightingChannels.h"
#include "scene/LodGroup.h"
#include "scene/Instancing.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

#include <glm/gtc/matrix_transform.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace lila::maya
{
namespace
{
using geo::Index;
using geo::kInvalid;
using scene::Graph;
using scene::NodeId;
using scene::PlugRef;
using json = nlohmann::json;

constexpr int kMaxRefDepth = 8;

const std::set<std::string_view>& transformTypes()
{
    static const std::set<std::string_view> k = {"transform", "joint", "lodGroup", "ikHandle"};
    return k;
}

const std::set<std::string_view>& creatorTypes()
{
    static const std::set<std::string_view> k = {"polyCube", "polySphere", "polyCylinder", "polyCone", "polyPlane", "polyTorus", "polyPipe"};
    return k;
}

const std::set<std::string_view>& deformerTypes()
{
    static const std::set<std::string_view> k = {"skinCluster", "blendShape", "cluster", "ffd", "nonLinear", "wire", "sculpt", "deltaMush",
                                                 "tension", "proximityWrap", "wrap", "softMod", "jiggle", "shrinkWrap", "textureDeformer",
                                                 "solidify", "morph", "tweak", "polyModifierWorld"};
    return k;
}

/** Bookkeeping nodes every scene has; nothing to convert and nothing worth reporting. */
const std::set<std::string_view>& quietTypes()
{
    static const std::set<std::string_view> k = {
        "lightLinker", "displayLayerManager", "displayLayer", "renderLayerManager", "renderLayer", "script", "materialInfo",
        "nodeGraphEditorInfo", "dagPose", "place2dTexture", "shapeEditorManager", "poseInterpolatorManager", "groupId", "groupParts",
        "aiOptions", "aiAOVDriver", "aiAOVFilter", "aiImagerDenoiserOidn", "reference", "hyperGraphInfo", "hyperLayout", "hyperView",
        "partition", "sequenceManager", "sequencer", "objectSet", "colorManagementGlobals", "polyBlindData", "uvChooser",
        "ikRPsolver", "ikSCsolver", "ikSplineSolver", "ik2Bsolver", "ikSystem", "hikSolver", "renderGlobals", "brush", "unitConversion", "timeEditor",
        "timeEditorTracks", "defaultRenderUtilityList", "renderSetup", "lookdevXGraph"};
    return k;
}

bool isLight(std::string_view t)
{
    return t.ends_with("Light") || t.ends_with("light") || t == "aiSkyDomeLight" || t == "aiMeshLight" || t == "aiPhotometricLight";
}

/** The Lila kind a Maya or Arnold light shape converts to; nothing for lights Lila has no equivalent of (mesh lights). */
std::optional<scene::lights::Kind> lightKind(std::string_view t)
{
    using scene::lights::Kind;
    if (t == "pointLight" || t == "volumeLight" || t == "aiPhotometricLight")
        return Kind::Point;
    if (t == "spotLight")
        return Kind::Spot;
    if (t == "directionalLight")
        return Kind::Directional;
    if (t == "areaLight" || t == "aiAreaLight")
        return Kind::Area;
    if (t == "ambientLight" || t == "aiSkyDomeLight")
        return Kind::Dome;
    return std::nullopt;
}

glm::dvec3 color(const MNode& n, Names whole, glm::dvec3 def)
{
    std::string_view base = *whole.begin();
    std::string r = std::string(base) + "R", g = std::string(base) + "G", b = std::string(base) + "B";
    return vec3(n, whole, {Names{r}, Names{g}, Names{b}}, def);
}

/** Maya edge loop to vertex indices: an edge walked forwards starts at its first vertex, backwards at its second. */
std::vector<int32_t> loopVertices(const MeshData& m, const MeshData::Loop& l, bool* bad)
{
    std::vector<int32_t> v;
    v.reserve(l.edges.size());
    for (int32_t e : l.edges)
    {
        int32_t idx = e >= 0 ? e : ~e;
        if (idx < 0 || size_t(idx) >= m.edges.size())
        {
            *bad = true;
            return {};
        }
        v.push_back(e >= 0 ? m.edges[size_t(idx)][0] : m.edges[size_t(idx)][1]);
    }
    return v;
}

/** Geometry in Maya space (cm, Maya axes) while history is evaluated. */
struct Geom
{
    geo::Mesh mesh;
    /** Point indices are Maya vertex indices (no topology change since the stored data). */
    bool mayaPoints = false;
    /** Maya face index to Lila face; empty when faces no longer match Maya's. */
    std::vector<Index> faceOf;
    /** Maya edges by index, while the topology matches. */
    std::vector<std::array<int32_t, 2>> mayaEdges;
    /** Per uv set, Maya uv index per corner (-1 none), while the corners match. */
    std::vector<std::vector<int32_t>> cornerUv;
};

struct Units
{
    double lin = 1.0; // file linear value to cm
    double ang = 1.0; // file angle to degrees
    double fps = 24.0;
};

/** Assembles mesh data from a mesh node's ASCII attributes (vt, ed, fc, uvst, clst, n, cd, cvd) or its binary blob. */
std::shared_ptr<const MeshData> meshDataOf(const MNode& n, std::vector<std::string>& warnings)
{
    for (const AttrValue& a : n.attrs)
        if (a.type == "mesh" && a.mesh)
            return a.mesh;

    auto m = std::make_shared<MeshData>();
    bool any = false;
    auto ensure = [](auto& vec, size_t size) {
        if (vec.size() < size)
            vec.resize(size);
    };
    for (const AttrValue& a : n.attrs)
    {
        std::vector<AttrSeg> segs = splitAttr(a.attr);
        if (segs.empty())
            continue;
        const AttrSeg& s0 = segs[0];
        const size_t lo = size_t(std::max(s0.lo, 0));
        if (segs.size() == 1 && (s0.name == "vt" || s0.name == "vrts"))
        {
            any = true;
            ensure(m->points, lo + a.nums.size() / 3);
            for (size_t i = 0; i + 2 < a.nums.size(); i += 3)
                m->points[lo + i / 3] = {a.nums[i], a.nums[i + 1], a.nums[i + 2]};
        }
        else if (segs.size() == 1 && (s0.name == "ed" || s0.name == "edge"))
        {
            any = true;
            ensure(m->edges, lo + a.nums.size() / 3);
            ensure(m->edgeHard, lo + a.nums.size() / 3);
            for (size_t i = 0; i + 2 < a.nums.size(); i += 3)
            {
                m->edges[lo + i / 3] = {int32_t(a.nums[i]), int32_t(a.nums[i + 1])};
                m->edgeHard[lo + i / 3] = a.nums[i + 2] == 0.0 ? 1 : 0;
            }
        }
        else if (segs.size() == 1 && (s0.name == "fc" || s0.name == "face") && a.mesh)
        {
            any = true;
            ensure(m->faces, lo + a.mesh->faces.size());
            for (size_t i = 0; i < a.mesh->faces.size(); ++i)
                m->faces[lo + i] = a.mesh->faces[i];
        }
        else if (segs.size() == 1 && (s0.name == "n" || s0.name == "normals"))
        {
            ensure(m->normals, lo + a.nums.size() / 3);
            for (size_t i = 0; i + 2 < a.nums.size(); i += 3)
                m->normals[lo + i / 3] = glm::vec3(float(a.nums[i]), float(a.nums[i + 1]), float(a.nums[i + 2]));
        }
        else if (segs.size() == 2 && (s0.name == "uvst" || s0.name == "uvSet") && s0.lo >= 0)
        {
            ensure(m->uvSets, size_t(s0.lo) + 1);
            MeshData::UvSet& set = m->uvSets[size_t(s0.lo)];
            const AttrSeg& s1 = segs[1];
            if ((s1.name == "uvsn" || s1.name == "uvSetName") && !a.strs.empty())
                set.name = a.strs[0];
            else if (s1.name == "uvsp" || s1.name == "uvSetPoints")
            {
                size_t at = size_t(std::max(s1.lo, 0));
                ensure(set.uvs, at + a.nums.size() / 2);
                for (size_t i = 0; i + 1 < a.nums.size(); i += 2)
                    set.uvs[at + i / 2] = glm::vec2(float(a.nums[i]), float(a.nums[i + 1]));
            }
        }
        else if (segs.size() == 2 && (s0.name == "clst" || s0.name == "colorSet") && s0.lo >= 0)
        {
            ensure(m->colorSets, size_t(s0.lo) + 1);
            MeshData::ColorSet& set = m->colorSets[size_t(s0.lo)];
            const AttrSeg& s1 = segs[1];
            if ((s1.name == "clsn" || s1.name == "colorName") && !a.strs.empty())
                set.name = a.strs[0];
            else if ((s1.name == "rprt" || s1.name == "representation") && !a.nums.empty())
                set.channels = a.nums[0] == 2.0 ? 1 : a.nums[0] == 3.0 ? 3 : 4;
            else if (s1.name == "clsp" || s1.name == "colorSetPoints")
            {
                const size_t k = size_t(set.channels);
                size_t at = size_t(std::max(s1.lo, 0));
                ensure(set.colors, at + a.nums.size() / k);
                for (size_t i = 0; i + k - 1 < a.nums.size(); i += k)
                {
                    glm::vec4 c(1.0f);
                    if (k == 1)
                        c.a = float(a.nums[i]);
                    else
                        for (size_t j = 0; j < k; ++j)
                            c[int(j)] = float(a.nums[i + j]);
                    set.colors[at + i / k] = c;
                }
            }
        }
        else if (segs.size() == 1 && (s0.name == "cd" || s0.name == "cvd") && a.strs.size() >= 3)
        {
            // Index_Data Edge|Vertex count (index value)...
            auto& out = s0.name == "cd" ? m->edgeCreases : m->vertexCreases;
            for (size_t i = 3; i + 1 < a.strs.size(); i += 2)
                out.push_back({int32_t(std::strtol(a.strs[i].c_str(), nullptr, 10)), std::strtof(a.strs[i + 1].c_str(), nullptr)});
        }
    }
    if (!any)
        return nullptr;
    if (m->faces.empty() && !m->points.empty())
        warnings.push_back(std::format("{}: mesh has points but no faces", n.name));
    return m;
}

Geom buildGeom(const MeshData& d, double lin, const std::string& name, std::vector<std::string>& warnings)
{
    LILA_ZONE();

    Geom g;
    geo::Mesh& m = g.mesh;
    g.mayaPoints = true;
    g.mayaEdges = d.edges;
    m.reserve(d.points.size(), d.edges.size(), d.faces.size(), d.edges.size() * 2);
    for (const glm::dvec3& p : d.points)
        m.addPoint(p * lin);

    struct Placed
    {
        Index first;
        const MeshData::Loop* loop;
        size_t fv; // global face-vertex index of the loop's first corner
    };
    std::vector<Placed> placed;
    size_t bad = 0, holes = 0, fv = 0;
    std::vector<Index> pts;
    g.faceOf.assign(d.faces.size(), kInvalid);
    for (size_t f = 0; f < d.faces.size(); ++f)
    {
        const MeshData::Face& face = d.faces[f];
        size_t faceFv = fv;
        for (const auto& l : face.loops)
            fv += l.edges.size();
        holes += face.loops.size() > 1 ? face.loops.size() - 1 : 0;
        if (face.loops.empty())
            continue;
        bool broken = false;
        std::vector<int32_t> verts = loopVertices(d, face.loops[0], &broken);
        pts.clear();
        for (int32_t v : verts)
        {
            if (v < 0 || size_t(v) >= d.points.size())
                broken = true;
            pts.push_back(Index(v));
        }
        Index lf = broken || pts.size() < 3 ? kInvalid : m.addFace(pts);
        if (lf == kInvalid)
        {
            ++bad;
            continue;
        }
        g.faceOf[f] = lf;
        placed.push_back({m.faceCorner(lf), &face.loops[0], faceFv});
    }
    if (bad)
        warnings.push_back(std::format("{}: {} degenerate face(s) skipped", name, bad));
    if (holes)
        warnings.push_back(std::format("{}: {} face hole(s) dropped (Lila faces have no holes)", name, holes));

    const size_t corners = m.cornerCapacity();
    g.cornerUv.assign(d.uvSets.size(), std::vector<int32_t>(corners, -1));
    for (size_t s = 0; s < d.uvSets.size(); ++s)
    {
        const auto& uvs = d.uvSets[s].uvs;
        std::span<glm::vec2> out = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(s)));
        for (const Placed& p : placed)
        {
            if (p.loop->uv.size() <= s || p.loop->uv[s].size() != p.loop->edges.size())
                continue;
            for (size_t k = 0; k < p.loop->edges.size(); ++k)
            {
                int32_t i = p.loop->uv[s][k];
                g.cornerUv[s][p.first + k] = i;
                if (i >= 0 && size_t(i) < uvs.size())
                    out[p.first + k] = uvs[size_t(i)];
            }
        }
    }
    for (size_t s = 0; s < d.colorSets.size(); ++s)
    {
        const auto& cols = d.colorSets[s].colors;
        const std::string& given = d.colorSets[s].name;
        std::span<glm::vec4> out = geo::colors::add(m, geo::colors::uniqueName(m, given.empty() ? "colorSet" : given));
        for (const Placed& p : placed)
        {
            if (p.loop->color.size() <= s || p.loop->color[s].size() != p.loop->edges.size())
                continue;
            for (size_t k = 0; k < p.loop->edges.size(); ++k)
                if (int32_t i = p.loop->color[s][k]; i >= 0 && size_t(i) < cols.size())
                    out[p.first + k] = cols[size_t(i)];
        }
    }

    if (!d.edgeHard.empty())
    {
        std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        for (size_t e = 0; e < d.edges.size() && e < d.edgeHard.size(); ++e)
            if (d.edgeHard[e])
                if (Index le = m.findEdge(Index(d.edges[e][0]), Index(d.edges[e][1])); le != kInvalid)
                    hard[le] = true;
    }
    if (!d.edgeCreases.empty())
    {
        std::span<float> crease = m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
        for (const auto& [e, v] : d.edgeCreases)
            if (e >= 0 && size_t(e) < d.edges.size())
                if (Index le = m.findEdge(Index(d.edges[size_t(e)][0]), Index(d.edges[size_t(e)][1])); le != kInvalid)
                    crease[le] = v;
    }
    if (!d.vertexCreases.empty())
        warnings.push_back(std::format("{}: {} vertex crease(s) dropped (Lila creases edges only)", name, d.vertexCreases.size()));

    // Locked normals: per face-vertex, unset ones share a set normal of the same point, else the computed one.
    bool anyNormal = false;
    std::vector<glm::vec3> set(corners, glm::vec3(0.0f));
    std::vector<char> has(corners, 0);
    std::unordered_map<Index, glm::vec3> byPoint;
    for (const Placed& p : placed)
        for (size_t k = 0; k < p.loop->edges.size(); ++k)
        {
            size_t i = p.fv + k;
            if (i >= d.normals.size())
                continue;
            glm::vec3 n = d.normals[i];
            if (std::abs(n.x) >= 1e19f || std::abs(n.y) >= 1e19f || std::abs(n.z) >= 1e19f || glm::dot(n, n) == 0.0f)
                continue;
            n = glm::normalize(n);
            set[p.first + k] = n;
            has[p.first + k] = 1;
            byPoint.emplace(m.cornerPoint(Index(p.first + k)), n);
            anyNormal = true;
        }
    if (anyNormal)
    {
        std::vector<glm::vec3> computed = geo::cornerNormals(m);
        std::span<glm::vec3> out = m.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        for (Index c = 0; c < corners; ++c)
        {
            if (!m.cornerValid(c))
                continue;
            if (has[c])
                out[c] = set[c];
            else if (auto it = byPoint.find(m.cornerPoint(c)); it != byPoint.end())
                out[c] = it->second;
            else
                out[c] = computed[c];
        }
    }
    return g;
}

/** Edges of a component list on a Maya-topology mesh; empty when it can't be mapped. */
std::vector<Index> edgesOf(const Geom& g, const std::vector<std::string>& comps)
{
    std::vector<Index> out;
    for (const std::string& s : comps)
    {
        std::string kind;
        int lo = 0, hi = 0;
        if (!parseComponent(s, &kind, &lo, &hi) || kind != "e")
            continue;
        if (hi < 0)
            hi = int(g.mayaEdges.size()) - 1;
        for (int e = lo; e <= hi && e >= 0 && size_t(e) < g.mayaEdges.size(); ++e)
            if (Index le = g.mesh.findEdge(Index(g.mayaEdges[size_t(e)][0]), Index(g.mayaEdges[size_t(e)][1])); le != kInvalid)
                out.push_back(le);
    }
    return out;
}

bool coversAll(const std::vector<std::string>& comps)
{
    if (comps.empty())
        return true;
    for (const std::string& s : comps)
        if (s.ends_with("[*]"))
            return true;
    return false;
}

/** While a referenced file is converted to its own .lila: where it goes, and the reference records it collects. */
struct Nested
{
    fs::path outDir;
    std::vector<json> refs;
};

/** Bumped when a conversion would come out differently, so cached .lila files of referenced Maya files are redone. */
constexpr int kConverterVersion = 7;

/**
 * How a referenced file's node is named relative to its namespace: the bare name when no other node has it, else its
 * path from the file's top ("Body|L|Arm"), the form reference edits resolve.
 */
std::string relName(const Graph& g, NodeId id, const std::string& ns)
{
    auto strip = [&](const std::string& name) { return !ns.empty() && name.rfind(ns + ":", 0) == 0 ? name.substr(ns.size() + 1) : name; };
    const scene::Node* n = g.node(id);
    if (g.findAll(n->name()).size() == 1)
        return strip(n->name());
    std::string out;
    for (NodeId cur = id; cur != scene::kNoNode; cur = g.parent(cur))
    {
        const std::string& name = g.node(cur)->name();
        if (!ns.empty() && name.rfind(ns + ":", 0) != 0)
            break;
        out = strip(name) + (out.empty() ? "" : "|" + out);
    }
    return out;
}

/** A relName under ns as a scene path: the namespace on every name. */
std::string nsPath(const std::string& ns, const std::string& rel)
{
    std::string out;
    for (size_t start = 0;;)
    {
        size_t bar = rel.find('|', start);
        out += ns + ":" + rel.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (bar == std::string::npos)
            return out;
        out += '|';
        start = bar + 1;
    }
}

/** A scene path with ns: taken off every name. */
std::string stripNsPath(const std::string& ns, const std::string& path)
{
    std::string out;
    for (size_t start = 0;;)
    {
        size_t bar = path.find('|', start);
        std::string seg = path.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        out += seg.rfind(ns + ":", 0) == 0 ? seg.substr(ns.size() + 1) : seg;
        if (bar == std::string::npos)
            return out;
        out += '|';
        start = bar + 1;
    }
}

/** Whether a node record of a scene document is the one a relName names. */
bool docNodeIs(const json& n, const std::string& rel)
{
    const std::string name = n.value("name", "");
    if (rel.find('|') == std::string::npos)
        return name == rel;
    std::string parent = n.value("parent", "");
    if (parent.starts_with('|'))
        parent.erase(0, 1);
    return (parent.empty() ? name : parent + "|" + name) == rel;
}

class Converter
{
public:
    Converter(Graph& g, const File& f, const fs::path& path, const ImportOptions& opts, ImportResult& r, int depth, Nested* nested)
        : mG(g), mFile(f), mIx(f), mPath(path), mOpts(opts), mR(r), mDepth(depth), mNested(nested)
    {
    }

    void run()
    {
        LILA_ZONE();

        setupUnits();
        for (const std::string& p : mIx.unresolvedParents)
            mR.warnings.push_back(std::format("{}: parent not found, imported at the top", p));

        // Shares of the bar; referenced files are converted or loaded in importReferences, usually the bulk.
        const bool refs = mOpts.references != "none" && !mFile.references.empty();
        progress::Scope scope("", 10.0);
        collectInstances();
        {
            std::vector<int> roots;
            for (size_t i = 0; i < mFile.nodes.size(); ++i)
                if (mIx.parent[i] < 0 && isDagTransform(int(i)))
                    roots.push_back(int(i));
            progress::Scope nodes("Creating nodes", double(roots.size()), progress::None, refs ? 0.5 : 1.0);
            // DAG roots, in file order.
            for (int i : roots)
            {
                progress::checkCancel();
                createDag(i, mOpts.parent);
                nodes.step();
            }
        }
        {
            progress::Scope meshes("Building meshes", 0.0, progress::None, refs ? 2.0 : 7.0);
            finishMeshes();
        }
        progress::checkCancel();
        connectInstances();

        mG.evaluate();
        if (mOpts.skins)
        {
            std::vector<int> skins;
            for (size_t i = 0; i < mFile.nodes.size(); ++i)
                if (mFile.nodes[i].type == "skinCluster")
                    skins.push_back(int(i));
            progress::Scope weights("Skin weights", double(skins.size()), progress::None, refs ? 0.5 : 1.0);
            for (int i : skins)
            {
                progress::checkCancel();
                buildSkin(i);
                weights.step();
            }
        }
        mRig = std::make_unique<RigBuilder>(RigContext{
            .g = mG,
            .ix = mIx,
            .opts = mOpts,
            .r = mR,
            .lin = mU.lin,
            .ang = mU.ang,
            .fps = mU.fps,
            .c3 = mC3,
            .axes = mAxes,
            .identityAxes = mIdentityAxes,
            .transforms = mLila,
            .meshes = mShapeNode,
            .pointOf = mPointOf,
            .handled = mHandled,
            .blendPlugs = [this](int node, const std::string& attr) { return blendPlugs(node, attr); },
            .readCurve = [this](const MNode& n, double unit, double sign, double tangentUnit, bool* approx) {
                return readCurve(n, unit, sign, tangentUnit, approx);
            },
            .resolveImage = [this](const std::string& stored, const std::string& what) { return resolveImage(stored, what); },
        });
        if (mOpts.morphs)
        {
            buildBlendShapes();
            connectMorphInputs();
        }
        connectInverseScales();
        progress::checkCancel();
        if (refs)
            importReferences(6.5);
        progress::checkCancel();
        if (mOpts.animation)
        {
            progress::Scope curves("Animation curves", 0.0, progress::None, 0.5);
            buildAnimation();
        }
        {
            progress::Scope rig("Rig", 0.0, progress::None, 0.5);
            mRig->build();
        }
        connectPlaceholders();
        if (mOpts.setPlayback && mDepth == 0)
            setPlayback();
        // After the curves and playback, so planes on animated cameras are placed as the scene opens.
        buildImagePlanes();
        buildDisplayLayers();
        if (mOpts.lightingChannels && mDepth == 0)
            buildLightLinks();
        // Last, so the reference keeps the edits everything above made to its nodes.
        unloadDeferredRefs();
        for (const ParentEdit& p : mFile.parents)
            if (p.add && !mInstanceEdits.contains(&p))
                mR.skipped.push_back(std::format("instance of '{}' under '{}' (only mesh shapes are converted)", p.child, p.parent));
        report();
        for (const auto& [i, id] : mLila)
            mR.mayaNames[mIx.fullPath(i)] = relName(mG, id, "");
        for (const auto& [i, id] : mShapeNode)
            mR.mayaNames[mIx.fullPath(i)] = relName(mG, id, "");
        for (const auto& [i, id] : mLocatorShape)
            mR.mayaNames[mIx.fullPath(i)] = relName(mG, id, "");
        for (const auto& [i, id] : mCameraNode)
            mR.mayaNames[mIx.fullPath(i)] = relName(mG, id, "");
        for (const auto& [i, id] : mPlaneNode)
            mR.mayaNames[mIx.fullPath(i)] = relName(mG, id, "");
    }

private:
    // ---- units and axes -------------------------------------------------------------------------------------

    void setupUnits()
    {
        mU.fps = fpsOf(mFile.timeUnit);
        if (mU.fps <= 0.0)
        {
            mR.warnings.push_back(std::format("unknown time unit '{}', using 24 fps", mFile.timeUnit));
            mU.fps = 24.0;
        }
        if (mFile.internalUnits)
        {
            mU.lin = 1.0;
            mU.ang = 180.0 / math::kPi;
        }
        else
        {
            mU.lin = cmPerUnit(mFile.linearUnit);
            if (mU.lin <= 0.0)
            {
                mR.warnings.push_back(std::format("unknown linear unit '{}', read as cm", mFile.linearUnit));
                mU.lin = 1.0;
            }
            mU.ang = mFile.angleUnit.starts_with("rad") ? 180.0 / math::kPi : 1.0;
        }
        mR.unitCm = cmPerUnit(mFile.linearUnit) > 0.0 ? cmPerUnit(mFile.linearUnit) : 1.0;
        mR.fps = mU.fps;

        bool zUp = false;
        if (mOpts.upAxis == "z")
            zUp = true;
        else if (mOpts.upAxis != "y")
        {
            // Maya keeps the up axis in its preferences, not the scene; the default top camera looks down it.
            int top = mIx.resolve("top");
            if (top >= 0 && mIx.node(top).shared)
            {
                glm::dvec3 t = vec3(mIx.node(top), {"t", "translate"}, {Names{"tx"}, Names{"ty"}, Names{"tz"}}, glm::dvec3(0.0));
                zUp = std::abs(t.z) > std::abs(t.y);
            }
        }
        mR.upAxis = zUp ? "z" : "y";
        mC3 = glm::transpose(fbx::profileBasis(zUp ? fbx::AxisProfile::Ue5 : fbx::AxisProfile::Ue6, math::upAxis()));
        mAxes = fbx::axisMap(mC3);
        mIdentityAxes = mC3 == glm::dmat3(1.0);
    }

    glm::dvec3 point(const glm::dvec3& mayaCm) const { return mC3 * mayaCm; }

    /** A Maya-space column matrix in file units to Lila space. */
    glm::dmat4 matrix(glm::dmat4 m) const
    {
        m[3] = glm::dvec4(glm::dvec3(m[3]) * mU.lin, m[3].w);
        glm::dmat4 c(mC3);
        return c * m * glm::transpose(c);
    }

    // ---- DAG ------------------------------------------------------------------------------------------------

    bool isDagTransform(int i) const { return transformTypes().contains(mIx.node(i).type); }

    bool isIntermediate(int i) const { return num(mIx.node(i), {"io", "intermediateObject"}, 0.0) != 0.0; }

    bool isDefaultCamera(int i) const
    {
        const MNode& n = mIx.node(i);
        return n.shared && oneOf(shortName(n.name), {"persp", "top", "front", "side", "perspShape", "topShape", "frontShape", "sideShape"});
    }

    /**
     * Maya instances a mesh by parenting its shape under a second transform (`parent -add`). Each such transform becomes
     * a Lila mesh instancing the shape's mesh, connected once the meshes are built.
     */
    void collectInstances()
    {
        for (const ParentEdit& p : mFile.parents)
        {
            if (!p.add)
                continue;
            const int shape = mIx.resolve(p.child);
            const int xform = mIx.resolve(p.parent);
            if (shape < 0 || xform < 0 || mIx.node(shape).type != "mesh" || !isDagTransform(xform) || mIx.node(xform).type == "joint")
                continue;
            if (mInstanceOf.emplace(xform, shape).second)
                mInstanceEdits.insert(&p);
        }
    }

    void connectInstances()
    {
        for (const auto& [id, shape] : mPendingInstances)
        {
            auto it = mShapeNode.find(shape);
            if (it == mShapeNode.end())
            {
                mR.warnings.push_back(std::format("{}: instanced shape '{}' was not imported", mG.node(id)->path(), shortName(mIx.node(shape).name)));
                continue;
            }
            scene::instancing::connectInstance(mG, it->second, id);
        }
    }

    void createDag(int i, NodeId parent)
    {
        const MNode& mn = mIx.node(i);
        mHandled.insert(i);
        if (isDefaultCamera(i))
        {
            for (int c : mIx.children[size_t(i)])
                mHandled.insert(c);
            return;
        }

        std::vector<int> meshes, locators, cameras, planes, lights, others, kids;
        for (int c : mIx.children[size_t(i)])
        {
            const std::string& t = mIx.node(c).type;
            if (isDagTransform(c))
                kids.push_back(c);
            else if (t == "mesh")
            {
                mHandled.insert(c);
                if (!isIntermediate(c))
                    meshes.push_back(c);
            }
            else if (t == "locator")
                locators.push_back(c);
            else if (t == "camera")
                cameras.push_back(c);
            else if (t == "imagePlane")
                planes.push_back(c);
            else if (lightKind(t) && !isIntermediate(c))
                lights.push_back(c);
            else
                others.push_back(c);
        }

        const bool joint = mn.type == "joint";
        // A transform holding only an instanced mesh shape becomes the instance itself.
        const bool instanceXform = !joint && meshes.empty() && mInstanceOf.contains(i);
        // A camera or image plane shape makes its transform that Lila type, unless the axes differ: then the shape is a
        // child holding the turn between the axes, since both look along fixed local axes.
        const bool cameraXform = !joint && !instanceXform && meshes.empty() && !cameras.empty();
        const bool planeXform = !joint && !instanceXform && meshes.empty() && cameras.empty() && locators.empty() && !planes.empty();
        const bool lightXform =
            !joint && !instanceXform && meshes.empty() && cameras.empty() && locators.empty() && planes.empty() && !lights.empty();
        const bool latticeXform = !joint && meshes.empty() &&
                                  std::any_of(others.begin(), others.end(), [&](int o) { return mIx.node(o).type == "lattice" && !isIntermediate(o); });
        const char* type = joint                               ? "joint"
                           : mn.type == "ikHandle"             ? "ikHandle"
                           : mn.type == "lodGroup"             ? "lodGroup"
                           : latticeXform                      ? "lattice"
                           : !meshes.empty() || instanceXform ? "mesh"
                           : cameraXform     ? (mIdentityAxes ? "camera" : "transform")
                           : planeXform      ? (mIdentityAxes ? "imagePlane" : "transform")
                           : lightXform      ? (mIdentityAxes ? "light" : "transform")
                           : !locators.empty() ? "locator"
                                               : "transform";
        scene::Node* n = mG.createNode(*scene::types::find(type), lilaName(mn.name, mOpts.nameSpace, "transform"), parent, false);
        const NodeId id = n->id();
        mLila[i] = id;
        mR.nodes.push_back(id);
        if (parent == mOpts.parent)
            mR.roots.push_back(id);
        if (joint)
            mR.joints.push_back(id);

        setTransform(i, id);
        if (std::string_view(type) == "lodGroup")
            lodSizes(mn, id);
        if (instanceXform)
            mPendingInstances.emplace_back(id, mInstanceOf.at(i));
        bool visible = num(mn, {"v", "visibility"}, 1.0) != 0.0;
        if (num(mn, {"it", "inheritsTransform"}, 1.0) == 0.0)
            mR.warnings.push_back(std::format("{}: inheritsTransform off is not supported; it inherits", n->path()));

        if (!meshes.empty())
        {
            if (joint)
                for (int m : meshes)
                    childMesh(m, id);
            else
            {
                visible = visible && num(mIx.node(meshes[0]), {"v", "visibility"}, 1.0) != 0.0;
                buildMesh(meshes[0], id);
                for (size_t k = 1; k < meshes.size(); ++k)
                    childMesh(meshes[k], id);
            }
        }
        for (int l : locators)
        {
            mHandled.insert(l);
            mLocatorShape[l] = id;
            if (std::string_view(type) == "locator")
            {
                glm::dvec3 ls = vec3(mIx.node(l), {"los", "localScale"}, {Names{"lsx"}, Names{"lsy"}, Names{"lsz"}}, glm::dvec3(1.0));
                mG.setValue({id, scene::Locator::kLocalScale, -1}, mAxes.scale(ls));
                mG.setValue({id, scene::Locator::kDrawSize, -1}, 1.0);
                glm::dvec3 lp = vec3(mIx.node(l), {"lp", "localPosition"}, {Names{"lpx"}, Names{"lpy"}, Names{"lpz"}}, glm::dvec3(0.0));
                if (glm::dot(lp, lp) > 0.0)
                    mR.warnings.push_back(std::format("{}: locator localPosition dropped", n->path()));
            }
        }
        for (size_t k = 0; k < cameras.size(); ++k)
        {
            mHandled.insert(cameras[k]);
            buildCamera(cameras[k], i, k == 0 && cameraXform && mIdentityAxes ? id : shapeChild("camera", cameras[k], id));
        }
        for (size_t k = 0; k < planes.size(); ++k)
        {
            mHandled.insert(planes[k]);
            NodeId pid = k == 0 && planeXform && mIdentityAxes ? id : shapeChild("imagePlane", planes[k], id);
            buildPlane(planes[k], i, pid);
            buildFreePlane(planes[k], pid);
        }
        for (size_t k = 0; k < lights.size(); ++k)
        {
            mHandled.insert(lights[k]);
            buildLight(lights[k], k == 0 && lightXform && mIdentityAxes ? id : shapeChild("light", lights[k], id));
        }
        for (int o : others)
        {
            const std::string& t = mIx.node(o).type;
            // Constraints and pins are converted, or reported, by the rig pass; the deformer handles are only helpers.
            if (RigBuilder::ownsShape(t))
            {
                if (!t.ends_with("Constraint") && t != "follicle")
                    mHandled.insert(o);
                continue;
            }
            mHandled.insert(o);
            if (isIntermediate(o))
                continue;
            mR.skipped.push_back(std::format("{} '{}'{}", isLight(t) ? "light" : t.c_str(), shortName(mIx.node(o).name),
                                             isLight(t) ? std::format(" ({}, kept as its transform)", t) : std::string(" (kept as its transform)")));
            if (!isLight(t) && (t.ends_with("Constraint") || t == "ikEffector" || t == "ikHandle"))
                mR.skipped.back() = std::format("{} '{}'", t, shortName(mIx.node(o).name));
        }
        if (!visible)
            mG.setValue({id, scene::Transform::kVisibility, -1}, false);

        for (int c : kids)
            createDag(c, id);
    }

    /** An extra mesh shape under a transform (or any under a joint) becomes a child mesh node. */
    void childMesh(int shape, NodeId parent)
    {
        scene::Node* n = mG.createNode(*scene::types::find("mesh"), lilaName(mIx.node(shape).name, mOpts.nameSpace, "mesh"), parent, false);
        mR.nodes.push_back(n->id());
        mShapeNode[shape] = n->id();
        buildMesh(shape, n->id());
        if (num(mIx.node(shape), {"v", "visibility"}, 1.0) == 0.0)
            mG.setValue({n->id(), scene::Transform::kVisibility, -1}, false);
    }

    // ---- cameras and image planes ---------------------------------------------------------------------------

    /**
     * A node for a camera or image plane shape under a converted transform. Its local turn is the axis change, so in
     * its own frame Maya's local axes hold as they are: it looks down -Z, and children placed in it need no axis map.
     */
    NodeId shapeChild(const char* type, int shape, NodeId parent)
    {
        scene::Node* n = mG.createNode(*scene::types::find(type), lilaName(mIx.node(shape).name, mOpts.nameSpace, type), parent, false);
        mR.nodes.push_back(n->id());
        if (!mIdentityAxes)
            scene::Transform::setLocalMatrix(mG, n->id(), glm::dmat4(mC3));
        return n->id();
    }

    /** True when every translate and rotate channel of a transform is locked (Maya's Lock Camera). */
    bool transformLocked(int xf) const
    {
        const MNode& n = mIx.node(xf);
        auto locked = [&](std::string_view whole, std::string_view shortWhole, char axis) {
            const std::string comp = std::string(shortWhole) + axis;
            const std::string longComp = std::string(whole) + char(std::toupper(static_cast<unsigned char>(axis)));
            int v = -1;
            for (const AttrValue& a : n.attrs)
                if (a.lock >= 0 && oneOf(a.attr, {whole, shortWhole, comp, longComp}))
                    v = a.lock;
            return v == 1;
        };
        for (char axis : {'x', 'y', 'z'})
            if (!locked("translate", "t", axis) || !locked("rotate", "r", axis))
                return false;
        return true;
    }

    void buildCamera(int shape, int xf, NodeId camId)
    {
        using scene::Camera;
        const MNode& n = mIx.node(shape);
        auto set = [&](int attr, scene::PlugValue v) { mG.setValue({camId, attr, -1}, std::move(v)); };
        glm::dvec2 aperture(1.41732, 0.94488);
        for (const AttrValue& a : n.attrs)
        {
            if (oneOf(a.attr, {"cap", "cameraAperture"}) && a.nums.size() >= 2)
                aperture = {a.nums[0], a.nums[1]};
            else if (oneOf(a.attr, {"hfa", "horizontalFilmAperture"}) && !a.nums.empty())
                aperture.x = a.nums[0];
            else if (oneOf(a.attr, {"vfa", "verticalFilmAperture"}) && !a.nums.empty())
                aperture.y = a.nums[0];
        }
        set(Camera::kFocalLength, num(n, {"fl", "focalLength"}, 35.0));
        set(Camera::kHorizontalFilmAperture, aperture.x);
        set(Camera::kVerticalFilmAperture, aperture.y);
        set(Camera::kFilmFit, int64_t(std::clamp(num(n, {"ff", "filmFit"}, 1.0), 0.0, 3.0)));
        set(Camera::kNearClipPlane, num(n, {"ncp", "nearClipPlane"}, 0.1) * mU.lin);
        set(Camera::kFarClipPlane, num(n, {"fcp", "farClipPlane"}, 10000.0) * mU.lin);
        set(Camera::kOrthographic, num(n, {"o", "orthographic"}, 0.0) != 0.0);
        set(Camera::kOrthographicWidth, num(n, {"ow", "orthographicWidth"}, 10.0) * mU.lin);
        set(Camera::kLocked, transformLocked(xf));
        if (num(n, {"v", "visibility"}, 1.0) == 0.0)
            set(scene::Transform::kVisibility, false);

        const std::string name = mG.node(camId)->path();
        std::vector<std::string> dropped;
        if (std::abs(num(n, {"lsr", "lensSqueezeRatio"}, 1.0) - 1.0) > 1e-9)
            dropped.push_back("lens squeeze");
        if (num(n, {"hfo", "horizontalFilmOffset"}, 0.0) != 0.0 || num(n, {"vfo", "verticalFilmOffset"}, 0.0) != 0.0)
            dropped.push_back("film offset");
        if (num(n, {"shke", "shakeEnabled"}, 0.0) != 0.0)
            dropped.push_back("shake");
        if (num(n, {"dof", "depthOfField"}, 0.0) != 0.0)
            dropped.push_back("depth of field");
        for (const std::string& d : dropped)
            mR.warnings.push_back(std::format("{}: camera {} dropped", name, d));
        mCameraNode[shape] = camId;
        mR.cameras.push_back(camId);
    }

    // ---- lights ---------------------------------------------------------------------------------------------

    /**
     * Maya and Arnold lights: kind, colour, intensity (with Arnold's exposure folded in), temperature, cone, area size
     * and shadows. Maya's decay, dropoff and emit flags have no Lila equivalent and are kept in the light's extra JSON.
     */
    void buildLight(int shape, NodeId lightId)
    {
        namespace lights = scene::lights;
        mLightNode[shape] = lightId;
        const MNode& n = mIx.node(shape);
        lights::Settings s;
        s.kind = *lightKind(n.type);
        s.sourceFormat = "maya:" + n.type;
        s.color = color(n, {"cl", "color"}, glm::dvec3(1.0));
        const double exposure = num(n, {"ai_exposure", "aiExposure", "exposure"}, 0.0);
        s.intensity = num(n, {"in", "intensity"}, 1.0) * std::exp2(exposure);
        s.useTemperature = num(n, {"ai_use_color_temperature", "aiUseColorTemperature"}, 0.0) != 0.0;
        s.temperature = num(n, {"ai_color_temperature", "aiColorTemperature"}, 6500.0);
        s.sourceRadius = num(n, {"ai_radius", "aiRadius", "lr", "lightRadius"}, 0.0) * mU.lin;
        if (const AttrValue* a = lastAttr(n, {"ai_cast_shadows", "aiCastShadows"}); a && !a->nums.empty())
            s.castShadows = a->nums[0] != 0.0;
        else if (lastAttr(n, {"dms", "useDepthMapShadows"}) || lastAttr(n, {"urs", "useRayTraceShadows"}))
            s.castShadows = num(n, {"dms", "useDepthMapShadows"}, 0.0) != 0.0 || num(n, {"urs", "useRayTraceShadows"}, 0.0) != 0.0;
        json extra = json::object();
        if (s.kind == lights::Kind::Spot)
        {
            const double half = num(n, {"ca", "coneAngle"}, 40.0) * mU.ang / 2.0;
            const double penumbra = num(n, {"pa", "penumbraAngle"}, 0.0) * mU.ang;
            s.outerConeAngle = std::clamp(half + std::max(penumbra, 0.0), 0.0, 90.0);
            s.innerConeAngle = std::clamp(half + std::min(penumbra, 0.0), 0.0, s.outerConeAngle);
            extra["dropoff"] = num(n, {"dro", "dropoff"}, 0.0);
        }
        if (s.kind == lights::Kind::Area)
            s.width = s.height = 2.0 * mU.lin; // Maya's area lights are 2 units square before their transform's scale
        if (n.type == "aiSkyDomeLight")
            if (std::optional<Plug> tex = mIx.input(shape, {"cl", "color", "sc"}); tex && mIx.node(tex->node).type == "file")
            {
                mHandled.insert(tex->node);
                s.texture = resolveImage(text(mIx.node(tex->node), {"ftn", "fileTextureName"}), mG.node(lightId)->path());
            }
        if (n.type == "aiPhotometricLight")
            s.texture = resolveImage(text(n, {"ai_filename", "aiFilename"}), mG.node(lightId)->path());
        if (const AttrValue* a = lastAttr(n, {"de", "decayRate"}); a && !a->nums.empty())
            extra["decayRate"] = a->nums[0];
        for (auto [shortName, longName] : {std::pair("ed", "emitDiffuse"), std::pair("sn", "emitSpecular")})
            if (const AttrValue* a = lastAttr(n, {shortName, longName}); a && !a->nums.empty())
                extra[longName] = a->nums[0] != 0.0;
        if (exposure != 0.0)
            extra["exposure"] = exposure;
        if (!extra.empty())
            s.extra = extra.dump();
        lights::setSettings(mG, lightId, s);
        if (num(n, {"v", "visibility"}, 1.0) == 0.0)
            mG.setValue({lightId, scene::Transform::kVisibility, -1}, false);
        mR.lights.push_back(lightId);
    }

    /** An image file as Lila stores it, looked for where Maya projects keep them when the stored path is gone. */
    std::string resolveImage(const std::string& stored, const std::string& name)
    {
        if (stored.empty())
            return stored;
        const std::string path = resolveTexture(stored);
        auto exists = [](const fs::path& p) {
            std::error_code ec;
            return fs::is_regular_file(p, ec);
        };
        const fs::path p(std::u8string(path.begin(), path.end()));
        if (p.is_absolute() && exists(p))
            return paths::storedPath(p);
        const fs::path dir = mPath.parent_path();
        for (const fs::path& base : {dir / "sourceimages", dir.parent_path() / "sourceimages", dir})
            if (exists(base / p.filename()))
            {
                std::error_code ec;
                const std::string found = utf8Of(fs::weakly_canonical(base / p.filename(), ec));
                mR.warnings.push_back(std::format("{}: image '{}' not found, using {}", name, stored, found));
                return paths::storedPath(paths::fromUtf8(found));
            }
        mR.warnings.push_back(std::format("{}: image '{}' not found", name, stored));
        return path;
    }

    /** What every image plane carries, attached or free: the image, sequence, fit, opacity, rotation and display. */
    void buildPlane(int shape, int xf, NodeId pid)
    {
        using scene::ImagePlane;
        namespace ip = scene::imagePlane;
        const MNode& n = mIx.node(shape);
        auto set = [&](int attr, scene::PlugValue v) { mG.setValue({pid, attr, -1}, std::move(v)); };
        const std::string name = mG.node(pid)->path();

        std::string file = text(n, {"imn", "imageName"});
        const int kind = int(num(n, {"t", "type"}, 0.0));
        if (kind == 1)
        {
            std::optional<Plug> tex = mIx.input(shape, {"stx", "sourceTexture"});
            if (tex && mIx.node(tex->node).type == "file")
            {
                mHandled.insert(tex->node);
                file = text(mIx.node(tex->node), {"ftn", "fileTextureName"});
            }
            else
                mR.warnings.push_back(std::format("{}: texture image plane without a file texture; its image name is used", name));
        }
        else if (kind == 2)
            mR.warnings.push_back(std::format("{}: movie image planes are not supported; the file is kept but won't show", name));
        set(ImagePlane::kImageName, resolveImage(file, name));
        set(ImagePlane::kUseFrameExtension, num(n, {"ufe", "useFrameExtension"}, 0.0) != 0.0);
        // Lila sequences follow the time slider, which is what Maya's time-driven frame extension does.
        if (std::optional<Plug> fe = mIx.input(shape, {"fe", "frameExtension"}); fe && oneOf(mIx.node(fe->node).type, {"timeToUnitConversion", "expression"}))
            mHandled.insert(fe->node);
        set(ImagePlane::kFrameOffset, int64_t(num(n, {"fo", "frameOffset"}, 0.0)));
        set(ImagePlane::kFit, int64_t(std::clamp(num(n, {"f", "fit"}, 1.0), 0.0, 4.0)));
        set(ImagePlane::kAlphaGain, std::clamp(num(n, {"ag", "alphaGain"}, 1.0), 0.0, 1.0));
        set(ImagePlane::kRotation, num(n, {"r", "rotate"}, 0.0) * mU.ang);
        // Maya depth tests image planes against the scene at their depth.
        set(ImagePlane::kPlacement, int64_t(ip::Placement::InScene));

        // Maya's display modes: None, Outline, RGB, RGBA, Luminance, Alpha.
        const int display = int(num(n, {"dm", "displayMode"}, 3.0));
        if (display == 4)
            set(ImagePlane::kDisplayColor, int64_t(1));
        else if (display == 1 || display == 5)
            mR.warnings.push_back(std::format("{}: {} display shown as RGBA", name, display == 1 ? "outline" : "alpha"));
        if (display == 0 || num(n, {"v", "visibility"}, 1.0) == 0.0)
            set(ImagePlane::kVisibility, false);
        if (color(n, {"cg", "colorGain"}, glm::dvec3(1.0)) != glm::dvec3(1.0) || color(n, {"cof", "colorOffset"}, glm::dvec3(0.0)) != glm::dvec3(0.0))
            mR.warnings.push_back(std::format("{}: color gain and offset dropped", name));
        if (std::abs(num(n, {"sqc", "squeezeCorrection"}, 1.0) - 1.0) > 1e-9)
            mR.warnings.push_back(std::format("{}: squeeze correction dropped", name));
        if (xf >= 0)
            mHandled.insert(xf);
        mPlaneNode[shape] = pid;
        mR.imagePlanes.push_back(pid);
    }

    /** A free plane: width and height in scene units, centred at imageCenter in its own plane. */
    void buildFreePlane(int shape, NodeId pid)
    {
        using scene::ImagePlane;
        const MNode& n = mIx.node(shape);
        mG.setValue({pid, ImagePlane::kWidth, -1}, num(n, {"w", "width"}, 10.0) * mU.lin);
        mG.setValue({pid, ImagePlane::kHeight, -1}, num(n, {"h", "height"}, 10.0) * mU.lin);
        mG.setValue({pid, ImagePlane::kMaintainRatio, -1}, num(n, {"mr", "maintainRatio"}, 1.0) != 0.0);
        const glm::dvec3 c = vec3(n, {"ic", "imageCenter"}, {Names{"icx"}, Names{"icy"}, Names{"icz"}}, glm::dvec3(0.0)) * mU.lin;
        mG.setValue({pid, ImagePlane::kOffsetX, -1}, c.x);
        mG.setValue({pid, ImagePlane::kOffsetY, -1}, c.y);
        if (std::abs(c.z) > 1e-9)
            mR.warnings.push_back(std::format("{}: image center depth dropped", mG.node(pid)->path()));
    }

    /** The camera an image plane belongs to: the shape it sits under, or the one its message feeds (older files). */
    int planeCamera(int shape, int xf) const
    {
        if (xf >= 0)
            if (int p = mIx.parent[size_t(xf)]; p >= 0 && mIx.node(p).type == "camera")
                return p;
        for (const auto& [src, dst] : mIx.outputs(shape))
            if (oneOf(src.attr, {"msg", "message"}) && mIx.node(dst.node).type == "camera")
                return dst.node;
        return -1;
    }

    /**
     * Image planes on cameras, and free ones without a transform. On a default camera a plane becomes a Lila plane
     * attached to that view. Lila attaches planes only to its views, so on any other camera it is placed against the
     * camera as it is at import: a perspective one gets it as a child at its depth, sized from the lens; an
     * orthographic one gets it fixed in the world at its center, facing along the view.
     */
    void buildImagePlanes()
    {
        using scene::ImagePlane;
        namespace ip = scene::imagePlane;
        for (size_t i = 0; i < mFile.nodes.size(); ++i)
        {
            const MNode& n = mFile.nodes[i];
            if (n.type != "imagePlane" || mHandled.contains(int(i)) || isIntermediate(int(i)))
                continue;
            mHandled.insert(int(i));
            const int shape = int(i);
            int xf = mIx.parent[i];
            if (xf >= 0 && mIx.node(xf).type != "transform")
                xf = -1;
            const int cam = planeCamera(shape, xf);
            const bool startup = cam >= 0 && mIx.parent[size_t(cam)] >= 0 && isDefaultCamera(mIx.parent[size_t(cam)]);
            const std::string mayaName = shortName(xf >= 0 ? mIx.node(xf).name : n.name);
            if (cam >= 0 && !startup && !mCameraNode.contains(cam))
            {
                mR.skipped.push_back(std::format("image plane '{}' (its camera '{}' was not converted)", mayaName, shortName(mIx.node(cam).name)));
                continue;
            }
            if (cam >= 0 && num(n, {"dlc", "lockedToCamera"}, 1.0) == 0.0)
                mR.warnings.push_back(std::format("{}: not locked to its camera; placed as if it were", mayaName));

            const MNode& cn = cam >= 0 ? mIx.node(cam) : n;
            const bool ortho = cam >= 0 && num(cn, {"o", "orthographic"}, 0.0) != 0.0;
            const bool perspChild = cam >= 0 && !startup && !ortho;
            const NodeId parent = perspChild ? mCameraNode[cam] : mOpts.parent;
            scene::Node* pn = mG.createNode(*scene::types::find("imagePlane"), lilaName(mayaName, mOpts.nameSpace, "imagePlane"), parent, false);
            const NodeId pid = pn->id();
            mR.nodes.push_back(pid);
            if (parent == mOpts.parent)
                mR.roots.push_back(pid);
            buildPlane(shape, xf, pid);
            if (xf >= 0 && num(mIx.node(xf), {"v", "visibility"}, 1.0) == 0.0)
                mG.setValue({pid, ImagePlane::kVisibility, -1}, false);
            if (cam < 0)
            {
                buildFreePlane(shape, pid);
                continue;
            }
            auto set = [&](int attr, scene::PlugValue v) { mG.setValue({pid, attr, -1}, std::move(v)); };

            glm::dvec2 size(1.41732, 0.94488), offset(0.0);
            if (const AttrValue* a = lastAttr(n, {"s", "size"}); a && a->nums.size() >= 2)
                size = {a->nums[0], a->nums[1]};
            size = {num(n, {"sx", "sizeX"}, size.x), num(n, {"sy", "sizeY"}, size.y)};
            if (const AttrValue* a = lastAttr(n, {"o", "offset"}); a && a->nums.size() >= 2)
                offset = {a->nums[0], a->nums[1]};
            offset = {num(n, {"ox", "offsetX"}, offset.x), num(n, {"oy", "offsetY"}, offset.y)};
            const double depth = num(n, {"d", "depth"}, 100.0) * mU.lin;
            const glm::dvec3 center = vec3(n, {"ic", "imageCenter"}, {Names{"icx"}, Names{"icy"}, Names{"icz"}}, glm::dvec3(0.0)) * mU.lin;
            const glm::dvec2 box(num(n, {"w", "width"}, 10.0) * mU.lin, num(n, {"h", "height"}, 10.0) * mU.lin);
            const bool keepRatio = num(n, {"mr", "maintainRatio"}, 1.0) != 0.0;
            const double focal = mCameraNode.contains(cam) ? std::get<double>(mG.value({mCameraNode[cam], scene::Camera::kFocalLength, -1}))
                                                           : num(cn, {"fl", "focalLength"}, 35.0);
            // Film inches to cm across the plane at its depth.
            const double perInch = 25.4 / std::max(focal, 1e-6) * depth;
            const int camXf = mIx.parent[size_t(cam)];

            if (startup)
            {
                set(ImagePlane::kCamera, ip::cameraName(shortName(mIx.node(camXf).name)));
                const bool current = num(n, {"dic", "displayOnlyIfCurrent"}, 0.0) != 0.0;
                set(ImagePlane::kDisplayMode, int64_t(current ? ip::DisplayMode::LookingThrough : ip::DisplayMode::AllViews));
                if (!ortho)
                {
                    set(ImagePlane::kDepth, depth);
                    set(ImagePlane::kOffsetX, offset.x * perInch);
                    set(ImagePlane::kOffsetY, offset.y * perInch);
                    set(ImagePlane::kWidth, size.x * perInch);
                    set(ImagePlane::kHeight, size.y * perInch);
                    set(ImagePlane::kMaintainRatio, false);
                    continue;
                }
                // Lila keeps an orthographic plane `depth` behind the origin along the view, shifted within its plane.
                const MNode& xn = mIx.node(camXf);
                const glm::dvec3 r = vec3(xn, {"r", "rotate"}, {Names{"rx", "rotateX"}, Names{"ry", "rotateY"}, Names{"rz", "rotateZ"}}, glm::dvec3(0.0));
                const auto order = scene::RotateOrder(std::clamp(int64_t(num(xn, {"ro", "rotateOrder"}, 0.0)), int64_t(0), int64_t(5)));
                const glm::dmat3 rot(scene::eulerToMatrix(r * mU.ang, order));
                set(ImagePlane::kOffsetX, glm::dot(center, rot[0]));
                set(ImagePlane::kOffsetY, glm::dot(center, rot[1]));
                set(ImagePlane::kDepth, glm::dot(center, -rot[2]));
                set(ImagePlane::kWidth, box.x);
                set(ImagePlane::kHeight, box.y);
                set(ImagePlane::kMaintainRatio, keepRatio);
                if (offset != glm::dvec2(0.0))
                    mR.warnings.push_back(std::format("{}: offset on an orthographic camera dropped", pn->path()));
                continue;
            }

            const std::string camName = mG.node(mCameraNode[cam])->path();
            if (perspChild)
            {
                // The camera node's frame is Maya's camera space, so this is Maya's placement as it stands.
                set(scene::Transform::kTranslate, glm::dvec3(0.0, 0.0, -depth));
                set(ImagePlane::kOffsetX, offset.x * perInch);
                set(ImagePlane::kOffsetY, offset.y * perInch);
                set(ImagePlane::kWidth, size.x * perInch);
                set(ImagePlane::kHeight, size.y * perInch);
                set(ImagePlane::kMaintainRatio, false);
                mR.warnings.push_back(std::format("{}: placed under camera '{}' at its depth (Lila attaches image planes only to its views); "
                                                  "it follows the camera, sized for the lens at import",
                                                  pn->path(), camName));
                if (mIx.input(cam, {"fl", "focalLength"}))
                    mR.warnings.push_back(std::format("{}: camera '{}' has a driven focal length; the plane keeps its import-time size", pn->path(), camName));
                continue;
            }
            const glm::dmat4 camWorld = std::get<glm::dmat4>(mG.value({mCameraNode[cam], scene::Transform::kWorldMatrix, -1}));
            glm::dmat4 world(1.0);
            for (int k = 0; k < 3; ++k)
                world[k] = glm::dvec4(glm::normalize(glm::dvec3(camWorld[k])), 0.0);
            const glm::dmat4 parentWorld =
                parent == scene::kNoNode ? glm::dmat4(1.0) : std::get<glm::dmat4>(mG.value({parent, scene::Transform::kWorldMatrix, -1}));
            world[3] = parentWorld * glm::dvec4(point(center), 1.0);
            scene::Transform::setLocalMatrix(mG, pid, glm::inverse(parentWorld) * world);
            set(ImagePlane::kWidth, box.x);
            set(ImagePlane::kHeight, box.y);
            set(ImagePlane::kMaintainRatio, keepRatio);
            mR.warnings.push_back(std::format("{}: placed in the world facing along orthographic camera '{}' as at import "
                                              "(Lila attaches image planes only to its views)",
                                              pn->path(), camName));
        }
    }

    void setTransform(int i, NodeId id)
    {
        using scene::Transform;
        const MNode& n = mIx.node(i);
        const glm::dvec3 zero(0.0);
        glm::dvec3 t = vec3(n, {"t", "translate"}, {Names{"tx", "translateX"}, Names{"ty", "translateY"}, Names{"tz", "translateZ"}}, zero);
        glm::dvec3 r = vec3(n, {"r", "rotate"}, {Names{"rx", "rotateX"}, Names{"ry", "rotateY"}, Names{"rz", "rotateZ"}}, zero);
        glm::dvec3 s = vec3(n, {"s", "scale"}, {Names{"sx", "scaleX"}, Names{"sy", "scaleY"}, Names{"sz", "scaleZ"}}, glm::dvec3(1.0));
        glm::dvec3 rp = vec3(n, {"rp", "rotatePivot"}, {Names{"rpx"}, Names{"rpy"}, Names{"rpz"}}, zero);
        glm::dvec3 sp = vec3(n, {"sp", "scalePivot"}, {Names{"spx"}, Names{"spy"}, Names{"spz"}}, zero);
        glm::dvec3 rpt = vec3(n, {"rpt", "rotatePivotTranslate"}, {Names{"rptx"}, Names{"rpty"}, Names{"rptz"}}, zero);
        glm::dvec3 spt = vec3(n, {"spt", "scalePivotTranslate"}, {Names{"sptx"}, Names{"spty"}, Names{"sptz"}}, zero);
        glm::dvec3 sh = vec3(n, {"sh", "shear"}, {Names{"shxy"}, Names{"shxz"}, Names{"shyz"}}, zero);
        glm::dvec3 ra = vec3(n, {"ra", "rotateAxis"}, {Names{"rax"}, Names{"ray"}, Names{"raz"}}, zero);
        auto order = scene::RotateOrder(std::clamp(int64_t(num(n, {"ro", "rotateOrder"}, 0.0)), int64_t(0), int64_t(5)));
        r *= mU.ang;
        ra *= mU.ang;
        const std::string name = mG.node(id)->path();

        if (glm::dot(ra, ra) > 1e-18)
        {
            // Maya rotates by rotateAxis before rotate; folding it in keeps the pose, not separate channels.
            glm::dmat3 m = glm::dmat3(scene::eulerToMatrix(r, order)) * glm::dmat3(scene::eulerToMatrix(ra, scene::RotateOrder::XYZ));
            r = fbx::eulerNear(m, order, r);
            mR.warnings.push_back(std::format("{}: rotateAxis folded into rotate", name));
            mRotateAxisFolded.insert(i);
        }
        if (glm::dot(sh, sh) > 1e-18)
            mR.warnings.push_back(std::format("{}: shear dropped", name));

        const scene::RotateOrder lilaOrder = mAxes.order(order);
        mG.setValue({id, Transform::kTranslate, -1}, mAxes.translate(t * mU.lin));
        mG.setValue({id, Transform::kRotate, -1}, cleanAngles(mAxes.rotate(r)));
        mG.setValue({id, Transform::kScale, -1}, mAxes.scale(s));
        if (lilaOrder != scene::RotateOrder::XYZ)
            mG.setValue({id, Transform::kRotateOrder, -1}, int64_t(lilaOrder));
        if (rp != zero)
            mG.setValue({id, Transform::kRotatePivot, -1}, mAxes.translate(rp * mU.lin));
        if (sp != zero)
            mG.setValue({id, Transform::kScalePivot, -1}, mAxes.translate(sp * mU.lin));
        if (rpt != zero)
            mG.setValue({id, Transform::kRotatePivotTranslate, -1}, mAxes.translate(rpt * mU.lin));
        if (spt != zero)
            mG.setValue({id, Transform::kScalePivotTranslate, -1}, mAxes.translate(spt * mU.lin));

        if (n.type == "joint")
        {
            using scene::Joint;
            glm::dvec3 jo = vec3(n, {"jo", "jointOrient"}, {Names{"jox"}, Names{"joy"}, Names{"joz"}}, zero) * mU.ang;
            glm::dmat3 m = mC3 * glm::dmat3(scene::eulerToMatrix(jo, scene::RotateOrder::XYZ)) * glm::transpose(mC3);
            mG.setValue({id, Joint::kJointOrient, -1}, cleanAngles(mIdentityAxes ? jo : scene::matrixToEuler(m, scene::RotateOrder::XYZ)));
            mG.setValue({id, Joint::kRadius, -1}, num(n, {"radi", "radius"}, 1.0) * mU.lin);
            mG.setValue({id, Joint::kSegmentScaleCompensate, -1}, num(n, {"ssc", "segmentScaleCompensate"}, 1.0) != 0.0);
            mG.setValue({id, Joint::kSide, -1}, int64_t(std::clamp(num(n, {"sd", "side"}, 0.0), 0.0, 3.0)));
            mG.setValue({id, Joint::kJointType, -1}, int64_t(std::clamp(num(n, {"typ", "type"}, 0.0), 0.0, 29.0)));
            if (std::string otp = text(n, {"otp", "otherType"}); !otp.empty())
                mG.setValue({id, Joint::kOtherType, -1}, otp);
            if (double ds = num(n, {"ds", "drawStyle"}, 0.0); ds >= 1.0 && ds <= 2.0)
                mG.setValue({id, Joint::kDrawStyle, -1}, int64_t(ds));
        }
    }

    static glm::dvec3 cleanAngles(glm::dvec3 v)
    {
        for (int k = 0; k < 3; ++k)
            if (std::abs(v[k]) < 1e-9)
                v[k] = 0.0;
        return v;
    }

    // ---- meshes ---------------------------------------------------------------------------------------------

    /** What evaluating history reports; kept apart from the result so meshes can evaluate in parallel. */
    struct EvalLog
    {
        std::vector<std::string> warnings;
        std::vector<int> handled;
    };

    /** The geometry a node's output carries, evaluating what history it can. */
    std::optional<Geom> evalNode(int node, const std::string& outAttr, const std::string& shape, int depth, EvalLog& log) const
    {
        if (node < 0 || depth > 200)
            return std::nullopt;
        log.handled.push_back(node);
        const MNode& n = mIx.node(node);
        const std::string& t = n.type;
        auto upstream = [&](Names names) -> std::optional<Geom> {
            if (auto src = mIx.input(node, names))
                return evalNode(src->node, src->attr, shape, depth + 1, log);
            return std::nullopt;
        };

        if (t == "mesh")
        {
            std::optional<Geom> g;
            if (mIx.input(node, {"i", "inMesh"}))
                g = upstream({"i", "inMesh"});
            else if (auto data = meshDataOf(n, log.warnings))
                g = buildGeom(*data, mU.lin, shape, log.warnings);
            if (g)
                applyTweaks(*g, n, {"pt", "pnts"}, shape, log);
            return g;
        }
        if (creatorTypes().contains(t))
            return makePrimitive(node);
        if (t == "polyTweak")
        {
            std::optional<Geom> g = upstream({"ip", "inputPolymesh"});
            if (g)
                applyTweaks(*g, n, {"tk", "tweak"}, shape, log);
            return g;
        }
        if (t == "polyTweakUV")
        {
            std::optional<Geom> g = upstream({"ip", "inputPolymesh"});
            if (g)
                applyUvTweaks(*g, n, shape, log);
            return g;
        }
        if (t == "transformGeometry")
        {
            std::optional<Geom> g = upstream({"ig", "inputGeometry"});
            if (g)
            {
                glm::dmat4 m = matrixOf(lastAttr(n, {"txf", "transform"}) ? lastAttr(n, {"txf", "transform"})->nums : std::vector<double>{});
                m[3] = glm::dvec4(glm::dvec3(m[3]) * mU.lin, 1.0);
                for (Index p = 0; p < g->mesh.pointCapacity(); ++p)
                    g->mesh.setPosition(p, glm::dvec3(m * glm::dvec4(g->mesh.position(p), 1.0)));
                if (g->mesh.cornerAttrs().has(geo::attr::kNormal))
                    geo::updateCornerNormals(g->mesh);
            }
            return g;
        }
        if (t == "polySmoothFace")
        {
            std::optional<Geom> g = upstream({"ip", "inputPolymesh"});
            if (g)
            {
                const AttrValue* ics = lastAttr(n, {"ics", "inputComponents"});
                if (ics && !coversAll(ics->strs) && !ics->strs.empty())
                    log.warnings.push_back(std::format("{}: {} smooths only some faces in Maya; the whole mesh was smoothed", shape, n.name));
                if (num(n, {"mth", "method"}, 0.0) != 0.0)
                    log.warnings.push_back(std::format("{}: {} uses linear smoothing; converted as Catmull-Clark", shape, n.name));
                int levels = std::clamp(int(num(n, {"dv", "divisions"}, 1.0)), 0, 4);
                if (levels > 0)
                {
                    Geom out;
                    out.mesh = geo::subdivide(g->mesh, {.levels = levels,
                                                                .sharpBoundary = num(n, {"kb", "keepBorder"}, 1.0) != 0.0,
                                                                .keepHardEdges = num(n, {"khe", "keepHardEdge"}, 1.0) != 0.0,
                                                                .uvs = geo::SubdivideUvs::Blend});
                    return out;
                }
            }
            return g;
        }
        if (t == "polyTriangulate")
        {
            std::optional<Geom> g = upstream({"ip", "inputPolymesh"});
            if (g)
            {
                std::vector<Index> faces;
                for (Index f = 0; f < g->mesh.faceCapacity(); ++f)
                    if (g->mesh.faceValid(f))
                        faces.push_back(f);
                geo::triangulateFaces(g->mesh, faces);
                g->faceOf.clear();
                g->cornerUv.clear();
            }
            return g;
        }
        if (t == "polySoftEdge")
        {
            std::optional<Geom> g = upstream({"ip", "inputPolymesh"});
            if (g)
            {
                const AttrValue* ics = lastAttr(n, {"ics", "inputComponents"});
                std::vector<Index> edges;
                if (ics && !coversAll(ics->strs))
                {
                    edges = edgesOf(*g, ics->strs);
                    if (edges.empty())
                        log.warnings.push_back(std::format("{}: {} edges could not be matched; applied to every edge", shape, n.name));
                }
                geo::setHardEdgesByAngle(g->mesh, num(n, {"a", "angle"}, 30.0) * mU.ang, edges);
            }
            return g;
        }
        if (t == "groupParts" || t == "groupId")
            return upstream({"ig", "inputGeometry"});
        if (deformerTypes().contains(t))
        {
            // A deformer's output k comes from its input k.
            std::string k = "0";
            if (auto b = outAttr.find('['); b != std::string::npos)
                k = outAttr.substr(b + 1, outAttr.find(']') - b - 1);
            std::string ig = "ip[" + k + "].ig", ig2 = "input[" + k + "].inputGeometry";
            if (t != "skinCluster" && t != "tweak" && !(t == "blendShape" && mOpts.morphs) && !RigBuilder::convertsDeformer(t))
                log.warnings.push_back(std::format("{}: deformer {} '{}' not applied", shape, t, shortName(n.name)));
            return upstream({ig, ig2});
        }

        // Anything else: its input mesh, so at least the geometry before that step arrives.
        log.warnings.push_back(std::format("{}: history node {} '{}' not evaluated; the mesh is its input (delete history in Maya to keep it)",
                                          shape, t, shortName(n.name)));
        return upstream({"ip", "inputPolymesh", "ig", "inputGeometry", "i", "inMesh", "im", "inputMesh", "ip[0].ig", "input[0].inputGeometry"});
    }

    void applyTweaks(Geom& g, const MNode& n, Names names, const std::string& shape, EvalLog& log) const
    {
        bool any = false;
        for (const AttrValue& a : n.attrs)
        {
            std::vector<AttrSeg> segs = splitAttr(a.attr);
            if (segs.size() != 1 || !oneOf(segs[0].name, names) || segs[0].lo < 0 || a.nums.size() < 3)
                continue;
            if (!g.mayaPoints)
            {
                log.warnings.push_back(std::format("{}: vertex tweaks on {} could not be matched to points and were dropped", shape, n.name));
                return;
            }
            for (size_t i = 0; i + 2 < a.nums.size(); i += 3)
            {
                Index p = Index(segs[0].lo) + Index(i / 3);
                if (p < g.mesh.pointCapacity() && g.mesh.pointValid(p))
                    g.mesh.setPosition(p, g.mesh.position(p) + glm::dvec3(a.nums[i], a.nums[i + 1], a.nums[i + 2]) * mU.lin);
            }
            any = true;
        }
        if (any && g.mesh.cornerAttrs().has(geo::attr::kNormal))
            geo::updateCornerNormals(g.mesh);
    }

    void applyUvTweaks(Geom& g, const MNode& n, const std::string& shape, EvalLog& log) const
    {
        std::string setName = text(n, {"uvs", "uvSetName"});
        for (const AttrValue& a : n.attrs)
        {
            std::vector<AttrSeg> segs = splitAttr(a.attr);
            if (segs.size() != 1 || !oneOf(segs[0].name, {"uvtk", "uvTweak"}) || segs[0].lo < 0)
                continue;
            if (g.cornerUv.empty())
            {
                log.warnings.push_back(std::format("{}: uv tweaks on {} could not be matched and were dropped", shape, n.name));
                return;
            }
            size_t set = 0;
            // The tweaked set is named; map1 is the usual first one.
            (void)setName;
            std::span<glm::vec2> uv = g.mesh.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(set)));
            std::unordered_map<int32_t, glm::vec2> delta;
            for (size_t i = 0; i + 1 < a.nums.size(); i += 2)
                delta[int32_t(segs[0].lo + int(i / 2))] = glm::vec2(float(a.nums[i]), float(a.nums[i + 1]));
            for (size_t c = 0; c < g.cornerUv[set].size() && c < uv.size(); ++c)
                if (auto it = delta.find(g.cornerUv[set][c]); it != delta.end())
                    uv[c] += it->second;
        }
    }

    /** Maya-space primitive from a creator node, in cm. */
    std::optional<Geom> makePrimitive(int node) const
    {
        const MNode& n = mIx.node(node);
        const std::string& t = n.type;
        const double L = mU.lin;
        auto d = [&](Names names, double def) { return num(n, names, def); };
        auto i = [&](Names names, double def, int lo = 1) { return std::max(lo, int(num(n, names, def))); };
        Geom g;
        if (t == "polyCube")
            g.mesh = geo::makeCube({d({"w", "width"}, 1) * L, d({"h", "height"}, 1) * L, d({"d", "depth"}, 1) * L, i({"sw", "subdivisionsWidth"}, 1),
                                    i({"sh", "subdivisionsHeight"}, 1), i({"sd", "subdivisionsDepth"}, 1), false});
        else if (t == "polySphere")
            g.mesh = geo::makeSphere({d({"r", "radius"}, 1) * L, i({"sa", "subdivisionsAxis"}, 20, 3), i({"sh", "subdivisionsHeight"}, 20, 2), false});
        else if (t == "polyCylinder")
            g.mesh = geo::makeCylinder({d({"r", "radius"}, 1) * L, d({"h", "height"}, 2) * L, i({"sa", "subdivisionsAxis"}, 20, 3),
                                        i({"sh", "subdivisionsHeight"}, 1), i({"sc", "subdivisionsCaps"}, 0, 0), true, false});
        else if (t == "polyCone")
            g.mesh = geo::makeCone({d({"r", "radius"}, 1) * L, d({"h", "height"}, 2) * L, i({"sa", "subdivisionsAxis"}, 20, 3),
                                    i({"sh", "subdivisionsHeight"}, 1), i({"sc", "subdivisionsCap"}, 0, 0), false});
        else if (t == "polyPlane")
            g.mesh = geo::makePlane({d({"w", "width"}, 1) * L, d({"h", "height"}, 1) * L, i({"sw", "subdivisionsWidth"}, 10),
                                     i({"sh", "subdivisionsHeight"}, 10)});
        else if (t == "polyTorus")
            g.mesh = geo::makeTorus({d({"r", "radius"}, 1) * L, d({"sr", "sectionRadius"}, 0.5) * L, i({"sa", "subdivisionsAxis"}, 20, 3),
                                     i({"sh", "subdivisionsHeight"}, 20, 3)});
        else if (t == "polyPipe")
            g.mesh = geo::makePipe({d({"r", "radius"}, 1) * L, d({"t", "thickness"}, 0.5) * L, d({"h", "height"}, 2) * L,
                                    i({"sa", "subdivisionsAxis"}, 20, 3), i({"sh", "subdivisionsHeight"}, 1), false});
        else
            return std::nullopt;
        glm::dvec3 ax = vec3(n, {"ax", "axis"}, {Names{"axx"}, Names{"axy"}, Names{"axz"}}, glm::dvec3(0, 1, 0));
        if (glm::length(ax) > 1e-9 && glm::length(glm::normalize(ax) - glm::dvec3(0, 1, 0)) > 1e-9)
        {
            ax = glm::normalize(ax);
            glm::dvec3 axis = glm::cross(glm::dvec3(0, 1, 0), ax);
            double angle = std::acos(std::clamp(ax.y, -1.0, 1.0));
            glm::dmat4 rot = glm::length(axis) < 1e-9 ? glm::scale(glm::dmat4(1.0), glm::dvec3(1, -1, -1))
                                                       : glm::rotate(glm::dmat4(1.0), angle, glm::normalize(axis));
            for (Index p = 0; p < g.mesh.pointCapacity(); ++p)
                g.mesh.setPosition(p, glm::dvec3(rot * glm::dvec4(g.mesh.position(p), 1.0)));
            if (g.mesh.cornerAttrs().has(geo::attr::kNormal))
                geo::updateCornerNormals(g.mesh);
        }
        return g;
    }

    /** Shading groups on a shape: (shading group, Maya faces or empty for all). */
    std::vector<std::pair<int, std::vector<int>>> assignments(int shape)
    {
        std::vector<std::pair<int, std::vector<int>>> out;
        const MNode& sn = mIx.node(shape);
        for (const auto& [src, dst] : mIx.outputs(shape))
        {
            if (mIx.node(dst.node).type != "shadingEngine" && shortName(mIx.node(dst.node).name) != "initialShadingGroup")
                continue;
            std::vector<AttrSeg> d = splitAttr(dst.attr);
            if (d.empty() || !oneOf(d[0].name, {"dsm", "dagSetMembers"}))
                continue;
            std::vector<AttrSeg> s = splitAttr(src.attr);
            if (s.empty() || !oneOf(s[0].name, {"iog", "instObjGroups"}))
                continue;
            if (s.size() == 1)
            {
                out.push_back({dst.node, {}});
                continue;
            }
            const int group = std::max(s[1].lo, 0);
            std::vector<int> faces;
            bool all = false;
            for (const AttrValue& a : sn.attrs)
            {
                std::vector<AttrSeg> as = splitAttr(a.attr);
                if (as.size() == 3 && oneOf(as[0].name, {"iog", "instObjGroups"}) && oneOf(as[1].name, {"og", "objectGroups"}) &&
                    as[1].lo == group && oneOf(as[2].name, {"gcl", "objectGrpCompList"}))
                    for (const std::string& c : a.strs)
                    {
                        std::string kind;
                        int lo = 0, hi = 0;
                        if (parseComponent(c, &kind, &lo, &hi) && kind == "f")
                        {
                            all = all || hi < 0;
                            for (int f = lo; f <= hi; ++f)
                                faces.push_back(f);
                        }
                    }
            }
            if (all)
                faces.clear();
            else if (faces.empty())
                continue; // a group with no faces assigns nothing
            out.push_back({dst.node, std::move(faces)});
        }
        return out;
    }

    /** Material slot for a shading group: its surface shader's Lila material, created on first use. */
    std::string slotFor(int sg)
    {
        mHandled.insert(sg);
        if (shortName(mIx.node(sg).name) == "initialShadingGroup")
            return scene::materials::kDefaultSlot;
        auto shader = mIx.input(sg, {"ss", "surfaceShader"});
        if (!shader)
            return lilaName(mIx.node(sg).name, mOpts.nameSpace, "Material");
        if (auto it = mMaterial.find(shader->node); it != mMaterial.end())
            return it->second;
        std::string name = buildMaterial(shader->node);
        mMaterial[shader->node] = name;
        return name;
    }

    /** The file texture feeding a material input, or empty. Reports inputs from anything else. fileNode gets the file node. */
    std::string textureInput(int mat, Names names, const std::string& matName, bool* bumpOnly = nullptr, int* fileNode = nullptr)
    {
        auto src = mIx.input(mat, names);
        if (!src)
            return {};
        int s = src->node;
        mHandled.insert(s);
        const std::string& t = mIx.node(s).type;
        if (t == "bump2d" || t == "aiNormalMap")
        {
            if (t == "bump2d" && num(mIx.node(s), {"bi", "bumpInterp"}, 0.0) == 0.0)
            {
                if (bumpOnly)
                    *bumpOnly = true;
                mR.warnings.push_back(std::format("{}: bump map (height) not converted; only tangent-space normal maps are", matName));
                return {};
            }
            auto f = mIx.input(s, t == "bump2d" ? Names{"bv", "bumpValue"} : Names{"input", "i"});
            if (!f)
                return {};
            s = f->node;
            mHandled.insert(s);
        }
        if (mIx.node(s).type != "file" && mIx.node(s).type != "aiImage")
        {
            mR.warnings.push_back(std::format("{}.{} is driven by a {} node, not converted", matName, *names.begin(), mIx.node(s).type));
            return {};
        }
        if (auto uv = mIx.input(s, {"uvc", "uvCoord"}))
            mHandled.insert(uv->node);
        if (fileNode)
            *fileNode = s;
        std::string path = text(mIx.node(s), {"ftn", "fileTextureName", "filename"});
        return resolveTexture(path);
    }

    /** A file node's wrap and filter: wrap and mirror from its place2dTexture (or itself), filter type Off as nearest. */
    scene::material::TextureSampling fileSampling(int file)
    {
        using namespace scene::material;
        const MNode& f = mIx.node(file);
        const auto place = mIx.input(file, {"uv", "uvc", "uvCoord"});
        const MNode& p = place ? mIx.node(place->node) : f;
        TextureSampling s;
        if (num(p, {"mu", "mirrorU"}, 0.0) != 0.0)
            s.wrapU = TextureWrap::Mirror;
        else if (num(p, {"wu", "wrapU"}, 1.0) == 0.0)
            s.wrapU = TextureWrap::Black;
        if (num(p, {"mv", "mirrorV"}, 0.0) != 0.0)
            s.wrapV = TextureWrap::Mirror;
        else if (num(p, {"wv", "wrapV"}, 1.0) == 0.0)
            s.wrapV = TextureWrap::Black;
        if (num(f, {"ft", "filterType"}, 1.0) == 0.0)
            s.filter = TextureFilter::Nearest;
        return s;
    }

    std::string resolveTexture(std::string path) const
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.empty())
            return path;
        fs::path p = fs::path(std::u8string(path.begin(), path.end()));
        std::error_code ec;
        if (p.is_relative())
        {
            // Maya resolves relative paths against the project; the scene usually sits in <project>/scenes.
            fs::path dir = mPath.parent_path();
            for (const fs::path& base : {dir, dir.parent_path()})
                if (fs::exists(base / p, ec))
                {
                    std::u8string s = fs::weakly_canonical(base / p, ec).generic_u8string();
                    return std::string(s.begin(), s.end());
                }
        }
        return path;
    }

    std::string buildMaterial(int mat)
    {
        using namespace scene;
        mHandled.insert(mat);
        const MNode& n = mIx.node(mat);
        const std::string& t = n.type;
        std::string name = lilaName(n.name, mOpts.nameSpace, "Material");
        if (!mOpts.materials || !types::find("material"))
            return name;
        if (materials::find(mG, name))
        {
            mR.materials.push_back(name);
            return name;
        }
        scene::Node* m = materials::create(mG, name);
        name = m->name();
        mR.materials.push_back(name);
        const NodeId id = m->id();
        auto set = [&](int attr, PlugValue v) { mG.setValue({id, attr, -1}, std::move(v)); };
        auto map = [&](int attr, Names names) {
            int file = -1;
            if (std::string p = textureInput(mat, names, name, nullptr, &file); !p.empty())
            {
                set(attr, p);
                if (file >= 0)
                {
                    material::TextureSampling s = fileSampling(file);
                    const std::string space = text(mIx.node(file), {"cs", "colorSpace"});
                    if (const std::optional<bool> srgb = materials::srgbFromName(space))
                        s.colorSpace = materials::colorSpaceFor(attr, *srgb);
                    else if (!space.empty())
                        mR.warnings.push_back(std::format("{}: texture colour space {} is unknown; read as {}", name, space,
                                                          materials::colorMap(attr) ? "sRGB" : "linear"));
                    materials::setSampling(mG, id, attr, s);
                }
            }
        };
        auto opacityFrom = [&](double opacity) {
            if (opacity < 0.999)
            {
                set(material::kOpacity, std::clamp(opacity, 0.0, 1.0));
                set(material::kBlendMode, int64_t(material::BlendMode::Translucent));
            }
        };
        auto avg = [](glm::dvec3 v) { return (v.x + v.y + v.z) / 3.0; };

        if (t == "lambert" || t == "blinn" || t == "phong" || t == "phongE" || t == "anisotropic")
        {
            glm::dvec3 c = color(n, {"c", "color"}, glm::dvec3(0.5));
            set(material::kBaseColor, c * num(n, {"dc", "diffuse"}, 0.8));
            map(material::kBaseColorMap, {"c", "color"});
            map(material::kNormalMap, {"n", "normalCamera"});
            opacityFrom(1.0 - avg(color(n, {"it", "transparency"}, glm::dvec3(0.0))));
            glm::dvec3 ic = color(n, {"ic", "incandescence"}, glm::dvec3(0.0));
            if (ic != glm::dvec3(0.0))
                set(material::kEmissiveColor, ic);
            map(material::kEmissiveMap, {"ic", "incandescence"});
            double rough = 1.0;
            if (t == "blinn")
                rough = std::clamp(num(n, {"ec", "eccentricity"}, 0.3), 0.0, 1.0);
            else if (t == "phong")
                rough = std::sqrt(2.0 / (std::max(num(n, {"cp", "cosinePower"}, 20.0), 0.0) + 2.0));
            else if (t == "phongE")
                rough = std::clamp(num(n, {"rn", "roughness"}, 0.5), 0.0, 1.0);
            set(material::kRoughness, rough);
        }
        else if (t == "standardSurface" || t == "aiStandardSurface")
        {
            // Arnold's own node names its attributes in snake_case; Maya's standardSurface uses short names.
            double base = num(n, {"b", "base"}, 1.0);
            set(material::kBaseColor, color(n, {"bc", "baseColor", "base_color"}, glm::dvec3(0.8)) * base);
            set(material::kMetallic, std::clamp(num(n, {"m", "metalness"}, 0.0), 0.0, 1.0));
            set(material::kRoughness, std::clamp(num(n, {"sr", "specularRoughness", "specular_roughness"}, 0.2), 0.0, 1.0));
            glm::dvec3 e = color(n, {"ec", "emissionColor", "emission_color"}, glm::dvec3(1.0)) * num(n, {"e", "emission"}, 0.0);
            if (e != glm::dvec3(0.0))
                set(material::kEmissiveColor, e);
            opacityFrom(avg(color(n, {"op", "opacity"}, glm::dvec3(1.0))));
            if (num(n, {"thw", "thinWalled", "thin_walled"}, 0.0) != 0.0)
                set(material::kTwoSided, true);
            map(material::kBaseColorMap, {"bc", "baseColor", "base_color"});
            map(material::kMetallicMap, {"m", "metalness"});
            map(material::kRoughnessMap, {"sr", "specularRoughness", "specular_roughness"});
            map(material::kEmissiveMap, {"ec", "emissionColor", "emission_color"});
            map(material::kNormalMap, {"n", "normalCamera"});
        }
        else if (t == "openPBRSurface")
        {
            double base = num(n, {"b", "baseWeight"}, 1.0);
            set(material::kBaseColor, color(n, {"bc", "baseColor"}, glm::dvec3(0.8)) * base);
            set(material::kMetallic, std::clamp(num(n, {"m", "baseMetalness"}, 0.0), 0.0, 1.0));
            set(material::kRoughness, std::clamp(num(n, {"sr", "specularRoughness"}, 0.3), 0.0, 1.0));
            // Emission is in nits; 1000 maps to a full-strength emissive color.
            double lum = num(n, {"elu", "emissionLuminance"}, 0.0);
            if (lum > 0.0)
                set(material::kEmissiveColor, color(n, {"ec", "emissionColor"}, glm::dvec3(1.0)) * std::min(lum / 1000.0, 1.0));
            opacityFrom(num(n, {"geop", "geometryOpacity"}, 1.0));
            if (num(n, {"getw", "geometryThinWalled"}, 0.0) != 0.0)
                set(material::kTwoSided, true);
            map(material::kBaseColorMap, {"bc", "baseColor"});
            map(material::kMetallicMap, {"m", "baseMetalness"});
            map(material::kRoughnessMap, {"sr", "specularRoughness"});
            map(material::kEmissiveMap, {"ec", "emissionColor"});
            map(material::kNormalMap, {"n", "normalCamera"});
        }
        else if (t == "surfaceShader")
        {
            // Maya's surface shader shows its color with no lighting.
            set(material::kShadingModel, int64_t(material::ShadingModel::Unlit));
            set(material::kBaseColor, color(n, {"oc", "outColor"}, glm::dvec3(0.0)));
            map(material::kBaseColorMap, {"oc", "outColor"});
            opacityFrom(1.0 - avg(color(n, {"ot", "outTransparency"}, glm::dvec3(0.0))));
        }
        else if (t == "aiFlat")
        {
            set(material::kShadingModel, int64_t(material::ShadingModel::Unlit));
            set(material::kBaseColor, color(n, {"color"}, glm::dvec3(1.0)));
            map(material::kBaseColorMap, {"color"});
        }
        else if (t == "usdPreviewSurface")
        {
            set(material::kBaseColor, color(n, {"dc", "diffuseColor"}, glm::dvec3(0.18)));
            set(material::kMetallic, std::clamp(num(n, {"mt", "metallic"}, 0.0), 0.0, 1.0));
            set(material::kRoughness, std::clamp(num(n, {"rgh", "roughness"}, 0.5), 0.0, 1.0));
            opacityFrom(num(n, {"opc", "opacity"}, 1.0));
            map(material::kBaseColorMap, {"dc", "diffuseColor"});
            map(material::kNormalMap, {"nrm", "normal"});
        }
        else
            mR.warnings.push_back(std::format("material '{}' is a {}; created with default settings", name, t));
        return name;
    }

    /** The primitive creator directly feeding a shape, when it can stay a live Lila generator. */
    int liveCreator(int shape) const
    {
        if (!mOpts.history || !mIdentityAxes || mU.lin != 1.0)
            return -1;
        auto src = mIx.input(shape, {"i", "inMesh"});
        if (!src || !creatorTypes().contains(mIx.node(src->node).type) || !scene::types::find(mIx.node(src->node).type))
            return -1;
        const MNode& c = mIx.node(src->node);
        glm::dvec3 ax = vec3(c, {"ax", "axis"}, {Names{"axx"}, Names{"axy"}, Names{"axz"}}, glm::dvec3(0, 1, 0));
        if (glm::length(ax - glm::dvec3(0, 1, 0)) > 1e-9)
            return -1;
        for (const AttrValue& a : mIx.node(shape).attrs)
            if (splitAttr(a.attr)[0].name == "pt" || splitAttr(a.attr)[0].name == "pnts")
                return -1;
        return src->node;
    }

    void buildGenerator(int creator, NodeId meshId)
    {
        const MNode& c = mIx.node(creator);
        mHandled.insert(creator);
        const scene::NodeType& type = *scene::types::find(c.type);
        scene::Node* gen = mG.createNode(type, lilaName(c.name, mOpts.nameSpace, c.type.c_str()), scene::kNoNode, false);
        auto set = [&](const char* attr, Names names, double def, bool isInt) {
            int a = type.attrIndex(attr);
            if (a < 0)
                return;
            double v = num(c, names, def);
            if (isInt)
                mG.setValue({gen->id(), a, -1}, int64_t(std::max(0.0, v)));
            else
                mG.setValue({gen->id(), a, -1}, v);
        };
        const std::string& t = c.type;
        if (t == "polyCube")
        {
            set("width", {"w", "width"}, 1, false), set("height", {"h", "height"}, 1, false), set("depth", {"d", "depth"}, 1, false);
            set("subdivisionsX", {"sw", "subdivisionsWidth"}, 1, true), set("subdivisionsY", {"sh", "subdivisionsHeight"}, 1, true);
            set("subdivisionsZ", {"sd", "subdivisionsDepth"}, 1, true);
        }
        else if (t == "polySphere")
        {
            set("radius", {"r", "radius"}, 1, false), set("subdivisionsAxis", {"sa", "subdivisionsAxis"}, 20, true);
            set("subdivisionsHeight", {"sh", "subdivisionsHeight"}, 20, true);
        }
        else if (t == "polyCylinder" || t == "polyCone")
        {
            set("radius", {"r", "radius"}, 1, false), set("height", {"h", "height"}, 2, false);
            set("subdivisionsAxis", {"sa", "subdivisionsAxis"}, 20, true), set("subdivisionsHeight", {"sh", "subdivisionsHeight"}, 1, true);
            set(t == "polyCone" ? "subdivisionsCap" : "subdivisionsCaps", {"sc", "subdivisionsCaps", "subdivisionsCap"}, 0, true);
        }
        else if (t == "polyPlane")
        {
            set("width", {"w", "width"}, 1, false), set("depth", {"h", "height"}, 1, false);
            set("subdivisionsX", {"sw", "subdivisionsWidth"}, 10, true), set("subdivisionsZ", {"sh", "subdivisionsHeight"}, 10, true);
        }
        else if (t == "polyTorus")
        {
            set("radius", {"r", "radius"}, 1, false), set("sectionRadius", {"sr", "sectionRadius"}, 0.5, false);
            set("subdivisionsAxis", {"sa", "subdivisionsAxis"}, 20, true), set("subdivisionsHeight", {"sh", "subdivisionsHeight"}, 20, true);
        }
        else if (t == "polyPipe")
        {
            set("radius", {"r", "radius"}, 1, false), set("thickness", {"t", "thickness"}, 0.5, false), set("height", {"h", "height"}, 2, false);
            set("subdivisionsAxis", {"sa", "subdivisionsAxis"}, 20, true), set("subdivisionsHeight", {"sh", "subdivisionsHeight"}, 1, true);
        }
        mG.connect({gen->id(), type.attrIndex("outMesh"), -1}, {meshId, scene::MeshNode::kInMesh, -1});
    }

    /** A mesh whose geometry is evaluated with the others, in parallel, once the DAG is built. */
    struct PendingMesh
    {
        int shape = -1;
        NodeId id = scene::kNoNode;
        std::string name;
        std::vector<std::pair<std::string, std::vector<int>>> slots; // material slot, Maya faces (empty: all of them)
        EvalLog log;
        scene::MeshPtr mesh;
        std::vector<Index> pointOf;
        bool mayaPoints = false;
    };

    void buildMesh(int shape, NodeId meshId)
    {
        mShapeNode[shape] = meshId;
        const std::string name = mG.node(meshId)->path();
        auto sgs = mOpts.materials ? assignments(shape) : std::vector<std::pair<int, std::vector<int>>>{};
        const bool plainMaterial = sgs.empty() || (sgs.size() == 1 && sgs[0].second.empty() &&
                                                   shortName(mIx.node(sgs[0].first).name) == "initialShadingGroup");
        if (int creator = liveCreator(shape); creator >= 0 && plainMaterial && !isSkinned(shape))
        {
            buildGenerator(creator, meshId);
            for (const auto& [sg, faces] : sgs)
                mHandled.insert(sg);
            return;
        }
        PendingMesh p{.shape = shape, .id = meshId, .name = name};
        for (const auto& [sg, faces] : sgs)
            p.slots.push_back({slotFor(sg), faces});
        mPending.push_back(std::move(p));
    }

    /** History, materials and axes of one pending mesh. Touches nothing shared, so any thread. */
    void evaluateMesh(PendingMesh& p) const
    {
        LILA_ZONE();

        std::optional<Geom> g = evalNode(p.shape, "o", p.name, 0, p.log);
        if (!g)
        {
            p.log.warnings.push_back(std::format("{}: no geometry could be read; left empty", p.name));
            return;
        }
        geo::Mesh& m = g->mesh;

        if (!p.slots.empty())
        {
            std::span<int32_t> mat = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
            bool perFace = false;
            for (const auto& [slotName, faces] : p.slots)
            {
                const int32_t slot = scene::materials::slotIndex(m, slotName);
                if (faces.empty())
                {
                    std::fill(mat.begin(), mat.end(), slot);
                    continue;
                }
                perFace = true;
                if (g->faceOf.empty())
                    continue;
                for (int f : faces)
                    if (f >= 0 && size_t(f) < g->faceOf.size() && g->faceOf[size_t(f)] != kInvalid)
                        mat[g->faceOf[size_t(f)]] = slot;
            }
            if (perFace && g->faceOf.empty())
            {
                p.log.warnings.push_back(std::format("{}: per-face materials could not be matched after history; the first one covers the mesh", p.name));
                std::fill(mat.begin(), mat.end(), 0);
            }
            scene::materials::pruneSlots(m);
        }

        if (!mIdentityAxes)
        {
            for (Index i = 0; i < m.pointCapacity(); ++i)
                m.setPosition(i, point(m.position(i)));
            if (m.cornerAttrs().has(geo::attr::kNormal))
                for (glm::vec3& nrm : m.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal))
                    nrm = glm::vec3(mC3 * glm::dvec3(nrm));
        }

        std::vector<Index> pointOf(m.pointCapacity());
        for (Index i = 0; i < pointOf.size(); ++i)
            pointOf[i] = i;
        m.removeAllLoose();
        if (m.hasDeleted())
        {
            geo::MeshRemap remap;
            m.compact(&remap);
            for (Index& i : pointOf)
                i = i < remap.points.size() ? remap.points[i] : kInvalid;
        }
        p.mayaPoints = g->mayaPoints;
        if (p.mayaPoints)
            p.pointOf = std::move(pointOf);
        p.mesh = std::make_shared<geo::Mesh>(std::move(m));
    }

    /** Evaluates every pending mesh off the main thread, then hands each to its node in creation order. */
    void finishMeshes()
    {
        LILA_ZONE();

        if (mPending.empty())
            return;
        const size_t count = mPending.size();
        progress::Scope scope("Building meshes", double(count));
        std::atomic<size_t> built{0};
        progress::runOnWorker([&] {
            tbb::parallel_for(size_t(0), count, [&](size_t i) {
                if (scope.cancelled())
                    return;
                evaluateMesh(mPending[i]);
                scope.step();
                scope.setStatus(std::format("Building meshes {}/{}", ++built, count));
            });
        });
        scope.checkCancel();
        for (PendingMesh& p : mPending)
        {
            mHandled.insert(p.log.handled.begin(), p.log.handled.end());
            mR.warnings.insert(mR.warnings.end(), p.log.warnings.begin(), p.log.warnings.end());
            if (!p.mesh)
                continue;
            if (p.mayaPoints)
                mPointOf[p.shape] = std::move(p.pointOf);
            mG.setValue({p.id, scene::MeshNode::kInMesh, -1}, p.mesh);
        }
        mPending.clear();
    }

    bool isSkinned(int shape) const
    {
        auto src = mIx.input(shape, {"i", "inMesh"});
        for (int guard = 0; src && guard < 64; ++guard)
        {
            const std::string& t = mIx.node(src->node).type;
            if (t == "skinCluster")
                return true;
            if (!deformerTypes().contains(t) && t != "groupParts")
                return false;
            src = mIx.input(src->node, {"ip[0].ig", "ig", "input[0].inputGeometry", "inputGeometry"});
        }
        return false;
    }

    // ---- skins ----------------------------------------------------------------------------------------------

    /** The mesh shape a deformer's output k (any output for -1) ends in, through any deformers after it; -1 for none. */
    int drivenShape(int deformer, int k) const
    {
        std::vector<int> frontier{deformer};
        for (int guard = 0; !frontier.empty() && guard < 64; ++guard)
        {
            int cur = frontier.back();
            frontier.pop_back();
            for (const auto& [src, dst] : mIx.outputs(cur))
            {
                std::string_view sa = src.attr;
                if (!(sa.starts_with("og") || sa.starts_with("outputGeometry") || sa.starts_with("out")))
                    continue;
                if (cur == deformer && k >= 0)
                    if (std::vector<AttrSeg> s = splitAttr(sa); s.empty() || std::max(s[0].lo, 0) != k)
                        continue;
                if (mIx.node(dst.node).type == "mesh" && oneOf(dst.attr, {"i", "inMesh"}))
                    return dst.node;
                frontier.push_back(dst.node);
            }
        }
        return -1;
    }

    void buildSkin(int sc)
    {
        LILA_ZONE();

        using namespace scene;
        mHandled.insert(sc);
        const MNode& n = mIx.node(sc);
        if (!types::find("skinCluster"))
            return;

        const int shape = drivenShape(sc, -1);
        auto meshIt = mShapeNode.find(shape);
        if (shape < 0 || meshIt == mShapeNode.end())
        {
            mR.skipped.push_back(std::format("skinCluster '{}' (its mesh was not imported)", shortName(n.name)));
            return;
        }
        auto pts = mPointOf.find(shape);
        if (pts == mPointOf.end())
        {
            mR.warnings.push_back(std::format("skinCluster '{}': the mesh's points no longer match Maya's, weights not imported",
                                              shortName(n.name)));
            return;
        }
        const NodeId mesh = meshIt->second;

        // Influences by matrix index.
        std::map<int, NodeId> joints;
        for (const auto& [src, dst] : mIx.inputs(sc))
        {
            std::vector<AttrSeg> d = splitAttr(dst.attr);
            if (d.size() == 1 && oneOf(d[0].name, {"ma", "matrix"}) && d[0].lo >= 0)
                if (auto it = mLila.find(src.node); it != mLila.end())
                    joints[d[0].lo] = it->second;
        }
        if (joints.empty())
        {
            mR.warnings.push_back(std::format("skinCluster '{}': no imported influences", shortName(n.name)));
            return;
        }

        PlugRef top{mesh, MeshNode::kInMesh, -1};
        scene::Node* skinNode = mG.createNode(*types::find("skinCluster"), lilaName(n.name, mOpts.nameSpace, "skinCluster"), kNoNode, false);
        const NodeId skinId = skinNode->id();
        scene::Node* source = mG.createNode(*types::find("meshSource"), "meshSource", kNoNode, true);
        mG.setValue({source->id(), MeshSourceNode::kMesh, -1}, mG.storedValue(top));
        mG.setValue(top, MeshPtr());
        mG.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {skinId, SkinClusterNode::kInMesh, -1});
        mG.connect({skinId, SkinClusterNode::kOutMesh, -1}, top);

        const glm::dmat4 parentWorld =
            mOpts.parent != kNoNode ? mG.node(mOpts.parent)->as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
        const glm::dmat4 parentInv = glm::inverse(parentWorld);
        double skm = num(n, {"skm", "skinningMethod"}, 0.0);
        if (skm == 2.0)
            mR.warnings.push_back(std::format("skinCluster '{}': weight-blended skinning imported as linear", shortName(n.name)));
        mG.setValue({skinId, SkinClusterNode::kSkinningMethod}, int64_t(skm == 1.0 ? 1 : 0));
        if (num(n, {"nw", "normalizeWeights"}, 1.0) == 0.0)
            mG.setValue({skinId, SkinClusterNode::kNormalizeWeights}, false);
        if (const AttrValue* gm = lastAttr(n, {"gm", "geomMatrix"}); gm && gm->nums.size() >= 16)
            mG.setValue({skinId, SkinClusterNode::kGeomMatrix}, parentWorld * matrix(matrixOf(gm->nums)));

        std::map<int, uint32_t> lilaIndex;
        for (const auto& [i, joint] : joints)
        {
            uint32_t index = skin::addInfluence(mG, skinId, joint);
            lilaIndex[i] = index;
            for (const skin::Influence& inf : skin::influences(mG, skinId))
                if (inf.index == index)
                {
                    std::string pmName = std::format("pm[{}]", i), pmLong = std::format("bindPreMatrix[{}]", i);
                    if (const AttrValue* pm = lastAttr(n, {pmName, pmLong}); pm && pm->nums.size() >= 16)
                        mG.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, matrix(matrixOf(pm->nums)) * parentInv);
                }
        }

        // Weights: "wl[a:b].w" holds count, (influence, weight)... per point; "wl[i].w[j:k]" holds dense values.
        MeshPtr bind = skin::inputMesh(mG, skinId);
        WeightTable t;
        t.points.resize(bind ? bind->pointCapacity() : 0);
        const std::vector<Index>& pointOf = pts->second;
        size_t used = 0;
        auto add = [&](int vertex, int inf, double w) {
            if (w <= 0.0 || vertex < 0 || size_t(vertex) >= pointOf.size())
                return;
            Index p = pointOf[size_t(vertex)];
            auto li = lilaIndex.find(inf);
            if (p == kInvalid || p >= t.points.size() || li == lilaIndex.end())
                return;
            t.points[p].push_back({li->second, float(w)});
        };
        for (const AttrValue& a : n.attrs)
        {
            std::vector<AttrSeg> s = splitAttr(a.attr);
            if (s.size() != 2 || !oneOf(s[0].name, {"wl", "weightList"}) || !oneOf(s[1].name, {"w", "weights"}) || s[0].lo < 0)
                continue;
            if (s[1].lo >= 0)
            {
                for (size_t k = 0; k < a.nums.size(); ++k)
                    add(s[0].lo, s[1].lo + int(k), a.nums[k]);
                continue;
            }
            size_t at = 0;
            for (int v = s[0].lo; v <= s[0].hi && at < a.nums.size(); ++v)
            {
                size_t cnt = size_t(a.nums[at++]);
                for (size_t k = 0; k < cnt && at + 1 < a.nums.size(); ++k, at += 2)
                    add(v, int(a.nums[at]), a.nums[at + 1]);
            }
        }
        for (const auto& pt : t.points)
            used = std::max(used, pt.size());
        int mi = int(num(n, {"mi", "maxInfluences"}, 0.0));
        mG.setValue({skinId, SkinClusterNode::kMaxInfluences}, int64_t(std::max<size_t>({4, used, size_t(std::max(mi, 0))})));
        skin::setWeights(mG, skinId, t);
        mR.skins.push_back(skinId);
    }

    // ---- blend shapes ---------------------------------------------------------------------------------------

    /** One inputTargetItem: stored sparse offsets, and the live target mesh connected to it. */
    struct BlendItem
    {
        std::vector<double> points; // x, y, z, w per stored point
        std::vector<std::string> comps;
        bool stored = false;
        std::optional<Plug> geom;
    };
    struct BlendTarget
    {
        std::map<int, BlendItem> items; // by item index: 6000 the full shape, 5000 + 1000 w an in-between at w
        std::map<int, double> weights;  // targetWeights by Maya vertex
    };
    /** inputTarget k of a blendShape: what it does to its k-th geometry. */
    struct BlendGeom
    {
        std::map<int, BlendTarget> targets; // by weight index
        std::map<int, double> baseWeights;
    };
    /** A converted blendShape: its Lila morphs and the targets each of its weights feeds. */
    struct Blend
    {
        std::map<std::string, int> aliases; // alias -> weight index
        std::map<int, std::vector<NodeId>> targets;
        std::vector<NodeId> morphs;
    };

    static std::map<int, BlendGeom> blendData(const MNode& n, const FileIndex& ix, int node)
    {
        std::map<int, BlendGeom> out;
        auto single = [](const AttrSeg& s) { return s.lo >= 0 && s.lo == s.hi; };
        for (const AttrValue& a : n.attrs)
        {
            std::vector<AttrSeg> s = splitAttr(a.attr);
            if (s.size() < 2 || !oneOf(s[0].name, {"it", "inputTarget"}) || !single(s[0]))
                continue;
            BlendGeom& g = out[s[0].lo];
            if (s.size() == 2 && oneOf(s[1].name, {"bw", "baseWeights"}) && s[1].lo >= 0)
            {
                for (size_t k = 0; k < a.nums.size(); ++k)
                    g.baseWeights[s[1].lo + int(k)] = a.nums[k];
                continue;
            }
            if (!oneOf(s[1].name, {"itg", "inputTargetGroup"}) || !single(s[1]))
                continue;
            BlendTarget& t = g.targets[s[1].lo];
            if (s.size() == 3 && oneOf(s[2].name, {"tw", "targetWeights"}) && s[2].lo >= 0)
                for (size_t k = 0; k < a.nums.size(); ++k)
                    t.weights[s[2].lo + int(k)] = a.nums[k];
            else if (s.size() == 4 && oneOf(s[2].name, {"iti", "inputTargetItem"}) && single(s[2]))
            {
                BlendItem& item = t.items[s[2].lo];
                if (oneOf(s[3].name, {"ipt", "inputPointsTarget"}))
                {
                    item.points = a.nums;
                    item.stored = true;
                }
                else if (oneOf(s[3].name, {"ict", "inputComponentsTarget"}))
                    item.comps = a.strs;
            }
        }
        for (const auto& [src, dst] : ix.inputs(node))
        {
            std::vector<AttrSeg> s = splitAttr(dst.attr);
            if (s.size() == 4 && oneOf(s[0].name, {"it", "inputTarget"}) && single(s[0]) && oneOf(s[1].name, {"itg", "inputTargetGroup"}) &&
                single(s[1]) && oneOf(s[2].name, {"iti", "inputTargetItem"}) && single(s[2]) && oneOf(s[3].name, {"igt", "inputGeomTarget"}))
                out[s[0].lo].targets[s[1].lo].items[s[2].lo].geom = src;
        }
        return out;
    }

    /** Whether a skinCluster deforms the geometry before this deformer's input k. */
    bool skinBefore(int deformer, int k) const
    {
        const std::string ig = std::format("ip[{}].ig", k), ig2 = std::format("input[{}].inputGeometry", k);
        auto src = mIx.input(deformer, {ig, ig2});
        for (int guard = 0; src && guard < 64; ++guard)
        {
            const std::string& t = mIx.node(src->node).type;
            if (t == "skinCluster")
                return true;
            if (!deformerTypes().contains(t) && t != "groupParts")
                return false;
            src = mIx.input(src->node, {"ip[0].ig", "ig", "input[0].inputGeometry", "inputGeometry"});
        }
        return false;
    }

    /** Lila-space offsets, sparse over Lila points, from a stored item. */
    std::shared_ptr<scene::MorphDeltas> storedDeltas(const BlendItem& item, const std::vector<Index>& pointOf, const std::string& what)
    {
        auto d = std::make_shared<scene::MorphDeltas>();
        std::vector<int> verts;
        for (const std::string& c : item.comps)
        {
            std::string kind;
            int lo = 0, hi = 0;
            if (!parseComponent(c, &kind, &lo, &hi) || (kind != "vtx" && kind != "pt"))
                continue;
            if (hi < 0)
                hi = int(pointOf.size()) - 1;
            for (int v = lo; v <= hi; ++v)
                verts.push_back(v);
        }
        const size_t count = item.points.size() / 4;
        if (item.comps.empty())
            for (size_t v = 0; v < count; ++v)
                verts.push_back(int(v));
        if (verts.size() != count)
            mR.warnings.push_back(std::format("{}: {} stored offsets for {} vertices; the extra ones were dropped", what, count, verts.size()));
        for (size_t k = 0; k < count && k < verts.size(); ++k)
        {
            const int v = verts[k];
            if (v < 0 || size_t(v) >= pointOf.size() || pointOf[size_t(v)] == kInvalid)
                continue;
            glm::dvec3 off = point(glm::dvec3(item.points[k * 4], item.points[k * 4 + 1], item.points[k * 4 + 2]) * mU.lin);
            if (glm::dot(off, off) == 0.0)
                continue;
            d->indices.push_back(uint32_t(pointOf[size_t(v)]));
            d->deltas.push_back(off);
        }
        return d;
    }

    /**
     * Offsets from a target mesh in the file, evaluated through its history: its points against the morph's input,
     * through both world matrices for a world-origin blendShape. Null when its points don't match the base's.
     */
    std::shared_ptr<scene::MorphDeltas> meshDeltas(const Plug& src, const std::vector<Index>& pointOf, const geo::Mesh& base, NodeId baseNode,
                                                    bool world, const std::string& what)
    {
        EvalLog log;
        std::optional<Geom> g = evalNode(src.node, src.attr, what, 0, log);
        mHandled.insert(log.handled.begin(), log.handled.end());
        mR.warnings.insert(mR.warnings.end(), log.warnings.begin(), log.warnings.end());
        if (!g || !g->mayaPoints || g->mesh.pointCapacity() != pointOf.size())
            return nullptr;
        glm::dmat4 toBase(1.0);
        if (world)
        {
            const int xf = mIx.parent[size_t(src.node)];
            auto it = xf >= 0 ? mLila.find(xf) : mLila.end();
            const glm::dmat4 targetWorld = it != mLila.end() ? mG.node(it->second)->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
            toBase = glm::inverse(mG.node(baseNode)->as<glm::dmat4>(scene::Transform::kWorldMatrix)) * targetWorld;
        }
        auto d = std::make_shared<scene::MorphDeltas>();
        for (size_t v = 0; v < pointOf.size(); ++v)
        {
            const Index p = pointOf[v];
            if (p == kInvalid || p >= base.pointCapacity())
                continue;
            glm::dvec3 off = glm::dvec3(toBase * glm::dvec4(point(g->mesh.position(Index(v))), 1.0)) - base.position(p);
            if (glm::length(off) > 1e-7)
            {
                d->indices.push_back(uint32_t(p));
                d->deltas.push_back(off);
            }
        }
        return d;
    }

    /** A Maya per-vertex weight list as a Lila per-point map; null when every weight is 1. */
    static scene::DataPtr pointWeightsOf(const std::map<int, double>& weights, const std::vector<Index>& pointOf, size_t points)
    {
        bool any = false;
        auto w = std::make_shared<scene::PointWeights>();
        w->values.assign(points, 1.0f);
        for (const auto& [v, value] : weights)
            if (v >= 0 && size_t(v) < pointOf.size() && pointOf[size_t(v)] < points && value != 1.0)
            {
                w->values[pointOf[size_t(v)]] = float(value);
                any = true;
            }
        return any ? scene::DataPtr(std::move(w)) : scene::DataPtr();
    }

    void buildBlendShapes()
    {
        LILA_ZONE();

        using namespace scene;
        if (!types::find("morph"))
            return;
        for (size_t i = 0; i < mFile.nodes.size(); ++i)
            if (mFile.nodes[i].type == "blendShape")
                buildBlendShape(int(i));
    }

    void buildBlendShape(int bs)
    {
        using namespace scene;
        mHandled.insert(bs);
        const MNode& n = mIx.node(bs);
        const std::string bsName = shortName(n.name);
        Blend blend;
        if (const AttrValue* aal = lastAttr(n, {"aal", "attributeAliasList"}))
        {
            std::vector<std::string> list;
            for (const std::string& s : aal->strs)
                if (s != "{" && s != "}" && s != ",")
                    list.push_back(s);
            for (size_t k = 0; k + 1 < list.size(); k += 2)
                if (std::vector<AttrSeg> s = splitAttr(list[k + 1]); s.size() == 1 && oneOf(s[0].name, {"w", "weight"}) && s[0].lo >= 0)
                    blend.aliases[list[k]] = s[0].lo;
        }
        std::map<int, std::string> nameOf;
        for (const auto& [alias, w] : blend.aliases)
            nameOf[w] = alias;
        const std::map<int, double> weights = elements(n, {"w", "weight"});
        const double envelope = num(n, {"en", "envelope"}, 1.0);
        const bool world = num(n, {"or", "origin"}, 0.0) == 1.0;

        for (auto& [k, geom] : blendData(n, mIx, bs))
        {
            const int shape = drivenShape(bs, k);
            auto meshIt = mShapeNode.find(shape);
            if (shape < 0 || meshIt == mShapeNode.end())
            {
                mR.skipped.push_back(std::format("blendShape '{}' geometry {} (its mesh was not imported)", bsName, k));
                continue;
            }
            auto pts = mPointOf.find(shape);
            if (pts == mPointOf.end())
            {
                mR.warnings.push_back(std::format("blendShape '{}': the mesh's points no longer match Maya's, targets not imported", bsName));
                continue;
            }
            const NodeId mesh = meshIt->second;
            const std::vector<Index>& pointOf = pts->second;
            if (skinBefore(bs, k))
                mR.warnings.push_back(std::format("blendShape '{}' deforms after its skinCluster in Maya; Lila applies morphs before skinning, so "
                                                  "posed shapes differ",
                                                  bsName));

            NodeId morph = morphs::find(mG, mesh);
            const bool merged = morph != kNoNode;
            if (!merged)
            {
                morph = morphs::create(mG, mesh, lilaName(n.name, mOpts.nameSpace, "blendShape"));
                mR.morphs.push_back(morph);
                if (envelope != 1.0)
                    mG.setValue({morph, DeformerNode::kEnvelope, -1}, envelope);
            }
            else if (envelope != 1.0)
                mR.warnings.push_back(std::format("blendShape '{}' shares {}'s morph; its envelope {} was dropped", bsName, mG.node(mesh)->path(), envelope));
            blend.morphs.push_back(morph);
            mG.evaluate();
            const MeshPtr base = std::get<MeshPtr>(mG.value({morph, DeformerNode::kInMesh, -1}));
            if (!base)
                continue;
            const size_t points = base->pointCapacity();
            DataPtr baseWeights = pointWeightsOf(geom.baseWeights, pointOf, points);
            if (baseWeights && !merged)
                mG.setValue({morph, DeformerNode::kWeights, -1}, baseWeights);

            for (auto& [w, target] : geom.targets)
            {
                const std::string name = nameOf.contains(w) ? nameOf[w] : std::format("{}_{}", bsName, w);
                const std::string what = std::format("blendShape '{}' target '{}'", bsName, name);
                auto deltasOf = [&](const BlendItem& item) -> std::shared_ptr<MorphDeltas> {
                    if (item.geom && mIx.node(item.geom->node).type == "mesh")
                        if (auto d = meshDeltas(*item.geom, pointOf, *base, mesh, world, what))
                            return d;
                    if (item.stored)
                        return storedDeltas(item, pointOf, what);
                    if (item.geom)
                        mR.warnings.push_back(std::format("{}: its target geometry could not be matched to the base's points; left empty", what));
                    return std::make_shared<MorphDeltas>();
                };

                // A live target: an imported mesh with the base's point order, read in its own space as morphTarget does.
                NodeId live = kNoNode;
                if (auto full = target.items.find(6000); full != target.items.end() && full->second.geom && !world)
                    if (auto it = mShapeNode.find(full->second.geom->node); it != mShapeNode.end())
                        if (auto tp = mPointOf.find(full->second.geom->node); tp != mPointOf.end() && tp->second == pointOf)
                            if (MeshPtr m = std::get<MeshPtr>(mG.value({it->second, MeshNode::kOutMesh, -1})); m && m->pointCapacity() == points)
                                live = it->second;

                std::shared_ptr<MorphDeltas> full;
                if (auto it = target.items.find(6000); it != target.items.end() && live == kNoNode)
                    full = deltasOf(it->second);
                else
                    full = std::make_shared<MorphDeltas>();
                if (!target.items.contains(6000))
                    mR.warnings.push_back(std::format("{}: no full shape (only in-betweens); its full shape is left empty", what));
                const double weight = weights.contains(w) ? weights.at(w) : 0.0;
                const NodeId t = morphs::addTarget(mG, morph, name, full, weight);
                if (live != kNoNode)
                    mG.connect({live, MeshNode::kOutMesh, -1}, {t, morphs::kTargetMesh, -1});
                for (const auto& [index, item] : target.items)
                {
                    const double at = double(index - 5000) / 1000.0;
                    if (index == 6000)
                        continue;
                    if (std::abs(at) < 1e-9)
                    {
                        mR.warnings.push_back(std::format("{}: an in-between at weight 0 was dropped", what));
                        continue;
                    }
                    morphs::setInBetween(mG, t, at, deltasOf(item));
                }
                std::map<int, double> tw = target.weights;
                if (merged)
                    for (const auto& [v, b] : geom.baseWeights)
                        tw[v] = (tw.contains(v) ? tw[v] : 1.0) * b;
                if (DataPtr pw = pointWeightsOf(tw, pointOf, points))
                    morphs::setPointWeights(mG, t, pw);
                blend.targets[w].push_back(t);
            }
        }
        if (!blend.morphs.empty())
            mBlends[bs] = std::move(blend);
    }

    /** The Lila plugs a plug of a converted blendShape lands on: a weight's targets, or its morphs' envelopes. */
    std::vector<PlugRef> blendPlugs(int bs, const std::string& attr) const
    {
        std::vector<PlugRef> out;
        auto it = mBlends.find(bs);
        if (it == mBlends.end())
            return out;
        const Blend& b = it->second;
        if (oneOf(attr, {"en", "envelope"}))
        {
            for (NodeId m : b.morphs)
                out.push_back({m, scene::DeformerNode::kEnvelope, -1});
            return out;
        }
        int w = -1;
        if (std::vector<AttrSeg> s = splitAttr(attr); s.size() == 1 && oneOf(s[0].name, {"w", "weight"}) && s[0].lo >= 0 && s[0].lo == s[0].hi)
            w = s[0].lo;
        else if (auto a = b.aliases.find(attr); a != b.aliases.end())
            w = a->second;
        if (auto t = b.targets.find(w); t != b.targets.end())
            for (NodeId id : t->second)
                out.push_back({id, scene::morphs::kWeight, -1});
        return out;
    }

    /**
     * A Maya plug as the Lila plug carrying the same value, and the factor from the Lila value to the value Maya's plug
     * hands on: transform channels (in the file's units, or internal ones behind a unitConversion) and converted
     * blendShape weights.
     */
    std::optional<std::pair<PlugRef, double>> lilaDriver(const Plug& src, bool internal = false, int depth = 0) const
    {
        if (src.node < 0 || depth > 8)
            return std::nullopt;
        const MNode& n = mIx.node(src.node);
        if (n.type == "unitConversion")
        {
            auto in = mIx.input(src.node, {"i", "input"});
            auto r = in ? lilaDriver(*in, true, depth + 1) : std::nullopt;
            if (r)
                r->second *= num(n, {"cf", "conversionFactor"}, 1.0);
            return r;
        }
        if (std::vector<PlugRef> b = blendPlugs(src.node, src.attr); !b.empty())
            return std::pair{b.front(), 1.0};
        auto target = channelTarget(src);
        if (!target)
            return std::nullopt;
        static constexpr std::string_view kNames[3][3][2] = {
            {{"tx", "translateX"}, {"ty", "translateY"}, {"tz", "translateZ"}},
            {{"rx", "rotateX"}, {"ry", "rotateY"}, {"rz", "rotateZ"}},
            {{"sx", "scaleX"}, {"sy", "scaleY"}, {"sz", "scaleZ"}},
        };
        static const int kAttr[3] = {scene::Transform::kTranslate, scene::Transform::kRotate, scene::Transform::kScale};
        for (int g = 0; g < 3; ++g)
            for (int a = 0; a < 3; ++a)
                if (target->second == kNames[g][a][0] || target->second == kNames[g][a][1])
                {
                    // Lila holds translate in cm and rotate in degrees, mirrored where the axis change flips a channel.
                    const double sign = g == 0 ? mAxes.sign[a] : g == 1 ? mAxes.sign[a] * mAxes.det : 1.0;
                    const double unit = g == 0 ? (internal ? 1.0 : 1.0 / mU.lin) : g == 1 ? (internal ? math::kPi / 180.0 : 1.0 / mU.ang) : 1.0;
                    return std::pair{PlugRef{target->first, kAttr[g], mAxes.to[a]}, sign * unit};
                }
        return std::nullopt;
    }

    /** A Maya driven-key curve (keys over its input's value) as keys over the Lila driver's value. */
    static scene::anim::Curve overDriver(scene::anim::Curve c, double fps, double factor)
    {
        for (scene::anim::Key& k : c.keys)
        {
            k.time = k.time * fps / factor;
            for (double* a : {&k.inAngle, &k.outAngle})
                *a = std::atan(std::tan(*a * math::kPi / 180.0) * factor) * 180.0 / math::kPi;
            if (factor < 0.0)
            {
                std::swap(k.in, k.out);
                std::swap(k.inAngle, k.outAngle);
            }
        }
        if (factor < 0.0)
            std::swap(c.pre, c.post);
        c.sort();
        return c;
    }

    /** Animation curves, set driven keys and direct connections into converted blendShape weights and envelopes. */
    void connectMorphInputs()
    {
        using namespace scene;
        bool approx = false;
        std::map<std::string, int> skipped;
        for (const auto& entry : mBlends)
            for (const int bs = entry.first; const auto& [src, dst] : mIx.inputs(bs))
            {
                std::vector<PlugRef> plugs = blendPlugs(bs, dst.attr);
                if (plugs.empty())
                    continue;
                const MNode& sn = mIx.node(src.node);
                const std::string where = std::format("{}.{}", shortName(mIx.node(bs).name), dst.attr);
                const bool timeCurve = sn.type.starts_with("animCurveT");
                if (timeCurve || sn.type.starts_with("animCurveU"))
                {
                    mHandled.insert(src.node);
                    if (timeCurve && !mOpts.animation)
                        continue;
                    std::optional<anim::Curve> curve = readCurve(sn, 1.0, 1.0, 1.0, &approx);
                    if (!curve)
                        continue;
                    if (timeCurve)
                    {
                        NodeId id = mG.createNode(*types::find("animCurveTU"), lilaName(sn.name, mOpts.nameSpace, "animCurveTU"), kNoNode, false)->id();
                        anim::setCurve(mG, id, *curve);
                        for (const PlugRef& p : plugs)
                            mG.connect({id, anim::CurveNode::kOutput, -1}, p);
                        ++mR.curves;
                        continue;
                    }
                    auto in = mIx.input(src.node, {"i", "input"});
                    auto driver = in ? lilaDriver(*in) : std::nullopt;
                    if (!driver || !types::find("drivenChannel"))
                    {
                        std::string why;
                        if (mRig && mRig->drive(src, plugs, &why))
                            continue;
                        ++skipped[std::format("set driven key on {} (its driver {} is not converted: {})", where,
                                              in ? std::format("{}.{}", shortName(mIx.node(in->node).name), in->attr) : std::string("is missing"), why)];
                        continue;
                    }
                    if (std::abs(driver->second) < 1e-12)
                        continue;
                    NodeId ch = drivers::createChannel(mG, driver->first, plugs.front(), overDriver(*curve, mU.fps, driver->second), "",
                                                       lilaName(sn.name, mOpts.nameSpace, "drivenChannel"));
                    for (size_t k = 1; k < plugs.size(); ++k)
                        mG.connect({ch, drivers::kChannelOutput, -1}, plugs[k]);
                    continue;
                }
                if (auto driver = lilaDriver(src); driver && types::find("drivenChannel"))
                {
                    if (sn.type == "unitConversion")
                        mHandled.insert(src.node);
                    // A plain connection: a linear channel scaling the driver into Maya's value.
                    anim::Curve line;
                    line.keys = {{.time = 0.0, .value = 0.0, .in = anim::Tangent::Linear, .out = anim::Tangent::Linear},
                                 {.time = 1.0, .value = driver->second, .in = anim::Tangent::Linear, .out = anim::Tangent::Linear}};
                    line.pre = line.post = anim::Infinity::Linear;
                    NodeId ch = drivers::createChannel(mG, driver->first, plugs.front(), line, "", "");
                    for (size_t k = 1; k < plugs.size(); ++k)
                        mG.connect({ch, drivers::kChannelOutput, -1}, plugs[k]);
                    continue;
                }
                std::string why;
                if (mRig && mRig->drive(src, plugs, &why))
                    continue;
                if (sn.type == "expression")
                    ++skipped[std::format("an expression driving {} (MEL expressions are not converted)", where)];
                else
                    ++skipped[std::format("{}.{} driving {} ({})", shortName(sn.name), src.attr, where, why)];
            }
        if (approx)
            mR.warnings.push_back("blendShape weight curves: slow, fast, plateau, step next and smooth tangents were approximated");
        for (const auto& [what, count] : skipped)
            mR.skipped.push_back(count > 1 ? std::format("{} ({} connections)", what, count) : what);
    }

    void connectInverseScales()
    {
        for (const FileIndex::Conn& c : mIx.conns)
        {
            if (!oneOf(c.dst.attr, {"is", "inverseScale"}) || !oneOf(c.src.attr, {"s", "scale"}))
                continue;
            auto d = mLila.find(c.dst.node), s = mLila.find(c.src.node);
            if (d == mLila.end() || s == mLila.end() || !scene::rig::isJoint(*mG.node(d->second)))
                continue;
            mG.connect({s->second, scene::Transform::kScale, -1}, {d->second, scene::Joint::kInverseScale, -1});
        }
    }

    // ---- animation ------------------------------------------------------------------------------------------

    static scene::anim::Tangent tangentOf(int maya, bool* approx)
    {
        using scene::anim::Tangent;
        switch (maya)
        {
        case 1: return Tangent::Fixed;
        case 2: return Tangent::Linear;
        case 3: return Tangent::Flat;
        case 5: return Tangent::Step;
        case 9: return Tangent::Spline;
        case 10: return Tangent::Clamped;
        case 17: *approx = true; return Tangent::Step;
        case 4:
        case 6:
        case 7: *approx = true; return Tangent::Spline;
        case 16: *approx = true; return Tangent::Auto;
        default: return Tangent::Auto;
        }
    }

    static scene::anim::Infinity infinityOf(double v)
    {
        using scene::anim::Infinity;
        switch (int(v))
        {
        case 1: return Infinity::Linear;
        case 3: return Infinity::Cycle;
        case 4: return Infinity::CycleRelative;
        case 5: return Infinity::Oscillate;
        default: return Infinity::Constant;
        }
    }

    /**
     * Maya keeps a group's switch points as percentageThreshold (screen height percent, level i+1 at element i) when
     * useScreenHeightPercentage is on, and as threshold distances otherwise. Percentages become screen sizes, as FBX's
     * percentage thresholds do; distances have no screen size, so the group keeps Unreal's automatic sizes.
     */
    void lodSizes(const MNode& mn, NodeId id)
    {
        const std::map<int, double> percent = elements(mn, {"pth", "percentageThreshold"});
        if (num(mn, {"ush", "useScreenHeightPercentage"}, 0.0) == 0.0 || percent.empty())
        {
            if (!elements(mn, {"th", "threshold"}).empty())
                mR.warnings.push_back(std::format("{}: LOD distances kept as automatic screen sizes", mG.path(id)));
            return;
        }
        std::vector<double> sizes = {1.0};
        for (const auto& [k, v] : percent)
            if (k == int(sizes.size()) - 1)
                sizes.push_back(v / 100.0);
        mG.setValue({id, scene::LodGroup::kScreenSizes, -1}, scene::lod::formatSizes(sizes));
    }

    /** Values of a ranged multi such as kit[0:9], by element index. */
    static std::map<int, double> elements(const MNode& n, Names names)
    {
        std::map<int, double> out;
        for (const AttrValue& a : n.attrs)
        {
            std::vector<AttrSeg> s = splitAttr(a.attr);
            if (s.size() != 1 || !oneOf(s[0].name, names) || s[0].lo < 0)
                continue;
            for (size_t k = 0; k < a.nums.size(); ++k)
                out[s[0].lo + int(k)] = a.nums[k];
        }
        return out;
    }

    /** A Maya time curve's keys, tangents and infinities, values times unit and sign. Empty without keys. */
    std::optional<scene::anim::Curve> readCurve(const MNode& cn, double unit, double sign, double tangentUnit, bool* approx)
    {
        using namespace scene;
        anim::Curve curve;
        for (const AttrValue& a : cn.attrs)
        {
            std::vector<AttrSeg> s = splitAttr(a.attr);
            if (s.size() != 1 || !oneOf(s[0].name, {"ktv", "keyTimeValue"}))
                continue;
            for (size_t k = 0; k + 1 < a.nums.size(); k += 2)
                curve.keys.push_back({.time = a.nums[k] / mU.fps, .value = a.nums[k + 1] * unit * sign});
        }
        if (curve.keys.empty())
            return std::nullopt;
        const int def = int(num(cn, {"tan", "tangentType"}, 18.0));
        std::map<int, double> kit = elements(cn, {"kit", "keyTanInType"}), kot = elements(cn, {"kot", "keyTanOutType"});
        std::map<int, double> kix = elements(cn, {"kix", "keyTanInX"}), kiy = elements(cn, {"kiy", "keyTanInY"});
        std::map<int, double> kox = elements(cn, {"kox", "keyTanOutX"}), koy = elements(cn, {"koy", "keyTanOutY"});
        auto angle = [&](std::map<int, double>& xs, std::map<int, double>& ys, int k) {
            double x = xs.contains(k) ? xs[k] : 1.0, y = ys.contains(k) ? ys[k] : 0.0;
            return std::atan2(y * tangentUnit * sign, x) * 180.0 / math::kPi;
        };
        for (size_t k = 0; k < curve.keys.size(); ++k)
        {
            anim::Key& key = curve.keys[k];
            const int ki = int(k);
            key.in = tangentOf(kit.contains(ki) ? int(kit[ki]) : def, approx);
            key.out = tangentOf(kot.contains(ki) ? int(kot[ki]) : def, approx);
            if (key.in == anim::Tangent::Fixed)
                key.inAngle = angle(kix, kiy, ki);
            if (key.out == anim::Tangent::Fixed)
                key.outAngle = angle(kox, koy, ki);
        }
        curve.sort();
        curve.pre = infinityOf(num(cn, {"pre", "preInfinity"}, 0.0));
        curve.post = infinityOf(num(cn, {"pst", "postInfinity"}, 0.0));
        if (num(cn, {"wgt", "weightedTangents"}, 0.0) != 0.0)
            mR.warnings.push_back(std::format("{}: weighted tangents imported unweighted", shortName(cn.name)));
        return curve;
    }

    /** The Lila camera attr a keyed Maya camera attribute lands on, or -1. */
    static int cameraChannel(const std::string& attr)
    {
        using scene::Camera;
        if (oneOf(attr, {"fl", "focalLength"}))
            return Camera::kFocalLength;
        if (oneOf(attr, {"hfa", "horizontalFilmAperture"}))
            return Camera::kHorizontalFilmAperture;
        if (oneOf(attr, {"vfa", "verticalFilmAperture"}))
            return Camera::kVerticalFilmAperture;
        if (oneOf(attr, {"ncp", "nearClipPlane"}))
            return Camera::kNearClipPlane;
        if (oneOf(attr, {"fcp", "farClipPlane"}))
            return Camera::kFarClipPlane;
        if (oneOf(attr, {"ow", "orthographicWidth"}))
            return Camera::kOrthographicWidth;
        return -1;
    }

    void buildAnimation()
    {
        LILA_ZONE();

        using namespace scene;
        bool approx = false;
        std::map<std::string, int> skippedTargets;
        for (const FileIndex::Conn& c : mIx.conns)
        {
            const MNode& cn = mIx.node(c.src.node);
            if (!cn.type.starts_with("animCurve"))
                continue;
            mHandled.insert(c.src.node);
            if (!oneOf(c.src.attr, {"o", "output"}) || mBlends.contains(c.dst.node))
                continue;
            const char kind = cn.type.size() == 11 ? cn.type[10] : '?';
            // Set driven keys, and time curves on anything but transform channels, are the rig pass's.
            if (cn.type.size() != 11 || cn.type[9] != 'T' || (kind != 'L' && kind != 'A' && kind != 'U'))
            {
                if (!cn.type.starts_with("animCurveU"))
                    ++skippedTargets[cn.type];
                continue;
            }
            const double tangentUnit = kind == 'L' ? 1.0 : kind == 'A' ? 180.0 / math::kPi : 1.0; // tangent y is in internal units
            static constexpr const char* kCurveType[3] = {"animCurveTL", "animCurveTA", "animCurveTU"};
            if (auto cam = mCameraNode.find(c.dst.node); cam != mCameraNode.end())
            {
                const int attr = cameraChannel(c.dst.attr);
                const PlugRef plug{cam->second, attr, -1};
                if (attr < 0 || mG.source(plug))
                {
                    ++skippedTargets[std::format("{}.{}", mG.node(cam->second)->path(), c.dst.attr)];
                    continue;
                }
                // Clipping distances and the orthographic width are lengths; the lens and film back are not.
                const bool length = attr == Camera::kNearClipPlane || attr == Camera::kFarClipPlane || attr == Camera::kOrthographicWidth;
                std::optional<anim::Curve> curve = readCurve(cn, length ? mU.lin : 1.0, 1.0, tangentUnit, &approx);
                if (!curve)
                    continue;
                const char* type = kCurveType[length ? 0 : 2];
                NodeId curveId = mG.createNode(*types::find(type), lilaName(cn.name, mOpts.nameSpace, type), kNoNode, false)->id();
                anim::setCurve(mG, curveId, *curve);
                mG.connect({curveId, anim::CurveNode::kOutput, -1}, plug);
                ++mR.curves;
                continue;
            }
            std::optional<std::pair<NodeId, std::string>> target = channelTarget(c.dst);
            static constexpr std::string_view kTargets[3][3][2] = {
                {{"tx", "translateX"}, {"ty", "translateY"}, {"tz", "translateZ"}},
                {{"rx", "rotateX"}, {"ry", "rotateY"}, {"rz", "rotateZ"}},
                {{"sx", "scaleX"}, {"sy", "scaleY"}, {"sz", "scaleZ"}},
            };
            int group = -1, axis = -1;
            for (int gi = 0; gi < 3; ++gi)
                for (int ai = 0; ai < 3; ++ai)
                    if (target && (target->second == kTargets[gi][ai][0] || target->second == kTargets[gi][ai][1]))
                        group = gi, axis = ai;
            if (!target || group < 0)
            {
                if (mLila.contains(c.dst.node) || RigBuilder::convertsInputsOf(mIx.node(c.dst.node).type))
                    continue;
                ++skippedTargets[target ? std::format("{}.{}", mG.node(target->first)->path(), target->second)
                                        : std::format("{}.{}", shortName(mIx.node(c.dst.node).name), c.dst.attr)];
                continue;
            }
            if (group == 1 && mRotateAxisFolded.contains(c.dst.node))
                mR.warnings.push_back(std::format("{}: rotate is animated and rotateAxis was folded in; the curve keeps Maya's values",
                                                  mG.node(target->first)->path()));

            // Maya channel `axis` lands on Lila channel to[axis], negated where the swap mirrors it.
            const int lilaAxis = mAxes.to[axis];
            const double sign = group == 0 ? mAxes.sign[axis] : group == 1 ? mAxes.sign[axis] * mAxes.det : 1.0;
            const double unit = kind == 'L' ? mU.lin : kind == 'A' ? mU.ang : 1.0;
            static const int kAttr[3] = {Transform::kTranslate, Transform::kRotate, Transform::kScale};
            const PlugRef plug{target->first, kAttr[group], lilaAxis};
            if (mG.source(plug))
                continue;

            std::optional<anim::Curve> curve = readCurve(cn, unit, sign, tangentUnit, &approx);
            if (!curve)
                continue;
            NodeId curveId = mG.createNode(*types::find(kCurveType[group]), lilaName(cn.name, mOpts.nameSpace, kCurveType[group]), kNoNode, false)->id();
            anim::setCurve(mG, curveId, *curve);
            mG.connect({curveId, anim::CurveNode::kOutput, -1}, plug);
            ++mR.curves;
        }
        if (approx)
            mR.warnings.push_back("slow, fast, plateau, step next and smooth tangents were approximated with Lila's tangent types");
        for (const auto& [what, count] : skippedTargets)
            mR.skipped.push_back(std::format("animation on {} ({} curve(s))", what, count));
    }

    void setPlayback()
    {
        using namespace scene;
        int script = mIx.resolve("sceneConfigurationScriptNode");
        bool any = false;
        anim::Playback& pb = anim::playback();
        if (script >= 0)
        {
            mHandled.insert(script);
            std::string b = text(mIx.node(script), {"b", "before"});
            auto flag = [&](std::string_view f, double* out) {
                size_t at = b.find(std::string(f) + " ");
                if (at == std::string::npos)
                    return;
                *out = std::strtod(b.c_str() + at + f.size() + 1, nullptr) / mU.fps;
                any = true;
            };
            double mn = pb.minTime, mx = pb.maxTime, as = pb.animStart, ae = pb.animEnd;
            flag("-min", &mn);
            flag("-max", &mx);
            flag("-ast", &as);
            flag("-aet", &ae);
            if (any)
            {
                pb.minTime = mn;
                pb.maxTime = mx;
                pb.animStart = std::min(as, mn);
                pb.animEnd = std::max(ae, mx);
            }
        }
        if (any || mR.curves > 0)
        {
            pb.fps = mU.fps;
            anim::playbackChanged();
        }
    }

    // ---- references -----------------------------------------------------------------------------------------

    struct RefInfo
    {
        const Reference* ref = nullptr;
        fs::path path;
        std::string mayaNs;
        /** Namespace of its nodes: in the graph when live, in the .lila being written when nested. */
        std::string lilaNs;
        /** Maya DAG path inside the referenced file to its Lila name there. */
        std::map<std::string, std::string> names;
        bool identityNames = false; // FBX and OBJ keep their own names
        bool live = false;
        fs::path lila;
        json nestedDoc;                    // nested: the referenced .lila, for values the edits don't set
        json nestedEdits = json::object(); // nested: node -> attrs
        /**
         * FBX: Maya keeps the file's local transforms below its roots and turns only the roots onto the scene axes,
         * while Lila conjugates every local transform by the file-to-Lila rotation, so edits below the roots convert
         * through that rotation instead of the scene's.
         */
        std::optional<glm::dmat3> fbxToLila;
        /** FBX: the turn Lila's roots of the file lack against Maya's import of it (scene to Lila after Maya's axis turn). */
        glm::dmat3 fbxRootTurn{1.0};
    };

    fs::path resolveRefPath(const std::string& stored) const
    {
        fs::path p = fs::path(std::u8string(stored.begin(), stored.end()));
        std::error_code ec;
        if (p.is_relative())
            p = mPath.parent_path() / p;
        if (fs::exists(p, ec))
            return p;
        // Maya falls back to the file name next to the scene when the stored path is gone.
        fs::path local = mPath.parent_path() / p.filename();
        return fs::exists(local, ec) ? local : fs::path();
    }

    static std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    static std::string utf8Of(const fs::path& p)
    {
        std::u8string s = p.generic_u8string();
        return std::string(s.begin(), s.end());
    }

    /** span: the share of the caller's progress the references stand for. */
    void importReferences(double span)
    {
        LILA_ZONE();

        progress::Scope all("References", double(mFile.references.size()), progress::None, span);
        for (const Reference& ref : mFile.references)
        {
            progress::Scope one("Reference " + fs::path(ref.path).filename().string());
            fs::path p = resolveRefPath(ref.path);
            if (p.empty())
            {
                mR.warnings.push_back(std::format("reference '{}' not found", ref.path));
                continue;
            }
            RefInfo info;
            info.ref = &ref;
            info.path = p;
            info.mayaNs = ref.ns.empty() ? p.stem().string() : ref.ns;
            const std::string ext = lower(p.extension().string());
            if (ext != ".ma" && ext != ".mb" && ext != ".fbx" && ext != ".obj")
            {
                mR.warnings.push_back(std::format("reference '{}': {} files are not supported", ref.path, ref.type.empty() ? ext : ref.type));
                continue;
            }
            info.identityNames = ext == ".fbx" || ext == ".obj";
            if (ext == ".fbx")
                if (std::optional<fbx::ImportAxes> axes = fbx::importAxes(p))
                {
                    info.fbxToLila = axes->toLila;
                    info.fbxRootTurn = mC3 * mayaFbxTurn(*axes) * glm::transpose(axes->toLila);
                }
            if (mDepth + 1 >= kMaxRefDepth)
            {
                mR.warnings.push_back(std::format("reference '{}' nested too deep, skipped", ref.path));
                continue;
            }
            if (!ref.loaded && mOpts.references == "import")
            {
                mR.skipped.push_back(std::format("reference '{}' (unloaded in Maya)", ref.path));
                continue;
            }
            const bool ok = mOpts.references == "import" ? importRefNodes(info) : referenceFile(info);
            if (!ok)
                continue;
            applyRefEdits(info);
            if (mNested && !info.live && !info.lila.empty())
            {
                json edits = json::array();
                for (auto& [node, attrs] : info.nestedEdits.items())
                    edits.push_back({{"node", node}, {"attrs", attrs}});
                mNested->refs.push_back({{"namespace", info.lilaNs},
                                         {"path", paths::storedPath(info.lila, mNested->outDir)},
                                         {"loaded", ref.loaded},
                                         {"edits", edits}});
            }
            mRefs.push_back(std::move(info));
        }
    }

    /** references="import": the file's nodes as ordinary scene nodes in the namespace. */
    bool importRefNodes(RefInfo& info)
    {
        const std::string ext = lower(info.path.extension().string());
        const std::string ns = lilaName(info.mayaNs, mOpts.nameSpace, "ref");
        info.lilaNs = ns;
        info.live = true;
        if (ext == ".ma" || ext == ".mb")
        {
            ImportOptions o = mOpts;
            o.nameSpace = ns;
            o.setPlayback = false;
            ImportResult sub = importAt(mG, info.path, o, mDepth + 1, mNested);
            if (!sub.ok)
            {
                mR.warnings.push_back(std::format("reference '{}': {}", info.ref->path, sub.error));
                return false;
            }
            for (const auto& [path, name] : sub.mayaNames)
                info.names[path] = stripNsPath(ns, name);
            merge(sub, info.ref->path);
            return true;
        }
        std::vector<NodeId> ids = importForeign(mG, info.path, mR.warnings);
        for (NodeId id : ids)
            mG.rename(id, ns + ":" + mG.node(id)->name());
        mR.nodes.insert(mR.nodes.end(), ids.begin(), ids.end());
        for (NodeId id : ids)
            if (mG.parent(id) == scene::kNoNode)
                mR.roots.push_back(id);
        return !ids.empty();
    }

    /** FBX or OBJ nodes into a graph, names as in the file. */
    static std::vector<NodeId> importForeign(Graph& g, const fs::path& p, std::vector<std::string>& warnings)
    {
        if (lower(p.extension().string()) == ".fbx")
        {
            fbx::ImportOptions o;
            o.setPlayback = false;
            fbx::ImportResult sub = fbx::importFbx(g, p, o);
            if (!sub.ok)
                warnings.push_back(std::format("{}: {}", p.filename().string(), sub.error));
            for (const std::string& w : sub.warnings)
                warnings.push_back(std::format("{}: {}", p.filename().string(), w));
            return sub.nodes;
        }
        std::vector<NodeId> out;
        std::u8string u8 = p.u8string();
        geo::ObjReadResult obj = geo::readObjFile(std::string(u8.begin(), u8.end()));
        if (!obj.ok())
            warnings.push_back(std::format("{}: {}", p.filename().string(), obj.error));
        for (geo::ObjObject& o : obj.objects)
        {
            scene::Node* n = g.createNode(*scene::types::find("mesh"), lilaName(o.name, "", "mesh"), scene::kNoNode, false);
            g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(o.mesh))));
            out.push_back(n->id());
        }
        return out;
    }

    struct UndoOff
    {
        bool was = UndoStack::get().enabled();
        UndoOff() { UndoStack::get().setEnabled(false); }
        ~UndoOff() { UndoStack::get().setEnabled(was); }
    };

    /** references="reference": the file converted to a .lila beside it and referenced under its namespace. */
    bool referenceFile(RefInfo& info)
    {
        std::string error;
        info.lila = ensureLila(info.path, info.names, &info.identityNames, &error);
        if (info.lila.empty())
        {
            mR.warnings.push_back(std::format("reference '{}': {}", info.ref->path, error));
            return false;
        }
        if (mNested)
        {
            // Converting a file for its own .lila: its references become that document's reference records.
            info.lilaNs = lilaName(info.mayaNs, "", "ref");
            std::vector<std::string> warnings;
            try
            {
                info.nestedDoc = io::readSceneFile(info.lila, &warnings);
            }
            catch (const std::exception& e)
            {
                mR.warnings.push_back(std::format("reference '{}': {}", info.ref->path, e.what()));
                return false;
            }
            return true;
        }
        std::vector<std::string> warnings;
        try
        {
            UndoOff off;
            const std::string ns = mOpts.nameSpace.empty() ? info.mayaNs : mOpts.nameSpace + "_" + info.mayaNs;
            io::Reference& r = io::refs::create(info.lila, ns, warnings);
            info.lilaNs = r.ns;
            info.live = true;
            mR.references.push_back(r.ns);
            for (NodeId id : mG.nodes())
                if (mG.node(id)->referenceId() == r.id)
                {
                    mR.nodes.push_back(id);
                    if (mG.parent(id) == scene::kNoNode && mG.node(id)->type().dag)
                        mR.roots.push_back(id);
                    if (scene::rig::isJoint(*mG.node(id)))
                        mR.joints.push_back(id);
                }
        }
        catch (const std::exception& e)
        {
            mR.warnings.push_back(std::format("reference '{}': {}", info.ref->path, e.what()));
            return false;
        }
        for (const std::string& w : warnings)
            mR.warnings.push_back(std::format("reference '{}': {}", info.ref->path, w));
        return true;
    }

    /** Unloads the live references Maya saved unloaded, their edits kept for when they load. */
    void unloadDeferredRefs()
    {
        bool any = false;
        for (const RefInfo& info : mRefs)
            if (info.live && !info.ref->loaded && mOpts.references == "reference")
                if (io::Reference* r = io::refs::find(info.lilaNs))
                {
                    UndoOff off;
                    io::refs::unload(*r);
                    any = true;
                }
        if (!any)
            return;
        auto gone = [&](NodeId id) { return mG.node(id) == nullptr; };
        for (std::vector<NodeId>* ids : {&mR.nodes, &mR.roots, &mR.joints, &mR.skins, &mR.morphs, &mR.cameras, &mR.lights, &mR.imagePlanes})
            std::erase_if(*ids, gone);
    }

    /** The source a conversion records, relative to the .lila's folder when it can be. */
    static std::string sourcePath(const fs::path& src, const fs::path& dir, const std::string& srcKey)
    {
        std::error_code ec;
        const fs::path rel = paths::relativeTo(fs::weakly_canonical(src, ec), fs::weakly_canonical(dir, ec));
        return rel.empty() ? srcKey : utf8Of(rel);
    }

    static bool sameSource(const std::string& stored, const fs::path& dir, const std::string& srcKey)
    {
        if (stored.empty())
            return false;
        fs::path p = paths::fromUtf8(stored);
        if (p.is_relative())
            p = dir / p;
        std::error_code ec;
        return utf8Of(fs::weakly_canonical(p, ec)) == srcKey;
    }

    /**
     * The .lila standing in for a referenced file: <stem>.lila beside it, made by this converter (its "mayaSource"
     * provenance names the file, its size and time). A current one is reused and a stale one of ours rewritten. A
     * .lila the converter didn't make is never overwritten: newer than the source it is used as it is, otherwise the
     * conversion goes to <stem>_<ext>.lila, then <stem>.maya.lila.
     */
    fs::path ensureLila(const fs::path& src, std::map<std::string, std::string>& names, bool* identity, std::string* error)
    {
        std::error_code ec;
        const std::string srcKey = utf8Of(fs::weakly_canonical(src, ec));
        const auto size = int64_t(fs::file_size(src, ec));
        const auto mtime = int64_t(fs::last_write_time(src, ec).time_since_epoch().count());
        const std::string stem = src.stem().string();
        const std::string ext = lower(src.extension().string()).substr(1);
        for (const fs::path& target : {src.parent_path() / (stem + ".lila"), src.parent_path() / (stem + "_" + ext + ".lila"),
                                       src.parent_path() / (stem + ".maya.lila")})
        {
            if (!fs::exists(target, ec))
                return convertTo(src, target, srcKey, size, mtime, names, error);
            json doc = json::object();
            try
            {
                doc = io::readSceneHeader(target);
            }
            catch (const std::exception&)
            {
            }
            const json prov = doc.value("mayaSource", json());
            if (prov.is_object())
            {
                if (!sameSource(prov.value("path", ""), target.parent_path(), srcKey))
                    continue; // another source's conversion
                // Its values are in the axes of the scene that converted it.
                const std::string up = math::upAxis() == math::UpAxis::Z ? "z" : "y";
                if (prov.value("size", int64_t(-1)) == size && prov.value("mtime", int64_t(-1)) == mtime &&
                    prov.value("converter", 0) == kConverterVersion && doc.value("upAxis", "y") == up)
                {
                    const json stored = doc.value("mayaNames", json::object()); // items() must not iterate a temporary
                    for (auto& [k, v] : stored.items())
                        if (v.is_string())
                            names[k] = v.get<std::string>();
                    return target;
                }
                return convertTo(src, target, srcKey, size, mtime, names, error);
            }
            if (fs::last_write_time(target, ec) > fs::last_write_time(src, ec))
            {
                mR.warnings.push_back(std::format("{}: using the existing {} (not made by the converter, newer than the Maya file)",
                                                  src.filename().string(), target.filename().string()));
                *identity = true;
                return target;
            }
        }
        *error = "every .lila name beside it is taken by another file";
        return {};
    }

    fs::path convertTo(const fs::path& src, const fs::path& target, const std::string& srcKey, int64_t size, int64_t mtime,
                       std::map<std::string, std::string>& names, std::string* error)
    {
        LILA_ZONE();

        progress::Scope scope("Converting reference " + src.filename().string());
        Graph tmp;
        UndoOff off;
        Nested nested;
        nested.outDir = target.parent_path();
        const std::string ext = lower(src.extension().string());
        if (ext == ".ma" || ext == ".mb")
        {
            ImportOptions o = mOpts;
            o.nameSpace.clear();
            o.parent = scene::kNoNode;
            o.setPlayback = false;
            ImportResult sub = importAt(tmp, src, o, mDepth + 1, &nested);
            if (!sub.ok)
            {
                *error = sub.error;
                return {};
            }
            names = sub.mayaNames;
            for (const std::string& s : sub.skipped)
                mR.skipped.push_back(std::format("{}: {}", src.filename().string(), s));
            for (const std::string& w : sub.warnings)
                mR.warnings.push_back(std::format("{}: {}", src.filename().string(), w));
            mR.converted.insert(mR.converted.end(), sub.converted.begin(), sub.converted.end());
        }
        else
        {
            for (NodeId id : importForeign(tmp, src, mR.warnings))
                names["|" + tmp.node(id)->name()] = relName(tmp, id, "");
        }
        json doc = documentOf(tmp, nested.refs, nested.outDir);
        doc["mayaSource"] = {{"path", sourcePath(src, target.parent_path(), srcKey)}, {"size", size}, {"mtime", mtime}, {"converter", kConverterVersion}};
        json jn = json::object();
        for (const auto& [k, v] : names)
            jn[k] = v;
        doc["mayaNames"] = std::move(jn);
        try
        {
            std::string bytes = io::encodeBinary(std::move(doc));
            fs::path part = target;
            part += ".part";
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(bytes.data(), std::streamsize(bytes.size())) || !out.flush())
                    throw std::runtime_error("can't write " + utf8Of(part));
            }
            std::error_code ec;
            fs::rename(part, target, ec);
            if (ec)
                throw std::runtime_error("can't replace " + utf8Of(target) + ": " + ec.message());
        }
        catch (const std::exception& e)
        {
            *error = e.what();
            return {};
        }
        mR.converted.push_back(utf8Of(target));
        return target;
    }

    /** A scene document of a graph, the way io::serialize writes the current one; dir is where it will live. */
    static json documentOf(Graph& g, const std::vector<json>& refs, const fs::path& dir)
    {
        std::vector<NodeId> order;
        std::function<void(NodeId)> dag = [&](NodeId id) {
            order.push_back(id);
            for (NodeId c : g.children(id))
                dag(c);
        };
        for (NodeId r : g.roots())
            dag(r);
        for (NodeId id : g.nodes())
            if (!g.node(id)->type().dag)
                order.push_back(id);
        // Values first, all at once, so meshes convert in parallel.
        std::vector<const scene::PlugValue*> values;
        std::vector<std::vector<int>> stored(order.size());
        for (size_t k = 0; k < order.size(); ++k)
        {
            scene::Node* n = g.node(order[k]);
            const auto& defs = n->type().attrs;
            for (int i = 0; i < int(defs.size()); ++i)
            {
                const scene::AttrDef& a = defs[size_t(i)];
                if (!(a.flags & scene::AttrStorable) || !(a.flags & scene::AttrInput) || g.source({order[k], i, -1}))
                    continue;
                if (n->value(i) != a.storedDefault())
                {
                    stored[k].push_back(i);
                    values.push_back(&n->value(i));
                }
            }
        }
        std::vector<json> converted = scene::toJsonAll(values);
        size_t next = 0;
        json nodes = json::array();
        for (size_t k = 0; k < order.size(); ++k)
        {
            const NodeId id = order[k];
            scene::Node* n = g.node(id);
            json attrs = json::object();
            for (int i : stored[k])
            {
                const scene::AttrDef& a = n->type().attrs[size_t(i)];
                json& v = attrs[a.name] = std::move(converted[next++]);
                if ((a.flags & scene::AttrFile) && v.is_string())
                    v = paths::storedPath(paths::fromUtf8(v.get<std::string>()), dir);
            }
            json node = {{"name", n->name()}, {"type", n->type().name}};
            if (NodeId p = g.parent(id); p != scene::kNoNode)
                node["parent"] = g.fullPath(p);
            if (!attrs.empty())
                node["attrs"] = std::move(attrs);
            nodes.push_back(std::move(node));
        }
        json connections = json::array();
        for (auto& [dst, src] : g.connections())
            connections.push_back({g.fullPlugName(src), g.fullPlugName(dst)});
        return {{"lila", 1},
                {"upAxis", math::upAxis() == math::UpAxis::Z ? "z" : "y"},
                {"units", "cm"},
                {"time", 0.0},
                {"nodes", std::move(nodes)},
                {"connections", std::move(connections)},
                {"selection", json::array()},
                {"references", json(refs)}};
    }

    void merge(const ImportResult& sub, const std::string& what)
    {
        mR.nodes.insert(mR.nodes.end(), sub.nodes.begin(), sub.nodes.end());
        mR.roots.insert(mR.roots.end(), sub.roots.begin(), sub.roots.end());
        mR.joints.insert(mR.joints.end(), sub.joints.begin(), sub.joints.end());
        mR.skins.insert(mR.skins.end(), sub.skins.begin(), sub.skins.end());
        mR.morphs.insert(mR.morphs.end(), sub.morphs.begin(), sub.morphs.end());
        mR.materials.insert(mR.materials.end(), sub.materials.begin(), sub.materials.end());
        mR.displayLayers.insert(mR.displayLayers.end(), sub.displayLayers.begin(), sub.displayLayers.end());
        mR.references.insert(mR.references.end(), sub.references.begin(), sub.references.end());
        mR.converted.insert(mR.converted.end(), sub.converted.begin(), sub.converted.end());
        mR.curves += sub.curves;
        for (const std::string& s : sub.skipped)
            mR.skipped.push_back(std::format("{}: {}", what, s));
        for (const std::string& w : sub.warnings)
            mR.warnings.push_back(std::format("{}: {}", what, w));
    }

    // ---- display layers -------------------------------------------------------------------------------------

    /** Maya's default index colors (display values), index 1 to 31; 0 is no color. */
    static glm::dvec3 indexColor(int i)
    {
        static const double k[31][3] = {
            {0.0, 0.0, 0.0},       {0.247, 0.247, 0.247}, {0.498, 0.498, 0.498}, {0.608, 0.0, 0.157},   {0.0, 0.016, 0.373},
            {0.0, 0.0, 1.0},       {0.0, 0.275, 0.094},   {0.145, 0.0, 0.263},   {0.78, 0.0, 0.78},     {0.537, 0.278, 0.2},
            {0.243, 0.133, 0.122}, {0.6, 0.145, 0.0},     {1.0, 0.0, 0.0},       {0.0, 1.0, 0.0},       {0.0, 0.255, 0.6},
            {1.0, 1.0, 1.0},       {1.0, 1.0, 0.0},       {0.388, 0.863, 1.0},   {0.263, 1.0, 0.635},   {1.0, 0.686, 0.686},
            {0.89, 0.675, 0.475},  {1.0, 1.0, 0.384},     {0.0, 0.6, 0.325},     {0.627, 0.412, 0.188}, {0.62, 0.627, 0.188},
            {0.408, 0.627, 0.188}, {0.188, 0.627, 0.365}, {0.188, 0.627, 0.627}, {0.188, 0.4, 0.627},   {0.435, 0.188, 0.627},
            {0.627, 0.188, 0.412}};
        const double* c = k[std::clamp(i, 1, 31) - 1];
        return {c[0], c[1], c[2]};
    }

    static double linearOf(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

    /** The Lila node a Maya transform or shape became, or kNoNode. */
    NodeId lilaNodeOf(int i) const
    {
        for (const auto* m : {&mLila, &mShapeNode, &mLocatorShape, &mCameraNode, &mPlaneNode})
            if (auto it = m->find(i); it != m->end())
                return it->second;
        return scene::kNoNode;
    }

    /** displayLayer nodes (not defaultLayer) with their members, which Maya wires drawInfo -> drawOverride. */
    // ---- light linking ----------------------------------------------------------------------------------------

    /** The node a plug ("|grp|boxShape.iog[0]") belongs to. */
    static std::string plugNode(const std::string& plug) { return plug.substr(0, plug.find('.')); }

    /**
     * Lila nodes a light-linking name stands for: found by `own` on the node or its children (a light or mesh under its
     * transform), else the members of the set it names (defaultLightSet, a shading group).
     */
    template <class Map>
    std::vector<NodeId> linkedNodes(const std::string& name, const Map& own, const std::map<std::string, std::vector<std::string>>& members) const
    {
        std::vector<NodeId> out;
        auto add = [&](const std::string& n) {
            const int i = mIx.resolve(n);
            if (i < 0)
                return;
            if (auto it = own.find(i); it != own.end())
                return out.push_back(it->second);
            for (int c : mIx.children[size_t(i)])
                if (auto it = own.find(c); it != own.end())
                    out.push_back(it->second);
        };
        add(name);
        if (out.empty())
            if (auto it = members.find(shortName(name)); it != members.end())
                for (const std::string& m : it->second)
                    add(m);
        return out;
    }

    /**
     * The lightLinker's links and ignores as lighting channels. A link pairs a light (or a light set: defaultLightSet holds
     * the lights that illuminate by default) with an object (or a set: a shading group); an ignore breaks one pair.
     * Only meshes some link names take part. A light lighting all of them keeps default; a file without links changes nothing.
     */
    void buildLightLinks()
    {
        std::map<std::string, std::vector<std::string>> members;
        for (const Connection& c : mFile.connections)
        {
            const std::string_view attr = std::string_view(c.dst).substr(std::min(c.dst.size(), c.dst.find('.') + 1));
            if (attr.starts_with("dsm") || attr.starts_with("dagSetMembers"))
                members[shortName(plugNode(c.dst))].push_back(plugNode(c.src));
        }
        std::map<NodeId, std::set<NodeId>> lit, ignored;
        std::set<NodeId> known;
        bool anyLink = false;
        for (const Relationship& r : mFile.relationships)
        {
            if (r.type != "link" && r.type != "ignore")
                continue;
            std::vector<std::string> objects(r.plugs.begin(), r.plugs.end() - 1);
            std::vector<NodeId> lights = linkedNodes(plugNode(r.plugs.back()), mLightNode, members);
            if (lights.empty() && r.plugs.size() == 2)
            {
                lights = linkedNodes(plugNode(r.plugs.front()), mLightNode, members);
                objects = {r.plugs.back()};
            }
            for (const std::string& o : objects)
                for (NodeId object : linkedNodes(plugNode(o), mShapeNode, members))
                {
                    known.insert(object);
                    for (NodeId light : lights)
                        (r.type == "link" ? lit : ignored)[light].insert(object);
                }
            anyLink = anyLink || r.type == "link";
        }
        if (!anyLink || known.empty())
            return;
        // With set membership in the file, a light no link names illuminates nothing, as in Maya; without it, leave it be.
        const bool membership = members.contains("defaultLightSet");
        std::vector<std::pair<NodeId, std::vector<NodeId>>> links;
        for (const auto& [shape, light] : mLightNode)
        {
            if (!membership && !lit.contains(light) && !ignored.contains(light))
                continue;
            std::set<NodeId> on = lit[light];
            for (NodeId o : ignored[light])
                on.erase(o);
            if (on != known)
                links.push_back({light, {on.begin(), on.end()}});
        }
        if (!links.empty())
            mR.lightingChannels = scene::lightingChannels::bakeLinks(mG, links);
    }

    void buildDisplayLayers()
    {
        namespace dl = scene::displayLayers;
        for (size_t i = 0; i < mFile.nodes.size(); ++i)
        {
            const MNode& n = mFile.nodes[i];
            if (n.type != "displayLayer" || n.existing || shortName(n.name) == "defaultLayer")
                continue;
            mHandled.insert(int(i));
            scene::Node* layer = mG.createNode(*scene::types::find(dl::kTypeName), lilaName(n.name, mOpts.nameSpace, "layer"), scene::kNoNode, false);
            const NodeId id = layer->id();
            auto set = [&](int attr, scene::PlugValue v) {
                if (layer->value(attr) != v)
                    mG.setValue({id, attr, -1}, std::move(v));
            };
            set(scene::DisplayLayer::kVisibility, num(n, {"v", "visibility"}, 1.0) != 0.0);
            set(scene::DisplayLayer::kHideOnPlayback, num(n, {"hpb", "hideOnPlayback"}, 0.0) != 0.0);
            set(scene::DisplayLayer::kDisplayType, int64_t(std::clamp(num(n, {"dt", "displayType"}, 0.0), 0.0, 2.0)));
            set(scene::DisplayLayer::kDisplayOrder, int64_t(num(n, {"do", "displayOrder"}, 0.0)));
            std::optional<glm::dvec3> tint;
            if (num(n, {"ovrgbf", "overrideRGBColors"}, 0.0) != 0.0)
                tint = vec3(n, {"ovrgb", "overrideColorRGB"},
                            {Names{"ovcr", "overrideColorR"}, Names{"ovcg", "overrideColorG"}, Names{"ovcb", "overrideColorB"}}, glm::dvec3(0.0));
            else if (int index = int(num(n, {"c", "color"}, 0.0)); index > 0)
                tint = indexColor(index);
            if (tint)
            {
                set(scene::DisplayLayer::kColor, glm::dvec3(linearOf(tint->x), linearOf(tint->y), linearOf(tint->z)));
                set(scene::DisplayLayer::kUseColor, true);
            }
            int missing = 0;
            for (const auto& [src, dst] : mIx.outputs(int(i)))
            {
                if (!oneOf(src.attr, {"di", "drawInfo"}))
                    continue;
                if (NodeId m = lilaNodeOf(dst.node); m != scene::kNoNode)
                    dl::addMembers(mG, id, {m});
                else
                    ++missing;
            }
            if (missing)
                mR.skipped.push_back(std::format("{}: {} member(s) that were not converted", layer->name(), missing));
            mR.displayLayers.push_back(layer->name());
        }
    }

    // ---- reference edits ------------------------------------------------------------------------------------

    /** The Lila name (without namespace) of a node a reference edit names by its Maya path in the parent scene. */
    std::optional<std::string> refNodeName(const RefInfo& info, std::string_view mayaPath) const
    {
        std::vector<std::string> segs;
        for (size_t i = 0; i <= mayaPath.size();)
        {
            size_t j = mayaPath.find('|', i);
            if (j == std::string_view::npos)
                j = mayaPath.size();
            if (j > i)
                segs.emplace_back(mayaPath.substr(i, j - i));
            i = j + 1;
        }
        // The referenced nodes are the trailing run in its namespace; anything before is scene nodes they now sit under.
        const std::string prefix = info.mayaNs + ":";
        size_t first = segs.size();
        while (first > 0 && segs[first - 1].rfind(prefix, 0) == 0)
            --first;
        if (first == segs.size())
            return std::nullopt;
        std::string path;
        for (size_t k = first; k < segs.size(); ++k)
            path += "|" + segs[k].substr(prefix.size());
        const std::string leaf = segs.back().substr(prefix.size());
        if (info.identityNames)
            return lilaName(leaf, "", "node");
        if (auto it = info.names.find(path); it != info.names.end())
            return it->second;
        for (const auto& [k, v] : info.names)
            if (k.size() > leaf.size() && k.ends_with("|" + leaf))
                return v;
        return std::nullopt;
    }

    /** The Lila node a Maya path in this scene names: one of its own nodes or one of a live reference's. */
    std::optional<std::string> sceneNodeName(std::string_view mayaPath) const
    {
        for (const RefInfo& other : mRefs)
            if (other.live && mayaPath.find(other.mayaNs + ":") != std::string_view::npos)
                if (auto rel = refNodeName(other, mayaPath))
                    return nsPath(other.lilaNs, *rel);
        int i = mIx.resolve(mayaPath);
        if (auto it = mLila.find(i); it != mLila.end())
            return mG.node(it->second)->path();
        return std::nullopt;
    }

    /** A live reference's node by relName; null when it is gone or ambiguous. */
    scene::Node* refFind(const RefInfo& info, const std::string& rel) const
    {
        try
        {
            return mG.find(nsPath(info.lilaNs, rel));
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    json currentValue(RefInfo& info, const std::string& rel, const std::string& attr)
    {
        if (info.live)
        {
            scene::Node* n = refFind(info, rel);
            int idx = n ? n->type().attrIndex(attr) : -1;
            return idx >= 0 ? scene::toJson(n->value(idx)) : json();
        }
        if (info.nestedEdits.contains(rel) && info.nestedEdits[rel].contains(attr))
            return info.nestedEdits[rel][attr];
        for (const json& n : info.nestedDoc.value("nodes", json::array()))
            if (docNodeIs(n, rel))
            {
                if (n.contains("attrs") && n["attrs"].contains(attr))
                    return n["attrs"][attr];
                const scene::NodeType* t = scene::types::find(n.value("type", ""));
                int idx = t ? t->attrIndex(attr) : -1;
                return idx >= 0 ? scene::toJson(t->attrs[size_t(idx)].storedDefault()) : json();
            }
        return json();
    }

    bool storeValue(RefInfo& info, const std::string& rel, const std::string& attr, const json& value)
    {
        if (!info.live)
        {
            info.nestedEdits[rel][attr] = value;
            return true;
        }
        scene::Node* n = refFind(info, rel);
        int idx = n ? n->type().attrIndex(attr) : -1;
        if (idx < 0)
            return false;
        try
        {
            mG.setValue({n->id(), idx, -1}, scene::fromJson(n->type().attrs[size_t(idx)], value));
        }
        catch (const std::exception& e)
        {
            mR.warnings.push_back(std::format("{}:{}.{}: {}", info.lilaNs, rel, attr, e.what()));
            return false;
        }
        return true;
    }

    /** A setAttr reference edit onto the Lila node, in Lila units and axes. False when the attribute isn't supported. */
    bool applySetAttrEdit(RefInfo& info, const std::string& rel, const std::string& attr, const std::string& valueText)
    {
        struct Row
        {
            std::string_view mayaLong, mayaShort, lila;
            char kind; // T translate-like, R rotate, S scale-like, J jointOrient, B bool, D distance, O rotate order
            int comp;
        };
        static constexpr Row kRows[] = {
            {"translate", "t", "translate", 'T', -1},
            {"translateX", "tx", "translate", 'T', 0},
            {"translateY", "ty", "translate", 'T', 1},
            {"translateZ", "tz", "translate", 'T', 2},
            {"rotate", "r", "rotate", 'R', -1},
            {"rotateX", "rx", "rotate", 'R', 0},
            {"rotateY", "ry", "rotate", 'R', 1},
            {"rotateZ", "rz", "rotate", 'R', 2},
            {"scale", "s", "scale", 'S', -1},
            {"scaleX", "sx", "scale", 'S', 0},
            {"scaleY", "sy", "scale", 'S', 1},
            {"scaleZ", "sz", "scale", 'S', 2},
            {"jointOrient", "jo", "jointOrient", 'J', -1},
            {"jointOrientX", "jox", "jointOrient", 'J', 0},
            {"jointOrientY", "joy", "jointOrient", 'J', 1},
            {"jointOrientZ", "joz", "jointOrient", 'J', 2},
            {"rotatePivot", "rp", "rotatePivot", 'T', -1},
            {"scalePivot", "sp", "scalePivot", 'T', -1},
            {"rotatePivotTranslate", "rpt", "rotatePivotTranslate", 'T', -1},
            {"scalePivotTranslate", "spt", "scalePivotTranslate", 'T', -1},
            {"localScale", "los", "localScale", 'S', -1},
            {"visibility", "v", "visibility", 'B', -1},
            {"segmentScaleCompensate", "ssc", "segmentScaleCompensate", 'B', -1},
            {"radius", "radi", "radius", 'D', -1},
            {"rotateOrder", "ro", "rotateOrder", 'O', -1},
        };
        const Row* row = nullptr;
        for (const Row& r : kRows)
            if (attr == r.mayaLong || attr == r.mayaShort)
                row = &r;
        if (!row)
            return false;
        File parsed = readAscii("createNode transform -n \"x\";\nsetAttr \".a\" " + valueText + ";");
        if (parsed.nodes.empty() || parsed.nodes[0].attrs.empty() || parsed.nodes[0].attrs[0].nums.empty())
            return false;
        const std::vector<double>& v = parsed.nodes[0].attrs[0].nums;
        // Edits are MEL text, so they are in the scene's UI units however the scene was saved.
        const double lin = cmPerUnit(mFile.linearUnit) > 0.0 ? cmPerUnit(mFile.linearUnit) : 1.0;
        const double ang = mFile.angleUnit.starts_with("rad") ? 180.0 / math::kPi : 1.0;
        const std::string lilaAttr(row->lila);
        // Below an FBX reference's roots the channels are in the file's axes; a root's translate is in the scene's.
        const bool fbxRef = info.fbxToLila.has_value();
        const bool fbxRoot = fbxRef && isRefRoot(info, rel);
        const glm::dmat3& C3 = fbxRef && !fbxRoot ? *info.fbxToLila : mC3;
        const fbx::AxisMap ax = fbxRef && !fbxRoot ? fbx::axisMap(C3) : mAxes;
        const fbx::AxisMap fileAx = fbxRef ? fbx::axisMap(*info.fbxToLila) : mAxes;
        const bool identity = C3 == glm::dmat3(1.0);
        if (fbxRoot && row->kind == 'R')
            return applyFbxRootRotate(info, rel, row->comp, v, ang);
        json out;
        if (row->kind == 'B')
            out = v[0] != 0.0;
        else if (row->kind == 'D')
            out = v[0] * lin;
        else if (row->kind == 'O')
            out = int64_t(fileAx.order(scene::RotateOrder(std::clamp(int(v[0]), 0, 5))));
        else if (row->comp < 0)
        {
            if (v.size() < 3)
                return false;
            const glm::dvec3 x(v[0], v[1], v[2]);
            glm::dvec3 lv;
            if (row->kind == 'T')
                lv = ax.translate(x * lin);
            else if (row->kind == 'R')
                lv = cleanAngles(ax.rotate(x * ang));
            else if (row->kind == 'S')
                lv = fileAx.scale(x);
            else
            {
                const glm::dvec3 jo = x * ang;
                const glm::dmat3 m = C3 * glm::dmat3(scene::eulerToMatrix(jo, scene::RotateOrder::XYZ)) * glm::transpose(C3);
                lv = cleanAngles(identity ? jo : scene::matrixToEuler(m, scene::RotateOrder::XYZ));
            }
            out = json::array({lv.x, lv.y, lv.z});
        }
        else
        {
            json cur = currentValue(info, rel, lilaAttr);
            if (!cur.is_array() || cur.size() != 3)
                return false;
            const int c = row->comp;
            if (row->kind == 'T')
                cur[size_t(ax.to[c])] = ax.sign[c] * v[0] * lin;
            else if (row->kind == 'R')
                cur[size_t(ax.to[c])] = ax.sign[c] * ax.det * v[0] * ang;
            else if (row->kind == 'S')
                cur[size_t(fileAx.to[c])] = v[0];
            else if (identity)
                cur[size_t(c)] = v[0] * ang;
            else
                return false;
            out = cur;
        }
        return storeValue(info, rel, lilaAttr, out);
    }

    /** Whether a referenced node sits at the top of its file: no parent from the same reference. */
    bool isRefRoot(const RefInfo& info, const std::string& rel) const
    {
        if (info.live)
        {
            const scene::Node* n = refFind(info, rel);
            const NodeId p = n ? mG.parent(n->id()) : scene::kNoNode;
            return p == scene::kNoNode || !mG.node(p)->name().starts_with(info.lilaNs + ":");
        }
        for (const json& n : info.nestedDoc.value("nodes", json::array()))
            if (docNodeIs(n, rel))
                return !n.contains("parent");
        return true;
    }

    /**
     * A rotate edit on the root of an FBX reference. Maya folds the file-to-scene axis turn into the root's rotate
     * while Lila conjugates it by the file-to-Lila rotation F, so the Lila rotation is C * R * F^T (C: scene to Lila).
     * comp < 0 sets all three channels.
     */
    bool applyFbxRootRotate(RefInfo& info, const std::string& rel, int comp, const std::vector<double>& v, double ang)
    {
        const glm::dmat3& F = *info.fbxToLila;
        const fbx::AxisMap fileAx = fbx::axisMap(F);
        const json curOrder = currentValue(info, rel, "rotateOrder");
        const auto lilaOrder = scene::RotateOrder(curOrder.is_number_integer() ? std::clamp(int(curOrder.get<int64_t>()), 0, 5) : 0);
        auto mayaOrder = scene::RotateOrder::XYZ;
        for (int o = 0; o < 6; ++o)
            if (fileAx.order(scene::RotateOrder(o)) == lilaOrder)
                mayaOrder = scene::RotateOrder(o);
        const json cur = currentValue(info, rel, "rotate");
        const glm::dvec3 lilaCur = cur.is_array() && cur.size() == 3 ? glm::dvec3(cur[0].get<double>(), cur[1].get<double>(), cur[2].get<double>())
                                                                     : glm::dvec3(0.0);
        glm::dvec3 maya;
        if (comp < 0)
        {
            if (v.size() < 3)
                return false;
            maya = glm::dvec3(v[0], v[1], v[2]) * ang;
        }
        else
        {
            const glm::dmat3 rl(scene::eulerToMatrix(lilaCur, lilaOrder));
            maya = scene::matrixToEuler(glm::transpose(mC3) * rl * F, mayaOrder);
            maya[comp] = v[0] * ang;
        }
        const glm::dmat3 rl = mC3 * glm::dmat3(scene::eulerToMatrix(maya, mayaOrder)) * glm::transpose(F);
        const glm::dvec3 lv = cleanAngles(fbx::eulerNear(rl, lilaOrder, lilaCur));
        return storeValue(info, rel, "rotate", json::array({lv.x, lv.y, lv.z}));
    }

    /** The rotation Maya's FBX import gives the file's roots: its axes onto the scene's (right +X; Z up front -Y, Y up front +Z). */
    glm::dmat3 mayaFbxTurn(const fbx::ImportAxes& axes) const
    {
        const bool sceneZUp = mR.upAxis == "z";
        if (axes.zUp == sceneZUp)
            return axes.toCanonical;
        const glm::dmat3 zToY({1, 0, 0}, {0, 0, -1}, {0, 1, 0});
        return (sceneZUp ? glm::transpose(zToY) : zToY) * axes.toCanonical;
    }

    /** The reference's nodes, by Lila name without namespace, with their types. */
    std::vector<std::pair<std::string, const scene::NodeType*>> refNodes(const RefInfo& info) const
    {
        std::vector<std::pair<std::string, const scene::NodeType*>> out;
        if (info.live)
        {
            const std::string prefix = info.lilaNs + ":";
            for (NodeId id : mG.nodes())
                if (const scene::Node* n = mG.node(id); n->name().starts_with(prefix))
                    out.emplace_back(relName(mG, id, info.lilaNs), &n->type());
            return out;
        }
        std::map<std::string, int> uses;
        for (const json& n : info.nestedDoc.value("nodes", json::array()))
            ++uses[n.value("name", "")];
        for (const json& n : info.nestedDoc.value("nodes", json::array()))
            if (const scene::NodeType* t = scene::types::find(n.value("type", "")))
            {
                std::string name = n.value("name", ""), parent = n.value("parent", "");
                if (parent.starts_with('|'))
                    parent.erase(0, 1);
                out.emplace_back(uses[name] == 1 || parent.empty() ? name : parent + "|" + name, t);
            }
        return out;
    }

    /**
     * Turns an FBX reference by fbxRootTurn, so the file sits in the scene where Maya puts it: its roots, and the
     * skins' bind-time world matrices with them so the skinned meshes don't take the turn twice.
     */
    void turnFbxRoots(RefInfo& info)
    {
        const glm::dmat3& D = info.fbxRootTurn;
        double off = 0.0;
        for (int c = 0; c < 3; ++c)
            off = std::max(off, glm::length(D[c] - glm::dmat3(1.0)[c]));
        if (off < 1e-9)
            return;
        auto vec3 = [](const json& j, glm::dvec3 fallback) {
            return j.is_array() && j.size() == 3 ? glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : fallback;
        };
        auto turnMatrix = [&](const std::string& rel, const scene::NodeType& t, const char* attr, bool before) {
            const int idx = t.attrIndex(attr);
            if (idx < 0)
                return;
            const scene::AttrDef& def = t.attrs[size_t(idx)];
            const json cur = currentValue(info, rel, attr);
            const auto m = std::get<glm::dmat4>(cur.is_null() ? def.defaultValue : scene::fromJson(def, cur));
            const glm::dmat4 turn(D);
            storeValue(info, rel, attr, scene::toJson(before ? turn * m : m * glm::transpose(turn)));
        };
        for (const auto& [rel, type] : refNodes(info))
        {
            if (type->name == "skinCluster")
                turnMatrix(rel, *type, "geomMatrix", true);
            else if (type->name == "skinInfluence")
                turnMatrix(rel, *type, "bindPreMatrix", false);
            if (type->attrIndex("translate") < 0 || !isRefRoot(info, rel))
                continue;
            const glm::dvec3 t = vec3(currentValue(info, rel, "translate"), glm::dvec3(0.0));
            const glm::dvec3 r = vec3(currentValue(info, rel, "rotate"), glm::dvec3(0.0));
            const glm::dvec3 jo = vec3(currentValue(info, rel, "jointOrient"), glm::dvec3(0.0));
            const json ro = currentValue(info, rel, "rotateOrder");
            const auto order = scene::RotateOrder(ro.is_number_integer() ? std::clamp(int(ro.get<int64_t>()), 0, 5) : 0);
            const glm::dmat3 J(scene::eulerToMatrix(jo, scene::RotateOrder::XYZ));
            const glm::dmat3 R = glm::transpose(J) * D * J * glm::dmat3(scene::eulerToMatrix(r, order));
            const glm::dvec3 nt = D * t, nr = cleanAngles(fbx::eulerNear(R, order, r));
            storeValue(info, rel, "translate", json::array({nt.x, nt.y, nt.z}));
            storeValue(info, rel, "rotate", json::array({nr.x, nr.y, nr.z}));
        }
    }

    void applyRefEdits(RefInfo& info)
    {
        LILA_ZONE();

        if (info.fbxToLila)
            turnFbxRoots(info);

        int refNode = info.ref->refNode.empty() ? -1 : mIx.resolve(info.ref->refNode);
        if (refNode < 0)
            refNode = mIx.resolve(info.mayaNs + "RN");
        if (refNode < 0)
            return;
        mHandled.insert(refNode);
        std::map<std::string, int> unsupported;
        int locks = 0, applied = 0;
        for (const RefEdit& e : mIx.node(refNode).refEdits)
        {
            const std::vector<std::string>& a = e.args;
            if (e.kind == 2 && a.size() >= 3)
            {
                auto rel = refNodeName(info, a[0]);
                if (!rel)
                    ++unsupported["setAttr on a node not in the referenced file"];
                else if (applySetAttrEdit(info, *rel, a[1], a[2]))
                    ++applied;
                else
                    ++unsupported["setAttr ." + a[1]];
            }
            else if (e.kind == 0 && a.size() >= 2)
            {
                auto rel = refNodeName(info, a[0]);
                const bool toWorld = a[1].empty() || a[1] == "|";
                std::optional<std::string> parent = toWorld ? std::optional<std::string>("") : sceneNodeName(a[1]);
                scene::Node* child = rel && info.live ? refFind(info, *rel) : nullptr;
                scene::Node* p = parent && !parent->empty() ? mG.find(*parent) : nullptr;
                if (!child || !parent || (!toWorld && !p))
                {
                    ++unsupported[info.live ? "parent (target not found)" : "parent inside a nested reference"];
                    continue;
                }
                try
                {
                    // Maya records the resulting channels as setAttr edits too, so the local transform is kept here.
                    mG.setParent(child->id(), p ? p->id() : scene::kNoNode, false);
                    ++applied;
                }
                catch (const std::exception& ex)
                {
                    mR.warnings.push_back(std::format("{}: {}", child->path(), ex.what()));
                }
            }
            else if (e.kind == 5 && a.size() >= 4)
            {
                // A connection to a referenced plug goes through a placeholder on the reference node.
                const std::string& plug = a[2];
                const size_t dot = plug.find('.');
                auto rel = dot == std::string::npos ? std::nullopt : refNodeName(info, std::string_view(plug).substr(0, dot));
                const size_t lb = a[3].rfind('[');
                scene::Node* n = rel && info.live ? refFind(info, *rel) : nullptr;
                if (!n || lb == std::string::npos)
                {
                    ++unsupported["connection to a referenced node"];
                    continue;
                }
                mPlaceholders[{refNode, std::atoi(a[3].c_str() + lb + 1)}] = {n->id(), plug.substr(dot + 1)};
            }
            else if (e.kind == 8)
                ++locks;
            else if (e.kind == 1)
                ++unsupported["addAttr (custom attributes)"];
            else if (e.kind == 3)
                ++unsupported["disconnectAttr"];
            else if (e.kind == 7)
                ++unsupported["deleteAttr"];
            else
                ++unsupported[std::format("kind {}", e.kind)];
        }
        const std::string where = std::format("reference '{}'", info.lilaNs);
        if (applied)
            mR.warnings.push_back(std::format("{}: {} reference edit(s) applied", where, applied));
        if (locks)
            mR.skipped.push_back(std::format("{}: {} attribute lock(s) (Lila has no attribute locks)", where, locks));
        for (const auto& [what, count] : unsupported)
            mR.skipped.push_back(std::format("{}: {} {} edit(s)", where, count, what));
    }

    /** A Maya destination plug as a converted node and Maya attribute, through reference placeholders too. */
    std::optional<std::pair<NodeId, std::string>> channelTarget(const Plug& dst) const
    {
        if (auto it = mLila.find(dst.node); it != mLila.end())
            return std::pair{it->second, dst.attr};
        std::vector<AttrSeg> s = splitAttr(dst.attr);
        if (s.size() == 1 && oneOf(s[0].name, {"phl", "placeHolderList"}) && s[0].lo >= 0)
            if (auto it = mPlaceholders.find({dst.node, s[0].lo}); it != mPlaceholders.end())
                return it->second;
        return std::nullopt;
    }

    /** Scene connections into or out of referenced translate / rotate / scale channels; curves are buildAnimation's. */
    void connectPlaceholders()
    {
        if (mPlaceholders.empty())
            return;
        auto channel = [&](const std::pair<NodeId, std::string>& t) -> std::optional<PlugRef> {
            static constexpr std::string_view kNames[3][3][2] = {
                {{"tx", "translateX"}, {"ty", "translateY"}, {"tz", "translateZ"}},
                {{"rx", "rotateX"}, {"ry", "rotateY"}, {"rz", "rotateZ"}},
                {{"sx", "scaleX"}, {"sy", "scaleY"}, {"sz", "scaleZ"}},
            };
            static const int kAttr[3] = {scene::Transform::kTranslate, scene::Transform::kRotate, scene::Transform::kScale};
            for (int g = 0; g < 3; ++g)
                for (int a = 0; a < 3; ++a)
                    if (t.second == kNames[g][a][0] || t.second == kNames[g][a][1])
                        return PlugRef{t.first, kAttr[g], mAxes.to[a]};
            return std::nullopt;
        };
        auto lookup = [&](const Plug& p) -> std::optional<std::pair<NodeId, std::string>> {
            if (mIx.node(p.node).type != "reference")
                return channelTarget(p);
            std::vector<AttrSeg> s = splitAttr(p.attr);
            if (s.size() == 1 && s[0].lo >= 0)
                if (auto it = mPlaceholders.find({p.node, s[0].lo}); it != mPlaceholders.end())
                    return it->second;
            return std::nullopt;
        };
        int skipped = 0;
        for (const FileIndex::Conn& c : mIx.conns)
        {
            const bool srcRef = mIx.node(c.src.node).type == "reference", dstRef = mIx.node(c.dst.node).type == "reference";
            if ((!srcRef && !dstRef) || mIx.node(c.src.node).type.starts_with("animCurve"))
                continue;
            auto from = lookup(c.src), to = lookup(c.dst);
            std::optional<PlugRef> a = from ? channel(*from) : std::nullopt, b = to ? channel(*to) : std::nullopt;
            if (!a || !b || !mIdentityAxes)
            {
                ++skipped;
                continue;
            }
            try
            {
                mG.connect(*a, *b, true);
            }
            catch (const std::exception&)
            {
                ++skipped;
            }
        }
        if (skipped)
            mR.skipped.push_back(std::format("{} connection(s) with referenced nodes other than translate / rotate / scale channels", skipped));
    }

public:
    static ImportResult importAt(Graph& g, const fs::path& path, const ImportOptions& opts, int depth, Nested* nested = nullptr);
    static ImportResult convertFile(Graph& g, const File& file, const fs::path& path, const ImportOptions& opts, int depth, Nested* nested);
    static fs::path lilaFor(const fs::path& src, const ImportOptions& opts, ImportResult& r);

private:
    // ---- report ---------------------------------------------------------------------------------------------

    void report()
    {
        std::map<std::string, int> unconverted;
        size_t custom = 0;
        std::vector<std::string> customNames;
        for (size_t i = 0; i < mFile.nodes.size(); ++i)
        {
            const MNode& n = mFile.nodes[i];
            for (const AddedAttr& a : n.added)
            {
                // Maya's own bookkeeping: lockInfluenceWeights on every skinned joint, a converted blendShape's aliases.
                if (a.longName == "lockInfluenceWeights" || (a.longName == "attributeAliasList" && mBlends.contains(int(i))) || n.existing)
                    continue;
                // Converted user attributes, and the attributes the rig pass reads off converted nodes (constraint weights, nonlinear settings).
                if ((mRig && mRig->convertedAttr(int(i), a.longName)) || (mHandled.contains(int(i)) && RigBuilder::convertsInputsOf(n.type)))
                    continue;
                ++custom;
                if (customNames.size() < 20)
                    customNames.push_back(std::format("{}.{}", shortName(n.name), a.longName));
            }
            if (n.existing || n.type.empty() || mHandled.contains(int(i)) || quietTypes().contains(n.type) || n.shared)
                continue;
            if (n.type.starts_with("animCurve") || n.type == "shadingEngine" || n.type == "file" || n.type == "bump2d")
                continue;
            if (isIntermediate(int(i)))
                continue;
            ++unconverted[n.type];
        }
        for (const auto& [type, count] : unconverted)
            mR.skipped.push_back(std::format("{} {} node(s)", count, type));
        if (custom)
            mR.skipped.push_back(std::format("{} custom attribute(s) (only numeric, bool and enum attributes on transforms and joints convert): {}{}", custom,
                                             [&] {
                                                 std::string s;
                                                 for (const std::string& c : customNames)
                                                     s += (s.empty() ? "" : ", ") + c;
                                                 return s;
                                             }(),
                                             custom > customNames.size() ? ", ..." : ""));
    }

    Graph& mG;
    const File& mFile;
    FileIndex mIx;
    fs::path mPath;
    const ImportOptions& mOpts;
    ImportResult& mR;
    int mDepth = 0;
    Units mU;
    glm::dmat3 mC3{1.0};
    fbx::AxisMap mAxes;
    bool mIdentityAxes = true;
    std::unordered_map<int, NodeId> mLila;      // Maya transform -> Lila node
    std::unordered_map<int, NodeId> mShapeNode; // Maya mesh shape -> Lila mesh node
    std::unordered_map<int, int> mInstanceOf;   // transform -> the mesh shape `parent -add` instances under it
    std::set<const ParentEdit*> mInstanceEdits; // the parent edits converted into instances
    std::vector<std::pair<NodeId, int>> mPendingInstances; // Lila mesh -> instanced shape, connected after the meshes
    std::unordered_map<int, NodeId> mLocatorShape;
    std::unordered_map<int, NodeId> mCameraNode; // Maya camera shape -> Lila camera node
    std::map<int, NodeId> mLightNode;            // Maya light shape -> Lila light node, in file order
    std::unordered_map<int, NodeId> mPlaneNode;  // Maya imagePlane shape -> Lila image plane
    std::unordered_map<int, std::vector<Index>> mPointOf;
    std::unordered_map<int, std::string> mMaterial;
    std::set<int> mHandled;
    std::set<int> mRotateAxisFolded;
    std::map<int, Blend> mBlends; // converted blendShapes by Maya node
    std::unique_ptr<RigBuilder> mRig;
    std::vector<PendingMesh> mPending;
    Nested* mNested = nullptr;
    std::vector<RefInfo> mRefs;
    /** (reference node, placeholder index) -> the referenced Lila node and the Maya attribute it stands for. */
    std::map<std::pair<int, int>, std::pair<NodeId, std::string>> mPlaceholders;
};

ImportResult Converter::importAt(Graph& g, const fs::path& path, const ImportOptions& opts, int depth, Nested* nested)
{
    LILA_ZONE();

    progress::Scope scope("Converting " + path.filename().string(), 10.0);
    File file;
    try
    {
        progress::Scope read("Reading " + path.filename().string(), 0.0, progress::None, 2.0);
        progress::runOnWorker([&] { file = readFile(path); });
    }
    catch (const progress::Cancelled&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        ImportResult r;
        r.error = e.what();
        return r;
    }
    progress::Scope convert("Converting " + path.filename().string(), 0.0, progress::None, 8.0);
    return convertFile(g, file, path, opts, depth, nested);
}

ImportResult Converter::convertFile(Graph& g, const File& file, const fs::path& path, const ImportOptions& opts, int depth, Nested* nested)
{
    LILA_ZONE();

    Graph::Batch batch(g);
    ImportResult r;
    r.format = file.binary ? "binary" : "ascii";
    r.version = file.version;
    r.warnings = file.warnings;
    Converter c(g, file, path, opts, r, depth, nested);
    try
    {
        c.run();
    }
    catch (const std::exception& e)
    {
        r.error = e.what();
        return r;
    }
    r.ok = true;
    // References can't be undone, so neither can an import that made them.
    if (depth == 0 && !r.references.empty())
        UndoStack::get().clear();
    return r;
}
fs::path Converter::lilaFor(const fs::path& src, const ImportOptions& opts, ImportResult& r)
{
    LILA_ZONE();

    const File none;
    Graph unused;
    Converter c(unused, none, src, opts, r, 0, nullptr);
    std::map<std::string, std::string> names;
    bool identity = false;
    fs::path lila = c.ensureLila(src, names, &identity, &r.error);
    r.ok = !lila.empty();
    return lila;
}
} // namespace

bool isMayaFile(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".ma" || ext == ".mb";
}

ImportResult importMaya(scene::Graph& graph, const fs::path& path, const ImportOptions& opts)
{
    return Converter::importAt(graph, path, opts, 0);
}

ImportResult importMaya(scene::Graph& graph, const fs::path& path, const File& file, const ImportOptions& opts)
{
    return Converter::convertFile(graph, file, path, opts, 0, nullptr);
}

fs::path lilaForReference(const fs::path& path, const ImportOptions& opts, ImportResult& report)
{
    return Converter::lilaFor(path, opts, report);
}
} // namespace lila::maya
