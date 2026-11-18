// The Ground Tiles's scene half: the generated group's node type, generation, the bake and ground planes, height
// metrics, the ID map bake, the FBX pair export with its sidecars, and the preset and palette prefs.

#include "GroundTiles.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Prefs.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/Undo.h>

// TBB before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include <QImage>
#include <QString>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace groundtiles
{
namespace
{
constexpr int kHeightSampleGrid = 256;

Graph& graph() { return Graph::current(); }

int attrOf(const Node& n, const char* name) { return n.type().attrIndex(name); }

const std::string& strAttr(const Node& n, const char* name) { return n.as<std::string>(attrOf(n, name)); }

void setStr(Graph& g, NodeId id, const char* name, const std::string& v) { g.setValue({id, attrOf(*g.node(id), name), -1}, v); }
void setNum(Graph& g, NodeId id, const char* name, double v) { g.setValue({id, attrOf(*g.node(id), name), -1}, v); }

json parseOr(const std::string& text, const json& fallback)
{
    if (text.empty())
        return fallback;
    try
    {
        return json::parse(text);
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

fs::path utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

Node* findNode(Graph& g, const std::string& name)
{
    try
    {
        return name.empty() ? nullptr : g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

bool isGroup(const Node& n) { return n.type().isA(kGroupType); }

/** The generated group a node is, or sits under; null when there is none. */
Node* generatedFor(Graph& g, NodeId id)
{
    for (NodeId cur = id; cur; cur = g.parent(cur))
        if (Node* n = g.node(cur); n && isGroup(*n))
            return n;
    return nullptr;
}

/** The named group, or the first selected node that is one or sits under one. */
Node* groupArg(Graph& g, const Args& a, const char* cmd, bool required = true)
{
    if (!a.positional.empty() && a.positional[0].is_string())
    {
        const std::string name = a.positional[0].get<std::string>();
        Node* n = findNode(g, name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (Node* grp = generatedFor(g, n->id()))
            return grp;
        throw CommandError(std::string(cmd) + ": " + name + " is not a generated ground tile group");
    }
    for (NodeId id : g.selection())
        if (Node* grp = generatedFor(g, id))
            return grp;
    if (required)
        throw CommandError(std::string(cmd) + ": select a generated ground tile group");
    return nullptr;
}

std::vector<NodeId> meshDescendants(Graph& g, NodeId root)
{
    std::vector<NodeId> out;
    for (NodeId d : g.descendants(root))
        if (Node* n = g.node(d); n && n->type().isA("mesh"))
            out.push_back(d);
    return out;
}

struct WorldBox
{
    glm::dvec3 lo{std::numeric_limits<double>::max()}, hi{-std::numeric_limits<double>::max()};
    bool valid() const { return lo.x <= hi.x; }
    void add(const glm::dvec3& p)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
};

WorldBox worldBox(Graph& g, const std::vector<NodeId>& meshes)
{
    WorldBox box;
    for (NodeId id : meshes)
    {
        const Node& n = *g.node(id);
        const MeshPtr& m = n.as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        const glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                box.add(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)));
    }
    return box;
}

/** {values, sources, preset, palette, legend, export} stored on a group. */
json readStored(const Node& grp)
{
    json exp = {{"path", ""}, {"name", ""}, {"material", ""}};
    const json stored = parseOr(strAttr(grp, kAttrExport), json::object());
    if (stored.is_object())
        exp.update(stored);
    json sources = parseOr(strAttr(grp, kAttrSources), json::array());
    json palette = parseOr(strAttr(grp, kAttrPalette), json::object());
    json legend = parseOr(strAttr(grp, kAttrIds), json::object());
    return {{"values", sanitize(parseOr(strAttr(grp, kAttrSettings), json::object()))},
            {"sources", sources.is_array() ? sources : json::array()},
            {"preset", strAttr(grp, kAttrPreset)},
            {"version", strAttr(grp, kAttrVersion)},
            {"palette", palette.is_object() ? palette : json::object()},
            {"legend", legend.is_object() ? legend : json::object()},
            {"export", exp}};
}

void writeStored(Graph& g, NodeId grp, const json& values, const json& sources, const std::string& preset, const json& palette, const json& legend,
                 const json& exp)
{
    setStr(g, grp, kAttrSettings, values.dump());
    setStr(g, grp, kAttrSources, sources.dump());
    setStr(g, grp, kAttrPreset, preset);
    setStr(g, grp, kAttrVersion, kVersion);
    setStr(g, grp, kAttrPalette, (palette.is_object() ? palette : json::object()).dump());
    setStr(g, grp, kAttrIds, (legend.is_object() ? legend : json::object()).dump());
    json e = {{"path", ""}, {"name", ""}, {"material", ""}};
    if (exp.is_object())
        e.update(exp);
    setStr(g, grp, kAttrExport, e.dump());
}

json metricsOf(const Node& grp)
{
    json m = json::object();
    for (const char* a : {kAttrMinHeight, kAttrMaxHeight, kAttrHeightRange, kAttrPlaneHeight, kAttrAbovePlane})
        m[a] = grp.as<double>(attrOf(grp, a));
    if (grp.as<bool>(attrOf(grp, kAttrMeasured)))
        for (const char* a : {kAttrHeightP50, kAttrHeightP99, kAttrHeightTop})
            m[a] = grp.as<double>(attrOf(grp, a));
    return m;
}

/** The material an id's faces use, made or recoloured to the palette colour. */
void ensureIdMaterial(Graph& g, int64_t id, const glm::dvec3& colour)
{
    const std::string name = idMaterialName(id);
    Node* m = materials::find(g, name);
    if (!m)
        m = materials::create(g, name);
    g.setValue({m->id(), material::kBaseColor, -1}, colour);
    g.setValue({m->id(), material::kRoughness, -1}, 1.0);
    g.setValue({m->id(), material::kSpecular, -1}, 0.0);
}

/** Only the masks: FBX and every baker read colour set 0 rather than the current one, so a second set would win. */
void writeMasks(geo::Mesh& m, std::span<const glm::vec4> perPoint)
{
    for (const std::string& set : geo::colors::sets(m))
        geo::colors::remove(m, set);
    geo::colors::add(m, kColourSet, glm::vec4(1.0f));
    geo::colors::setFromPoints(m, kColourSet, perPoint);
    geo::colors::setCurrent(m, kColourSet);
}

std::optional<AuthoredHandle> authoredHandle(Graph& g, NodeId mesh, const char* type)
{
    for (NodeId d : deformers::list(g, mesh))
    {
        const Node* n = g.node(d);
        if (!n || n->type().name != type)
            continue;
        const int hm = attrOf(*n, "handleMatrix"), lo = attrOf(*n, "lowBound"), hi = attrOf(*n, "highBound");
        if (hm < 0 || lo < 0 || hi < 0)
            continue;
        return AuthoredHandle{n->as<glm::dmat4>(hm), n->as<double>(lo), n->as<double>(hi)};
    }
    return std::nullopt;
}

/** The enabled sources that are meshes in the scene, read for planning; warnings for the ones skipped. */
std::vector<Source> readSources(Graph& g, const json& entries, json& active, std::vector<std::string>& warnings)
{
    std::vector<Source> out;
    g.evaluate();
    for (const json& e : entries)
    {
        if (!e.is_object() || !e.value("enabled", true))
            continue;
        const std::string name = e.value("name", "");
        Node* n = findNode(g, name);
        if (!n || !n->type().isA("mesh") || !n->as<MeshPtr>(MeshNode::kOutMesh) || n->as<MeshPtr>(MeshNode::kOutMesh)->numFaces() == 0)
        {
            warnings.push_back("Skipping source '" + name + "', it is not a polygon mesh in this scene.");
            continue;
        }
        Source s;
        s.name = name;
        s.objectId = e.contains("objectId") && e["objectId"].is_number() ? e["objectId"].get<int64_t>() : 0;
        s.heightOffset = e.contains("heightOffset") && e["heightOffset"].is_number() ? e["heightOffset"].get<double>() : 0.0;
        s.weight = e.contains("weight") && e["weight"].is_number() ? e["weight"].get<double>() : 1.0;
        s.mesh = *n->as<MeshPtr>(MeshNode::kOutMesh);
        s.world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        s.rotatePivot = n->as<glm::dvec3>(Transform::kRotatePivot);
        s.scalePivot = n->as<glm::dvec3>(Transform::kScalePivot);
        s.authored[kBend] = authoredHandle(g, n->id(), "bendDeformer");
        s.authored[kTwist] = authoredHandle(g, n->id(), "twistDeformer");
        s.authored[kSine] = authoredHandle(g, n->id(), "sineDeformer");
        s.authored[kFlare] = authoredHandle(g, n->id(), "taperDeformer");
        prepareSource(s, e.value("lengthAxis", "Auto"), e.value("baseEnd", "Auto"));
        out.push_back(std::move(s));
        active.push_back(e);
    }
    return out;
}

std::string shortName(const std::string& path)
{
    std::string s = path.substr(path.find_last_of('|') == std::string::npos ? 0 : path.find_last_of('|') + 1);
    const size_t colon = s.find_last_of(':');
    return colon == std::string::npos ? s : s.substr(colon + 1);
}

Node* meshNode(Graph& g, const std::string& name, NodeId parent, geo::Mesh mesh)
{
    Node* n = g.createNode(*types::find("mesh"), name, parent);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

void setUpOffset(Graph& g, NodeId id, const std::string& upAxis, double height)
{
    g.setValue({id, Transform::kTranslate, -1}, upAxis == "Z" ? glm::dvec3(0.0, 0.0, height) : glm::dvec3(0.0, height, 0.0));
}

/** A tile plane node under parent (or the world), its geometry flat at the group's own origin, lifted to height. */
Node* tilePlaneNode(Graph& g, const json& values, const std::string& suffix, const std::string& preset, double height, int subdivisions, NodeId parent)
{
    Node* n = meshNode(g, nodeStem(preset) + suffix + "1", parent, tilePlane(num(values, "tileSize"), text(values, "upAxis"), subdivisions));
    setUpOffset(g, n->id(), text(values, "upAxis"), height);
    return n;
}

/** The slot of the plane's own material, made when it has none. Empty when create is off and it has none. */
std::string bakePlaneMaterial(Graph& g, NodeId plane, const std::string& preset, bool create)
{
    g.evaluate();
    Node* n = g.node(plane);
    const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh);
    if (m)
        for (const auto& [slot, count] : materials::slotUsage(*m))
            if (count && slot != materials::kDefaultSlot && materials::find(g, slot))
                return slot;
    if (!create)
        return {};
    Node* mat = materials::create(g, nodeStem(preset) + kBakeMaterialSuffix + "1");
    PlugRef base = modifiers::editPlug(g, plane);
    geo::Mesh edit = *g.node(plane)->as<MeshPtr>(MeshNode::kInMesh);
    materials::assign(edit, mat->name(), {});
    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(edit))));
    return mat->name();
}

Node* findBakePlane(Graph& g, NodeId grp)
{
    for (NodeId c : g.children(grp))
        if (Node* n = g.node(c); n && n->type().isA("mesh") && n->name().find(kBakePlaneSuffix) != std::string::npos)
            return n;
    return nullptr;
}

/** Meshes under the group a bake reads: everything but the bake plane. */
std::vector<NodeId> bakeableMeshes(Graph& g, NodeId grp, NodeId plane)
{
    std::vector<NodeId> out;
    for (NodeId id : meshDescendants(g, grp))
        if (id != plane)
            out.push_back(id);
    return out;
}

void stampMetrics(Graph& g, NodeId grp, const json& values)
{
    g.evaluate();
    const WorldBox box = worldBox(g, meshDescendants(g, grp));
    const int up = text(values, "upAxis") == "Z" ? 2 : 1;
    const double low = box.valid() ? box.lo[up] : 0.0, high = box.valid() ? box.hi[up] : 0.0;
    const double plane = lowestHeight(values);
    setNum(g, grp, kAttrMinHeight, low);
    setNum(g, grp, kAttrMaxHeight, high);
    setNum(g, grp, kAttrHeightRange, high - low);
    setNum(g, grp, kAttrPlaneHeight, plane);
    setNum(g, grp, kAttrAbovePlane, high - plane);
}

struct Generated
{
    NodeId group = kNoNode;
    size_t instances = 0;
    std::vector<std::string> warnings;
};

Generated generate(const json& rawValues, const json& sources, const std::string& preset, const json& palette, const json& exp)
{
    Graph& g = graph();
    Generated out;
    const json values = sanitize(rawValues);
    json active = json::array();
    std::vector<Source> srcs = readSources(g, sources.is_array() ? sources : json::array(), active, out.warnings);
    if (srcs.empty())
    {
        out.warnings.push_back("No usable source meshes. Add at least one polygon mesh source.");
        return out;
    }

    const bool deformed = modifiesGeometry(values);
    const bool shear = flag(values, "shearEnabled");
    const bool shared = flag(values, "useInstances") && !deformed && !shear;
    if (flag(values, "useInstances") && !shared)
        out.warnings.push_back("Shared meshes are unavailable while any deformation or shear is enabled. Using copies.");

    const Plan p = plan(values, srcs);
    out.warnings.insert(out.warnings.end(), p.warnings.begin(), p.warnings.end());
    if (p.placements.empty())
        return out;
    const std::vector<InstanceGeometry> built = buildAll(values, srcs, p);

    Graph::Batch batch(g);
    const std::string up = text(values, "upAxis");
    const bool idMats = flag(values, "idMaterials");
    const bool masks = flag(values, "vertexColours");
    const bool combine = flag(values, "combineResult") && built.size() > 1 && !shared;
    if (flag(values, "combineResult") && built.size() > 1 && shared)
        out.warnings.push_back("Shared mesh output cannot be combined. Leaving the group intact.");

    std::set<int64_t> ids;
    for (const Source& s : srcs)
        ids.insert(s.objectId);
    const bool ground = flag(values, "groundPlane");
    const int64_t groundId = whole(values, "groundId");
    if (ground)
        ids.insert(groundId);
    if (idMats)
        for (int64_t id : ids)
            ensureIdMaterial(g, id, idColour(id, palette));

    Node* grp = g.createNode(*types::find(kGroupType), nodeStem(preset) + kGroupSuffix + "1");
    const NodeId gid = grp->id();

    // Per source: the mesh every instance starts from, already on its ID slot.
    std::vector<geo::Mesh> templates;
    for (const Source& s : srcs)
    {
        geo::Mesh m = s.mesh;
        if (idMats)
            materials::assign(m, idMaterialName(s.objectId), {});
        templates.push_back(std::move(m));
    }

    geo::Mesh groundMesh;
    if (ground)
    {
        groundMesh = tilePlane(num(values, "tileSize"), up, int(whole(values, "groundSubdivisions")));
        if (idMats)
            materials::assign(groundMesh, idMaterialName(groundId), {});
    }
    const double groundHeight = lowestHeight(values) + num(values, "groundHeight");

    if (combine)
    {
        geo::Mesh merged;
        for (size_t i = 0; i < built.size(); ++i)
        {
            geo::Mesh m = templates[p.placements[i].source];
            std::span<glm::dvec3> pos = m.positions();
            for (size_t k = 0; k < pos.size() && k < built[i].world.size(); ++k)
                pos[k] = built[i].world[k];
            if (masks)
                writeMasks(m, built[i].colours);
            merged.append(m);
        }
        if (ground)
        {
            geo::Mesh gm = groundMesh;
            const int upi = up == "Z" ? 2 : 1;
            for (glm::dvec3& q : gm.positions())
                q[upi] += groundHeight;
            merged.append(gm);
        }
        if (masks)
            geo::colors::setCurrent(merged, kColourSet);
        meshNode(g, nodeStem(preset) + "_combined", gid, std::move(merged));
        g.evaluate();
        const WorldBox box = worldBox(g, meshDescendants(g, gid));
        if (box.valid())
        {
            const glm::dvec3 centre = (box.lo + box.hi) * 0.5;
            g.setValue({gid, Transform::kRotatePivot, -1}, centre);
            g.setValue({gid, Transform::kScalePivot, -1}, centre);
        }
    }
    else
    {
        std::vector<MeshPtr> sharedMeshes;
        if (shared)
            for (geo::Mesh& t : templates)
                sharedMeshes.push_back(std::make_shared<geo::Mesh>(t));
        for (size_t i = 0; i < built.size(); ++i)
        {
            const Placement& pl = p.placements[i];
            const Source& s = srcs[pl.source];
            Node* n = g.createNode(*types::find("mesh"), shortName(s.name) + "_tile1", gid);
            g.setValue({n->id(), Transform::kRotatePivot, -1}, s.rotatePivot);
            g.setValue({n->id(), Transform::kScalePivot, -1}, s.scalePivot);
            if (shared)
            {
                glm::dvec3 scale(1.0);
                scale[s.axes.length] = pl.scale * pl.lengthFactor;
                scale[s.axes.width] = pl.scale * pl.widthFactor;
                scale[s.axes.thickness] = pl.scale * pl.thicknessFactor;
                const glm::dmat4 edit = glm::translate(glm::dmat4(1.0), s.scalePivot) * glm::scale(glm::dmat4(1.0), scale) *
                                        glm::translate(glm::dmat4(1.0), -s.scalePivot);
                g.setValue({n->id(), MeshNode::kInMesh, -1}, sharedMeshes[pl.source]);
                Transform::setLocalMatrix(g, n->id(), built[i].nodeMatrix * edit);
                continue;
            }
            geo::Mesh m = templates[pl.source];
            const glm::dmat4 toLocal = glm::inverse(built[i].nodeMatrix);
            std::span<glm::dvec3> pos = m.positions();
            for (size_t k = 0; k < pos.size() && k < built[i].world.size(); ++k)
                pos[k] = glm::dvec3(toLocal * glm::dvec4(built[i].world[k], 1.0));
            if (masks)
                writeMasks(m, built[i].colours);
            g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            Transform::setLocalMatrix(g, n->id(), built[i].nodeMatrix);
        }
        if (ground)
        {
            Node* gn = meshNode(g, nodeStem(preset) + kGroundSuffix + "1", gid, std::move(groundMesh));
            setUpOffset(g, gn->id(), up, groundHeight);
        }
    }
    out.instances = built.size();

    writeStored(g, gid, values, sources, preset, palette, idLegend(active, palette, values), exp);
    stampMetrics(g, gid, values);
    g.select({gid});
    out.group = gid;
    return out;
}

json generatedJson(Graph& g, const Generated& r)
{
    json w = json::array();
    for (const std::string& s : r.warnings)
    {
        log::warn("Ground Tiles: {}", s);
        w.push_back(s);
    }
    if (!r.group)
        return {{"group", nullptr}, {"instances", 0}, {"warnings", w}};
    return {{"group", g.node(r.group)->path()}, {"instances", r.instances}, {"warnings", w}};
}

Node* createBakePlane(Graph& g, const json& rawValues, const std::string& preset, NodeId parent)
{
    const json values = sanitize(rawValues);
    Node* plane = tilePlaneNode(g, values, kBakePlaneSuffix, preset, lowestHeight(values), 1, parent);
    bakePlaneMaterial(g, plane->id(), preset, true);
    g.select({plane->id()});
    return plane;
}

/** One mesh in world space with a BVH, and the colour each face bakes as. */
struct BakeMesh
{
    geo::Mesh mesh;
    geo::MeshBvh bvh;
    std::vector<glm::dvec3> faceColour;
    WorldBox box;
};

std::vector<BakeMesh> worldMeshes(Graph& g, const std::vector<NodeId>& ids, bool colours)
{
    std::vector<BakeMesh> out(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const Node& n = *g.node(ids[i]);
        const MeshPtr& m = n.as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        out[i].mesh = *m;
        const glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
        for (glm::dvec3& p : out[i].mesh.positions())
        {
            p = glm::dvec3(w * glm::dvec4(p, 1.0));
            out[i].box.add(p);
        }
        if (colours)
        {
            // A face without a material reads as the renderer's neutral default, as an unshaded face would bake.
            std::map<int32_t, glm::dvec3> bySlot;
            const geo::Attribute* mat = out[i].mesh.faceAttrs().find(geo::attr::kMaterial);
            out[i].faceColour.assign(out[i].mesh.faceCapacity(), glm::dvec3(0.5));
            for (geo::Index f = 0; f < out[i].mesh.faceCapacity(); ++f)
            {
                if (!out[i].mesh.faceValid(f))
                    continue;
                const int32_t slot = mat ? mat->values<int32_t>()[f] : 0;
                auto it = bySlot.find(slot);
                if (it == bySlot.end())
                {
                    glm::dvec3 c(0.5);
                    if (Node* mn = materials::find(g, materials::slotName(out[i].mesh, slot)))
                        c = mn->as<glm::dvec3>(material::kBaseColor);
                    it = bySlot.emplace(slot, c).first;
                }
                out[i].faceColour[f] = it->second;
            }
        }
    }
    tbb::parallel_for(size_t(0), out.size(), [&](size_t i) {
        if (!out[i].mesh.empty())
            out[i].bvh.build(out[i].mesh);
    });
    return out;
}

/** The ID colours straight down onto the plane's UVs: every texel takes the topmost face over it, no filtering. */
std::string bakeIdMap(Graph& g, NodeId grp, NodeId plane, const std::string& requested, int resolution)
{
    g.evaluate();
    const std::vector<NodeId> sources = bakeableMeshes(g, grp, plane);
    if (sources.empty())
        throw CommandError("groundTileBakeIdMap: nothing to bake onto the plane");
    fs::path path = utf8(requested);
    std::string ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext.empty())
        path.replace_extension(".png");
    if (path.has_parent_path())
        fs::create_directories(path.parent_path());

    const json values = readStored(*g.node(grp))["values"];
    const int up = text(values, "upAxis") == "Z" ? 2 : 1;
    std::vector<BakeMesh> meshes = worldMeshes(g, sources, true);
    WorldBox all;
    for (const BakeMesh& b : meshes)
        if (b.box.valid())
        {
            all.add(b.box.lo);
            all.add(b.box.hi);
        }
    const WorldBox planeBox = worldBox(g, {plane});
    // Rays start clear above everything so the topmost surface wins. Overshooting costs nothing.
    const double above = std::max(all.hi[up] - planeBox.hi[up], 0.0);
    const double below = std::max(planeBox.lo[up] - all.lo[up], 0.0);
    const double lift = above * 2.0 + std::max(above, 1.0) * 0.1;
    const double reach = lift + below + above + 1.0;

    const Node& pn = *g.node(plane);
    const MeshPtr& pm = pn.as<MeshPtr>(MeshNode::kOutMesh);
    const geo::Attribute* uvAttr = pm ? pm->cornerAttrs().find(geo::attr::uvName(0)) : nullptr;
    if (!pm || !uvAttr || uvAttr->type() != geo::AttrType::Float2)
        throw CommandError("groundTileBakeIdMap: the bake plane has no UVs");
    const glm::dmat4 pw = pn.as<glm::dmat4>(Transform::kWorldMatrix);
    std::span<const glm::vec2> uvs = uvAttr->values<glm::vec2>();

    struct Tri
    {
        glm::dvec2 uv[3];
        glm::dvec3 p[3];
        glm::dvec3 n;
    };
    std::vector<Tri> tris;
    for (geo::Index f = 0; f < pm->faceCapacity(); ++f)
    {
        if (!pm->faceValid(f))
            continue;
        const std::vector<geo::Index> corners = pm->faceCorners(f);
        const glm::dvec3 n = glm::normalize(glm::dvec3(glm::transpose(glm::inverse(glm::dmat3(pw))) * pm->faceNormal(f)));
        for (size_t k = 1; k + 1 < corners.size(); ++k)
        {
            Tri t;
            const geo::Index cs[3] = {corners[0], corners[k], corners[k + 1]};
            for (int j = 0; j < 3; ++j)
            {
                t.uv[j] = glm::dvec2(uvs[cs[j]]);
                t.p[j] = glm::dvec3(pw * glm::dvec4(pm->position(pm->cornerPoint(cs[j])), 1.0));
            }
            t.n = n;
            tris.push_back(t);
        }
    }

    const int size = std::clamp(resolution, 8, 16384);
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(qRgb(0, 0, 0));
    auto channel = [](double v) { return int(std::clamp(std::lround(v * 255.0), 0L, 255L)); };
    tbb::parallel_for(0, size, [&](int row) {
        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(row));
        // Image rows run top down while v runs up.
        const double v = 1.0 - (double(row) + 0.5) / double(size);
        for (int col = 0; col < size; ++col)
        {
            const glm::dvec2 at((double(col) + 0.5) / double(size), v);
            for (const Tri& t : tris)
            {
                const glm::dvec2 e0 = t.uv[1] - t.uv[0], e1 = t.uv[2] - t.uv[0], d = at - t.uv[0];
                const double den = e0.x * e1.y - e1.x * e0.y;
                if (std::abs(den) < 1e-20)
                    continue;
                const double b1 = (d.x * e1.y - e1.x * d.y) / den;
                const double b2 = (e0.x * d.y - d.x * e0.y) / den;
                if (b1 < -1e-9 || b2 < -1e-9 || b1 + b2 > 1.0 + 1e-9)
                    continue;
                const glm::dvec3 world = t.p[0] * (1.0 - b1 - b2) + t.p[1] * b1 + t.p[2] * b2;
                const glm::dvec3 origin = world + t.n * lift;
                double best = std::numeric_limits<double>::max();
                const glm::dvec3* colour = nullptr;
                for (const BakeMesh& b : meshes)
                {
                    if (b.bvh.empty())
                        continue;
                    geo::BvhHit hit;
                    if (b.bvh.raycast(origin, -t.n, hit, reach) && hit.t < best && hit.face < b.faceColour.size())
                    {
                        best = hit.t;
                        colour = &b.faceColour[hit.face];
                    }
                }
                if (colour)
                    line[col] = qRgb(channel(colour->r), channel(colour->g), channel(colour->b));
                break;
            }
        }
    });
    std::string fmt = utf8(path.extension()).substr(1);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (fmt == "TIFF")
        fmt = "TIF";
    if (!image.save(QString::fromStdString(utf8(path)), fmt.c_str()))
        throw CommandError("groundTileBakeIdMap: could not write " + utf8(path));
    return utf8(path);
}

HeightProfile measureHeight(Graph& g, NodeId grp, NodeId plane, int grid, std::string& refusal)
{
    g.evaluate();
    const std::vector<NodeId> sources = bakeableMeshes(g, grp, plane);
    if (sources.empty())
    {
        refusal = "Nothing to measure.";
        return {};
    }
    if (sources.size() > 8)
    {
        refusal = std::to_string(sources.size()) +
                  " separate meshes. Turn on Combine To Single Mesh and regenerate, ray casting against every instance is far too slow to be worth it.";
        return {};
    }
    const json values = readStored(*g.node(grp))["values"];
    const double tile = num(values, "tileSize");
    const double half = tile * 0.5;
    const int up = text(values, "upAxis") == "Z" ? 2 : 1;
    const int flat[2] = {0, up == 2 ? 1 : 2};
    const WorldBox planeBox = worldBox(g, {plane});
    const double floor = planeBox.hi[up];
    std::vector<BakeMesh> meshes = worldMeshes(g, sources, false);
    WorldBox box;
    for (const BakeMesh& b : meshes)
        if (b.box.valid())
        {
            box.add(b.box.lo);
            box.add(b.box.hi);
        }
    const double start = box.hi[up] + std::max(1.0, box.hi[up] - box.lo[up]);
    const glm::dvec3 centre = (planeBox.lo + planeBox.hi) * 0.5;
    glm::dvec3 dir(0.0);
    dir[up] = -1.0;

    std::vector<std::vector<double>> perRow(static_cast<size_t>(grid));
    tbb::parallel_for(0, grid, [&](int row) {
        for (const BakeMesh& b : meshes)
        {
            if (b.bvh.empty())
                continue;
            for (int column = 0; column < grid; ++column)
            {
                glm::dvec3 point(0.0);
                point[flat[0]] = centre[flat[0]] + ((double(column) + 0.5) / grid) * tile - half;
                point[flat[1]] = centre[flat[1]] + ((double(row) + 0.5) / grid) * tile - half;
                point[up] = start;
                geo::BvhHit hit;
                if (b.bvh.raycast(point, dir, hit, 1e6))
                    perRow[size_t(row)].push_back(hit.point[up] - floor);
            }
        }
    });
    std::vector<double> heights;
    for (const std::vector<double>& r : perRow)
        heights.insert(heights.end(), r.begin(), r.end());
    if (heights.empty())
    {
        refusal = "No hits. Is the bake plane under the geometry?";
        return {};
    }
    return profileOf(heights);
}

void writeJsonFile(const fs::path& path, const nlohmann::ordered_json& payload)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw CommandError("could not write " + utf8(path));
    f << payload.dump(4);
}

/** The palette file to hand edit. Only written when it is not already there. */
std::string writeColourSidecar(const fs::path& folder, const std::string& name, const json& legend)
{
    const fs::path path = folder / utf8(name + kColoursSuffix + ".json");
    if (fs::exists(path))
        return utf8(path);
    writeJsonFile(path, defaultColourRecipes(legend));
    return utf8(path);
}

/** The ID legend next to the FBX pair, for the Painter stack plugin; seeds the palette file too. */
std::pair<std::string, std::string> writeIdSidecar(Graph& g, NodeId grp, const fs::path& folder, const std::string& name)
{
    const Node& n = *g.node(grp);
    const json stored = readStored(n);
    const json& values = stored["values"];
    nlohmann::ordered_json payload;
    payload["tile"] = name;
    payload["preset"] = stored["preset"];
    payload["tileSize"] = values["tileSize"];
    payload["upAxis"] = values["upAxis"];
    payload["minHeight"] = n.as<double>(attrOf(n, kAttrMinHeight));
    payload["maxHeight"] = n.as<double>(attrOf(n, kAttrMaxHeight));
    payload["bakePlaneHeight"] = n.as<double>(attrOf(n, kAttrPlaneHeight));
    payload["heightAbovePlane"] = n.as<double>(attrOf(n, kAttrAbovePlane));
    payload["ids"] = nlohmann::ordered_json::parse(stored["legend"].dump());
    const fs::path path = folder / utf8(name + kIdsSuffix + ".json");
    writeJsonFile(path, payload);
    return {utf8(path), writeColourSidecar(folder, name, stored["legend"])};
}

json exportBakes(Graph& g, NodeId grp, const std::string& folderText, const std::string& name, const std::string& materialName)
{
    if (folderText.empty() || name.empty())
        throw CommandError("groundTileExport: set an export path and a file name first");
    g.evaluate();
    Node* plane = findBakePlane(g, grp);

    // The low poly's material name is what the baker names its outputs after, so it is renamed for the export only.
    // Checked before anything is touched.
    std::string original;
    if (plane && !materialName.empty())
    {
        const std::string current = bakePlaneMaterial(g, plane->id(), name, true);
        if (current != materialName)
        {
            bool taken = false;
            try
            {
                taken = g.find(materialName) != nullptr;
            }
            catch (const std::exception&)
            {
                taken = true;
            }
            if (taken)
                throw CommandError("groundTileExport: cannot rename the bake plane material to '" + materialName +
                                   "', a node by that name already exists. Rename it or pick another material name.");
            original = current;
        }
    }

    std::vector<std::string> high;
    for (NodeId c : g.children(grp))
        if (!plane || c != plane->id())
            high.push_back(g.fullPath(c));
    if (high.empty())
        throw CommandError("groundTileExport: " + g.node(grp)->path() + " has nothing to export");

    const fs::path folder = utf8(folderText);
    fs::create_directories(folder);
    const std::string highPath = utf8(folder / utf8(name + kHighSuffix + ".fbx"));
    const std::string lowPath = utf8(folder / utf8(name + kLowSuffix + ".fbx"));
    auto fbx = [](const std::string& path, const std::vector<std::string>& objects) {
        json args = json::array({path});
        for (const std::string& o : objects)
            args.push_back(o);
        ops::execute("exportFbx", {{"args", args}, {"kind", "static"}, {"profile", "generic"}, {"worldSpace", true}, {"triangulate", true},
                                   {"tangents", true}, {"smoothingGroups", true}});
    };

    std::string renamed;
    json result = json::object();
    try
    {
        if (!original.empty())
            renamed = ops::execute("renameMaterial", {{"args", {original, materialName}}}).get<std::string>();
        fbx(highPath, high);
        const auto [ids, colours] = writeIdSidecar(g, grp, folder, name);
        result = {{"high", highPath}, {"low", nullptr}, {"ids", ids}, {"colours", colours}};
        if (plane)
        {
            fbx(lowPath, {g.fullPath(plane->id())});
            result["low"] = lowPath;
        }
        else
            log::warn("Ground Tiles: no bake plane in {}, only the high poly was exported.", g.node(grp)->path());
    }
    catch (...)
    {
        if (!renamed.empty())
            ops::execute("renameMaterial", {{"args", {renamed, original}}});
        throw;
    }
    if (!renamed.empty())
        ops::execute("renameMaterial", {{"args", {renamed, original}}});

    json stored = readStored(*g.node(grp));
    json exp = stored["export"];
    exp["path"] = folderText;
    exp["name"] = name;
    exp["material"] = materialName;
    setStr(g, grp, kAttrExport, exp.dump());
    return result;
}

std::string findVertexMap(const std::string& folderText, const std::string& name)
{
    std::error_code ec;
    const fs::path folder = utf8(folderText);
    if (folderText.empty() || !fs::is_directory(folder, ec))
        return {};
    std::vector<std::string> candidates;
    for (const fs::directory_entry& e : fs::directory_iterator(folder, ec))
    {
        std::string ext = utf8(e.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (std::find(kBakeExtensions.begin(), kBakeExtensions.end(), ext) == kBakeExtensions.end())
            continue;
        std::string stem = utf8(e.path().stem());
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const char* suffix : kVertexMapSuffixes)
        {
            const std::string s = suffix;
            if (stem.size() >= s.size() && stem.compare(stem.size() - s.size(), s.size(), s) == 0)
            {
                candidates.push_back(utf8(e.path().filename()));
                break;
            }
        }
    }
    if (candidates.empty())
        return {};
    std::string lowName = name;
    std::transform(lowName.begin(), lowName.end(), lowName.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<std::string> preferred;
    for (const std::string& c : candidates)
    {
        std::string l = c;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        if (l.rfind(lowName, 0) == 0)
            preferred.push_back(c);
    }
    std::vector<std::string>& pick = preferred.empty() ? candidates : preferred;
    std::sort(pick.begin(), pick.end());
    return utf8(folder / utf8(pick.front()));
}

/** Three greyscale masks from an RGB vertex colour bake: Painter greys an RGB image before any effect sees it. */
std::vector<std::string> splitMasks(const std::string& path)
{
    QImage image;
    if (path.empty() || !fs::is_regular_file(utf8(path)) || !image.load(QString::fromStdString(path)))
        return {};
    image = image.convertToFormat(QImage::Format_RGB32);
    const std::string stem = maskStem(path);
    std::vector<std::string> written;
    for (int c = 0; c < 3; ++c)
    {
        QImage out(image.width(), image.height(), QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
        {
            const QRgb* in = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
            {
                const int v = c == 0 ? qRed(in[x]) : c == 1 ? qGreen(in[x]) : qBlue(in[x]);
                o[x] = qRgb(v, v, v);
            }
        }
        const std::string target = stem + kMaskSuffixes[size_t(c)] + ".png";
        if (out.save(QString::fromStdString(target), "PNG"))
            written.push_back(target);
    }
    return written;
}

AttrDef stringAttr(const char* name, const char* doc)
{
    return {.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = doc};
}
AttrDef metricAttr(const char* name, const char* doc)
{
    return {.name = name, .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrKeyable, .doc = doc, .category = "Height"};
}

bool registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        return false;
    NodeType t{.name = kGroupType,
               .base = transform,
               .dag = true,
               .doc = "A generated, seamlessly tiling ground tile. Holds the settings and sources that built it and its measured height range.",
               .icon = "grid-dots",
               .create = transform->create};
    t.ownAttrs = {
        stringAttr(kAttrSettings, "The generator settings that built this group, JSON."),
        stringAttr(kAttrSources, "The source meshes with their weight, Object ID, height offset and axis overrides, JSON."),
        stringAttr(kAttrPreset, "The preset the settings came from."),
        stringAttr(kAttrVersion, "The Ground Tiles version that built it."),
        stringAttr(kAttrPalette, "The Object ID colours in use when it was built, JSON."),
        stringAttr(kAttrIds, "Each Object ID's colour and the sources that landed on it, JSON."),
        stringAttr(kAttrExport, "Where Export FBX Pair writes: path, name and bake plane material, JSON."),
        metricAttr(kAttrMinHeight, "Lowest vertex in the group, along the up axis."),
        metricAttr(kAttrMaxHeight, "Highest vertex in the group, along the up axis."),
        metricAttr(kAttrHeightRange, "Max minus min height."),
        metricAttr(kAttrPlaneHeight, "Where Create Bake Plane puts the plane."),
        metricAttr(kAttrAbovePlane, "Max height minus the plane: the outright range the geometry spans."),
        metricAttr(kAttrHeightP50, "Measure Height Range: half the tile is below this, above the plane."),
        metricAttr(kAttrHeightP99, "Measure Height Range: 99% of the tile is below this. Scale a height bake by it."),
        metricAttr(kAttrHeightTop, "Measure Height Range: the tallest point that is actually over the plane."),
        {.name = kAttrMeasured, .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Whether Measure Height Range has stamped the p50, p99 and over-tile heights."},
    };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}

json valuesArg(const Args& a, const json& fallback)
{
    if (a.has("values") && a.flags["values"].is_object())
    {
        json merged = fallback.is_object() ? fallback : json::object();
        merged.update(a.flags["values"]);
        return sanitize(merged);
    }
    return sanitize(fallback);
}

json prefsPalette() { return loadPrefs()["Palette"]; }

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;

    ctx.registerCommand({
        .name = "groundTileGenerate",
        .doc = "Scatter the source meshes into a seamlessly tiling ground tile group with the given settings (a preset's when values is "
               "partial). Returns {group, instances, warnings}; group is null when nothing could be built.",
        .flags = {{"values", "v", T::Any, nullptr, "Settings, as groundTileParameters lists them; missing ones come from the preset."},
                  {"sources", "s", T::List, nullptr, "[{name, weight, objectId, heightOffset, enabled, lengthAxis, baseEnd}, ...]."},
                  {"preset", "p", T::String, nullptr, "Preset the settings start from and the group is named after."},
                  {"palette", "", T::Any, nullptr, "{\"<id>\": [r, g, b]} ID colours; default the saved palette."},
                  {"export", "", T::Any, nullptr, "{path, name, material} stamped on the group for Export FBX Pair."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const json prefs = loadPrefs();
            const std::string preset = a.get<std::string>("preset", defaultPreset());
            const json base = presetSettings(preset, prefs["Presets"]);
            const json values = valuesArg(a, base);
            if (!a.has("sources") || !a.flags["sources"].is_array() || a.flags["sources"].empty())
                throw CommandError("groundTileGenerate: add at least one source mesh (sources=[{\"name\": ...}])");
            const json palette = a.has("palette") ? a.flags["palette"] : prefs["Palette"];
            const json exp = a.has("export") ? a.flags["export"] : prefs["Export"];
            return generatedJson(g, generate(values, a.flags["sources"], preset, palette, exp));
        },
    });

    ctx.registerCommand({
        .name = "groundTileRegenerate",
        .doc = "Delete a generated group and build it again, from its stored settings or the ones given. Returns as groundTileGenerate.",
        .positionalDoc = "group (default the selection)",
        .flags = {{"values", "v", T::Any, nullptr, "Settings to change; the rest stay as stored."},
                  {"sources", "s", T::List, nullptr, "Replacement source list."},
                  {"preset", "p", T::String, nullptr, "Preset name to store."},
                  {"palette", "", T::Any, nullptr, "ID colours; default the stored ones."},
                  {"export", "", T::Any, nullptr, "Export settings; default the stored ones."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileRegenerate");
            const json stored = readStored(*grp);
            const json values = valuesArg(a, stored["values"]);
            const json sources = a.has("sources") ? a.flags["sources"] : stored["sources"];
            const std::string preset = a.get<std::string>("preset", stored["preset"].get<std::string>());
            const json palette = a.has("palette") ? a.flags["palette"] : stored["palette"];
            const json exp = a.has("export") ? a.flags["export"] : stored["export"];
            ops::execute("delete", {{"args", {g.fullPath(grp->id())}}});
            return generatedJson(g, generate(values, sources, preset, palette, exp));
        },
    });

    ctx.registerCommand({
        .name = "groundTileDelete",
        .doc = "Delete a generated ground tile group. Returns its name.",
        .positionalDoc = "group (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileDelete");
            const std::string name = grp->path();
            ops::execute("delete", {{"args", {g.fullPath(grp->id())}}});
            return name;
        },
    });

    ctx.registerCommand({
        .name = "groundTileCreateBakePlane",
        .doc = "A tile-sized plane with 0-1 UVs and its own material to bake onto, at the lowest height layer. Inside the generated "
               "group when one is given or selected, taking its settings. Returns the plane.",
        .positionalDoc = "group (default the selection; none for a loose plane)",
        .flags = {{"values", "v", T::Any, nullptr, "Settings for the tile size, up axis and layers; default the group's or the preset's."},
                  {"preset", "p", T::String, nullptr, "Preset the plane is named after."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileCreateBakePlane", false);
            const json prefs = loadPrefs();
            const json stored = grp ? readStored(*grp) : json::object();
            const std::string preset = a.get<std::string>("preset", grp ? stored["preset"].get<std::string>() : defaultPreset());
            const json base = grp ? stored["values"] : presetSettings(preset, prefs["Presets"]);
            return createBakePlane(g, valuesArg(a, base), preset, grp ? grp->id() : kNoNode)->path();
        },
    });

    ctx.registerCommand({
        .name = "groundTileBakeIdMap",
        .doc = "Bake the Object ID colours of a generated group straight down onto its bake plane's UVs and write the image: exact "
               "palette values, one colour per texel, no filtering, black where nothing is over the tile. Returns the path.",
        .positionalDoc = "group (default the selection)",
        .flags = {{"path", "", T::String, nullptr, "Image file; default <export path>/<name>_id.png."},
                  {"resolution", "r", T::Int, nullptr, "Pixels per side; default the group's Bake Map Size.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileBakeIdMap");
            Node* plane = findBakePlane(g, grp->id());
            if (!plane)
                throw CommandError("groundTileBakeIdMap: " + grp->path() + " has no bake plane. Create one first.");
            const json stored = readStored(*grp);
            std::string path = a.get<std::string>("path", "");
            if (path.empty())
            {
                const std::string folder = stored["export"].value("path", ""), name = stored["export"].value("name", "");
                if (folder.empty() || name.empty())
                    throw CommandError("groundTileBakeIdMap: give path=, or set an export path and name");
                path = utf8(utf8(folder) / utf8(name + kIdMapSuffix + ".png"));
            }
            const int res = a.has("resolution") ? a.get<int>("resolution") : int(whole(stored["values"], "bakeResolution"));
            return bakeIdMap(g, grp->id(), plane->id(), path, res);
        },
    });

    ctx.registerCommand({
        .name = "groundTileMeasureHeight",
        .doc = "Sample the top surface straight down over the tile and stamp where it actually sits: heightP50, heightP99 (scale a "
               "height bake by this) and heightOverTile. Needs a bake plane and a combined mesh. Returns the numbers and the bake "
               "settings they give.",
        .positionalDoc = "group (default the selection)",
        .flags = {{"grid", "", T::Int, kHeightSampleGrid, "Samples per side.", {}, {}, 1.0}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileMeasureHeight");
            Node* plane = findBakePlane(g, grp->id());
            if (!plane)
                throw CommandError("groundTileMeasureHeight: " + grp->path() + " has no bake plane to measure against");
            std::string refusal;
            const HeightProfile p = measureHeight(g, grp->id(), plane->id(), std::max(1, a.get<int>("grid", kHeightSampleGrid)), refusal);
            if (!refusal.empty())
                throw CommandError("groundTileMeasureHeight: " + refusal);
            const NodeId id = grp->id();
            setNum(g, id, kAttrHeightP50, p.p50);
            setNum(g, id, kAttrHeightP99, p.p99);
            setNum(g, id, kAttrHeightTop, p.top);
            g.setValue({id, attrOf(*grp, kAttrMeasured), -1}, true);
            const double below = std::max(0.0, grp->as<double>(attrOf(*grp, kAttrPlaneHeight)) - grp->as<double>(attrOf(*grp, kAttrMinHeight)));
            const json numbers = {{"cageMax", p.top * 1.2}, {"cageMin", std::max(1.0, below * 1.2)}, {"heightOuter", p.p99}, {"heightInner", below}};
            log::info("{} bake settings\n  cage max / outer offset : {:.3f}\n  cage min / inner offset : {:.3f}\n  height outer distance   : {:.3f}\n"
                      "  height inner distance   : {:.3f}\n  half the tile is under  : {:.3f}\n  tallest over the tile   : {:.3f}",
                      grp->path(), p.top * 1.2, std::max(1.0, below * 1.2), p.p99, below, p.p50, p.top);
            return {{kAttrHeightP50, p.p50}, {kAttrHeightP99, p.p99}, {kAttrHeightTop, p.top}, {"samples", p.hits}, {"bake", numbers}};
        },
    });

    ctx.registerCommand({
        .name = "groundTileExport",
        .doc = "Write a generated group as <name>_Hi.fbx (everything but the bake plane) and its bake plane as <name>_Lo.fbx, world "
               "space, with <name>_ids.json and, when missing, <name>_colours.json for the Painter stack plugin. The plane's material "
               "is named material for the export only. Returns {high, low, ids, colours}.",
        .positionalDoc = "group (default the selection)",
        .flags = {{"path", "", T::String, nullptr, "Folder; default the group's export path."},
                  {"name", "n", T::String, nullptr, "File name stem; default the group's."},
                  {"material", "m", T::String, nullptr, "Bake plane material name for the export; default the group's."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileExport");
            const json exp = readStored(*grp)["export"];
            return exportBakes(g, grp->id(), a.get<std::string>("path", exp.value("path", "")), a.get<std::string>("name", exp.value("name", "")),
                               a.get<std::string>("material", exp.value("material", "")));
        },
    });

    ctx.registerCommand({
        .name = "groundTileSplitMasks",
        .doc = "Split a baked vertex colour map into <tile>_maskR/G/B.png greyscale masks (R root to tip, G per instance, B height "
               "layer). With no path, finds the map in folder. Returns the written paths.",
        .positionalDoc = "path (optional)",
        .flags = {{"folder", "", T::String, nullptr, "Where to look for the vertex colour map when no path is given."},
                  {"name", "n", T::String, "", "Tile name preferred when several maps are there."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string path = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "";
            if (path.empty())
                path = findVertexMap(a.get<std::string>("folder", ""), a.get<std::string>("name", ""));
            if (path.empty())
                throw CommandError("groundTileSplitMasks: no vertex colour map found. Bake one first.");
            json out = json::array();
            for (const std::string& p : splitMasks(path))
                out.push_back(p);
            if (out.empty())
                throw CommandError("groundTileSplitMasks: could not read " + path);
            return out;
        },
    });

    ctx.registerCommand({
        .name = "groundTileFindVertexMap",
        .doc = "The baked vertex colour map in a folder (a name ending vertexcolor, vertexcolour, vertex_color or vertexcolormap), "
               "preferring ones starting with name. Null when there is none.",
        .positionalDoc = "folder",
        .flags = {{"name", "n", T::String, "", "Tile name."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string folder = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "";
            const std::string found = findVertexMap(folder, a.get<std::string>("name", ""));
            return found.empty() ? json(nullptr) : json(found);
        },
    });

    ctx.registerCommand({
        .name = "groundTileSettings",
        .doc = "What a generated group stores: {values, sources, preset, version, palette, legend, export, metrics}. Null when the node "
               "is not, and is not under, a generated group.",
        .positionalDoc = "node (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* grp = groupArg(g, a, "groundTileSettings", false);
            if (!grp)
                return nullptr;
            json out = readStored(*grp);
            out["group"] = grp->path();
            out["metrics"] = metricsOf(*grp);
            if (Node* plane = findBakePlane(g, grp->id()))
                out["bakePlane"] = plane->path();
            return out;
        },
    });

    ctx.registerCommand({
        .name = "groundTileBakeData",
        .doc = "What a bake project needs from an exported tile: the high and low FBX paths, the sidecars, the bake plane and its "
               "material, the height range and the ID palette.",
        .positionalDoc = "group (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            return bakeHandoff(groupArg(g, a, "groundTileBakeData")->path()).toJson();
        },
    });

    ctx.registerCommand({
        .name = "groundTileTextureSet",
        .doc = "Make a texture set for a generated group, painted natively instead of through Painter's stack script: a bake project from "
               "the exported tile (its ID palette, terrainMasks colour set, AO and the measured height range), then layers bound to them: "
               "a ground fill, the baked height, and per Object ID a folder masked by ID Select with the ID's colour recipe (base, root, "
               "per instance, tip, occlusion; the colours sidecar when exported). Export FBX Pair first for the mesh maps. Returns {set, "
               "bakeProject, ids, layers, warnings}.",
        .positionalDoc = "group (default the selection)",
        .flags = {{"name", "", T::String, nullptr, "Set name; <Preset>_Tex by default."},
                  {"target", "", T::String, "unreal", "Bake target and export preset."},
                  {"bake", "", T::Bool, true, "Make the bake project when the tile is exported."},
                  {"replace", "", T::Bool, false, "Replace a texture set of that name."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            return createTextureSet(g, *groupArg(g, a, "groundTileTextureSet"), a);
        },
    });

    ctx.registerCommand({
        .name = "groundTileSourceInfo",
        .doc = "What the generator reads off a source mesh: its length, width and thickness axes, which end is the base, and how far "
               "it reaches from its pivot. {valid: False} when it is not a mesh.",
        .positionalDoc = "mesh",
        .flags = {{"lengthAxis", "", T::String, "Auto", "Auto, X, Y or Z.", {"Auto", "X", "Y", "Z"}},
                  {"baseEnd", "", T::String, "Auto", "Auto, Min or Max.", {"Auto", "Min", "Max"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "";
            json active = json::array();
            std::vector<std::string> warnings;
            const json entry = {{"name", name}, {"lengthAxis", a.get<std::string>("lengthAxis", "Auto")}, {"baseEnd", a.get<std::string>("baseEnd", "Auto")}};
            std::vector<Source> s = readSources(g, json::array({entry}), active, warnings);
            if (s.empty())
                return {{"valid", false}};
            const Axes& ax = s[0].axes;
            return {{"valid", true},
                    {"length", kAxisNames[size_t(ax.length)]},
                    {"width", kAxisNames[size_t(ax.width)]},
                    {"thickness", kAxisNames[size_t(ax.thickness)]},
                    {"baseSign", ax.baseSign},
                    {"describe", describeAxes(ax)},
                    {"reach", s[0].reach},
                    {"bboxMin", {ax.min.x, ax.min.y, ax.min.z}},
                    {"bboxMax", {ax.max.x, ax.max.y, ax.max.z}}};
        },
    });

    ctx.registerCommand({
        .name = "groundTileParameters",
        .doc = "The settings schema: [{name, label, type, default, min, max, options, section, showIf, help}], and the sections.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json params = json::array();
            for (const Param& p : parameters())
            {
                json e = {{"name", p.name}, {"label", p.label}, {"type", p.type}, {"default", p.def}, {"section", p.section}, {"help", p.help}};
                if (p.type == "float" || p.type == "int")
                {
                    e["min"] = p.min;
                    e["max"] = p.max;
                }
                if (!p.options.empty())
                    e["options"] = p.options;
                if (!p.showIf.empty())
                    e["showIf"] = {p.showIf, p.showValues};
                params.push_back(e);
            }
            json secs = json::array();
            for (const auto& [id, label] : sections())
                secs.push_back({id, label});
            return {{"parameters", params}, {"sections", secs}};
        },
    });

    ctx.registerCommand({
        .name = "groundTilePresets",
        .doc = "Preset names: {builtIn, user, default}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            const json prefs = loadPrefs();
            json user = json::array();
            for (auto it = prefs["Presets"].begin(); it != prefs["Presets"].end(); ++it)
                if (!builtInOverrides(it.key()))
                    user.push_back(it.key());
            return {{"builtIn", builtInPresets()}, {"user", user}, {"default", prefs["_default"]}};
        },
    });

    ctx.registerCommand({
        .name = "groundTilePresetSettings",
        .doc = "A preset's full settings, user saved or built in.",
        .positionalDoc = "preset",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : defaultPreset();
            const json prefs = loadPrefs();
            if (!builtInOverrides(name) && !prefs["Presets"].contains(name))
                throw CommandError("groundTilePresetSettings: no preset named '" + name + "'");
            return presetSettings(name, prefs["Presets"]);
        },
    });

    ctx.registerCommand({
        .name = "groundTileSavePreset",
        .doc = "Save settings as a user preset in the Lila prefs. Returns its name.",
        .positionalDoc = "name",
        .flags = {{"values", "v", T::Any, nullptr, "The settings."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "";
            if (name.empty())
                throw CommandError("groundTileSavePreset: a preset needs a name");
            json prefs = loadPrefs();
            prefs["Presets"][name] = sanitize(a.has("values") ? a.flags["values"] : json::object());
            savePrefs(prefs);
            return name;
        },
    });

    ctx.registerCommand({
        .name = "groundTileDeletePreset",
        .doc = "Delete a user preset. Built-in presets cannot be deleted.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : "";
            if (builtInOverrides(name))
                throw CommandError("groundTileDeletePreset: built-in presets cannot be deleted");
            json prefs = loadPrefs();
            prefs["Presets"].erase(name);
            if (prefs["_default"] == name)
                prefs["_default"] = defaultPreset();
            savePrefs(prefs);
            return name;
        },
    });

    ctx.registerCommand({
        .name = "groundTileSetDefaultPreset",
        .doc = "The preset the panel opens with.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : defaultPreset();
            json prefs = loadPrefs();
            prefs["_default"] = name;
            savePrefs(prefs);
            return name;
        },
    });

    ctx.registerCommand({
        .name = "groundTileSetIdColour",
        .doc = "Set the colour an Object ID bakes as, in the saved palette. Returns the palette.",
        .positionalDoc = "objectId",
        .flags = {{"colour", "c", T::Vec3, nullptr, "[r, g, b], 0 to 1, exact (no gamma)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_number() || !a.has("colour"))
                throw CommandError("groundTileSetIdColour: give an id and colour=[r, g, b]");
            json prefs = loadPrefs();
            const json& c = a.flags["colour"];
            prefs["Palette"][std::to_string(a.positional[0].get<int64_t>())] = {c.at(0).get<double>(), c.at(1).get<double>(), c.at(2).get<double>()};
            savePrefs(prefs);
            return prefs["Palette"];
        },
    });
}
/** The keys this extension owns in its prefs file. */
constexpr const char* kPrefKeys[] = {"Presets", "Palette", "_default", "Export", "Window"};

/** Copies presets and settings from terrainGenerator.json into this extension's file, which has none yet. */
json adoptTerrainGeneratorPrefs()
{
    json data = json::object();
    const json src = PrefStore::get().merged("terrainGenerator");
    if (!src.is_object())
        return data;
    for (const char* key : kPrefKeys)
        if (src.contains(key))
        {
            data[key] = src[key];
            PrefStore::get().set(kPrefsFile, std::string("/") + key, src[key]);
        }
    return data;
}
} // namespace

json loadPrefs()
{
    json data = PrefStore::get().merged(kPrefsFile);
    if (!data.is_object() || data.empty())
        data = adoptTerrainGeneratorPrefs();
    if (!data.contains("Presets") || !data["Presets"].is_object())
        data["Presets"] = json::object();
    if (!data.contains("Palette") || !data["Palette"].is_object())
        data["Palette"] = json::object();
    if (!data.contains("_default") || !data["_default"].is_string())
        data["_default"] = defaultPreset();
    json exp = {{"path", ""}, {"name", ""}, {"material", ""}};
    if (data.contains("Export") && data["Export"].is_object())
        exp.update(data["Export"]);
    data["Export"] = exp;
    json window = {{"autoLoadSelection", true}};
    if (data.contains("Window") && data["Window"].is_object())
        window.update(data["Window"]);
    data["Window"] = window;
    return data;
}

void savePrefs(const json& prefs)
{
    for (const char* key : kPrefKeys)
        if (prefs.contains(key))
            PrefStore::get().set(kPrefsFile, std::string("/") + key, prefs[key]);
}

json BakeHandoff::toJson() const
{
    return {{"group", group},
            {"high", highPath},
            {"low", lowPath},
            {"ids", idsPath},
            {"colours", coloursPath},
            {"idMap", idMapPath},
            {"bakePlane", bakePlane},
            {"bakeMaterial", bakeMaterial},
            {"upAxis", upAxis},
            {"tileSize", tileSize},
            {"minHeight", minHeight},
            {"maxHeight", maxHeight},
            {"bakePlaneHeight", bakePlaneHeight},
            {"heightAbovePlane", heightAbovePlane},
            {"measured", measured},
            {"heightP50", heightP50},
            {"heightP99", heightP99},
            {"heightOverTile", heightOverTile},
            {"legend", legend}};
}

BakeHandoff bakeHandoff(const std::string& groupName)
{
    Graph& g = graph();
    Node* n = findNode(g, groupName);
    Node* grp = n ? generatedFor(g, n->id()) : nullptr;
    if (!grp)
        throw CommandError("groundTileBakeData: " + groupName + " is not a generated ground tile group");
    const json stored = readStored(*grp);
    BakeHandoff h;
    h.group = grp->path();
    h.upAxis = text(stored["values"], "upAxis");
    h.tileSize = num(stored["values"], "tileSize");
    h.legend = stored["legend"];
    auto metric = [&](const char* a) { return grp->as<double>(attrOf(*grp, a)); };
    h.minHeight = metric(kAttrMinHeight);
    h.maxHeight = metric(kAttrMaxHeight);
    h.bakePlaneHeight = metric(kAttrPlaneHeight);
    h.heightAbovePlane = metric(kAttrAbovePlane);
    h.measured = grp->as<bool>(attrOf(*grp, kAttrMeasured));
    h.heightP50 = metric(kAttrHeightP50);
    h.heightP99 = metric(kAttrHeightP99);
    h.heightOverTile = metric(kAttrHeightTop);
    if (Node* plane = findBakePlane(g, grp->id()))
    {
        h.bakePlane = plane->path();
        h.bakeMaterial = bakePlaneMaterial(g, plane->id(), stored["preset"].get<std::string>(), false);
    }
    const std::string folder = stored["export"].value("path", ""), name = stored["export"].value("name", "");
    if (!folder.empty() && !name.empty())
    {
        const fs::path f = utf8(folder);
        auto existing = [&](const std::string& file) {
            const fs::path p = f / utf8(file);
            return fs::exists(p) ? utf8(p) : std::string();
        };
        h.highPath = existing(name + kHighSuffix + ".fbx");
        h.lowPath = existing(name + kLowSuffix + ".fbx");
        h.idsPath = existing(name + kIdsSuffix + ".json");
        h.coloursPath = existing(name + kColoursSuffix + ".json");
        h.idMapPath = existing(name + kIdMapSuffix + ".png");
    }
    return h;
}

namespace
{
/** A per-ID colour recipe from the colours sidecar when the export wrote one (hand edits win), else the defaults. */
json colourRecipes(const BakeHandoff& h)
{
    if (!h.coloursPath.empty())
        if (std::ifstream f{utf8(h.coloursPath)}; f)
            if (json j = json::parse(f, nullptr, false); j.is_object() && !j.empty())
                return j;
    return json::parse(defaultColourRecipes(h.legend).dump());
}

json layerId(const json& r) { return r.at("id"); }

/** Makes the bake project the set reads its mesh maps from, when the tile is exported; else says why not. */
std::string bakeProjectFor(const BakeHandoff& h, const std::string& target, json& warnings)
{
    if (h.highPath.empty() || h.bakePlane.empty())
    {
        warnings.push_back("no bake project: create a bake plane and Export FBX Pair, then texSetSettings(set, {\"bakeProject\": ...}); "
                           "until then the mesh maps read as defaults");
        return {};
    }
    const json made = ops::execute("createBakeFromGenerator", {{"args", {h.group}}, {"kind", "groundTile"}, {"target", target}});
    const std::string project = made.at("project").get<std::string>();
    const double range = h.measured && h.heightP99 > 0.0 ? h.heightP99 : h.heightAbovePlane;
    ops::execute("setBakeMap", {{"args", {"vertexColor"}}, {"project", project}, {"enabled", true}, {"options", {{"set", kColourSet}}}});
    ops::execute("setBakeMap", {{"args", {"ao"}}, {"project", project}, {"enabled", true}});
    json height = {{"args", {"height"}}, {"project", project}, {"enabled", true}};
    if (range > 0.0)
        height["options"] = {{"range", range}};
    ops::execute("setBakeMap", height);
    return project;
}
} // namespace

json createTextureSet(Graph& g, Node& grp, const Args& a)
{
    const BakeHandoff h = bakeHandoff(grp.path());
    const json stored = readStored(grp);
    const std::string preset = stored.value("preset", std::string());
    const std::string name = a.get<std::string>("name", nodeStem(preset.empty() ? grp.name() : preset) + "_Tex");
    const std::string target = a.get<std::string>("target", "unreal");
    json warnings = json::array();

    UndoChunk chunk("Tile Texture Set");
    if (a.get<bool>("replace", false))
        if (Node* old = findNode(g, name); old && old->type().name == "textureSet")
            g.deleteNode(old->id());
    const std::string project = a.get<bool>("bake", true) ? bakeProjectFor(h, target, warnings) : std::string();
    const int res = int(num(stored["values"], "bakeResolution"));
    const std::string material = h.bakeMaterial.empty() ? nodeStem(preset) + kBakeMaterialSuffix + "1" : h.bakeMaterial;
    json settings = {{"bakeProject", project}, {"exportPreset", target}};
    if (res > 0)
        settings["width"] = settings["height"] = res;
    const std::string set = ops::execute("texCreateSet", {{"args", {name}}, {"materials", {material}}, {"settings", settings}}).get<std::string>();

    const json recipes = colourRecipes(h);
    std::vector<int64_t> ids;
    for (auto it = h.legend.begin(); it != h.legend.end(); ++it)
        ids.push_back(std::stoll(it.key()));
    std::sort(ids.begin(), ids.end());

    auto fill = [&](const std::string& layerName, const json& material, const json& extra) {
        json call = {{"args", {set, "fill"}}, {"name", layerName}, {"material", material}};
        call.update(extra);
        return layerId(ops::execute("texAddLayer", call)).get<std::string>();
    };
    auto mask = [&](const std::string& layer, const std::string& type, const json& values, const json& bindings, bool invert) {
        ops::execute("texAddGenerator", {{"args", {set, layer, type}}, {"values", values}, {"bindings", bindings}, {"invert", invert}});
    };

    const json ground = recipes.contains("-1") ? recipes["-1"] : json{{"base", "#30211A"}, {"roughness", 0.8}};
    fill("Ground", {{"baseColor", ground.value("base", std::string("#30211A"))}, {"roughness", ground.value("roughness", 0.8)}}, json::object());
    // The baked height drives the height channel (and through it the normals); its range is the tile's measured one.
    const json heightGraph = {{"name", "BakedHeight"},
                              {"inputs", json::array()},
                              {"outputs", {{{"name", "height"}, {"type", "gray"}}}},
                              {"nodes", {{{"name", "GroupInput"}, {"type", "GroupInput"}, {"position", {-400, 0}}},
                                         {{"name", "GroupOutput"}, {"type", "GroupOutput"}, {"position", {0, 0}}},
                                         {{"name", "Height"}, {"type", "meshHeight"}, {"position", {-200, 0}}}}},
                              {"links", {{"Height", "out", "GroupOutput", "height"}}}};
    ops::execute("texAddLayer", {{"args", {set, "procedural"}}, {"name", "Baked Height"}, {"graph", heightGraph}, {"channels", {{"height", {{"mode", "normal"}}}}}});

    int layers = 2;
    for (int64_t id : ids)
    {
        const std::string key = std::to_string(id);
        const json& entry = h.legend[key];
        const json r = recipes.contains(key) ? recipes[key] : json::object();
        std::string label = "ID " + key;
        if (entry.contains("sources") && !entry["sources"].empty())
            label += " " + entry["sources"][0].get<std::string>();
        const std::string folder = layerId(ops::execute("texAddLayer", {{"args", {set, "folder"}}, {"name", label}})).get<std::string>();
        json colour = entry.value("colour", json::array({0, 0, 0}));
        if (colour.is_array() && colour.size() == 3)
            colour.push_back(1.0);
        mask(folder, "genIdSelect", {{"color", colour}, {"tolerance", 0.02}}, {{"id", "id"}}, false);
        const json in = {{"parent", folder}};
        fill("Base", {{"baseColor", r.value("base", std::string("#708A3D"))}, {"roughness", r.value("roughness", 0.5)}}, in);
        if (id >= 0)
        {
            // terrainMasks: R root (0) to tip (1), G a random value per instance.
            mask(fill("Root", {{"baseColor", r.value("root", std::string("#516A30"))}}, in), "maskAxis", {{"axis", "x"}, {"start", 0.0}, {"end", 0.6}},
                 {{"position", "vertexColor"}}, true);
            mask(fill("Per Instance", {{"baseColor", r.value("perInstance", std::string("#516A30"))}}, json{{"parent", folder}, {"channels", {{"baseColor", {{"opacity", 0.5}}}}}}),
                 "maskAxis", {{"axis", "y"}, {"start", 0.0}, {"end", 1.0}}, {{"position", "vertexColor"}}, false);
            mask(fill("Tip", {{"baseColor", r.value("tip", std::string("#91A84C"))}}, in), "maskAxis", {{"axis", "x"}, {"start", 0.5}, {"end", 1.0}},
                 {{"position", "vertexColor"}}, false);
            layers += 3;
        }
        mask(fill("Occlusion", {{"baseColor", r.value("occlusion", std::string("#516A30"))}}, in), "genAoDarken", json::object(), {{"ao", "ao"}}, false);
        layers += 3;
    }
    return {{"set", set}, {"bakeProject", project.empty() ? json() : json(project)}, {"ids", ids.size()}, {"layers", layers}, {"warnings", warnings}};
}

bool registerScene(ext::Context& ctx)
{
    if (!registerTypes(ctx))
        return false;
    registerCommands(ctx);
    return true;
}
} // namespace groundtiles
