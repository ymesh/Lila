#include "link/UnrealLink.h"

#include "math/Unreal.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "link/Exporters.h"
#include "link/SplineSend.h"
#include "link/TextureSetSend.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/AttrComponents.h"
#include "scene/Deformers.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/LodGroup.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/ObjectSets.h"
#include "scene/Pin.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/Take.h"
#include "scene/UnrealCheck.h"
#include "scene/ViewState.h"
#include "seq/MeshSequence.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>

namespace lila::link
{
namespace
{
/** Hidden user attr holding a node's mapping as JSON. */
constexpr const char* kMappingAttr = "unrealMapping";
constexpr int kHelloTimeoutMs = 3000;
constexpr const char* kExporterOwner = "lila.unreal";
constexpr int kImportTimeoutMs = 10 * 60 * 1000;
constexpr int kImportIdleTimeoutMs = 60 * 1000;

const char* const kOptionKeys[] = {
    "skeleton", "importMaterials", "importTextures", "combineMeshes", "autoCollision", "collision", "generateLightmapUVs",
    "normals",  "lodGroup",        "existing",       "nanite",        "importAnimations", "reapplyOptions", "save",
    "materialFolder", "keepMaterials", "lodScreenSizes", "materialCurves", "facing", "lodForCollision",
};

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

fs::path projectRoot(const fs::path& p)
{
    return p.extension() == ".uproject" ? p.parent_path() : p;
}

bool sameDir(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    fs::path ca = fs::weakly_canonical(projectRoot(a), ec);
    fs::path cb = fs::weakly_canonical(projectRoot(b), ec);
    auto norm = [](const fs::path& p) {
        std::string s = lower(p.lexically_normal().generic_string());
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        return s;
    };
    return norm(ca) == norm(cb);
}

bool matchesProject(const Instance& i, const std::string& project)
{
    if (project.empty())
        return true;
    if (lower(i.project) == lower(project))
        return true;
    return !i.projectDir.empty() && sameDir(i.projectDir, fs::path(project));
}

std::string fileSafe(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
            c = '_';
    return out.empty() ? "unnamed" : out;
}

/** A mapping's node (a full path, or a name in older mappings) made file safe, without the leading '|'. */
std::string nodeFileStem(const std::string& node)
{
    return fileSafe(node.starts_with('|') ? node.substr(1) : node);
}

/** The node a mapping names; null when it is gone or the name has become ambiguous. */
scene::Node* mappedNode(const std::string& node)
{
    try
    {
        return node.empty() ? nullptr : scene::Graph::current().find(node);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** The mapping a node carries in kMappingAttr, without "node"; null when it has none. */
json storedMapping(const scene::Node& n)
{
    const int attr = n.type().attrIndex(kMappingAttr);
    if (attr < 0)
        return nullptr;
    const auto* text = std::get_if<std::string>(&n.value(attr));
    if (!text || text->empty())
        return nullptr;
    json m = json::parse(*text, nullptr, false);
    return m.is_object() ? m : json();
}

/** Static and skeletal mappings export meshes; one with none under it (a joint chain on its own) has nothing to send. */
bool sendsNothing(scene::Graph& g, const scene::Node& n, const std::string& kind)
{
    return (kind == "static" || kind == "skeletal") && !scene::objectSets::isSet(n) && seq::meshNodes(g, {n.id()}).empty();
}

void reportImport(const std::string& what, const RpcResult& r)
{
    if (!r.ok)
    {
        log::error("Unreal import of {} failed: {}", what, r.error);
        return;
    }
    const json& res = r.result;
    for (const json& w : res.value("warnings", json::array()))
        log::warn("Unreal: {}: {}", what, w.is_string() ? w.get<std::string>() : w.dump());
    json errors = res.value("errors", json::array());
    for (const json& e : errors)
        log::error("Unreal: {}: {}", what, e.is_string() ? e.get<std::string>() : e.dump());
    if (errors.empty())
        log::info("Unreal: {} -> {}", what, res.value("assets", json::array()).dump());
}

/** The editor's component exporters, from hello, replace the ones the last editor listed. */
void listUnrealExporters(const json& names)
{
    Registry<std::string>& r = scene::attrComponents::unrealExporters();
    r.removeOwned(kExporterOwner);
    for (const json& name : names.is_array() ? names : json::array())
        if (name.is_string() && !r.contains(name.get<std::string>()))
            r.add(name.get<std::string>(), "Registered in the Unreal editor", kExporterOwner);
}

/** One frame of a meshSequence mapping: a static mesh import at <folder>/<base>_<index>. */
json frameMapping(const json& m, const std::string& contentPath)
{
    json fm = m;
    fm["kind"] = "static";
    fm["contentPath"] = contentPath;
    return fm;
}

/** Imports the frames in order, points the Niagara renderer at them, then drops frames past the end. Worker thread. */
void syncSequence(RpcClient& client, const json& m, const std::vector<fs::path>& files, const json& materials, progress::Scope& scope)
{
    LILA_ZONE();

    const std::string node = m.value("node", std::string());
    const SequenceAssets names = sequenceAssets(m, int(files.size()));
    json meshes = json::array();
    for (size_t i = 0; i < files.size(); ++i)
    {
        scope.setStatus(std::format("Unreal: sending {} frame {} of {}", node, i + 1, files.size()));
        RpcResult r = client.call("importAsset", importParams(files[i], frameMapping(m, names.contentPaths[i]), materials), importCallOptions());
        if (!r.ok || !r.result.value("errors", json::array()).empty())
        {
            mainq::post([what = names.contentPaths[i], r] { reportImport(what, r); });
            return;
        }
        meshes.push_back(names.objectPaths[i]);
    }
    mainq::post([node, n = files.size(), folder = names.folder] { log::info("Unreal: {} -> {} frame(s) in {}", node, n, folder); });

    std::string system = m.value("niagaraSystem", std::string());
    if (!system.empty())
    {
        scope.setStatus("Unreal: updating " + system);
        json params = {{"system", system}, {"meshes", meshes}};
        if (m.contains("niagaraEmitter") && m["niagaraEmitter"].is_string())
            params["emitter"] = m["niagaraEmitter"];
        if (m.contains("niagaraRenderer") && m["niagaraRenderer"].is_number_integer())
            params["renderer"] = m["niagaraRenderer"];
        if (m.value("save", false))
            params["save"] = true;
        RpcResult r = client.call("setNiagaraMeshes", params, importCallOptions());
        mainq::post([system, r] {
            if (!r.ok)
                log::error("Unreal: setting the meshes of {} failed: {}", system, r.error);
            else
                log::info("Unreal: {} renderer(s) of {} now use the frames", r.result.value("renderers", 0), system);
        });
    }

    RpcResult listed = client.call("listAssets", {{"path", names.folder}, {"recursive", false}, {"class", "StaticMesh"}}, kHelloTimeoutMs);
    if (!listed.ok)
        return;
    json surplus = json::array();
    for (const std::string& p : surplusFrames(listed.result, names.base, int(files.size())))
        surplus.push_back(p);
    if (surplus.empty())
        return;
    RpcResult r = client.call("deleteAssets", {{"paths", surplus}}, importCallOptions());
    mainq::post([node, surplus, r] {
        if (!r.ok)
            log::error("Unreal: removing surplus frames of {} failed: {}", node, r.error);
        else
            log::info("Unreal: removed {} frame(s) of {} past the end of the range: {}", surplus.size(), node, surplus.dump());
    });
}

scene::NodeId topJointOf(scene::Graph& g, scene::NodeId j)
{
    while (g.parent(j) && scene::rig::isJoint(*g.node(g.parent(j))))
        j = g.parent(j);
    return j;
}

/** Whether a skeletal export of node writes the skeleton topped by top: its joints are under node or skin a mesh there. */
bool exportsSkeleton(scene::Graph& g, scene::NodeId node, scene::NodeId top)
{
    std::vector<scene::NodeId> all{node};
    for (scene::NodeId d : g.descendants(node))
        all.push_back(d);
    for (scene::NodeId id : all)
    {
        if (scene::rig::isJoint(*g.node(id)) && topJointOf(g, id) == top)
            return true;
        if (const scene::NodeId sc = scene::skin::find(g, id); sc != scene::kNoNode)
            for (const scene::skin::Influence& inf : scene::skin::influences(g, sc))
                if (inf.joint != scene::kNoNode && topJointOf(g, inf.joint) == top)
                    return true;
    }
    return false;
}

/**
 * A socket offset in the bone's Unreal space: the prop's matrix in the bone's rigid space (Lila) taken through the
 * skeletal export's axes on the bone side and the static export's on the prop's, so the prop's mesh attached to the
 * socket lands where it was in Lila.
 */
json unrealSocketOffset(const glm::dmat4& rel)
{
    auto colsOf = [](math::Vector (*f)(const math::Vector&)) {
        glm::dmat3 m;
        for (int c = 0; c < 3; ++c)
        {
            glm::dvec3 e(0.0);
            e[c] = 1.0;
            m[c] = f(math::Vector(e)).toGlm();
        }
        return m;
    };
    const glm::dmat3 bone = colsOf(&math::characterToUnreal), mesh = colsOf(&math::toUnreal);
    const glm::dmat3 r = bone * glm::dmat3(rel) * glm::inverse(mesh);
    const glm::dvec3 at = bone * glm::dvec3(rel[3]);
    const glm::dvec3 x = r[0], y = r[1];
    return {{"location", json::array({at.x, at.y, at.z})}, {"xAxis", json::array({x.x, x.y, x.z})}, {"yAxis", json::array({y.x, y.y, y.z})}};
}

/**
 * A pinned prop's socket for its import: the joint bakePin made for it (no offset), or the bone and offset
 * puppeteerPropUnreal gave it, and the skeletal mapping that sends that joint's skeleton, whose mesh gets the socket.
 * Null when the prop has none or no mapping sends the skeleton.
 */
json socketParams(scene::Graph& g, const scene::Node& prop, const json& list)
{
    const std::string socket = scene::pins::socketOf(prop);
    if (socket.empty())
        return nullptr;
    const std::string bone = scene::pins::socketBoneOf(prop);
    const std::string joint = bone.empty() ? socket : bone;
    scene::NodeId found = scene::kNoNode;
    for (scene::NodeId id : g.findAll(joint))
        if (scene::rig::isJoint(*g.node(id)))
            found = id;
    if (!found)
    {
        log::warn("Unreal sync: {} attaches to {}, which is gone", prop.path(), joint);
        return nullptr;
    }
    const scene::NodeId top = topJointOf(g, found);
    for (const json& m : list)
    {
        if (!m.is_object() || m.value("kind", std::string()) != "skeletal" || m.value("contentPath", std::string()).empty())
            continue;
        if (scene::Node* n = mappedNode(m.value("node", std::string())); n && exportsSkeleton(g, n->id(), top))
        {
            json out = {{"name", socket}, {"bone", joint}, {"skeletalMesh", m["contentPath"]}};
            if (!bone.empty())
                out["offset"] = unrealSocketOffset(scene::pins::socketOffsetOf(prop));
            return out;
        }
    }
    log::warn("Unreal sync: {} attaches to {}, but no skeletal mapping sends its skeleton; map the character to make the socket",
              prop.path(), joint);
    return nullptr;
}
} // namespace

SequenceAssets sequenceAssets(const json& mapping, int count)
{
    SequenceAssets out;
    std::string path = mapping.value("contentPath", std::string());
    const size_t dot = path.find('.', path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
    if (dot != std::string::npos)
        path.resize(dot);
    if (path.ends_with('/'))
    {
        out.folder = path.substr(0, path.size() - 1);
        std::string node = mapping.value("node", std::string("Mesh"));
        out.base = fileSafe(node.substr(node.rfind('|') == std::string::npos ? 0 : node.rfind('|') + 1));
    }
    else
    {
        const size_t slash = path.rfind('/');
        out.folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
        out.base = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    json seqOpts = mapping.value("sequence", json::object());
    const int padding = seqOpts.is_object() ? seqOpts.value("padding", 3) : 3;
    for (int i = 0; i < count; ++i)
    {
        std::string name = seq::frameName("{name}_{index}", out.base, i, double(i), padding);
        out.contentPaths.push_back(out.folder + "/" + name);
        out.objectPaths.push_back(out.folder + "/" + name + "." + name);
    }
    return out;
}

std::vector<std::string> surplusFrames(const json& listed, const std::string& base, int count)
{
    std::vector<std::string> out;
    if (!listed.is_array())
        return out;
    std::string escaped;
    for (char c : base)
    {
        if (std::string_view("\\^$.|?*+()[]{}").find(c) != std::string_view::npos)
            escaped += '\\';
        escaped += c;
    }
    const std::regex frame("^" + escaped + "_([0-9]+)$");
    for (const json& a : listed)
    {
        std::string path = a.is_object() ? a.value("path", std::string()) : a.is_string() ? a.get<std::string>() : std::string();
        std::string name = path.substr(path.rfind('/') + 1);
        if (size_t dot = name.find('.'); dot != std::string::npos)
            name.resize(dot);
        std::smatch match;
        if (std::regex_match(name, match, frame) && std::stoll(match[1].str()) >= count)
            out.push_back(path);
    }
    return out;
}

seq::SequenceOptions sequenceOptions(const json& mapping, const fs::path& folder)
{
    json s = mapping.value("sequence", json::object());
    if (!s.is_object())
        s = json::object();
    seq::SequenceOptions o = seq::fromJson(s);
    o.playbackRange = s.value("playbackRange", !s.contains("start") && !s.contains("end"));
    o.format = mapping.value("kind", std::string()) == "geometryCache" ? "abc" : "fbx";
    o.folder = folder.generic_string();
    o.pattern = "{name}_{index}";
    o.profile = "ue5";
    return o;
}

seq::VatOptions vertexAnimationOptions(const json& mapping, const fs::path& folder, const std::string& name)
{
    json v = mapping.value("vat", json::object());
    if (!v.is_object())
        throw std::invalid_argument("vat must be a dict");
    seq::VatOptions o = seq::vatFromJson(v, sequenceOptions(mapping, folder));
    o.folder = folder.generic_string();
    o.name = name;
    return o;
}

json vertexAnimationParams(const seq::VatResult& bake, const json& mapping)
{
    json options = json::object();
    for (const char* key : kOptionKeys)
        if (mapping.contains(key) && !mapping[key].is_null())
            options[key] = mapping[key];
    json params = {{"contentPath", mapping.value("contentPath", std::string())},
                   {"fbx", bake.mesh.generic_string()},
                   {"positions", bake.positions.generic_string()},
                   {"metaFile", bake.meta.generic_string()},
                   {"meta", bake.metadata},
                   {"options", options}};
    if (!bake.normals.empty())
        params["normals"] = bake.normals.generic_string();
    if (!bake.rotations.empty())
        params["rotations"] = bake.rotations.generic_string();
    // The plugin names the textures T_<asset without SM_>_VATPos / _VATNrm / _VATRot beside the mesh.
    std::string path = mapping.value("contentPath", std::string());
    if (size_t dot = path.find('.', path.rfind('/') == std::string::npos ? 0 : path.rfind('/')); dot != std::string::npos)
        path.resize(dot);
    const size_t slash = path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
    std::string asset = slash == std::string::npos ? path : path.substr(slash + 1);
    if (asset.starts_with("SM_"))
        asset = asset.substr(3);
    auto objectPath = [&](const std::string& suffix) {
        const std::string name = "T_" + asset + suffix;
        return folder + "/" + name + "." + name;
    };
    json& mp = params["meta"]["materialParams"];
    if (mp.is_object())
    {
        mp["PositionTexture"] = objectPath("_VATPos");
        if (!bake.normals.empty())
            mp["NormalTexture"] = objectPath("_VATNrm");
        if (!bake.rotations.empty())
            mp["RotationTexture"] = objectPath("_VATRot");
    }
    json v = mapping.value("vat", json::object());
    if (v.is_object() && v.contains("materialParent") && v["materialParent"].is_string() && !v["materialParent"].get<std::string>().empty())
        params["materialParent"] = v["materialParent"];
    if (v.is_object() && v.value("rebuildMaterial", false))
        params["options"]["rebuildMaterial"] = true;
    return params;
}

json toJson(const Instance& i)
{
    return {{"port", i.port},
            {"pid", i.pid},
            {"project", i.project},
            {"projectDir", i.projectDir.generic_string()},
            {"engineVersion", i.engineVersion},
            {"source", i.source}};
}

std::vector<Instance> readPortFiles(const std::vector<fs::path>& projectDirs)
{
    LILA_ZONE();

    std::vector<Instance> out;
    for (const fs::path& dir : projectDirs)
    {
        fs::path file = projectRoot(dir) / "Saved" / "LilaLink" / "port.json";
        std::ifstream in(file);
        if (!in)
            continue;
        json j = json::parse(in, nullptr, false);
        if (!j.is_object() || !j.contains("port") || !j["port"].is_number_integer())
            continue;
        Instance i;
        i.port = static_cast<uint16_t>(j["port"].get<int>());
        i.pid = j.value("pid", int64_t{0});
        i.project = j.value("project", std::string());
        i.projectDir = j.contains("projectDir") && j["projectDir"].is_string() ? fs::path(j["projectDir"].get<std::string>()) : projectRoot(dir);
        i.engineVersion = j.value("engineVersion", std::string());
        i.source = "portFile";
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<fs::path> configuredProjects()
{
    std::vector<fs::path> out;
    json list = PrefStore::get().value(kUnrealPrefFile, "/projects", json::array());
    if (!list.is_array())
        return out;
    for (const json& p : list)
    {
        if (!p.is_string())
            continue;
        out.push_back(paths::resolvePath(p.get<std::string>(), {}).lexically_normal());
    }
    return out;
}

std::vector<Instance> discover(bool probe, const std::string& host)
{
    LILA_ZONE();

    std::vector<Instance> out = readPortFiles(configuredProjects());
    if (!probe)
        return out;
    for (int p = kDefaultPort; p < kDefaultPort + kPortSearchCount; ++p)
    {
        uint16_t port = static_cast<uint16_t>(p);
        if (std::ranges::any_of(out, [&](const Instance& i) { return i.port == port; }))
            continue;
        if (TcpSocket::connect(host, port, 150).valid())
            out.push_back({.port = port, .source = "probe"});
    }
    return out;
}

json mappings()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> ids = g.nodes();
    std::ranges::sort(ids);
    json list = json::array();
    for (scene::NodeId id : ids)
        if (const scene::Node* n = g.node(id))
            if (json m = storedMapping(*n); m.is_object())
            {
                m["node"] = g.fullPath(id);
                list.push_back(std::move(m));
            }
    return list;
}

json mappingOf(const std::string& node)
{
    scene::Node* n = mappedNode(node);
    if (!n)
        return nullptr;
    json m = storedMapping(*n);
    if (m.is_object())
        m["node"] = n->path();
    return m;
}

void setMapping(const json& m)
{
    LILA_ZONE();

    const std::string node = m.value("node", std::string());
    scene::Node* n = mappedNode(node);
    if (!n)
        throw std::invalid_argument("no node named '" + node + "'");
    scene::Graph& g = scene::Graph::current();
    int attr = n->type().attrIndex(kMappingAttr);
    if (attr < 0)
        attr = g.addAttr(n->id(), {.name = kMappingAttr,
                                   .type = scene::PlugType::String,
                                   .defaultValue = std::string(),
                                   .flags = scene::AttrHidden | scene::AttrUnique,
                                   .doc = "Where this node goes in Unreal (unrealMap). Duplicates don't inherit it."});
    json stored = m;
    stored.erase("node");
    g.setValue({n->id(), attr, -1}, stored.dump());
}

bool removeMapping(const std::string& node)
{
    scene::Node* n = mappedNode(node);
    if (!n || storedMapping(*n).is_null())
        return false;
    scene::Graph::current().deleteAttr({n->id(), n->type().attrIndex(kMappingAttr), -1});
    return true;
}

void adoptWorkspaceMappings()
{
    LILA_ZONE();

    json legacy = PrefStore::get().value(kUnrealPrefFile, "/mappings", json::array());
    if (!legacy.is_array() || legacy.empty())
        return;
    scene::Graph& g = scene::Graph::current();
    UndoStack& undo = UndoStack::get();
    const bool wasEnabled = undo.enabled();
    undo.setEnabled(false);
    int adopted = 0;
    for (const json& m : legacy)
    {
        scene::Node* n = m.is_object() ? mappedNode(m.value("node", std::string())) : nullptr;
        if (!n || storedMapping(*n).is_object() || sendsNothing(g, *n, m.value("kind", std::string("static"))))
            continue;
        setMapping(m);
        ++adopted;
    }
    undo.setEnabled(wasEnabled);
    if (adopted > 0)
    {
        io::markModified();
        log::info("Unreal: {} mapping(s) moved from the workspace into this scene; save to keep them", adopted);
    }
}

void pruneWorkspaceMappings()
{
    json legacy = PrefStore::get().value(kUnrealPrefFile, "/mappings", json::array());
    if (!legacy.is_array() || legacy.empty())
        return;
    json kept = json::array();
    for (const json& m : legacy)
        if (m.is_object() && !mappedNode(m.value("node", std::string())))
            kept.push_back(m);
    if (kept.size() == legacy.size())
        return;
    if (kept.empty())
        PrefStore::get().remove(kUnrealPrefFile, "/mappings", PrefScope::Workspace);
    else
        PrefStore::get().set(kUnrealPrefFile, "/mappings", kept, PrefScope::Workspace);
}

/** Tension curves go out as plain animation curves; the plugin flags them as material curves on the skeleton. */
void addMaterialCurves(scene::Graph& g, json& mapping, const std::string& kind)
{
    if ((kind != "skeletal" && kind != "animation") || mapping.contains("materialCurves"))
        return;
    json names = json::array();
    for (const scene::deformers::TensionCurve& t : scene::deformers::tensionCurves(g))
        names.push_back(t.name);
    if (!names.empty())
        mapping["materialCurves"] = names;
}

json importParams(const fs::path& file, const json& mapping, const json& materials)
{
    json options = json::object();
    for (const char* key : kOptionKeys)
        if (mapping.contains(key) && !mapping[key].is_null() && !(mapping[key].is_string() && mapping[key].get<std::string>().empty()))
            options[key] = mapping[key];
    const std::string kind = mapping.value("kind", std::string("static"));
    if (kind == "static" && mapping.contains("socket") && mapping["socket"].is_object())
        options["socket"] = mapping["socket"];
    if ((kind == "skeletal" || kind == "animation") && !options.contains("facing"))
        options["facing"] = PrefStore::get().value<std::string>(kUnrealPrefFile, "/facing", "x");
    json params = {{"file", file.generic_string()},
                   {"contentPath", mapping.value("contentPath", std::string())},
                   {"kind", kind},
                   {"options", options}};
    if (materials.is_array())
        params["materials"] = materials;
    if (mapping.contains("components") && mapping["components"].is_array())
        params["components"] = mapping["components"];
    return params;
}

json materialParams(scene::Graph& g, const std::vector<scene::NodeId>& roots)
{
    LILA_ZONE();

    g.evaluate();
    std::vector<std::string> slots;
    auto addSlots = [&](scene::NodeId id) {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || n->type().isA("lattice"))
            return;
        const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!mesh)
            return;
        for (const auto& [slot, count] : scene::materials::slotUsage(*mesh))
            if (count > 0 && slot != scene::materials::kDefaultSlot && std::ranges::find(slots, slot) == slots.end())
                slots.push_back(slot);
    };
    for (scene::NodeId root : roots)
    {
        addSlots(root);
        for (scene::NodeId d : g.descendants(root))
            addSlots(d);
    }

    json out = json::array();
    for (const std::string& slot : slots)
        out.push_back(materialInputs(g, slot));
    return out;
}

json materialInputs(scene::Graph& g, const std::string& slot)
{
    json m = {{"name", slot}};
    if (scene::Node* mat = scene::materials::find(g, slot))
    {
        const auto& defs = mat->type().attrs;
        for (int i = 0; i < scene::material::kCount; ++i)
        {
            const scene::AttrDef& d = defs[size_t(i)];
            if (d.type == scene::PlugType::Enum)
            {
                int64_t v = mat->as<int64_t>(i);
                m[d.name] = v >= 0 && size_t(v) < d.enumNames.size() ? json(d.enumNames[size_t(v)]) : json(v);
            }
            else if (i == scene::material::kStrokeMap)
                m[d.name] = fs::path(scene::materials::strokeSheetPath(mat->as<std::string>(i))).generic_string();
            else if (d.flags & scene::AttrFile)
            {
                std::string path = mat->as<std::string>(i);
                if (!path.empty())
                    m[d.name] = fs::path(scene::materials::resolvePath(path)).generic_string();
            }
            else
                m[d.name] = scene::toJson(mat->value(i));
        }
    }
    return m;
}

CallOptions importCallOptions()
{
    PrefStore& prefs = PrefStore::get();
    return {.timeoutMs = prefs.value<int>(kUnrealPrefFile, "/importTimeoutMs", kImportTimeoutMs),
            .idleTimeoutMs = prefs.value<int>(kUnrealPrefFile, "/importIdleTimeoutMs", kImportIdleTimeoutMs)};
}

fs::path cacheDir()
{
    std::string custom = PrefStore::get().value<std::string>(kUnrealPrefFile, "/cacheDir", "");
    if (!custom.empty())
        return paths::resolvePath(custom, {});
    fs::path base = Workspace::get().isOpen() ? Workspace::get().stateDir() : paths::userConfigDir();
    return base / "cache" / "unreal";
}

Link& Link::get()
{
    static Link link;
    return link;
}

Link::~Link()
{
    shutdown();
}

json Link::connect(const ConnectOptions& opts)
{
    LILA_ZONE();

    std::vector<Instance> candidates;
    if (opts.port)
    {
        candidates.push_back({.port = *opts.port, .source = "explicit"});
    }
    else
    {
        for (const Instance& i : discover(true, opts.host))
            if (matchesProject(i, opts.project) || i.source == "probe")
                candidates.push_back(i);
    }
    if (candidates.empty())
        throw std::runtime_error("no Unreal editor with LilaLink found (add its project dir to the \"projects\" list in "
                                 "the unreal prefs, or pass a port)");

    std::string lastError = "no Unreal editor answered";
    for (const Instance& i : candidates)
    {
        auto client = std::make_shared<RpcClient>();
        std::string err;
        if (!client->connect(opts.host, i.port, opts.timeoutMs, &err))
        {
            lastError = err;
            continue;
        }
        RpcResult hello = client->call("hello", json::object(), kHelloTimeoutMs);
        if (!hello.ok || !hello.result.is_object())
        {
            lastError = "port " + std::to_string(i.port) + " did not answer hello: " + hello.error;
            continue;
        }
        // A probed port only counts if hello names the project we asked for.
        Instance named = i;
        named.project = hello.result.value("project", std::string());
        named.projectDir = hello.result.value("projectDir", std::string());
        if (!matchesProject(named, opts.project))
        {
            lastError = "port " + std::to_string(i.port) + " is project " + named.project;
            continue;
        }

        json features = hello.result.value("features", json::array());
        if (!features.is_array() || std::ranges::find(features, json("progress")) == features.end())
            log::warn("The LilaLink plugin in {} is out of date ({}); rebuild it from Lila's ue/LilaLink so imports "
                      "report progress and cannot hang the editor",
                      named.project, hello.result.value("pluginVersion", std::string("unknown version")));

        uint16_t port = i.port;
        client->onDisconnected([port] { mainq::post([port] { log::warn("Unreal editor on port {} disconnected", port); }); });
        json h = hello.result;
        h["port"] = port;
        {
            std::scoped_lock lock(mMutex);
            mClient = client;
            mHello = h;
        }
        mainq::post([exporters = h.value("componentExporters", json::array())] { listUnrealExporters(exporters); });
        log::info("Connected to Unreal {} ({}) on port {}", h.value("project", std::string()),
                  h.contains("engine") ? h["engine"].value("string", std::string()) : std::string(), port);
        return h;
    }
    throw std::runtime_error(lastError);
}

void Link::ensureConnected()
{
    if (!connected())
        connect({});
}

void Link::disconnect()
{
    std::shared_ptr<RpcClient> c;
    {
        std::scoped_lock lock(mMutex);
        c.swap(mClient);
        mHello = json();
    }
    if (c)
        c->disconnect();
}

bool Link::connected() const
{
    std::scoped_lock lock(mMutex);
    return mClient && mClient->connected();
}

json Link::status() const
{
    std::scoped_lock lock(mMutex);
    if (!mClient || !mClient->connected())
        return {{"connected", false}};
    json s = mHello;
    s["connected"] = true;
    return s;
}

std::shared_ptr<RpcClient> Link::client() const
{
    std::scoped_lock lock(mMutex);
    return mClient && mClient->connected() ? mClient : nullptr;
}

void Link::post(std::function<void()> fn)
{
    std::scoped_lock lock(mQueueMutex);
    if (mQuit)
        return;
    if (!mWorker.joinable())
        mWorker = std::thread([this] { workerLoop(); });
    mQueue.push_back(std::move(fn));
    mQueueCv.notify_all();
}

void Link::waitIdle()
{
    auto idle = [&] { return (mQueue.empty() && !mBusy) || mQuit; };
    std::unique_lock lock(mQueueMutex);
    if (!mainq::isMainThread())
    {
        mQueueCv.wait(lock, idle);
        return;
    }
    // Jobs hand scene snapshots to the main thread, and nothing else drains it while a command blocks here (always so in batch).
    while (!mQueueCv.wait_for(lock, std::chrono::milliseconds(20), idle))
    {
        lock.unlock();
        mainq::drain();
        lock.lock();
    }
}

void Link::shutdown()
{
    {
        std::scoped_lock lock(mQueueMutex);
        mQuit = true;
        mQueue.clear();
        mQueueCv.notify_all();
    }
    if (mWorker.joinable())
        mWorker.join();
    disconnect();
}

void Link::workerLoop()
{
    LILA_THREAD_NAME("Unreal sync");

    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(mQueueMutex);
            mBusy = false;
            mQueueCv.notify_all();
            mQueueCv.wait(lock, [&] { return mQuit || !mQueue.empty(); });
            if (mQuit)
                return;
            job = std::move(mQueue.front());
            mQueue.pop_front();
            mBusy = true;
        }
        try
        {
            job();
        }
        catch (const std::exception& e)
        {
            std::string msg = e.what();
            mainq::post([msg] { log::error("Unreal sync: {}", msg); });
        }
    }
}

void onSceneSaved()
{
    pruneWorkspaceMappings();
    syncMappings(false);
}

json preflight(scene::Graph& g, const std::vector<scene::NodeId>& nodes, const std::string& command, bool runValidators)
{
    LILA_ZONE();

    namespace dg = scene::diagnostics;
    if (runValidators)
        dg::validate();
    std::set<scene::NodeId> sent;
    for (scene::NodeId id : nodes)
    {
        sent.insert(id);
        for (scene::NodeId d : g.descendants(id))
            sent.insert(d);
    }
    json issues = json::array();
    std::string stops;
    for (const dg::Issue& i : dg::issues())
    {
        const bool scene = i.node == scene::kNoNode && i.object.empty();
        if (!scene && !sent.contains(i.node))
            continue;
        const bool blocks = std::ranges::any_of(i.blocks, [&](const std::string& b) { return b == command || b == "unreal" || b == "*"; });
        if (blocks)
        {
            stops += (stops.empty() ? "" : "; ") + (i.target.empty() ? std::string("scene") : i.target) + ": " + i.message;
            continue;
        }
        json j = {{"target", i.target}, {"severity", dg::severityName(i.severity)}, {"message", i.message}};
        if (!i.blocks.empty())
            j["blocks"] = i.blocks;
        issues.push_back(std::move(j));
    }
    if (!stops.empty())
        throw CommandError(command + ": blocked by validation: " + stops);
    return issues;
}

int syncMappings(bool force, const std::vector<std::string>& only, bool unrealCheck)
{
    LILA_ZONE();

    if (!force && !PrefStore::get().value<bool>(kUnrealPrefFile, "/autoSync", true))
        return 0;
    // The worker reads the evaluated meshes after this returns, so the scope lasts until it is done, then ends on the main thread.
    std::shared_ptr<scene::viewState::FinalScope> forOutput(new scene::viewState::FinalScope,
                                                            [](scene::viewState::FinalScope* f) { mainq::post([f] { delete f; }); });
    json list = mappings();
    const json all = list;
    if (!only.empty())
    {
        std::vector<scene::Node*> wanted;
        for (const std::string& name : only)
            if (scene::Node* n = mappedNode(name))
                wanted.push_back(n);
        json kept = json::array();
        for (json& m : list)
            if (std::ranges::find(wanted, mappedNode(m.value("node", std::string()))) != wanted.end())
                kept.push_back(std::move(m));
        list = std::move(kept);
    }
    auto withTextureSets = [&only](int count) { return only.empty() ? count + syncTextureSets() : count; };
    if (list.empty())
        return withTextureSets(0);

    const bool haveFbx = static_cast<bool>(findExporter("fbx"));
    int skipped = 0;

    struct Job
    {
        json mapping;
        json materials;
        std::vector<scene::NodeId> nodes;
        fs::path file;
        ExportFn exporter;
        /** meshSequence: the frame files, already written; the file above is then the first. */
        std::vector<fs::path> frames;
        /** vertexAnimation: importVertexAnimation params for the bake, already written. */
        json vat;
        /** spline: setSpline params, taken from the curve here on the main thread. */
        json spline;
    };
    std::vector<Job> jobs;
    scene::Graph& graph = scene::Graph::current();
    fs::path dir = cacheDir();
    for (const json& m : list)
    {
        if (!m.is_object())
            continue;
        std::string node = m.value("node", std::string());
        scene::Node* n = mappedNode(node);
        const std::string kind = m.value("kind", std::string("static"));
        if (!n || (kind != "spline" && m.value("contentPath", std::string()).empty()))
            continue;
        if (kind == "spline")
        {
            try
            {
                Job job{m, json(), {n->id()}, {}, nullptr, {}};
                job.spline = splineParams(graph, n->id(), m);
                jobs.push_back(std::move(job));
            }
            catch (const std::exception& e)
            {
                log::error("Unreal sync: {}: {}", node, e.what());
            }
            continue;
        }
        // "fbx:skeletal" / "fbx:animation" when registered, so the file matches the mapping's kind.
        const bool animated = kind == "meshSequence" || kind == "geometryCache" || kind == "vertexAnimation";
        if (!haveFbx && !animated)
        {
            ++skipped;
            continue;
        }
        ExportFn exporter = findExporter("fbx:" + kind);
        std::vector<scene::NodeId> nodes{n->id()};
        if (scene::objectSets::isSet(*n))
        {
            // A set is one asset: its members export together, keeping where they sit relative to each other.
            nodes = scene::objectSets::nodesOf(graph, n->id());
            if (nodes.empty())
                continue;
            if (ExportFn together = findExporter("fbx:set:" + kind))
                exporter = together;
        }
        if (kind == "vertexAnimation")
        {
            try
            {
                const seq::VatOptions vo = vertexAnimationOptions(m, dir / (nodeFileStem(node) + "_vat"), nodeFileStem(node));
                const seq::VatResult bake = seq::exportVat(graph, seq::meshNodes(graph, nodes), vo);
                Job job{m, json(), nodes, bake.mesh, nullptr, {}};
                job.vat = vertexAnimationParams(bake, m);
                log::info("Unreal sync: {} material params: {}", node, seq::vatMaterialSummary(job.vat["meta"]["materialParams"]));
                jobs.push_back(std::move(job));
            }
            catch (const std::exception& e)
            {
                log::error("Unreal sync: vertex animation bake of {} failed: {}", node, e.what());
            }
            continue;
        }
        json materials = materialParams(graph, nodes);
        if (animated)
        {
            const bool cache = kind == "geometryCache";
            const fs::path folder = cache ? dir : dir / (nodeFileStem(node) + "_frames");
            std::vector<fs::path> files;
            try
            {
                std::error_code ec;
                if (!cache)
                    fs::remove_all(folder, ec);
                seq::SequenceOptions so = sequenceOptions(m, folder);
                so.pattern = cache ? nodeFileStem(node) : "{name}_{index}";
                files = seq::exportSequence(graph, seq::meshNodes(graph, nodes), so);
            }
            catch (const std::exception& e)
            {
                log::error("Unreal sync: export of {} failed: {}", node, e.what());
                continue;
            }
            if (files.empty())
                continue;
            // Geometry caches have no material slots Lila manages; Unreal's Alembic importer makes them.
            Job job{m, cache ? json() : materials, nodes, files.front(), nullptr, {}};
            if (!cache)
                job.frames = files;
            jobs.push_back(std::move(job));
            continue;
        }
        // takes: all | [names] pushes each scene take as its own asset, <contentPath>_<take> (or <folder>/<take>).
        if (m.contains("takes") && !m["takes"].is_null() && (kind == "animation" || kind == "skeletal"))
        {
            TakeExportFn takeExporter = findTakeExporter("fbx:" + kind);
            std::vector<scene::NodeId> chosen = scene::anim::selectTakes(graph, m["takes"], n->id());
            if (!takeExporter)
                log::warn("Unreal sync: no take exporter for {}, {} skipped", kind, node);
            for (scene::NodeId t : takeExporter ? chosen : std::vector<scene::NodeId>{})
            {
                std::string take = graph.node(t)->name();
                json tm = m;
                addMaterialCurves(graph, tm, kind);
                std::string path = m.value("contentPath", std::string());
                tm["contentPath"] = path.ends_with('/') ? path + take : path + "_" + take;
                ExportFn fn = [takeExporter, take](const std::vector<scene::NodeId>& ns, const fs::path& file, std::string* err) {
                    return takeExporter(ns, file, take, err);
                };
                jobs.push_back({tm, materials, nodes, dir / (nodeFileStem(node) + "_" + fileSafe(take) + ".fbx"), fn});
            }
            continue;
        }
        if (sendsNothing(graph, *n, kind))
        {
            log::warn("Unreal sync: {} has no mesh under it, nothing to send (unrealUnmap(\"{}\") removes the mapping)", node, node);
            continue;
        }
        json mapping = m;
        addMaterialCurves(graph, mapping, kind);
        if (kind == "static")
            if (json socket = socketParams(graph, *n, all); !socket.is_null())
                mapping["socket"] = socket;
        // FBX carries the levels but not their screen sizes, so the plugin sets them on the mesh after the import.
        if (std::vector<double> sizes = scene::lod::exportScreenSizes(graph, nodes); !sizes.empty() && !mapping.contains("lodScreenSizes"))
            mapping["lodScreenSizes"] = sizes;
        // Sent even when empty, so the plugin clears components the object no longer has.
        mapping["components"] = scene::attrComponents::unrealPayload(graph, n->id());
        jobs.push_back({mapping, materials, nodes, dir / (nodeFileStem(node) + ".fbx"), exporter ? exporter : findExporter("fbx")});
    }
    if (skipped > 0)
        log::warn("Unreal sync: no FBX exporter registered, {} mapping(s) skipped", skipped);
    if (jobs.empty())
        return withTextureSets(0);

    int count = static_cast<int>(jobs.size());
    Link::get().post([jobs = std::move(jobs), dir, forOutput, unrealCheck] {
        LILA_ZONE_N("Unreal sync job");

        Link::get().ensureConnected();
        std::shared_ptr<RpcClient> client = Link::get().client();
        if (!client)
            throw std::runtime_error("not connected");

        std::error_code ec;
        fs::create_directories(dir, ec);
        progress::Scope scope("Sending to Unreal", double(jobs.size()), progress::Silent);
        for (const Job& job : jobs)
        {
            std::string node = job.mapping.value("node", std::string());
            std::optional<scene::uecheck::SkipScope> unchecked;
            if (!unrealCheck)
                unchecked.emplace();
            if (!job.spline.is_null())
            {
                scope.setStatus("Unreal: sending spline " + node);
                json features = Link::get().status().value("features", json::array());
                if (!features.is_array() || std::ranges::find(features, json(kSplineFeature)) == features.end())
                    mainq::post([node] { log::error("Unreal sync: {} not sent: the LilaLink plugin predates splines; rebuild it from Lila's ue/LilaLink", node); });
                else
                {
                    RpcResult r = client->call("setSpline", job.spline, importCallOptions());
                    mainq::post([node, r] {
                        if (!r.ok)
                            return log::error("Unreal: sending {} failed: {}", node, r.error);
                        for (const json& w : r.result.value("warnings", json::array()))
                            log::warn("Unreal: {}: {}", node, w.is_string() ? w.get<std::string>() : w.dump());
                        log::info("Unreal: {} -> {}", node, r.result.value("actor", std::string("spline")));
                    });
                }
                scope.step();
                continue;
            }
            if (!job.vat.is_null())
            {
                scope.setStatus("Unreal: sending vertex animation of " + node);
                RpcResult r = client->call("importVertexAnimation", job.vat, importCallOptions());
                mainq::post([node, r] { reportImport(node, r); });
                scope.step();
                continue;
            }
            if (!job.frames.empty())
            {
                syncSequence(*client, job.mapping, job.frames, job.materials, scope);
                scope.step();
                continue;
            }
            std::string err;
            scope.setStatus("Exporting " + node);
            if (job.exporter && !job.exporter(job.nodes, job.file, &err))
            {
                mainq::post([node, err] { log::error("Unreal sync: export of {} failed: {}", node, err); });
                scope.step();
                continue;
            }
            scope.setStatus("Unreal: sending " + node);
            CallOptions opts = importCallOptions();
            opts.onProgress = [&scope, node](const json& p) {
                scope.setStatus(std::format("Unreal: {} {} ({})", p.value("stage", std::string("working on")), node,
                                            progress::formatElapsed(p.value("elapsed", 0.0))));
            };
            RpcResult r = client->call("importAsset", importParams(job.file, job.mapping, job.materials), opts);
            mainq::post([node, r] { reportImport(node, r); });
            scope.step();
        }
    });
    // After the meshes, so their slots exist when the instances are set on them.
    return withTextureSets(count);
}
} // namespace lila::link
