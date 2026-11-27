// worldBuild's Unreal export: the world's terrain, settlements, buildings, site kits and zones sent over LilaLink into the
// open level, each step reporting what it sent, what the level already held unchanged and what it skipped and why. Every
// node the world made carries a link id derived from the world's name and the node's path, so a rebuild (replace=True)
// keys its placements as the build before it did: unchanged groups are skipped, and the world's groups the new build no
// longer makes are removed.

#include "WorldScene.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>

using namespace lila;
namespace fs = std::filesystem;

namespace openworld::world
{
namespace
{
using scene::Graph;
using scene::Node;

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

json runCmd(const char* cmd, json flags) { return ops::execute(cmd, std::move(flags)); }

/**
 * The Unreal paint layer of the terrain's road material layer (Road) when the landscape draws through Lila's terrain
 * material, which samples only its material layers; empty when the spec names its own landscape material or there is none.
 */
std::string terrainRoadLayer(const std::string& terrain, const json& ue)
{
    if (terrain.empty() || ue.contains("landscapeMaterial"))
        return {};
    json rules;
    try
    {
        rules = runCmd("terrainMaterialRules", {{"args", {terrain}}, {"query", true}});
    }
    catch (const std::exception&)
    {
        return {};
    }
    for (const json& l : rules.value("layers", json::array()))
    {
        std::string name = l.is_object() ? l.value("name", std::string()) : std::string();
        std::string lower = name;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (lower == "road")
        {
            // terrainSendToUnreal names a material layer's paint layer after it, first letter up.
            name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
            return name;
        }
    }
    return {};
}

/** A result cut down for a step report: long lists counted, deep objects summarized. */
json brief(const json& j, int depth = 0)
{
    if (j.is_array())
    {
        if (j.size() > 12 || depth > 2)
            return {{"count", j.size()}};
        json out = json::array();
        for (const json& v : j)
            out.push_back(brief(v, depth + 1));
        return out;
    }
    if (j.is_object())
    {
        if (depth > 2)
            return {{"keys", j.size()}};
        json out = json::object();
        for (const auto& [k, v] : j.items())
            out[k] = brief(v, depth + 1);
        return out;
    }
    return j;
}

/** path is root or lies under it (full paths). */
bool underPath(const std::string& path, const std::string& root)
{
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '|');
}

bool hasFeature(const json& features, const char* f) { return features.is_array() && std::find(features.begin(), features.end(), json(f)) != features.end(); }

/**
 * What one unrealPlaceInstances plan did: groups sent, held unchanged by the level, removed and failed (with why), the
 * assets imported, shared within the send or reused from the project, and the slot table rows. Every group key goes into keys.
 */
json placementReport(const json& plan, std::set<std::string>& keys)
{
    int sent = 0, unchanged = 0, removed = 0, imported = 0, shared = 0, reused = 0, placements = 0;
    json failed = json::array();
    for (const json& grp : plan.value("groups", json::array()))
    {
        const std::string key = grp.value("key", std::string());
        if (!key.empty())
            keys.insert(key);
        if (grp.value("remove", false))
        {
            ++removed;
            continue;
        }
        const json placed = grp.value("placed", json::object());
        const json imp = grp.value("import", json::object());
        std::string why;
        if (grp.contains("error"))
            why = grp["error"].get<std::string>();
        else if (imp.contains("error"))
            why = "import: " + imp["error"].get<std::string>();
        else if (placed.contains("error"))
            why = "place: " + placed["error"].get<std::string>();
        if (!why.empty())
        {
            if (failed.size() < 20)
                failed.push_back({{"group", key}, {"source", grp.value("source", std::string())}, {"error", why}});
            continue;
        }
        if (placed.value("unchanged", false))
        {
            ++unchanged;
            continue;
        }
        ++sent;
        placements += int(grp.value("placements", json::array()).size());
        if (imp.value("shared", false))
            ++shared;
        else if (imp.value("reused", false))
            ++reused;
        else if (!imp.empty())
            ++imported;
    }
    json out = {{"sent", sent}, {"unchanged", unchanged}, {"imported", imported}, {"sharedImports", shared}, {"reused", reused}, {"placements", placements}};
    if (removed)
        out["removed"] = removed;
    if (!failed.empty())
        out["failed"] = failed;
    if (const json& rows = plan.value("slotTable", json::array()); !rows.empty())
        out["slotRows"] = rows.size();
    if (plan.contains("slotTableWritten") && plan["slotTableWritten"].contains("error"))
        out["slotTableError"] = plan["slotTableWritten"]["error"];
    if (const json& issues = plan.value("issues", json::array()); !issues.empty())
        out["issues"] = issues.size();
    return out;
}

std::string fileSafe(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out;
}

/**
 * What the world sent to one Unreal project and level that no placement group names: its settlements (by their Unreal
 * key) and zone sets. A rebuild that no longer makes one removes it from that level.
 */
fs::path sentRecordPath(const std::string& project, const std::string& level, const std::string& prefix)
{
    return link::cacheDir() / "openworld" / ("sent_" + fileSafe(project) + "_" + link::levelFileKey(level) + "_" + prefix + ".json");
}

json readRecord(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return json::object();
    try
    {
        json j = json::parse(f);
        return j.is_object() ? j : json::object();
    }
    catch (const std::exception&)
    {
        return json::object();
    }
}

/** Counts of a report's failures, for the step's own failure list. */
void collectFailures(const json& report, const std::string& step, json& failures)
{
    for (const json& f : report.value("failed", json::array()))
    {
        json e = f;
        e["step"] = step;
        failures.push_back(e);
    }
}
} // namespace

std::string worldLinkPrefix(const std::string& world) { return std::format("w{:08x}", fnv1a(world) & 0xffffffffull); }

json assignWorldLinkIds(const std::string& world, const std::vector<std::string>& roots)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    const std::string prefix = worldLinkPrefix(world);
    std::vector<std::string> full;
    for (const std::string& r : roots)
        if (Node* n = r.empty() ? nullptr : g.find(r))
            full.push_back(n->fullPath());
    size_t set = 0, kept = 0;
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().dag || n->type().attrs.size() <= size_t(scene::Transform::kLinkId))
            continue;
        const std::string path = n->fullPath();
        if (!std::any_of(full.begin(), full.end(), [&](const std::string& r) { return underPath(path, r); }))
            continue;
        const std::string key = prefix + std::format("{:012x}", fnv1a(path) & 0xffffffffffffull);
        const std::string* now = std::get_if<std::string>(&n->value(scene::Transform::kLinkId));
        if (now && *now == key)
        {
            ++kept;
            continue;
        }
        g.setValue({id, scene::Transform::kLinkId, -1}, key);
        ++set;
    }
    return {{"prefix", prefix}, {"assigned", set}, {"kept", kept}};
}

json sendWorldToUnreal(const std::string& world, const json& built, const json& options, const StepFn& step)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    const json ue = options.is_object() ? options : json::object();
    const std::string terrain = built.value("terrain", std::string());
    std::set<std::string> keys;
    json failures = json::array();
    json report = json::object();
    json features = json::array();
    bool partitioned = false;
    std::string project;
    link::LevelIdentity level;
    // What this build sent that no placement group names, for the record the next build removes from.
    json sentSettlements = json::array(), sentZones = json::array();
    std::set<std::string> settlementNames;
    for (const json& n : built.value("settlementNames", json::array()))
        if (n.is_string())
            settlementNames.insert(n.get<std::string>());

    const bool linked = step("export.unreal.link", [&]() -> json {
        try
        {
            link::Link::get().ensureConnected();
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(std::string("Unreal is not connected (") + e.what() + "); start the editor with LilaLink, then unrealConnect()");
        }
        const json status = link::Link::get().status();
        features = status.value("features", json::array());
        project = status.value("project", std::string());
        json out = {{"project", project}};
        level = link::openLevelIdentity();
        if (!level.level.empty())
        {
            out["level"] = level.level;
            partitioned = level.partitioned;
            out["partitioned"] = partitioned;
            if (level.guid.empty())
                out["levelNote"] = "this LilaLink doesn't report level GUIDs, so a level made again under the same path is taken to hold the last build's "
                                   "settlements and zones; rebuild the plugin";
        }
        if (!hasFeature(features, "placementHashes"))
            out["note"] = "this LilaLink predates placementHashes: every group is sent again and the world's stale groups can't be found; rebuild the plugin";
        return out;
    });
    if (!linked)
        return {{"sent", false}};

    // Terrain: the landscape with its layers, and the terrain's water, coast routes and roads.
    if (!terrain.empty() && g.find(terrain))
        step("export.unreal.terrain", [&]() -> json {
            json flags = {{"args", {terrain}}};
            if (ue.contains("landscapeMaterial"))
                flags["material"] = ue["landscapeMaterial"];
            const json r = runCmd("terrainSendToUnreal", flags);
            json out = {{"landscape", r.value("actor", std::string())}, {"created", r.value("created", false)}, {"unchanged", r.value("unchanged", false)}};
            if (r.contains("layers"))
                out["layers"] = r["layers"].is_array() ? json(r["layers"].size()) : r["layers"];
            for (const char* k : {"water", "coast", "roads", "volume", "holes"})
                if (r.contains(k))
                    out[k] = brief(r[k]);
            json skipped = json::array();
            for (const char* k : {"coastError", "roadsError"})
                if (r.contains(k))
                    skipped.push_back({{"what", k}, {"why", r[k]}});
            if (!skipped.empty())
                out["skipped"] = skipped;
            return out;
        });
    else
        report["terrainSkipped"] = "the world has no terrain";

    // A landscape drawn in Lila's terrain material layers samples only those: settlement roads paint its road layer.
    const std::string roadLayer = terrainRoadLayer(terrain, ue);
    // Settlements: streets, lots and lane data; their massing only where Buildings doesn't stand in for it.
    std::set<std::string> withBuildings;
    for (const json& grp : built.value("buildingGroups", json::array()))
        if (Node* n = grp.is_string() ? g.find(grp.get<std::string>()) : nullptr)
            withBuildings.insert(n->fullPath());
    for (const json& sm : built.value("settlements", json::array()))
    {
        if (!sm.is_string() || !g.find(sm.get<std::string>()))
            continue;
        const std::string mesh = sm.get<std::string>();
        const std::string name = g.find(mesh)->name();
        settlementNames.insert(name);
        const bool buildings = std::any_of(withBuildings.begin(), withBuildings.end(), [&](const std::string& p) { return p.ends_with(name + "_Buildings"); });
        step("export.unreal.settlement." + name, [&]() -> json {
            json flags = {{"args", {mesh}}, {"massing", !buildings}};
            for (const char* k : {"roads", "roadMesh", "pcgGraph", "cell"})
                if (ue.contains(k))
                    flags[k] = ue[k];
            const bool paintsLandscape = ue.value("roads", std::string("landscape")) == "landscape";
            if (paintsLandscape && !roadLayer.empty())
            {
                flags["roadLayer"] = roadLayer;
                flags["shoulderLayer"] = "";
            }
            const json r = runCmd("settlementSendToUnreal", flags);
            sentSettlements.push_back({{"key", r.value("key", name)}, {"name", name}});
            const json d = r.value("diff", json::object());
            json out = {{"added", d.value("added", json::array()).size()},
                        {"changed", d.value("changed", json::array()).size()},
                        {"removed", d.value("removed", json::array()).size()},
                        {"unchanged", d.value("unchanged", 0)},
                        {"cells", r.value("cells", 0)},
                        {"massing", buildings ? "off: Buildings sends the shells" : "on"}};
            if (paintsLandscape && !roadLayer.empty())
                out["roadLayer"] = roadLayer + ": the terrain draws in material layers, so roads paint its road layer and no shoulder";
            if (const json& e = r.value("errors", json::array()); !e.empty())
            {
                out["errors"] = brief(e);
                for (const json& m : e)
                    failures.push_back({{"step", "settlement." + name}, {"error", m}});
            }
            if (r.contains("warnings"))
                out["warnings"] = r["warnings"];
            if (r.contains("resentAll"))
                out["resentAll"] = r["resentAll"];
            if (r.contains("seconds"))
                out["seconds"] = r["seconds"];
            if (r.contains("missingAssets") && r["missingAssets"].is_object())
            {
                size_t placements = 0;
                for (const auto& [asset, n] : r["missingAssets"].items())
                    placements += n.is_number() ? n.get<size_t>() : 0;
                out["skipped"] = std::format("{} placement groups (per cell and mesh) of {} meshes the project doesn't have, tried again on every send until it has them",
                                             placements, r["missingAssets"].size());
            }
            return out;
        });
    }

    // Buildings: every building mesh of the world's groups (shells, modules, slots, proxies, interiors).
    if (!withBuildings.empty())
        step("export.unreal.buildings", [&]() -> json {
            const json all = runCmd("buildingSend", {{"dryRun", true}});
            json meshes = json::array();
            for (const json& m : all.value("meshes", json::array()))
            {
                Node* n = g.find(m.get<std::string>());
                const std::string p = n ? n->fullPath() : std::string();
                if (n && std::any_of(withBuildings.begin(), withBuildings.end(), [&](const std::string& r) { return p != r && underPath(p, r); }))
                    meshes.push_back(m);
            }
            if (meshes.empty())
                return {{"sent", 0}, {"skipped", "the world's building groups hold no building meshes"}};
            json flags = {{"args", meshes}};
            for (const char* k : {"interiors", "hlodLayer", "hlodProxyLayer"})
                if (ue.contains(k))
                    flags[k] = ue[k];
            if (ue.contains("folder"))
                flags["folder"] = ue["folder"].get<std::string>() + "/Buildings";
            if (!partitioned)
                flags["proxies"] = false;
            else
            {
                // The proxies are made here first so they carry the world's link ids; buildingSend refills the same nodes.
                runCmd("buildingProxies", {{"args", meshes}});
                assignWorldLinkIds(world, std::vector<std::string>(withBuildings.begin(), withBuildings.end()));
            }
            const json r = runCmd("buildingSend", flags);
            json out = {{"meshes", meshes.size()}, {"shellsAndModules", placementReport(r.value("plan", json::object()), keys)}};
            collectFailures(out["shellsAndModules"], "buildings", failures);
            if (!partitioned)
                out["proxiesSkipped"] = "the level is not World Partition: HLOD proxies need Custom HLOD actors";
            const json hl = r.value("hlods", json::object());
            for (const char* lod : {"near", "far"})
                if (hl.contains(lod))
                {
                    out[std::string("proxies.") + lod] = placementReport(hl[lod], keys);
                    collectFailures(out[std::string("proxies.") + lod], "buildings.proxies", failures);
                }
            if (hl.contains("warning"))
                out["proxiesSkipped"] = hl["warning"];
            json interiors = json::array();
            for (const json& i : r.value("interiorLevels", json::array()))
            {
                for (const json& k : i.value("groups", json::array()))
                    if (k.is_string())
                        keys.insert(k.get<std::string>());
                json e = {{"building", i.value("building", std::string())}, {"mode", i.value("mode", std::string())}, {"groups", i.value("groups", json::array()).size()}};
                if (i.contains("levelInstance"))
                {
                    const json& li = i["levelInstance"];
                    if (li.contains("error"))
                    {
                        e["error"] = li["error"];
                        failures.push_back({{"step", "buildings.interiors"}, {"building", e["building"]}, {"error", li["error"]}});
                    }
                    else
                        e["levelInstance"] = li.value("unchanged", false) ? "unchanged" : "built";
                }
                interiors.push_back(e);
            }
            out["interiors"] = interiors;
            if (r.contains("interiorsNote"))
                out["interiorsSkipped"] = r["interiorsNote"];
            if (r.contains("materials"))
                out["materials"] = brief(r["materials"]);
            json seconds = {{"phases", r.value("seconds", json::object())}};
            auto sendSeconds = [](const json& plan) { return json{{"plan", plan.value("planSeconds", 0.0)}, {"send", plan.value("seconds", json::object())}}; };
            seconds["shellsAndModules"] = sendSeconds(r.value("plan", json::object()));
            for (const char* lod : {"near", "far"})
                if (hl.contains(lod))
                    seconds[std::string("proxies.") + lod] = sendSeconds(hl[lod]);
            out["seconds"] = seconds;
            return out;
        });

    // Site kits: their ground (quays, aprons, runways), props and slots; their buildings went with Buildings.
    json siteMeshes = json::array();
    for (const json& m : built.value("siteMeshes", json::array()))
        if (m.is_string() && g.find(m.get<std::string>()))
            siteMeshes.push_back(m);
    if (!siteMeshes.empty())
        step("export.unreal.sites", [&]() -> json {
            json flags = {{"args", siteMeshes}, {"nanite", true}, {"wait", true}, {"folder", ue.value("folder", std::string("/Game/Lila")) + "/Sites"}};
            const json plan = runCmd("unrealPlaceInstances", flags);
            json out = placementReport(plan, keys);
            out["sites"] = siteMeshes.size();
            collectFailures(out, "sites", failures);
            return out;
        });

    // Zones: volumes, the zone data asset and a data layer per layer; zones gone since the last send are removed.
    if (built.contains("zones") && built["zones"].is_string() && g.find(built["zones"].get<std::string>()))
        step("export.unreal.zones", [&]() -> json {
            const json r = runCmd("zoneSendToUnreal", {{"set", built["zones"]}});
            if (r.contains("id"))
                sentZones.push_back({{"id", r["id"]}, {"label", r.value("label", std::string())}, {"asset", r.value("asset", std::string())}});
            json out = brief(r.value("unreal", json::object()));
            if (r.contains("notes"))
                out["notes"] = r["notes"];
            return out;
        });

    // The world's groups the level holds that this build didn't send: what an earlier build made and this one no longer
    // does (a site or building taken out of the spec).
    step("export.unreal.stale", [&]() -> json {
        if (!hasFeature(features, "placementHashes"))
            return {{"removed", 0}, {"skipped", "this LilaLink can't list the level's groups (placementHashes); rebuild the plugin"}};
        auto c = link::Link::get().client();
        if (!c)
            throw std::runtime_error("not connected");
        const link::RpcResult h = c->call("placementHashes", json::object(), 120000);
        if (!h.ok)
            throw std::runtime_error("placementHashes: " + h.error);
        const std::string prefix = worldLinkPrefix(world);
        json stale = json::array();
        size_t ours = 0;
        const json groups = h.result.value("groups", json::object());
        for (const auto& [key, v] : groups.items())
        {
            if (!key.starts_with(prefix))
                continue;
            ++ours;
            if (!keys.contains(key))
                stale.push_back(key);
        }
        json out = {{"worldGroups", ours}, {"sentThisBuild", keys.size()}, {"stale", stale.size()}};
        if (!stale.empty())
        {
            // The meshes only they showed go too (under the world's content folder, and not shown by another level).
            const link::RpcResult r = c->call("removePlacements",
                                              {{"groups", stale}, {"levelInstances", true}, {"deleteMeshes", true}, {"meshFolder", ue.value("folder", std::string("/Game/Lila"))}},
                                              300000);
            if (!r.ok)
                throw std::runtime_error("removePlacements: " + r.error);
            out["removed"] = r.result;
            out["groups"] = brief(stale);
            // Their rows leave the slot table too: game code looking a slot up by id must not find one that is gone.
            if (hasFeature(features, "slots"))
            {
                const link::RpcResult t = c->call("writeSlotTable", {{"path", "/Game/Lila/Slots/DA_LilaSlotTable"}, {"rows", json::array()}, {"sources", stale}}, 120000);
                out["slotTable"] = t.ok ? t.result : json{{"error", t.error}};
            }
        }
        return out;
    });

    // Settlements and zone sets an earlier build sent to this level that this one no longer makes (taken out of the spec,
    // or renamed): they go whole, with their assets, data layers and lanes.
    step("export.unreal.dropped", [&]() -> json {
        const fs::path recordPath = sentRecordPath(project, level.level, worldLinkPrefix(world));
        json before = readRecord(recordPath);
        // A record of another level (this one deleted and made again under its path) names nothing this level holds.
        const std::string ignored = link::levelRecordMismatch(before, level);
        if (!ignored.empty())
            before = json::object();
        std::set<std::string> keysNow;
        for (const json& s : sentSettlements)
            keysNow.insert(s["key"].get<std::string>());
        std::set<std::string> zonesNow;
        for (const json& z : sentZones)
            zonesNow.insert(z["id"].get<std::string>());
        json kept = json::array(), keptZones = json::array();
        json settlements = json::array(), zoneSets = json::array();
        // Project-wide lane profiles the removals took out of the project ini, and the ini copies made first.
        json profilesRemoved = json::array(), configBackups = json::array();
        // Landscape paint layers Lila added for the roads, taken out with them or kept (warned) because someone painted them.
        json paintLayersRemoved = json::array(), warnings = json::array();
        for (const json& s : before.value("settlements", json::array()))
        {
            const std::string key = s.value("key", std::string());
            // A settlement still in the spec whose send failed this time stays: the level may hold it still.
            if (key.empty() || keysNow.contains(key) || settlementNames.contains(s.value("name", std::string())))
            {
                if (!key.empty() && !keysNow.contains(key))
                    kept.push_back(s);
                continue;
            }
            json e = {{"key", key}};
            try
            {
                const json r = runCmd("settlementRemoveFromUnreal", {{"key", key}});
                e["removed"] = brief(r);
                for (const json& name : r.value("laneProfilesRemoved", json::array()))
                    profilesRemoved.push_back(name);
                if (r.contains("configBackup"))
                    configBackups.push_back(r["configBackup"]);
                for (const json& l : r.value("paintLayersRemoved", json::array()))
                    paintLayersRemoved.push_back(l);
                for (const json& w : r.value("warnings", json::array()))
                    warnings.push_back(key + ": " + w.get<std::string>());
                if (r.contains("errors"))
                {
                    kept.push_back(s);
                    failures.push_back({{"step", "dropped"}, {"settlement", key}, {"error", r["errors"]}});
                }
            }
            catch (const std::exception& ex)
            {
                e["error"] = ex.what();
                kept.push_back(s);
                failures.push_back({{"step", "dropped"}, {"settlement", key}, {"error", ex.what()}});
            }
            settlements.push_back(e);
        }
        const bool zoneSetBuilt = built.contains("zones") && built["zones"].is_string();
        for (const json& z : before.value("zones", json::array()))
        {
            const std::string id = z.value("id", std::string());
            if (id.empty() || zonesNow.contains(id))
                continue;
            if (zoneSetBuilt && zonesNow.empty())
            {
                // The set was built but its send failed: it stays until a send says otherwise.
                keptZones.push_back(z);
                continue;
            }
            json e = {{"id", id}, {"label", z.value("label", std::string())}};
            try
            {
                e["removed"] = brief(runCmd("zoneRemoveFromUnreal", {{"id", id}, {"label", z.value("label", std::string())}, {"asset", z.value("asset", std::string("/Game/Lila/Zones"))}})["unreal"]);
            }
            catch (const std::exception& ex)
            {
                e["error"] = ex.what();
                keptZones.push_back(z);
                failures.push_back({{"step", "dropped"}, {"zones", id}, {"error", ex.what()}});
            }
            zoneSets.push_back(e);
        }
        json record = {{"format", "lila.world.sent"}, {"version", 1}, {"world", world}, {"settlements", sentSettlements}, {"zones", sentZones}};
        for (const json& s : kept)
            record["settlements"].push_back(s);
        for (const json& z : keptZones)
            record["zones"].push_back(z);
        link::stampLevelRecord(record, level);
        std::error_code ec;
        fs::create_directories(recordPath.parent_path(), ec);
        std::ofstream(recordPath, std::ios::binary) << record.dump(1);
        json out = {{"settlements", settlements}, {"zoneSets", zoneSets}, {"record", recordPath.generic_string()}};
        if (!ignored.empty())
            out["recordIgnored"] = "the last build's record was set aside: " + ignored + "; nothing of it is in this level to remove";
        if (!profilesRemoved.empty())
            out["laneProfilesRemoved"] = profilesRemoved;
        if (!configBackups.empty())
            out["configBackups"] = configBackups;
        if (!paintLayersRemoved.empty())
            out["paintLayersRemoved"] = paintLayersRemoved;
        if (!warnings.empty())
            out["warnings"] = warnings;
        return out;
    });

    report["groupsSent"] = keys.size();
    report["prefix"] = worldLinkPrefix(world);
    if (!failures.empty())
        report["failures"] = failures;
    return report;
}
} // namespace openworld::world
