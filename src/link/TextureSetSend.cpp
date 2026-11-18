#include "link/TextureSetSend.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "link/TextureSend.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/Node.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <set>

namespace lila::link
{
namespace
{
scene::Node* findNode(scene::Graph& g, const std::string& path)
{
    try
    {
        return path.empty() ? nullptr : g.find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** The exported maps (TextureSend's map names) a material map picked from a set comes from. */
std::vector<std::string> exportedMaps(const std::string& channel)
{
    if (channel == "ambientOcclusion")
        return {"ao", "orm", "mro"};
    if (channel == "roughness" || channel == "metallic")
        return {channel, "orm", "mro"};
    if (channel == "orm")
        return {"orm", "mro", "ao", "roughness", "metallic"};
    if (channel == "normal")
        return {"normal", "detailNormal"};
    return {channel};
}

/**
 * Which of the set's exported maps each material takes: null for all of them (it uses the whole set), else the map
 * names its per-map picks come from (exportedMaps).
 */
json materialMaps(scene::Graph& g, const std::vector<std::string>& users, const std::string& set)
{
    json out = json::object();
    for (const std::string& name : users)
    {
        scene::Node* m = scene::materials::find(g, name);
        if (!m)
            continue;
        if (scene::materials::textureSet(*m) == set)
        {
            out[name] = nullptr;
            continue;
        }
        json maps = json::array();
        for (const auto& [channel, picked] : scene::materials::textureSetMaps(*m))
            if (picked == set)
                for (const std::string& map : exportedMaps(channel))
                    if (std::ranges::find(maps, json(map)) == maps.end())
                        maps.push_back(map);
        out[name] = maps;
    }
    return out;
}

/** The materials using the set, whole or for some of their maps. */
std::vector<std::string> usersOf(scene::Graph& g, scene::Node& set) { return tex::materialsUsing(g, set.name()); }

/** Mapped Lila nodes whose meshes use one of the materials, and every mapped node's Unreal static mesh. */
void mappedUsers(scene::Graph& g, const std::vector<std::string>& slots, json& slotMeshes, json& meshes)
{
    slotMeshes = json::array();
    meshes = json::object();
    for (const json& m : mappings())
    {
        if (!m.is_object())
            continue;
        scene::Node* n = findNode(g, m.value("node", std::string()));
        if (!n)
            continue;
        if (m.value("kind", std::string("static")) == "static")
            meshes[n->path()] = m.value("contentPath", std::string());
        for (const json& mat : materialParams(g, {n->id()}))
            if (std::ranges::find(slots, mat.value("name", std::string())) != slots.end() &&
                std::ranges::find(slotMeshes, json(n->path())) == slotMeshes.end())
                slotMeshes.push_back(n->path());
    }
}

json rememberedSets() { return PrefStore::get().value<json>(kUnrealPrefFile, "/textureSets", json::array()); }

void rememberSet(const std::string& name)
{
    json sets = rememberedSets();
    if (!sets.is_array())
        sets = json::array();
    if (std::ranges::find(sets, json(name)) == sets.end())
    {
        sets.push_back(name);
        PrefStore::get().set(kUnrealPrefFile, "/textureSets", sets);
    }
}

struct Prepared
{
    std::string set;
    json params;
};

/** Exports the set and builds what goes to Unreal; textures is empty when nothing changed. */
Prepared prepare(scene::Graph& g, scene::Node& set, const json& flags, bool all)
{
    tex::ExportOptions eo;
    eo.preset = flags.value("preset", std::string("unreal"));
    eo.folder = flags.value("exportFolder", std::string("textures"));
    eo.incremental = !all;
    const json result = tex::exportSet(set, eo);
    for (const json& w : result.value("warnings", json::array()))
        log::warn("Unreal: {}: {}", set.name(), w.get<std::string>());

    const std::vector<std::string> users = usersOf(g, set);
    const std::string slot = users.empty() ? set.name() : users.front();
    PrefStore& prefs = PrefStore::get();
    json options = json::object();
    options["folder"] = flags.contains("folder") ? flags["folder"] : json(prefs.value<std::string>(kUnrealPrefFile, "/textureFolder", "/Game/Textures"));
    options["materialParent"] = flags.contains("materialParent") ? flags["materialParent"] : json(prefs.value<std::string>(kUnrealPrefFile, "/materialParent", ""));
    for (const char* k : {"materialFolder", "save"})
        if (flags.contains(k))
            options[k] = flags[k];
    json slotMeshes, meshes;
    mappedUsers(g, users, slotMeshes, meshes);
    options["slotMeshes"] = slotMeshes;
    options["materials"] = materialMaps(g, users, set.name());
    options["meshes"] = meshes;
    json warnings = json::array();
    Prepared p{set.name(), textureSetParams(result, materialInputs(g, slot), options, all, &warnings)};
    for (const json& w : warnings)
        log::warn("Unreal: {}: {}", set.name(), w.get<std::string>());
    return p;
}

void reportSetSend(const std::string& what, const RpcResult& r)
{
    if (!r.ok)
        return log::error("Unreal: sending {} failed: {}", what, r.error);
    for (const json& w : r.result.value("warnings", json::array()))
        log::warn("Unreal: {}: {}", what, w.is_string() ? w.get<std::string>() : w.dump());
    const json errors = r.result.value("errors", json::array());
    for (const json& e : errors)
        log::error("Unreal: {}: {}", what, e.is_string() ? e.get<std::string>() : e.dump());
    if (errors.empty())
        log::info("Unreal: {} -> {}", what, r.result.value("assets", json::array()).dump());
}

/** Why the connected editor's LilaLink can't take these params; empty when it can. */
std::string unsupported(const json& params)
{
    const json features = Link::get().status().value("features", json::array());
    auto has = [&](const char* f) { return features.is_array() && std::ranges::find(features, json(f)) != features.end(); };
    if (!has("textures"))
        return "the LilaLink plugin in this editor is too old for importTextures; rebuild it from Lila's ue/LilaLink";
    if (params.value("materialParent", std::string()).starts_with("lila:") && !has("stylisedInstances"))
        return "the LilaLink plugin in this editor can't make Painterly or Toon instances yet; rebuild it from Lila's ue/LilaLink";
    return {};
}
} // namespace

json textureSetParams(const json& exportResult, const json& material, const json& options, bool all, json* warnings)
{
    const std::string slot = material.value("name", std::string());
    const std::string model = material.value("shadingModel", std::string());
    const bool stylised = model == "painterly" || model == "toon";
    const std::string parent = stylised ? "lila:" + model : options.value("materialParent", std::string());

    // A UDIM texture is one asset: when any tile changed, all of its tiles go.
    std::set<std::string> changed;
    for (const json& o : exportResult.value("outputs", json::array()))
        if (all || o.value("changed", true))
            changed.insert(textureAssetName(o.value("file", std::string()), o.value("udim", 1001)));
    json outputs = json::array();
    const json takers = options.value("materials", json::object());
    for (json o : exportResult.value("outputs", json::array()))
    {
        if (!changed.contains(textureAssetName(o.value("file", std::string()), o.value("udim", 1001))))
            continue;
        o["materials"] = json::array();
        const std::string map = o.value("map", std::string());
        for (const auto& [m, maps] : takers.items())
            if (maps.is_null() || std::ranges::find(maps, json(map)) != maps.end())
                o["materials"].push_back(m);
        if (!options.contains("materials"))
            o["materials"].push_back(slot);
        o["meshes"] = options.value("slotMeshes", json::array());
        outputs.push_back(std::move(o));
    }

    json opts = {{"folder", options.value("folder", std::string("/Game/Textures"))}, {"materialMode", parent.empty() ? "none" : "instance"}};
    if (!parent.empty())
        opts["materialParent"] = parent;
    for (const char* k : {"materialFolder", "save", "meshes"})
        if (options.contains(k))
            opts[k] = options[k];
    json params = textureImportParams({{"outputs", outputs}}, opts, warnings);
    if (stylised)
        for (json& m : params["materials"])
        {
            // The stylised masters only sample a base colour map; their other inputs are values.
            json p = json::object();
            if (m["parameters"].contains("BaseColorMap"))
                p["BaseColorMap"] = m["parameters"]["BaseColorMap"];
            m["parameters"] = p;
            if (!p.empty())
                m["scalars"] = {{"UseBaseColorMap", 1.0}};
            m["stylised"] = material;
        }
    return params;
}

int syncTextureSets()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    std::set<std::string> names;
    for (const json& n : rememberedSets())
        if (n.is_string())
            names.insert(n.get<std::string>());
    for (const std::string& name : tex::setNames(g))
        if (scene::Node* set = tex::findSet(g, name))
        {
            json slotMeshes, meshes;
            mappedUsers(g, usersOf(g, *set), slotMeshes, meshes);
            if (!slotMeshes.empty())
                names.insert(name);
        }

    std::vector<Prepared> jobs;
    for (const std::string& name : names)
    {
        scene::Node* set = tex::findSet(g, name);
        if (!set)
            continue;
        try
        {
            Prepared p = prepare(g, *set, json::object(), false);
            if (!p.params["textures"].empty())
                jobs.push_back(std::move(p));
        }
        catch (const std::exception& e)
        {
            log::error("Unreal sync: texture set {}: {}", name, e.what());
        }
    }
    if (jobs.empty())
        return 0;
    const int count = int(jobs.size());
    Link::get().post([jobs = std::move(jobs)] {
        Link::get().ensureConnected();
        std::shared_ptr<RpcClient> client = Link::get().client();
        if (!client)
            throw std::runtime_error("not connected");
        for (const Prepared& p : jobs)
        {
            if (const std::string why = unsupported(p.params); !why.empty())
            {
                mainq::post([set = p.set, why] { log::error("Unreal: {} textures: {}", set, why); });
                continue;
            }
            RpcResult r = client->call("importTextures", p.params, importCallOptions());
            const std::string what = p.set + " textures";
            mainq::post([what, r] { reportSetSend(what, r); });
        }
    });
    return count;
}

void registerTextureSetCommands()
{
    using T = ArgType;
    if (ops::find("unrealSendTextureSet"))
        return;

    ops::registerCommand({
        .name = "unrealSendTextureSet",
        .doc = "Send a texture set to Unreal: exports it through the Unreal preset (only tiles and channels that changed since the last "
               "export are rewritten and sent, unless all=True) and updates the textures and the material instance of the set's slot "
               "(MI_<slot>), set on the slot of every mapped mesh using it. Painterly and Toon materials become instances of LilaLink's "
               "stylised masters with their palette values; others need materialParent (or the unreal pref), else only textures go. "
               "The set then also goes on every save. Returns {queued, textures} or, with wait=True, Unreal's {assets, warnings, errors}.",
        .positionalDoc = "set",
        .flags = {{"folder", "fo", T::String, nullptr, "Content folder for the textures; the unreal pref /textureFolder, else /Game/Textures."},
                  {"materialParent", "mp", T::String, nullptr, "Parent material for non-stylised sets; the unreal pref /materialParent."},
                  {"materialFolder", "mf", T::String, nullptr, "Where the instance goes; the texture folder by default."},
                  {"exportFolder", "ef", T::String, "textures", "Where the files are written; relative ones resolve like bake folders."},
                  {"preset", "pr", T::String, "unreal", "Export preset (a bake target)."},
                  {"all", "a", T::Bool, false, "Send every texture, changed or not."},
                  {"remember", "rm", T::Bool, true, "Send this set on every save from now on."},
                  {"save", "sv", T::Bool, nullptr, "Save the packages in Unreal afterwards."},
                  {"wait", "w", T::Bool, false, "Block until Unreal finishes and return its reply."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            scene::Graph& g = scene::Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("unrealSendTextureSet: name the texture set (texSets lists them)");
            scene::Node* set = tex::findSet(g, a.positional[0].get<std::string>());
            if (!set)
                throw CommandError("unrealSendTextureSet: no texture set named " + a.positional[0].get<std::string>());
            json flags = json::object();
            for (const char* k : {"folder", "materialParent", "materialFolder", "exportFolder", "preset", "save"})
                if (a.has(k))
                    flags[k] = a.flags[k];
            Prepared p;
            try
            {
                p = prepare(g, *set, flags, a.get<bool>("all", false));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("unrealSendTextureSet: ") + e.what());
            }
            if (a.get<bool>("remember", true))
                rememberSet(set->name());
            if (p.params["textures"].empty())
                return {{"queued", false}, {"textures", 0}, {"unchanged", true}};

            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("unrealSendTextureSet: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<RpcClient> client = Link::get().client();
            if (!client)
                throw CommandError("unrealSendTextureSet: not connected to Unreal");
            if (const std::string why = unsupported(p.params); !why.empty())
                throw CommandError("unrealSendTextureSet: " + why);
            if (a.get<bool>("wait"))
            {
                RpcResult r = client->call("importTextures", p.params, importCallOptions());
                if (!r.ok)
                    throw CommandError("unrealSendTextureSet: " + r.error);
                return r.result;
            }
            const std::string what = set->name() + " textures";
            client->callOnMain("importTextures", p.params, importCallOptions(), [what](RpcResult r) { reportSetSend(what, r); });
            return {{"queued", true}, {"textures", p.params["textures"].size()}, {"materials", p.params["materials"].size()}};
        },
    });
}
} // namespace lila::link
