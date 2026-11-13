// Tree Generator: grows trunks with recursive branches, drops canopy blobs on the branch tips and scatters a leaf mesh
// over them, bending each copy onto the canopy. TreeBuild does the geometry; this file puts it in the scene through
// commands, stamps the settings on what it built and exports the FBX pairs and json the texturing plugin reads.

#include "TreeGenerator.h"
#include "TreeBuild.h"
#include "TreeSchema.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Prefs.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/Undo.h>

#include <QImage>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace treegen
{
namespace
{
Graph& graph() { return Graph::current(); }

fs::path utf8(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

int attrOf(const Node& n, const char* name) { return n.type().attrIndex(name); }

bool ours(const Node& n) { return attrOf(n, kAttrSettings) >= 0; }

std::string str(const Node& n, const char* name)
{
    const int a = attrOf(n, name);
    return a < 0 ? std::string() : n.as<std::string>(a);
}

void setStr(Graph& g, NodeId id, const char* name, const std::string& v)
{
    if (Node* n = g.node(id); n && attrOf(*n, name) >= 0)
        g.setValue({id, attrOf(*n, name), -1}, v);
}

int64_t lodOf(const Node& n)
{
    const int a = attrOf(n, kAttrLod);
    return a < 0 ? -1 : n.as<int64_t>(a);
}

bool hasSettings(const Node& n) { return !str(n, kAttrSettings).empty(); }
bool isPart(const Node& n) { return !str(n, kAttrPart).empty() || !str(n, kAttrOwner).empty() || !str(n, kAttrParts).empty(); }

json parse(const std::string& text, json fallback)
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

Node* nodeArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return n;
}

/** A list flag given as a list or a space or comma separated string. */
std::vector<std::string> listOf(const json& v)
{
    std::vector<std::string> out;
    if (v.is_array())
    {
        for (const json& e : v)
            if (e.is_string() && !e.get<std::string>().empty())
                out.push_back(e.get<std::string>());
    }
    else if (v.is_string())
    {
        std::string cur;
        for (char c : v.get<std::string>() + " ")
        {
            if (c == ' ' || c == ',')
            {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
    }
    return out;
}

glm::dvec3 upVector(const std::string& choice)
{
    if (choice == "Y")
        return {0.0, 1.0, 0.0};
    if (choice == "Z")
        return {0.0, 0.0, 1.0};
    return math::axisBasis().up;
}

// ------------------------------------------------------------------------------------------------ prefs

json prefs()
{
    json p = PrefStore::get().merged(kPrefsFile);
    if (!p.is_object())
        p = json::object();
    if (!p["Presets"].is_object())
        p["Presets"] = json::object();
    if (!p.contains("Preset") || !p["Preset"].is_string())
        p["Preset"] = builtInPresets().front();
    if (!p["Export"].is_object())
        p["Export"] = json::object();
    return p;
}

// ------------------------------------------------------------------------------------------------ settings on nodes

void stamp(Graph& g, NodeId id, const json& values, const std::string& preset, const json& sources)
{
    Node* n = g.node(id);
    if (!n || !ours(*n))
        throw CommandError("Only nodes built by the Tree Generator carry its settings; " + (n ? n->path() : std::string("that node")) + " is not one.");
    setStr(g, id, kAttrSettings, sanitize(values).dump());
    setStr(g, id, kAttrSources, (sources.is_object() ? sources : json::object()).dump());
    setStr(g, id, kAttrPreset, preset);
    setStr(g, id, kAttrVersion, kVersion);
}

void tagPart(Graph& g, NodeId id, const char* kind) { setStr(g, id, kAttrPart, kind); }

void tagLod(Graph& g, NodeId id, int level)
{
    if (Node* n = g.node(id); n && attrOf(*n, kAttrLod) >= 0)
        g.setValue({id, attrOf(*n, kAttrLod), -1}, int64_t(level));
}

/** Records a part as belonging to a tree it is not parented under. */
void linkPart(Graph& g, NodeId owner, NodeId part)
{
    if (!owner || !part || owner == part)
        return;
    Node* o = g.node(owner);
    if (!o || !ours(*o))
        return;
    json parts = parse(str(*o, kAttrParts), json::array());
    if (!parts.is_array())
        parts = json::array();
    const std::string path = g.fullPath(part);
    if (std::find(parts.begin(), parts.end(), json(path)) == parts.end())
        parts.push_back(path);
    setStr(g, owner, kAttrParts, parts.dump());
    setStr(g, part, kAttrOwner, g.fullPath(owner));
}

std::vector<NodeId> linkedParts(Graph& g, NodeId owner)
{
    std::vector<NodeId> out;
    const Node* o = g.node(owner);
    if (!o)
        return out;
    const json parts = parse(str(*o, kAttrParts), json::array());
    if (parts.is_array())
        for (const json& p : parts)
            if (p.is_string())
                if (Node* n = [&]() -> Node* { try { return g.find(p.get<std::string>()); } catch (const std::exception&) { return nullptr; } }())
                    out.push_back(n->id());
    return out;
}

NodeId ownerLink(Graph& g, const Node& n)
{
    const std::string path = str(n, kAttrOwner);
    if (path.empty())
        return kNoNode;
    try
    {
        Node* o = g.find(path);
        return o ? o->id() : kNoNode;
    }
    catch (const std::exception&)
    {
        return kNoNode;
    }
}

/** The one node a tree's settings live on, from any part of it: part links first, then parents. */
NodeId treeOwner(Graph& g, NodeId id)
{
    std::set<NodeId> seen;
    NodeId cur = id;
    while (cur && !seen.contains(cur))
    {
        seen.insert(cur);
        Node* n = g.node(cur);
        if (!n)
            return kNoNode;
        if (NodeId linked = ownerLink(g, *n))
        {
            cur = linked;
            continue;
        }
        if (g.parent(cur) && !hasSettings(*n))
        {
            cur = g.parent(cur);
            continue;
        }
        return cur;
    }
    return cur;
}

Node* create(Graph& g, const char* type, const std::string& name, NodeId parent)
{
    const NodeType* t = types::find(type);
    if (!t)
        throw CommandError(std::string("Tree Generator: the ") + type + " node type is not registered");
    return g.createNode(*t, name, parent);
}

NodeId meshNode(Graph& g, const std::string& name, geo::Mesh mesh, NodeId parent, const char* type = kTreeMeshType)
{
    Node* n = create(g, type, name, parent);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n->id();
}

/** Every mesh node at or under the nodes, once, in order. */
std::vector<NodeId> meshesUnder(Graph& g, const std::vector<NodeId>& roots)
{
    std::vector<NodeId> out;
    std::set<NodeId> seen;
    for (NodeId r : roots)
    {
        std::vector<NodeId> all{r};
        for (NodeId d : g.descendants(r))
            all.push_back(d);
        for (NodeId id : all)
            if (Node* n = g.node(id); n && n->type().isA("mesh") && seen.insert(id).second)
                out.push_back(id);
    }
    return out;
}

MeshPtr meshOf(Graph& g, NodeId id)
{
    g.evaluate();
    Node* n = g.node(id);
    return n && n->type().isA("mesh") ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
}

glm::dmat4 worldOf(Graph& g, NodeId id)
{
    g.evaluate();
    return g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
}

int64_t faces(Graph& g, NodeId id)
{
    int64_t total = 0;
    for (NodeId m : meshesUnder(g, {id}))
        if (MeshPtr mesh = meshOf(g, m))
            total += mesh->numFaces();
    return total;
}

/** A mesh node's evaluated mesh in world space. */
geo::Mesh worldMesh(Graph& g, NodeId id)
{
    MeshPtr m = meshOf(g, id);
    if (!m)
        return {};
    geo::Mesh out = *m;
    const glm::dmat4 w = worldOf(g, id);
    for (glm::dvec3& p : out.positions())
        p = glm::dvec3(w * glm::dvec4(p, 1.0));
    out.cornerAttrs().remove(geo::attr::kNormal);
    return out;
}

/** The node and every node under it, then the same for each linked part, without repeats. */
std::vector<NodeId> treeNodes(Graph& g, NodeId root)
{
    std::vector<NodeId> out;
    std::set<NodeId> seen;
    std::vector<NodeId> tops{root};
    for (NodeId p : linkedParts(g, root))
        tops.push_back(p);
    for (NodeId top : tops)
    {
        std::vector<NodeId> all{top};
        for (NodeId d : g.descendants(top))
            all.push_back(d);
        for (NodeId id : all)
            if (seen.insert(id).second)
                out.push_back(id);
    }
    return out;
}

bool isBelow(Graph& g, NodeId node, NodeId ancestor)
{
    for (NodeId cur = g.parent(node); cur; cur = g.parent(cur))
        if (cur == ancestor)
            return true;
    return false;
}

/** Drops a node when something below it in the list would be exported anyway. */
std::vector<NodeId> innermost(Graph& g, const std::vector<NodeId>& nodes)
{
    std::vector<NodeId> out;
    for (NodeId n : nodes)
    {
        bool covered = false;
        for (NodeId other : nodes)
            if (other != n && isBelow(g, other, n))
                covered = true;
        if (!covered)
            out.push_back(n);
    }
    return out;
}

const char* kParts[] = {"trunk", "canopy", "leaves"};

/** The trunk, canopy and leaf nodes of a tree, keyed by part: tagged nodes first, then names ending in the part. */
std::map<std::string, std::vector<NodeId>> findParts(Graph& g, NodeId root)
{
    const std::vector<NodeId> nodes = treeNodes(g, root);
    std::map<std::string, std::vector<NodeId>> tagged;
    for (NodeId id : nodes)
        if (Node* n = g.node(id))
            if (const std::string part = str(*n, kAttrPart); !part.empty())
                tagged[part].push_back(id);
    std::map<std::string, std::vector<NodeId>> parts;
    for (auto& [kind, found] : tagged)
        parts[kind] = innermost(g, found);
    for (const char* kind : kParts)
    {
        if (!parts[kind].empty())
            continue;
        std::vector<NodeId> named;
        for (NodeId id : nodes)
        {
            std::string name = g.node(id)->name();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (name.size() >= std::strlen(kind) && name.compare(name.size() - std::strlen(kind), std::string::npos, kind) == 0)
                named.push_back(id);
        }
        if (!named.empty())
            parts[kind] = innermost(g, named);
    }
    for (auto it = parts.begin(); it != parts.end();)
        it = it->second.empty() ? parts.erase(it) : std::next(it);
    return parts;
}

/** One detail level out of a tagged set, by density rather than by the order given. */
std::vector<NodeId> pickLod(Graph& g, const std::vector<NodeId>& nodes, bool finest)
{
    std::vector<NodeId> tagged;
    for (NodeId id : nodes)
        if (lodOf(*g.node(id)) >= 0)
            tagged.push_back(id);
    if (tagged.empty())
        return nodes;
    std::stable_sort(tagged.begin(), tagged.end(), [&](NodeId a, NodeId b) {
        return std::pair(faces(g, a), lodOf(*g.node(a))) < std::pair(faces(g, b), lodOf(*g.node(b)));
    });
    return {finest ? tagged.back() : tagged.front()};
}

std::string describe(Graph& g, const std::vector<NodeId>& nodes)
{
    std::string out;
    for (NodeId id : nodes)
        out += (out.empty() ? "" : ", ") + g.node(id)->name() + " (" + std::to_string(faces(g, id)) + " faces)";
    return out;
}

// ------------------------------------------------------------------------------------------------ settings from a call

struct Call
{
    json values;   // what gets stamped
    json build;    // what gets built: values stylized
    std::string preset;
    json sources = json::object();
};

Call callOf(const Args& a)
{
    Call c;
    c.preset = a.get<std::string>("preset", "");
    json base = c.preset.empty() ? defaults() : presetSettings(c.preset, userPresets());
    if (a.has("settings"))
    {
        const json& s = a.flags["settings"];
        if (!s.is_object())
            throw CommandError("settings must be a {\"section.name\": value} dictionary");
        for (const auto& [k, v] : s.items())
            base[k] = v;
    }
    c.values = sanitize(base);
    c.build = stylize(c.values, c.values["global.style"].get<double>());
    c.sources = {{"leaf", listOf(a.flags.value("leaf", json()))}, {"canopy", listOf(a.flags.value("canopy", json()))}};
    return c;
}

glm::dvec3 upOf(const Call& c) { return upVector(c.build["global.up_axis"].get<std::string>()); }

// ------------------------------------------------------------------------------------------------ building

struct TrunkResult
{
    NodeId node = kNoNode;
    std::vector<Tip> tips;
};

TrunkResult growTrunk(Graph& g, const Call& c, const std::string& name, NodeId parent)
{
    const json trunk = sectionValues(c.build, "trunk");
    Trunk t;
    try
    {
        t = buildTrunk(trunk, upOf(c));
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(e.what());
    }
    if (!t.warning.empty())
        log::warn("Tree Generator: {}", t.warning);
    if (t.density > 0.0)
        log::info("Tree Generator: trunk packed at {:.0f} px/m on a {} map.", t.density, trunk["uv_map_size"].get<int64_t>());
    TrunkResult r;
    r.node = meshNode(g, name, std::move(t.mesh), parent);
    tagPart(g, r.node, "trunk");
    r.tips = std::move(t.tips);
    return r;
}

/** Where a stamped trunk's branch tips are, without rebuilding it. */
std::vector<Tip> branchTips(Graph& g, NodeId node)
{
    const json stored = storedSettings(g, node);
    if (stored.is_null())
        throw CommandError("Grow a trunk first, or select one built by this tool.");
    const json build = stylize(stored["values"], stored["values"]["global.style"].get<double>());
    const json trunk = sectionValues(build, "trunk");
    TrunkSpec spec;
    try
    {
        spec = trunkSpec(trunk, upVector(build["global.up_axis"].get<std::string>()));
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(e.what());
    }
    Rng rng(uint64_t(trunk["seed"].get<int64_t>()));
    std::vector<Tip> tips = skeleton(spec, rng).tips;
    const glm::dmat4 w = worldOf(g, node);
    if (w == glm::dmat4(1.0))
        return tips;
    for (Tip& t : tips)
    {
        t.point = glm::dvec3(w * glm::dvec4(t.point, 1.0));
        t.heading = glm::normalize(glm::dvec3(w * glm::dvec4(t.heading, 0.0)));
    }
    return tips;
}

struct CanopyResult
{
    NodeId group = kNoNode;
    std::vector<NodeId> blobs;
};

CanopyResult canopyBlobs(Graph& g, const Call& c, const std::vector<Tip>& tips, const std::string& name, NodeId parent)
{
    const json blob = sectionValues(c.build, "blob");
    const double height = sectionValues(c.build, "trunk")["height"].get<double>();
    const double radius = blob["blob_radius"].get<double>() > 0.0 ? blob["blob_radius"].get<double>()
                                                                   : (height > 0.0 ? height : kTrunkHeightMetres * kUnitsPerMetre) * 0.28;
    std::vector<geo::Mesh> meshes;
    try
    {
        const auto placements = placeBlobs(tips, radius, blob["blob_variation"].get<double>(), blob["blob_taper"].get<double>(),
                                           blob["blob_offset"].get<double>(), blob["blob_align"].get<double>(), blob["blob_tilt"].get<double>(), upOf(c),
                                           c.build["trunk.seed"].get<int64_t>());
        meshes = buildBlobs(placements, blob["blob_kind"].get<std::string>(), int(blob["blob_detail"].get<int64_t>()), blob["blob_clip"].get<double>());
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(e.what());
    }
    CanopyResult r;
    r.group = create(g, kTreeType, name, parent)->id();
    for (size_t i = 0; i < meshes.size(); ++i)
        r.blobs.push_back(meshNode(g, name + "_" + std::to_string(i), std::move(meshes[i]), r.group));
    tagPart(g, r.group, "canopy");
    return r;
}

struct Anchor
{
    glm::dvec3 point, heading;
};

/** Leaves over the canopy meshes: one output per leaf mesh given, the first measured for the frame they all bend in. */
NodeId scatterLeaves(Graph& g, const Call& c, const std::vector<NodeId>& canopy, const std::vector<NodeId>& leaves, const std::vector<Anchor>& anchors,
                     const std::string& name, NodeId parent)
{
    if (leaves.empty())
        throw CommandError("Pick a leaf mesh.");
    if (canopy.empty())
        throw CommandError("Pick at least one canopy mesh.");
    const json leaf = sectionValues(c.build, "leaf");
    const ScatterSettings s = scatterSettings(leaf);
    std::vector<MeshPtr> canopyMeshes;
    std::vector<CanopyInput> inputs;
    for (size_t i = 0; i < canopy.size(); ++i)
    {
        MeshPtr m = meshOf(g, canopy[i]);
        if (!m)
            throw CommandError("'" + g.node(canopy[i])->path() + "' is not a polygon mesh.");
        canopyMeshes.push_back(m);
        CanopyInput in;
        in.mesh = m.get();
        in.world = worldOf(g, canopy[i]);
        if (anchors.size() == canopy.size())
        {
            in.hasAnchor = true;
            in.anchor = anchors[i].point;
            in.anchorHeading = anchors[i].heading;
        }
        inputs.push_back(in);
    }
    std::vector<Leaf> variants;
    std::vector<LeafPlan> plans;
    try
    {
        const Canopy surface(inputs);
        for (size_t i = 0; i < leaves.size(); ++i)
        {
            MeshPtr m = meshOf(g, leaves[i]);
            if (!m)
                throw CommandError("'" + g.node(leaves[i])->path() + "' is not a polygon mesh.");
            variants.push_back(i == 0 ? makeLeaf(*m, s.leafUp, s.leafForward, s.pivot) : makeLeaf(*m, variants.front()));
        }
        plans = planLeaves(surface, variants.front(), s, upOf(c));
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(e.what());
    }

    auto emitOne = [&](const Leaf& variant, const std::string& outName, NodeId under) {
        std::vector<geo::Mesh> meshes = emitLeaves(variant, plans, s.combine, s.normalBlend, s.colorSet);
        if (s.combine)
            return meshNode(g, outName, std::move(meshes.front()), under);
        NodeId group = create(g, kTreeType, outName, under)->id();
        for (size_t i = 0; i < meshes.size(); ++i)
            meshNode(g, outName + "_" + std::to_string(i), std::move(meshes[i]), group);
        return group;
    };
    NodeId built;
    if (variants.size() == 1)
    {
        built = emitOne(variants.front(), name, parent);
        tagPart(g, built, "leaves");
    }
    else
    {
        built = create(g, kTreeType, name, parent)->id();
        for (size_t i = 0; i < variants.size(); ++i)
        {
            const NodeId level = emitOne(variants[i], name + "_lod" + std::to_string(i), built);
            tagPart(g, level, "leaves");
            tagLod(g, level, int(i));
        }
    }
    return built;
}

std::vector<NodeId> nodesNamed(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& n : names)
        out.push_back(nodeArg(g, n, cmd)->id());
    return out;
}

/** The tree a set of meshes belongs to, so a part built on its own can be linked back. */
NodeId ownerFor(Graph& g, const std::vector<NodeId>& nodes)
{
    for (NodeId id : nodes)
    {
        const NodeId owner = treeOwner(g, id);
        if (Node* o = g.node(owner); o && hasSettings(*o))
            return owner;
    }
    return kNoNode;
}

NodeId fullTree(Graph& g, const Call& c, const std::vector<NodeId>& leaves, const std::string& name)
{
    if (leaves.empty())
        throw CommandError("Select the leaf mesh, or pass leaf=.");
    const NodeId root = create(g, kTreeType, name, kNoNode)->id();
    TrunkResult trunk = growTrunk(g, c, name + "_trunk", root);
    CanopyResult canopy = canopyBlobs(g, c, trunk.tips, name + "_canopy", root);
    std::vector<Anchor> anchors;
    for (const Tip& t : trunk.tips)
        anchors.push_back({t.point, t.heading});
    scatterLeaves(g, c, canopy.blobs, leaves, anchors, name + "_leaves", root);
    if (c.build["blob.keep_canopy"].get<bool>())
        g.setValue({canopy.group, Transform::kVisibility, -1}, false);
    else
        g.deleteNode(canopy.group);
    return root;
}

/** The leaf meshes a call names, else the last selected mesh. */
std::vector<NodeId> leafArg(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> leaves = nodesNamed(g, listOf(a.flags.value("leaf", json())), cmd);
    if (leaves.empty() && !g.selection().empty())
        leaves.push_back(g.selection().back());
    return leaves;
}

// ------------------------------------------------------------------------------------------------ files

void writeJson(const fs::path& path, const json& data)
{
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw CommandError("Could not write " + utf8(path));
    out << data.dump(4);
}

/** Seeds the colour file next to an export, then never touches it again, so hand edits survive. */
std::string writePalette(const fs::path& folder, const std::string& name, const std::string& species)
{
    const fs::path path = folder / utf8(name + kPaletteSuffix);
    if (fs::exists(path))
        return utf8(path);
    try
    {
        writeJson(path, paletteFor(species));
    }
    catch (const CommandError& e)
    {
        log::warn("Tree Generator: {}", e.what());
        return {};
    }
    return utf8(path);
}

json sidecarData(Graph& g, const json& stored, const std::string& name, const json& materials, const std::map<std::string, std::vector<NodeId>>& parts,
                 const std::string& kind)
{
    const json values = stored.is_object() && stored.contains("values") ? stored["values"] : json::object();
    const std::string preset = stored.is_object() ? stored.value("preset", std::string()) : std::string();
    json facesOut = json::object();
    for (const auto& [part, found] : parts)
    {
        int64_t total = 0;
        for (NodeId id : found)
            total += faces(g, id);
        facesOut[part] = total;
    }
    return {{"tree", name},
            {"kind", kind},
            {"preset", preset},
            {"species", speciesOf(preset)},
            {"version", kVersion},
            {"colourSet", values.value("leaf.color_set", std::string())},
            {"materials", materials},
            {"faces", facesOut}};
}

std::string writeSidecar(Graph& g, const json& stored, const fs::path& folder, const std::string& name, const json& materials,
                         const std::map<std::string, std::vector<NodeId>>& parts, const std::string& kind)
{
    const json data = sidecarData(g, stored, name, materials, parts, kind);
    const fs::path path = folder / utf8(name + kSidecarSuffix);
    writeJson(path, data);
    writePalette(folder, name, data["species"].get<std::string>());
    return utf8(path);
}

void exportFbx(const std::string& path, const std::vector<NodeId>& nodes)
{
    Graph& g = graph();
    std::error_code ec;
    fs::create_directories(utf8(path).parent_path(), ec);
    json args = json::array({path});
    for (NodeId id : nodes)
        args.push_back(g.fullPath(id));
    ops::execute("exportFbx", {{"args", args}, {"kind", "static"}, {"worldSpace", true}});
}

/** Puts every mesh under the nodes into one named material, made when missing. Stays after the export. */
void assignMaterial(Graph& g, const std::vector<NodeId>& nodes, const std::string& name)
{
    const std::vector<NodeId> meshes = meshesUnder(g, nodes);
    if (name.empty() || meshes.empty())
        return;
    if (!materials::find(g, name))
        if (Node* m = materials::create(g, name); m && m->name() != name)
            log::warn("Tree Generator: '{}' is taken by another node, so its material is called {}.", name, m->name());
    json args = json::array({name});
    for (NodeId id : meshes)
        args.push_back(g.fullPath(id));
    ops::execute("assignMaterial", {{"args", args}});
}

/** World space copies of every mesh under the nodes, subdivided, merged into one. */
geo::Mesh smoothedMerge(Graph& g, const std::vector<std::pair<NodeId, int>>& parts)
{
    std::vector<geo::Mesh> pieces;
    for (const auto& [id, divisions] : parts)
        for (NodeId m : meshesUnder(g, {id}))
        {
            geo::Mesh w = worldMesh(g, m);
            if (divisions > 0)
                w = geo::subdivide(w, {.levels = divisions, .sharpBoundary = true});
            pieces.push_back(std::move(w));
        }
    std::vector<const geo::Mesh*> ptrs;
    for (const geo::Mesh& p : pieces)
        ptrs.push_back(&p);
    return geo::combine(ptrs);
}

/** Exports a merged mesh through a temporary node, removed again whatever happens. */
void exportMesh(Graph& g, const std::string& path, const std::string& name, geo::Mesh mesh)
{
    const NodeId temp = meshNode(g, name, std::move(mesh), kNoNode, "mesh");
    try
    {
        exportFbx(path, {temp});
    }
    catch (...)
    {
        g.deleteNode(temp);
        throw;
    }
    g.deleteNode(temp);
}

json exportSettings(const json& values) { return sectionValues(values, "export"); }

void requirePath(const json& e)
{
    if (e["path"].get<std::string>().empty() || e["name"].get<std::string>().empty())
        throw CommandError("Set an export path and a file name first.");
}

json exportTree(Graph& g, NodeId root, const json& values)
{
    const json e = exportSettings(values);
    requirePath(e);
    const auto parts = findParts(g, root);
    if (parts.empty())
        throw CommandError(g.node(root)->path() + " has no trunk, canopy or leaves on it.");
    for (const char* part : kParts)
        if (!parts.contains(part) && std::string(part) != "canopy")
            log::warn("Tree Generator: no {} found on {}, exporting without it.", part, g.node(root)->name());

    const std::map<std::string, int> divisions{{"trunk", int(e["trunk_divisions"].get<int64_t>())},
                                               {"canopy", int(e["canopy_divisions"].get<int64_t>())},
                                               {"leaves", int(e["leaf_divisions"].get<int64_t>())}};
    const json materialNames = {{"trunk", e["trunk_material"]}, {"canopy", e["canopy_material"]}, {"leaves", e["leaf_material"]}};
    for (const auto& [part, found] : parts)
        if (materialNames.contains(part))
            assignMaterial(g, found, materialNames[part].get<std::string>());

    const fs::path folder = utf8(e["path"].get<std::string>());
    const std::string name = e["name"].get<std::string>();
    const std::string lowPath = utf8(folder / utf8(name + kLowSuffix + ".fbx"));
    const std::string highPath = utf8(folder / utf8(name + kHighSuffix + ".fbx"));
    auto partOr = [&](const char* p) { return parts.contains(p) ? parts.at(p) : std::vector<NodeId>{}; };

    std::vector<NodeId> low;
    for (const char* part : kParts)
    {
        if (std::string(part) == "canopy" && !e["canopy_low"].get<bool>())
            continue;
        for (NodeId id : pickLod(g, partOr(part), false))
            low.push_back(id);
    }
    if (low.empty())
        throw CommandError("Nothing left to export in the low poly.");
    log::info("Tree Generator: low poly {}", describe(g, low));
    exportFbx(lowPath, low);
    std::map<std::string, std::vector<NodeId>> lowParts;
    for (const auto& [part, found] : parts)
        lowParts[part] = pickLod(g, found, false);
    const std::string sidecar = writeSidecar(g, storedSettings(g, root), folder, name, materialNames, lowParts, "tree");

    std::vector<std::pair<NodeId, int>> high;
    for (const char* part : kParts)
    {
        if (std::string(part) == "canopy" && !e["canopy_high"].get<bool>())
            continue;
        const std::vector<NodeId> chosen = pickLod(g, partOr(part), true);
        if (!chosen.empty())
            log::info("Tree Generator: high poly {}", describe(g, chosen));
        for (NodeId id : chosen)
            high.push_back({id, divisions.at(part)});
    }
    geo::Mesh merged = smoothedMerge(g, high);
    if (merged.numFaces() == 0)
        throw CommandError("Nothing left to export in the high poly.");
    exportMesh(g, highPath, name + kHighSuffix, std::move(merged));
    return {{"high", highPath}, {"low", lowPath}, {"sidecar", sidecar}, {"palette", utf8(folder / utf8(name + kPaletteSuffix))}};
}

void writeGreyPng(const std::string& path, int width, int height, const std::vector<uint8_t>& grey)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y)
    {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t v = grey[size_t(y) * width + x];
            row[x * 4 + 0] = v;
            row[x * 4 + 1] = v;
            row[x * 4 + 2] = v;
            row[x * 4 + 3] = 255;
        }
    }
    std::error_code ec;
    fs::create_directories(utf8(path).parent_path(), ec);
    if (!image.save(QString::fromStdString(path), "PNG"))
        throw CommandError("Could not write " + path);
}

std::string opacityMap(Graph& g, NodeId leaf, const std::string& folder, const std::string& name, int size)
{
    if (folder.empty() || name.empty())
        throw CommandError("Set an export path and a file name first.");
    MeshPtr m = meshOf(g, leaf);
    if (!m)
        throw CommandError("'" + g.node(leaf)->path() + "' is not a polygon mesh.");
    size = std::max(16, size);
    std::vector<uint8_t> cover;
    try
    {
        cover = opacityMask(*m, size, 0.0);
    }
    catch (const std::runtime_error&)
    {
        throw CommandError("'" + g.node(leaf)->name() + "' has no UVs to rasterise.");
    }
    const std::string path = utf8(utf8(folder) / utf8(name + kOpacitySuffix));
    writeGreyPng(path, size, size, cover);
    return path;
}

json exportLeafFiles(Graph& g, const std::vector<NodeId>& given, const json& values, const std::string& preset)
{
    std::vector<NodeId> meshes = given;
    if (meshes.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && !isPart(*n))
                meshes.push_back(id);
    if (meshes.empty())
        throw CommandError("Set a leaf mesh in the Leaf Mesh field, or select one. A tree built by this tool is not a leaf.");
    for (NodeId id : meshes)
        if (isPart(*g.node(id)))
            throw CommandError(g.node(id)->name() + " belongs to a generated tree. Point the Leaf Mesh field at the single leaf you want to export.");
    const json e = exportSettings(values);
    requirePath(e);

    std::vector<NodeId> ranked = meshes;
    std::stable_sort(ranked.begin(), ranked.end(), [&](NodeId a, NodeId b) { return faces(g, a) < faces(g, b); });
    const NodeId low = ranked.front(), high = ranked.back();
    const std::string leafName = e["name"].get<std::string>() + kLeafSuffix;
    const std::string material = e["leaf_material"].get<std::string>();
    assignMaterial(g, meshes, material);

    const fs::path folder = utf8(e["path"].get<std::string>());
    const std::string lowPath = utf8(folder / utf8(leafName + kLowSuffix + ".fbx"));
    const std::string highPath = utf8(folder / utf8(leafName + kHighSuffix + ".fbx"));
    log::info("Tree Generator: leaf low poly {}", describe(g, {low}));
    exportFbx(lowPath, {low});
    geo::Mesh smoothed = smoothedMerge(g, {{high, int(e["leaf_divisions"].get<int64_t>())}});
    log::info("Tree Generator: leaf high poly {} faces", smoothed.numFaces());
    exportMesh(g, highPath, leafName + kHighSuffix, std::move(smoothed));

    const json stored = {{"values", values}, {"preset", preset}};
    const std::string sidecar = writeSidecar(g, stored, folder, leafName, {{"leaves", material}}, {{"leaves", {low}}}, "leaf");
    std::string opacity;
    try
    {
        opacity = opacityMap(g, low, utf8(folder), leafName, int(e["opacity_size"].get<int64_t>()));
    }
    catch (const CommandError& err)
    {
        log::warn("Tree Generator: no opacity map for the leaf, {}", err.what());
    }
    return {{"high", highPath}, {"low", lowPath}, {"sidecar", sidecar}, {"palette", utf8(folder / utf8(leafName + kPaletteSuffix))}, {"opacity", opacity}};
}

/** Signed distances from the low poly's points to the nearest point of the high poly, positive outward. */
std::vector<double> deviation(Graph& g, const std::vector<NodeId>& low, const geo::Mesh& high)
{
    std::vector<double> out;
    if (high.numFaces() == 0)
        return out;
    const geo::MeshBvh bvh(high);
    for (NodeId id : meshesUnder(g, low))
    {
        const geo::Mesh m = worldMesh(g, id);
        const std::vector<glm::vec3> normals = geo::pointNormals(m);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            geo::BvhClosest hit;
            if (!bvh.closestPoint(m.position(p), hit))
                continue;
            const glm::dvec3 delta = hit.point - m.position(p);
            const double length = glm::length(delta);
            out.push_back(glm::dot(delta, glm::dvec3(normals[p])) >= 0.0 ? length : -length);
        }
    }
    return out;
}

json measureBake(Graph& g, NodeId root, const json& values)
{
    const auto parts = findParts(g, root);
    if (parts.empty())
        throw CommandError(g.node(root)->path() + " has no trunk, canopy or leaves on it.");
    const json e = exportSettings(values);
    g.evaluate();
    geo::Bounds box;
    bool any = false;
    for (NodeId m : meshesUnder(g, {root}))
    {
        const geo::Mesh w = worldMesh(g, m);
        if (w.numPoints() == 0)
            continue;
        const geo::Bounds b = w.bounds();
        box = any ? geo::Bounds{glm::min(box.min, b.min), glm::max(box.max, b.max)} : b;
        any = true;
    }
    if (any)
    {
        const glm::dvec3 size = box.max - box.min;
        const glm::dvec3 up = upVector("auto");
        const double height = glm::dot(glm::abs(size), glm::abs(up));
        log::info("Tree Generator: {} is {:.1f}cm tall ({:.2f} m), bounds {:.1f} x {:.1f} x {:.1f}.", g.node(root)->name(), height, height / kUnitsPerMetre,
                  size.x, size.y, size.z);
    }
    for (const char* part : kParts)
        if (!parts.contains(part) && std::string(part) != "canopy")
            log::warn("Tree Generator: no {} found on {}, exporting without it.", part, g.node(root)->name());
    const std::map<std::string, int> divisions{{"trunk", int(e["trunk_divisions"].get<int64_t>())},
                                               {"canopy", int(e["canopy_divisions"].get<int64_t>())},
                                               {"leaves", int(e["leaf_divisions"].get<int64_t>())}};
    json report = json::object();
    for (const char* part : kParts)
    {
        auto it = parts.find(part);
        if (it == parts.end() || (std::string(part) == "canopy" && !e["canopy_high"].get<bool>()))
            continue;
        const std::vector<NodeId> low = pickLod(g, it->second, false);
        const std::vector<NodeId> source = pickLod(g, it->second, true);
        const geo::Mesh high = smoothedMerge(g, {{source.front(), divisions.at(part)}});
        const json s = spread(deviation(g, {low.front()}, high));
        report[part] = s;
        log::info("  {:<7} {:>6} verts  out {:.3f}cm (99% {:.3f})  in {:.3f}cm (99% {:.3f})", part, s["count"].get<int64_t>(), s["out"].get<double>(),
                  s["out99"].get<double>(), s["in"].get<double>(), s["in99"].get<double>());
    }
    if (report.empty())
        log::warn("Tree Generator: nothing to measure.");
    return report;
}

const char* kMaskSuffixes[] = {"_maskR", "_maskG", "_maskB"};
const char* kVertexMapSuffixes[] = {"vertexcolor", "vertexcolour", "vertex_color", "vertexcolormap", "leafshade", "shade"};

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** The baked vertex colour map in an export folder, by the usual baker names. Empty when there is none. */
std::string findVertexMap(const std::string& folder, const std::string& name)
{
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(utf8(folder), ec))
        return {};
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(utf8(folder), ec))
        entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    std::vector<fs::path> candidates;
    for (const fs::path& p : entries)
    {
        const std::string ext = lower(utf8(p.extension()));
        if (ext != ".psd" && ext != ".png" && ext != ".tga" && ext != ".tif" && ext != ".tiff" && ext != ".exr")
            continue;
        const std::string stem = lower(utf8(p.stem()));
        bool mask = false;
        for (const char* m : kMaskSuffixes)
            if (stem.ends_with(lower(m)))
                mask = true;
        if (mask)
            continue;
        for (const char* s : kVertexMapSuffixes)
            if (stem.find(s) != std::string::npos)
            {
                candidates.push_back(p);
                break;
            }
    }
    if (candidates.empty())
        return {};
    for (const fs::path& c : candidates)
        if (!name.empty() && lower(utf8(c.filename())).starts_with(lower(name)))
            return utf8(c);
    return utf8(candidates.front());
}

/** Splits an RGB vertex colour bake into three greyscale masks, since the texturing app greys an RGB image first. */
json splitMasks(const std::string& path)
{
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(utf8(path), ec))
        throw CommandError("No vertex colour map found to split.");
    QImage image;
    if (!image.load(QString::fromStdString(path)))
        throw CommandError("Could not read " + path);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    std::string stem = utf8(utf8(path).parent_path() / utf8(path).stem());
    for (const char* s : kVertexMapSuffixes)
        if (lower(stem).ends_with(s))
        {
            stem = stem.substr(0, stem.size() - std::strlen(s));
            while (!stem.empty() && stem.back() == '_')
                stem.pop_back();
            break;
        }
    json written = json::array();
    const int w = image.width(), h = image.height();
    for (int channel = 0; channel < 3; ++channel)
    {
        std::vector<uint8_t> grey(size_t(w) * h);
        for (int y = 0; y < h; ++y)
        {
            const uchar* row = image.constScanLine(y);
            for (int x = 0; x < w; ++x)
                grey[size_t(y) * w + x] = row[x * 4 + channel];
        }
        const std::string target = stem + kMaskSuffixes[channel] + ".png";
        writeGreyPng(target, w, h, grey);
        written.push_back(target);
    }
    return written;
}

NodeId lastTrunk = kNoNode;

NodeId treeArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.positional.empty() && a.positional[0].is_string())
        return treeOwner(g, nodeArg(g, a.positional[0].get<std::string>(), cmd)->id());
    if (a.has("tree"))
        return treeOwner(g, nodeArg(g, a.get<std::string>("tree"), cmd)->id());
    return treeInSelection(g);
}

// ------------------------------------------------------------------------------------------------ native texture sets

/** The tree's bake project from its export, the leaves' colour set baked as vertex colours; empty with a warning before an export. */
std::string treeBakeProject(Graph& g, NodeId tree, const json& values, const std::string& target, json& warnings)
{
    const json h = bakeHandoff(g, tree, values);
    if (h.value("high", std::string()).empty() || !fs::exists(utf8(h["high"].get<std::string>())) || h.value("low", std::string()).empty() ||
        !fs::exists(utf8(h["low"].get<std::string>())))
    {
        warnings.push_back("no bake project: export the tree first, then texSetSettings(set, {\"bakeProject\": ...}); until then the mesh maps "
                           "read as defaults");
        return {};
    }
    const json made = ops::execute("createBakeFromGenerator", {{"args", {g.node(tree)->path()}}, {"kind", "tree"}, {"target", target}});
    const std::string project = made.at("project").get<std::string>();
    for (const char* map : {"ao", "curvature", "cavity"})
        ops::execute("setBakeMap", {{"args", {map}}, {"project", project}, {"enabled", true}});
    if (const std::string set = h.value("colourSet", std::string()); !set.empty())
        ops::execute("setBakeMap", {{"args", {"vertexColor"}}, {"project", project}, {"enabled", true}, {"options", {{"set", set}}}});
    return project;
}

json createTreeTextureSets(Graph& g, NodeId tree, const Args& a)
{
    const json stored = storedSettings(g, tree);
    const json values = stored.is_object() ? stored["values"] : callOf(a).values;
    const std::string preset = stored.is_object() ? stored.value("preset", std::string()) : a.get<std::string>("preset", "");
    const json palette = paletteFor(speciesOf(preset));
    const std::string season = a.get<std::string>("season", "summer");
    if (!palette["leaves"].contains(season))
        throw CommandError("treeTextureSet: season is spring, summer, autumn or lateAutumn");
    const json e = exportSettings(values);
    const std::string base = a.get<std::string>("name", g.node(tree)->name());
    const std::string target = a.get<std::string>("target", "unreal");
    json warnings = json::array();

    UndoChunk chunk("Tree Texture Sets");
    const std::string project = a.get<bool>("bake", true) ? treeBakeProject(g, tree, values, target, warnings) : std::string();
    auto makeSet = [&](const std::string& name, const std::string& material) {
        if (a.get<bool>("replace", false))
        {
            Node* old = nullptr;
            try
            {
                old = g.find(name);
            }
            catch (const std::exception&)
            {
            }
            if (old && old->type().name == "textureSet")
                g.deleteNode(old->id());
        }
        return ops::execute("texCreateSet", {{"args", {name}}, {"settings", {{"material", material}, {"bakeProject", project}, {"exportPreset", target}}}})
            .get<std::string>();
    };
    auto fill = [](const std::string& set, const std::string& name, const json& material, const json& extra) {
        json call = {{"args", {set, "fill"}}, {"name", name}, {"material", material}};
        call.update(extra);
        return ops::execute("texAddLayer", call).at("id").get<std::string>();
    };
    auto mask = [](const std::string& set, const std::string& layer, const std::string& type, const json& values, const json& bindings, bool invert) {
        ops::execute("texAddGenerator", {{"args", {set, layer, type}}, {"values", values}, {"bindings", bindings}, {"invert", invert}});
    };
    const json none = json::object();
    auto opacity = [](double v) { return json{{"channels", {{"baseColor", {{"opacity", v}}}}}}; };

    // Bark: the species' tones over curvature and occlusion.
    const json& bark = palette["bark"];
    const std::string barkSet = makeSet(base + "_Bark", e.value("trunk_material", std::string("Trunk")));
    fill(barkSet, "Bark", {{"baseColor", bark["base"]}, {"roughness", bark["roughness"]}}, none);
    mask(barkSet, fill(barkSet, "Crevices", {{"baseColor", bark["dark"]}}, none), "genCavityGrime", none, none, false);
    mask(barkSet, fill(barkSet, "Ridges", {{"baseColor", bark["light"]}}, opacity(0.6)), "genEdgeWear", none, none, false);
    mask(barkSet, fill(barkSet, "Deep Occlusion", {{"baseColor", bark["deep"]}}, none), "genAoDarken", none, {{"ao", "ao"}}, false);

    // Leaves: the season's tones over the leaf colour set (R exposure, G per leaf, B per bunch).
    const json& leaf = palette["leaves"][season];
    const std::string leafSet = makeSet(base + "_Leaves", e.value("leaf_material", std::string("Leaves")));
    const json vc = {{"position", "vertexColor"}};
    fill(leafSet, "Leaf", {{"baseColor", leaf["base"]}, {"roughness", leaf["roughness"]}}, none);
    mask(leafSet, fill(leafSet, "Per Bunch", {{"baseColor", leaf["bunch"]}}, opacity(0.5)), "maskAxis", {{"axis", "z"}, {"start", 0.0}, {"end", 1.0}}, vc, false);
    mask(leafSet, fill(leafSet, "Per Leaf", {{"baseColor", leaf["light"]}}, opacity(0.35)), "maskAxis", {{"axis", "y"}, {"start", 0.0}, {"end", 1.0}}, vc, false);
    mask(leafSet, fill(leafSet, "Shade", {{"baseColor", leaf["dark"]}}, none), "maskAxis", {{"axis", "x"}, {"start", 0.0}, {"end", 0.6}}, vc, true);
    mask(leafSet, fill(leafSet, "Deep Shade", {{"baseColor", leaf["deep"]}}, none), "maskAxis", {{"axis", "x"}, {"start", 0.0}, {"end", 0.25}}, vc, true);
    mask(leafSet, fill(leafSet, "Sunlit", {{"baseColor", leaf["fresh"]}}, none), "maskAxis", {{"axis", "x"}, {"start", 0.7}, {"end", 1.0}}, vc, false);

    return {{"sets", {barkSet, leafSet}}, {"bakeProject", project.empty() ? json() : json(project)}, {"season", season}, {"species", speciesOf(preset)},
            {"warnings", warnings}};
}

// ------------------------------------------------------------------------------------------------ registration

bool registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    const NodeType* mesh = types::find("mesh");
    if (!transform || !mesh)
        return false;
    auto text = [](const char* name, const char* doc) {
        return AttrDef{.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable, .doc = doc};
    };
    std::vector<AttrDef> attrs = {
        text(kAttrSettings, "Every setting the tree was built with, as json, so Load From Selection and Rebuild can reproduce it."),
        text(kAttrSources, "The leaf and canopy meshes it was built from, as json."),
        text(kAttrPreset, "The preset the settings started from."),
        text(kAttrVersion, "Tree Generator version that built it."),
        text(kAttrPart, "Which part of a tree this is: trunk, canopy or leaves."),
        text(kAttrParts, "Parts built a step at a time that belong to this tree, as a json list of paths."),
        text(kAttrOwner, "The tree this part belongs to when it is not parented under it."),
        AttrDef{.name = kAttrLod, .type = PlugType::Int, .defaultValue = int64_t(-1), .flags = AttrInput | AttrStorable,
                .doc = "Detail level of a leaf output built from several leaf meshes; -1 for none.", .minValue = -1.0},
    };
    NodeType tree{.name = kTreeType,
                  .base = transform,
                  .dag = true,
                  .doc = "A generated tree, canopy or leaf group, carrying the Tree Generator settings it was built with.",
                  .icon = "tree",
                  .create = transform->create};
    tree.ownAttrs = attrs;
    NodeType treeMesh{.name = kTreeMeshType,
                      .base = mesh,
                      .dag = true,
                      .doc = "A generated trunk, canopy blob or leaf mesh, tagged with the tree part it is.",
                      .icon = "plant",
                      .create = mesh->create};
    treeMesh.ownAttrs = attrs;
    for (NodeType* t : {&tree, &treeMesh})
        lila::scene::relocate::registerNeutral(t->name);
    for (NodeType* t : {&tree, &treeMesh})
        if (!types::registerType(ctx, std::move(*t)))
            return false;
    return true;
}

const std::vector<FlagDef>& settingsFlags()
{
    using T = ArgType;
    static const std::vector<FlagDef> f = {
        {"preset", "p", T::String, nullptr, "Preset the settings start from (built in or saved); defaults when not given."},
        {"settings", "s", T::Any, nullptr, "{\"section.name\": value} overrides on top of the preset, lengths in metres (\"trunk.height\": 6.0)."},
    };
    return f;
}

std::vector<FlagDef> withSettings(std::vector<FlagDef> extra)
{
    std::vector<FlagDef> f = settingsFlags();
    f.insert(f.end(), extra.begin(), extra.end());
    return f;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    const FlagDef leafFlag{"leaf", "l", T::Any, nullptr, "Leaf mesh(es), densest first for detail levels; a list or a space separated string."};
    const FlagDef canopyFlag{"canopy", "c", T::Any, nullptr, "Canopy mesh(es) to scatter over; a list or a space separated string."};

    ctx.registerCommand({
        .name = "treeGrowTrunk",
        .doc = "Grow a trunk with recursive branches from the trunk settings, stamped with the settings. Returns the trunk.",
        .flags = withSettings({{"name", "n", T::String, "trunk", "Name."}, leafFlag, canopyFlag}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Call c = callOf(a);
            TrunkResult t = growTrunk(g, c, a.get<std::string>("name", "trunk"), kNoNode);
            stamp(g, t.node, c.values, c.preset, c.sources);
            lastTrunk = t.node;
            g.select({t.node});
            return g.node(t.node)->path();
        },
    });

    ctx.registerCommand({
        .name = "treeBranchTips",
        .doc = "Where a stamped trunk's branch tips are, without rebuilding it: [{point, heading, radius}].",
        .positionalDoc = "tree (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId tree = treeArg(g, a, "treeBranchTips");
            if (!tree)
                throw CommandError("Grow a trunk first, or select one built by this tool.");
            json out = json::array();
            for (const Tip& t : branchTips(g, tree))
                out.push_back({{"point", {t.point.x, t.point.y, t.point.z}}, {"heading", {t.heading.x, t.heading.y, t.heading.z}}, {"radius", t.radius}});
            return out;
        },
    });

    ctx.registerCommand({
        .name = "treeCanopyBlobs",
        .doc = "Drop a canopy blob on each branch tip of a stamped trunk, ready to scatter leaves onto. Returns the canopy group.",
        .positionalDoc = "tree (default the selection, else the last trunk grown)",
        .flags = withSettings({{"name", "n", T::String, "canopy", "Name."}, leafFlag, canopyFlag}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId owner = treeArg(g, a, "treeCanopyBlobs");
            if (!owner && g.node(lastTrunk))
                owner = lastTrunk;
            if (!owner)
                throw CommandError("Grow a trunk first, or select one built by this tool.");
            const Call c = callOf(a);
            CanopyResult r = canopyBlobs(g, c, branchTips(g, owner), a.get<std::string>("name", "canopy"), kNoNode);
            linkPart(g, owner, r.group);
            stamp(g, owner, c.values, c.preset, c.sources);
            g.select({r.group});
            return g.node(r.group)->path();
        },
    });

    ctx.registerCommand({
        .name = "treeScatterLeaves",
        .doc = "Copy the leaf mesh over the canopy meshes, bending each copy onto the curvature and writing its shading into the leaf colour "
               "set (R exposure, G per leaf, B per bunch). Without leaf= and canopy= the selection is the canopy with the leaf last. Returns the leaves.",
        .flags = withSettings({leafFlag, canopyFlag, {"name", "n", T::String, "leaves", "Name."}}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Call c = callOf(a);
            std::vector<NodeId> leaves = nodesNamed(g, listOf(a.flags.value("leaf", json())), "treeScatterLeaves");
            std::vector<NodeId> canopy = nodesNamed(g, listOf(a.flags.value("canopy", json())), "treeScatterLeaves");
            if (leaves.empty() && canopy.empty())
            {
                const std::vector<NodeId>& sel = g.selection();
                if (sel.size() < 2)
                    throw CommandError("Select the canopy mesh(es) then the leaf mesh last.");
                canopy.assign(sel.begin(), sel.end() - 1);
                leaves = {sel.back()};
            }
            canopy = meshesUnder(g, canopy);
            const NodeId owner = ownerFor(g, canopy);
            const NodeId built = scatterLeaves(g, c, canopy, leaves, {}, a.get<std::string>("name", "leaves"), kNoNode);
            linkPart(g, owner, built);
            stamp(g, owner ? owner : built, c.values, c.preset, c.sources);
            g.select({built});
            return g.node(built)->path();
        },
    });

    ctx.registerCommand({
        .name = "treeBuild",
        .doc = "Build a whole tree: trunk, canopy blobs on the tips and the leaf scattered over them, under one root stamped with the settings. "
               "Returns the root.",
        .flags = withSettings({leafFlag, canopyFlag, {"name", "n", T::String, "tree", "Name."}}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Call c = callOf(a);
            const NodeId root = fullTree(g, c, leafArg(g, a, "treeBuild"), a.get<std::string>("name", "tree"));
            json sources = c.sources;
            if (sources["leaf"].empty())
                for (NodeId id : leafArg(g, a, "treeBuild"))
                    sources["leaf"].push_back(g.node(id)->path());
            stamp(g, root, c.values, c.preset, sources);
            g.select({root});
            return g.node(root)->path();
        },
    });

    ctx.registerCommand({
        .name = "treeRebuild",
        .doc = "Rebuild a stamped tree from the settings it carries, as <name>_rebuilt. Without its leaf mesh it grows the trunk alone. Returns the new tree.",
        .positionalDoc = "tree (default the selection)",
        .flags = {{"name", "n", T::String, nullptr, "Name of the rebuilt tree."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId tree = treeArg(g, a, "treeRebuild");
            const json stored = storedSettings(g, tree);
            if (stored.is_null())
                throw CommandError("Select a tree built by this tool.");
            Call c;
            c.values = stored["values"];
            c.build = stylize(c.values, c.values["global.style"].get<double>());
            c.preset = stored["preset"].get<std::string>();
            c.sources = stored["sources"];
            std::vector<NodeId> leaves;
            for (const std::string& n : listOf(c.sources.value("leaf", json())))
                if (Node* found = [&]() -> Node* { try { return g.find(n); } catch (const std::exception&) { return nullptr; } }())
                    leaves.push_back(found->id());
            const std::string name = a.get<std::string>("name", g.node(tree)->name() + "_rebuilt");
            const NodeId built = leaves.empty() ? growTrunk(g, c, name, kNoNode).node : fullTree(g, c, leaves, name);
            stamp(g, built, c.values, c.preset, c.sources);
            g.select({built});
            return g.node(built)->path();
        },
    });

    ctx.registerCommand({
        .name = "treeLeafPlane",
        .doc = "A leaf card to scatter, lying along +Z with its base at the origin, segmented so curl and wrap can bend it. Returns the mesh.",
        .flags = settingsFlags(),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Call c = callOf(a);
            const json p = sectionValues(c.values, "plane");
            const Arrays arrays = leafPlaneArrays(p["length"].get<double>(), p["width"].get<double>(), int(p["segments"].get<int64_t>()),
                                                  int(p["cross"].get<int64_t>()), p["taper"].get<double>(), p["double"].get<bool>());
            const std::string name = p["name"].get<std::string>().empty() ? "leafPlane" : p["name"].get<std::string>();
            const NodeId id = meshNode(g, name, toMesh(arrays), kNoNode, "mesh");
            g.select({id});
            return g.node(id)->path();
        },
    });

    ctx.registerCommand({
        .name = "treeStamp",
        .doc = "Stamp settings onto nodes built by the Tree Generator, so Load From Selection and Rebuild read them back.",
        .positionalDoc = "nodes (default the selection)",
        .flags = withSettings({leafFlag, canopyFlag}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Call c = callOf(a);
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("Select the node to stamp the settings onto.");
            json out = json::array();
            for (const std::string& o : objs)
            {
                Node* n = nodeArg(g, o, "treeStamp");
                stamp(g, n->id(), c.values, c.preset, c.sources);
                out.push_back(n->path());
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "treeSettings",
        .doc = "The settings, sources and preset stored on a tree: {tree, values, sources, preset}, or null when the selection is no tree.",
        .positionalDoc = "tree (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId tree = treeArg(g, a, "treeSettings");
            json s = storedSettings(g, tree);
            if (!s.is_null())
                s["tree"] = g.node(tree)->path();
            return s;
        },
    });

    ctx.registerCommand({
        .name = "treeExport",
        .doc = "Write a tree as <name>_Lo.fbx (lowest detail parts) and a subdivided, merged <name>_Hi.fbx, with <name>_tree.json and a "
               "<name>_palette.json seeded once. Assigns the part materials for real. Paths come from the export settings. Returns the paths.",
        .positionalDoc = "tree (default the selection)",
        .flags = settingsFlags(),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId tree = treeArg(g, a, "treeExport");
            if (!tree)
                throw CommandError("Select a tree built by this tool.");
            const Call c = callOf(a);
            json out = exportTree(g, tree, c.values);
            json remembered = json::object();
            for (const auto& [k, v] : c.values.items())
                if (k.starts_with("export."))
                    remembered[k] = v;
            PrefStore::get().set(kPrefsFile, "/Export", remembered);
            if (Node* n = g.node(tree); n && hasSettings(*n))
            {
                const json stored = storedSettings(g, tree);
                json values = stored["values"];
                for (const auto& [k, v] : remembered.items())
                    values[k] = v;
                stamp(g, tree, values, stored["preset"].get<std::string>(), stored["sources"]);
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "treeExportLeaf",
        .doc = "Write one leaf on its own as <name>_Leaf_Lo.fbx and <name>_Leaf_Hi.fbx (the densest mesh, subdivided), with its json and "
               "<name>_Leaf_opacity.png, for particle cards. Returns the paths.",
        .flags = withSettings({leafFlag}),
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Call c = callOf(a);
            return exportLeafFiles(g, nodesNamed(g, listOf(a.flags.value("leaf", json())), "treeExportLeaf"), c.values, c.preset);
        },
    });

    ctx.registerCommand({
        .name = "treeMeasureBake",
        .doc = "How far the subdivided high poly stands off the low poly per part ({part: {count, out, in, out99, in99}}, cm): the ray lengths a "
               "baker needs. Use the 99th percentiles.",
        .positionalDoc = "tree (default the selection)",
        .flags = settingsFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId tree = treeArg(g, a, "treeMeasureBake");
            if (!tree)
                throw CommandError("Select a tree built by this tool.");
            return measureBake(g, tree, callOf(a).values);
        },
    });

    ctx.registerCommand({
        .name = "treeOpacityMap",
        .doc = "Rasterise a leaf mesh's UV shells into <name>_opacity.png, a cutout mask for the leaf material. Returns the path.",
        .flags = withSettings({leafFlag}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Call c = callOf(a);
            const std::vector<NodeId> leaves = leafArg(g, a, "treeOpacityMap");
            if (leaves.empty())
                throw CommandError("Select the leaf mesh, or pass leaf=.");
            const json e = exportSettings(c.values);
            return opacityMap(g, leaves.front(), e["path"].get<std::string>(), e["name"].get<std::string>(), int(e["opacity_size"].get<int64_t>()));
        },
    });

    ctx.registerCommand({
        .name = "treeSplitMasks",
        .doc = "Split a baked leaf colour set map into <name>_maskR/G/B.png greyscale masks. Finds the bake in the export folder unless a path is "
               "given. Returns the masks.",
        .positionalDoc = "path (optional)",
        .flags = settingsFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string path = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
            if (path.empty())
            {
                const json e = exportSettings(callOf(a).values);
                path = findVertexMap(e["path"].get<std::string>(), e["name"].get<std::string>());
                if (path.empty())
                    throw CommandError("No vertex colour bake in " + (e["path"].get<std::string>().empty() ? std::string("the export path") : e["path"].get<std::string>()) +
                                       ". Bake the leaf colour set first.");
            }
            return splitMasks(path);
        },
    });

    ctx.registerCommand({
        .name = "treePresets",
        .doc = "Preset names, built in first then saved ones.",
        .undoable = false,
        .run = [](const Args&) -> json { return presetNames(userPresets()); },
    });

    ctx.registerCommand({
        .name = "treePresetSettings",
        .doc = "A preset's full settings, lengths in metres.",
        .positionalDoc = "preset",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("treePresetSettings: give a preset name");
            return presetSettings(a.positional[0].get<std::string>(), userPresets());
        },
    });

    ctx.registerCommand({
        .name = "treeSavePreset",
        .doc = "Save settings as a preset in the preferences. Built in names are refused.",
        .positionalDoc = "name",
        .flags = settingsFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
            if (name.empty())
                throw CommandError("Give the preset a name.");
            if (isBuiltIn(name))
                throw CommandError("'" + name + "' is a built in preset, pick another name.");
            json presets = userPresets();
            presets[name] = callOf(a).values;
            PrefStore::get().set(kPrefsFile, "/Presets", presets);
            PrefStore::get().set(kPrefsFile, "/Preset", name);
            return name;
        },
    });

    ctx.registerCommand({
        .name = "treeDeletePreset",
        .doc = "Delete a saved preset. Built in presets cannot be deleted.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = !a.positional.empty() && a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
            if (isBuiltIn(name))
                throw CommandError("'" + name + "' is a built in preset.");
            json presets = userPresets();
            if (!presets.contains(name))
                return false;
            presets.erase(name);
            PrefStore::get().set(kPrefsFile, "/Presets", presets);
            if (prefs()["Preset"] == name)
                PrefStore::get().set(kPrefsFile, "/Preset", builtInPresets().front());
            return true;
        },
    });

    ctx.registerCommand({
        .name = "treeBakeHandoff",
        .doc = "What a bake project needs from a tree's export: {kind, high, low, sidecar, palette, materials, faces, rays}. Reads, never writes.",
        .positionalDoc = "tree (default the selection)",
        .flags = settingsFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId tree = treeArg(g, a, "treeBakeHandoff");
            if (!tree)
                throw CommandError("Select a tree built by this tool.");
            // The stored settings carry the paths treeExport wrote to; settings passed here override them.
            const json stored = storedSettings(g, tree);
            if (!stored.is_object() || a.has("preset"))
                return bakeHandoff(g, tree, callOf(a).values);
            json values = stored["values"];
            if (a.has("settings") && a.flags["settings"].is_object())
                for (const auto& [k, v] : a.flags["settings"].items())
                    values[k] = v;
            return bakeHandoff(g, tree, sanitize(values));
        },
    });

    {
        std::vector<FlagDef> flags = settingsFlags();
        flags.push_back({"name", "", T::String, nullptr, "Base name of the sets (<name>_Bark, <name>_Leaves); the tree's by default."});
        flags.push_back({"season", "", T::String, "summer", "Leaf palette: spring, summer, autumn or lateAutumn."});
        flags.push_back({"target", "", T::String, "unreal", "Bake target and export preset."});
        flags.push_back({"bake", "", T::Bool, true, "Make the bake project when the tree is exported."});
        flags.push_back({"replace", "", T::Bool, false, "Replace texture sets of those names."});
        ctx.registerCommand({
            .name = "treeTextureSet",
            .doc = "Make native texture sets for a tree instead of handing the palette to Painter: <name>_Bark (the species' bark tones over "
                   "cavity, edges and AO) and <name>_Leaves (the season's leaf tones over the leaf colour set: R exposure, G per leaf, B per "
                   "bunch), on the trunk and leaf materials, with a bake project from the export when there is one. Returns {sets, "
                   "bakeProject, season, species, warnings}.",
            .positionalDoc = "tree (default the selection)",
            .flags = flags,
            .run = [](const Args& a) -> json {
                Graph& g = graph();
                const NodeId tree = treeArg(g, a, "treeTextureSet");
                if (!tree)
                    throw CommandError("Select a tree built by this tool.");
                return createTreeTextureSets(g, tree, a);
            },
        });
    }
}
} // namespace

json userPresets()
{
    json p = PrefStore::get().value(kPrefsFile, "/Presets", json::object());
    return p.is_object() ? p : json::object();
}

NodeId treeInSelection(Graph& g)
{
    for (NodeId id : g.selection())
        for (NodeId cur = id; cur; cur = g.parent(cur))
        {
            Node* n = g.node(cur);
            if (n && (hasSettings(*n) || isPart(*n)))
                return treeOwner(g, cur);
        }
    return kNoNode;
}

json storedSettings(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n || !hasSettings(*n))
        return nullptr;
    json sources = parse(str(*n, kAttrSources), json::object());
    return {{"values", sanitize(parse(str(*n, kAttrSettings), json::object()))},
            {"sources", sources.is_object() ? sources : json::object()},
            {"preset", str(*n, kAttrPreset)}};
}

json bakeHandoff(Graph& g, NodeId tree, const json& values)
{
    const json e = exportSettings(sanitize(values));
    const auto parts = findParts(g, tree);
    if (parts.empty())
        throw CommandError(g.node(tree)->path() + " has no trunk, canopy or leaves on it.");
    const fs::path folder = utf8(e["path"].get<std::string>());
    const std::string name = e["name"].get<std::string>();
    std::map<std::string, std::vector<NodeId>> lowParts;
    for (const auto& [part, found] : parts)
        lowParts[part] = pickLod(g, found, false);
    const json materials = {{"trunk", e["trunk_material"]}, {"canopy", e["canopy_material"]}, {"leaves", e["leaf_material"]}};
    const json side = sidecarData(g, storedSettings(g, tree), name, materials, lowParts, "tree");
    auto path = [&](const std::string& suffix) { return name.empty() ? std::string() : utf8(folder / utf8(name + suffix)); };
    return {{"kind", "tree"},
            {"tree", g.node(tree)->path()},
            {"high", path(std::string(kHighSuffix) + ".fbx")},
            {"low", path(std::string(kLowSuffix) + ".fbx")},
            {"sidecar", path(kSidecarSuffix)},
            {"palette", path(kPaletteSuffix)},
            {"materials", materials},
            {"faces", side["faces"]},
            {"species", side["species"]},
            {"colourSet", side["colourSet"]},
            {"rays", measureBake(g, tree, values)}};
}

bool registerScene(ext::Context& ctx)
{
    if (!registerTypes(ctx))
        return false;
    registerCommands(ctx);
    return true;
}
} // namespace treegen
