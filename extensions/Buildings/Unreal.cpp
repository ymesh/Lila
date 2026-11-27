// buildingSend: building meshes to Unreal. Shells go as Nanite static meshes placed where they stand (each cell mesh lies
// inside one of Settlement's World Partition cells), modules as slots keyed by slot id (W0's path in
// unrealPlaceInstances), the HLOD and far proxies as World Partition Custom HLOD actors, and MF_LilaBuildingVariation with M_LilaBuilding
// and an instance per style, assigned to the shells' building material slots. Interiors of enterable buildings go apart from
// their shells: their room meshes as actors colliding as modelled, made into a Level Instance (or put on a runtime data
// layer of their own), and the windows' glass gets the fake-room material where there is no interior, clear glass where
// there is.

#include "BuildingsScene.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/GeometryNodes.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <chrono>
#include <set>

using namespace lila;

namespace buildings
{
namespace
{
/**
 * The module material slots whose Lila material has Second Tone on (bld_trim, bld_frame as shipped), each with its own
 * colour: in Unreal they take MI_Bld_Tone_<slot>, which reads the second tone (custom data 4-7) instead of the colour and
 * shows the material's own colour on instances that carry none.
 */
json secondToneSlots(scene::Graph& g)
{
    json out = json::array();
    for (scene::Node* m : scene::materials::all(g))
    {
        const std::string& name = m->name();
        const int tone2 = m->type().attrIndex("instanceColor2");
        if (name.starts_with("bld_") && !name.starts_with("bld_int_") && tone2 >= 0 && m->as<bool>(tone2))
            out.push_back({{"name", name}, {"color", link::materialInputs(g, name).value("baseColor", json::array({0.85, 0.85, 0.82}))}});
    }
    return out;
}

/** The material instance for a style's shells. */
json styleInstance(const Style& s)
{
    return {{"name", "MI_Bld_" + s.id},
            {"scalars", {{"DirtAmount", 1.0}, {"PaletteStrength", 1.0}}},
            {"vectors", {{"DirtColor", {0.12, 0.1, 0.08}}, {"BaseColor", {0.8, 0.8, 0.8}}}}};
}
} // namespace

void registerUnreal(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "buildingSend",
        .doc = "Send building meshes to Unreal: shells as Nanite static meshes placed where they stand (one per cell mesh, inside "
               "Settlement's World Partition cells), modules as slots keyed by slot id (actors of a slot's Unreal class, instances "
               "of its Unreal mesh, or its placeholder), the HLOD and far proxies as World Partition Custom HLOD actors (proxies=True), and MF_LilaBuildingVariation with "
               "M_LilaBuilding and MI_Bld_<style> per style, assigned to the shells' building slots. Per-building colour and dirt "
               "travel in vertex colour. dryRun returns what would be sent. Returns the placement plan and the materials.",
        .positionalDoc = "building meshes (default every one)",
        .flags = {{"folder", "f", ArgType::String, "/Game/Lila/Buildings", "Content folder for the shells and module meshes."},
                  {"proxies", "px", ArgType::Bool, true, "Make and send the HLOD and far proxies too."},
                  {"hlodLayer", "hl", ArgType::String, "",
                   "HLOD layer asset for the shells and modules. Unset, they are left out of HLOD generation when the proxies go as "
                   "Custom HLOD actors (which stand in for them), else they keep the level's default."},
                  {"interiors", "in", ArgType::String, "levelInstance",
                   "How interiors of enterable buildings stream apart from their shells: a Level Instance each (on a runtime data layer "
                   "of its own in World Partition levels), their actors on a runtime data layer, or not sent.",
                   {"levelInstance", "dataLayer", "none"}},
                  {"hlodProxyLayer", "hpl", ArgType::String, "/Game/Lila/HLOD/HL_LilaBuildings",
                   "Custom HLOD Actor layer for the proxies (made when missing); the far proxies go in the same path + Far, linked to the "
                   "level's default HLOD layer so they show once its HLODs unload."},
                  {"dryRun", "dr", ArgType::Bool, false, "Only return what would be sent."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            scene::Graph& g = scene::Graph::current();
            g.evaluate();
            std::vector<std::string> meshes;
            for (const json& p : a.positional)
                if (p.is_string())
                    meshes.push_back(p.get<std::string>());
            std::set<std::string> styles;
            for (const auto& p : allPublished())
            {
                scene::Node* mod = g.node(scene::NodeId(p->modifier));
                if (!mod)
                    continue;
                const scene::NodeId mesh = scene::modifiers::meshOf(g, mod->id());
                if (mesh == scene::kNoNode)
                    continue;
                if (a.positional.empty())
                    meshes.push_back(g.node(mesh)->path());
                for (const auto& r : p->results)
                    styles.insert(r->style);
            }
            if (meshes.empty())
                throw CommandError("buildingSend: no building meshes (buildingCreate makes them)");
            json instances = json::array();
            for (const std::string& s : styles)
                if (std::shared_ptr<const Style> st = findStyle(s))
                    instances.push_back(styleInstance(*st));
            json out = {{"meshes", meshes}, {"instances", instances}};
            const std::string interiorMode = a.get<std::string>("interiors", "levelInstance");
            json interiorGroups = json::array();
            if (interiorMode != "none")
                for (const std::string& id : enterableBuildings())
                    if (const std::string grp = interiorGroupOf(id); !grp.empty())
                        interiorGroups.push_back({{"building", id}, {"group", grp}, {"name", "Interior_" + id.substr(id.find(':') + 1)}});
            out["interiors"] = interiorGroups;
            if (a.get<bool>("dryRun", false))
                return out;
            // Seconds per phase, in the result.
            auto last = std::chrono::steady_clock::now();
            auto lap = [&](const char* phase) {
                const auto now = std::chrono::steady_clock::now();
                out["seconds"][phase] = std::chrono::duration<double>(now - last).count();
                last = now;
            };
            std::vector<std::string> send = meshes;
            json nearProxies = json::array(), farProxies = json::array();
            if (a.get<bool>("proxies", true))
            {
                json args = json::array();
                for (const std::string& m : meshes)
                    args.push_back(m);
                const json made = ops::execute("buildingProxies", {{"args", args}});
                for (const json& p : made["proxies"])
                    (p.value("lod", std::string()) == "far" ? farProxies : nearProxies).push_back(p["mesh"]);
                out["proxies"] = made["proxies"].size();
                lap("proxies");
            }
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("buildingSend: Unreal is not connected (") + e.what() + ")");
            }
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::find(features.begin(), features.end(), json("buildings")) == features.end())
                throw CommandError("buildingSend: this LilaLink has no buildings feature; update the plugin");
            json args = json::array();
            for (const std::string& m : send)
                args.push_back(m);
            const bool customHlods = (!nearProxies.empty() || !farProxies.empty()) &&
                                     std::find(features.begin(), features.end(), json("customHlod")) != features.end();
            std::string shellHlod = a.get<std::string>("hlodLayer", "");
            if (shellHlod.empty() && customHlods)
                shellHlod = "none";
            const json plan = ops::execute("unrealPlaceInstances", {{"args", args},
                                                                    {"folder", a.get<std::string>("folder", "/Game/Lila/Buildings")},
                                                                    {"nanite", true},
                                                                    // Clear glass is translucent, which Nanite can't draw.
                                                                    {"naniteOffMaterials", {"bld_glassClear"}},
                                                                    // A module used more than once in a cell is one HISM there; the shells stay actors.
                                                                    {"threshold", 1},
                                                                    {"hlodLayer", shellHlod},
                                                                    {"wait", true}});
            out["plan"] = plan;
            lap("shellsAndModules");
            // Proxies as World Partition Custom HLOD actors: the near ones show while a cell is unloaded, the far ones
            // once the level's default HLOD layer unloads there too.
            const std::string folder = a.get<std::string>("folder", "/Game/Lila/Buildings");
            const std::string nearLayer = a.get<std::string>("hlodProxyLayer", "/Game/Lila/HLOD/HL_LilaBuildings");
            json hlods = json::object();
            if (!nearProxies.empty() || !farProxies.empty())
            {
                if (std::find(features.begin(), features.end(), json("customHlod")) == features.end())
                    hlods["warning"] = "this LilaLink has no Custom HLOD actors; proxies not sent; rebuild the plugin";
                else
                {
                    auto sendProxies = [&](const json& list, const std::string& layer, const std::string& linked) {
                        json args = list;
                        json flags = {{"args", args}, {"folder", folder + "/HLOD"}, {"mode", "customHlod"}, {"hlodLayer", layer},
                                      {"collision", "none"}, {"validate", false}, {"wait", true}};
                        if (!linked.empty())
                            flags["linkedHlodLayer"] = linked;
                        return ops::execute("unrealPlaceInstances", flags);
                    };
                    if (!nearProxies.empty())
                        hlods["near"] = sendProxies(nearProxies, nearLayer, "");
                    if (!farProxies.empty())
                        hlods["far"] = sendProxies(farProxies, nearLayer + "Far", "default");
                }
            }
            out["hlods"] = hlods;
            lap("hlods");
            // Every asset sent (shells, modules, proxies): their building slots get their style's instance, so modules and
            // proxies shade like the shell they sit on.
            std::set<std::string> paths;
            auto collect = [&paths](const json& sent) {
                for (const json& grp : sent.value("groups", json::array()))
                    if (grp.contains("objectPath") && !grp.contains("actorClass") && !grp.contains("unrealMesh"))
                        paths.insert(grp["objectPath"].get<std::string>());
            };
            collect(plan);
            for (const char* level : {"near", "far"})
                if (hlods.contains(level))
                    collect(hlods[level]);
            json assets = json::array();
            for (const std::string& path : paths)
                assets.push_back(path);
            auto client = link::Link::get().client();
            link::RpcResult r = client->call("buildingMaterials", {{"instances", instances}, {"assets", assets}, {"secondToneSlots", secondToneSlots(g)}},
                                             link::importCallOptions());
            if (!r.ok)
                throw CommandError("buildingSend: buildingMaterials: " + r.error);
            out["materials"] = r.result;
            lap("materials");

            // Interiors, and the windows' glass on every module mesh sent.
            json glass = json::array();
            for (const json& grp : plan.value("groups", json::array()))
                if (grp.contains("objectPath"))
                    glass.push_back(grp["objectPath"]);
            const bool canInteriors = std::find(features.begin(), features.end(), json("buildingInteriors")) != features.end();
            if (!canInteriors)
            {
                out["interiorsNote"] = "this LilaLink has no buildingInteriors feature: interiors and window glass were not sent; update the plugin";
                return out;
            }
            json entries = json::array();
            for (const json& i : interiorGroups)
            {
                json flags = {{"args", {i["group"]}}, {"folder", folder + "/Interiors"}, {"mode", "actors"}, {"collision", "complexAsSimple"}, {"wait", true}};
                json placed = ops::execute("unrealPlaceInstances", flags);
                // A Level Instance is rebuilt whole from the actors placed loose: when only some groups changed, all go again.
                const size_t same = placed.value("unchanged", 0);
                const size_t total = std::ranges::count_if(placed.value("groups", json::array()), [](const json& g) { return !g.value("remove", false); });
                if (interiorMode == "levelInstance" && same > 0 && same < total)
                {
                    flags["resend"] = true;
                    placed = ops::execute("unrealPlaceInstances", flags);
                }
                json keys = json::array();
                for (const json& grp : placed.value("groups", json::array()))
                    keys.push_back(grp.at("key"));
                const std::string key = "interior:" + i["building"].get<std::string>().substr(9);
                json entry = {{"building", i["building"]}, {"name", i["name"]}, {"mode", interiorMode}, {"groups", keys}};
                if (interiorMode == "levelInstance")
                {
                    const link::RpcResult li = client->call("makeLevelInstance", {{"key", key}, {"name", i["name"]}, {"kind", "levelInstance"},
                                                                                  {"level", folder + "/Interiors/LI_" + i["name"].get<std::string>()}, {"groups", keys},
                                                                                  {"load", false}},
                                                            120000);
                    entry["levelInstance"] = li.ok ? li.result : json{{"error", li.error}};
                    entry["assembly"] = key;
                }
                entries.push_back(entry);
            }
            const link::RpcResult ir = client->call("buildingInteriors", {{"folder", folder}, {"interiors", entries}, {"glassAssets", glass}}, link::importCallOptions());
            if (!ir.ok)
                throw CommandError("buildingSend: buildingInteriors: " + ir.error);
            out["interiorResult"] = ir.result;
            out["interiorLevels"] = entries;
            lap("interiors");
            return out;
        },
        .examples = {"buildingSend()", "buildingSend('Town_Buildings_0_0', dryRun=True)"},
    });
}
} // namespace buildings
