// The world spec's schema and checks, file paths made absolute against the spec's folder, and OSM clipped to one
// settlement's area. Pure: no scene access, so tests drive it directly.

#include "World.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace openworld::world
{
namespace
{
namespace fs = std::filesystem;

json obj(const char* description, json properties = json::object())
{
    json o = {{"type", "object"}, {"description", description}};
    if (!properties.empty())
        o["properties"] = std::move(properties);
    return o;
}
json str(const char* description) { return {{"type", "string"}, {"description", description}}; }
json num(const char* description) { return {{"type", "number"}, {"description", description}}; }
json boolean(const char* description, bool def) { return {{"type", "boolean"}, {"description", description}, {"default", def}}; }
json pt2(const char* description) { return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", description}}; }

const std::set<std::string>& topKeys()
{
    static const std::set<std::string> k = {"name", "culture", "seed", "description", "remix", "terrain", "settlements",
                                            "buildings", "slots", "sites", "zones", "export", "check", "version"};
    return k;
}

void need(bool ok, const std::string& what)
{
    if (!ok)
        throw std::runtime_error("world spec: " + what);
}

void checkArea(const json& a, const std::string& where)
{
    need(a.is_array() && a.size() == 4 && std::all_of(a.begin(), a.end(), [](const json& v) { return v.is_number(); }),
         where + ".area is [x0, z0, x1, z1], cm");
    need(a[0].get<double>() < a[2].get<double>() && a[1].get<double>() < a[3].get<double>(), where + ".area has x0 < x1 and z0 < z1");
}

fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string resolved(const std::string& p, const fs::path& dir, const std::function<bool(const std::string&)>& elsewhere)
{
    if (p.empty() || p == "remix")
        return p;
    const fs::path path = utf8Path(p);
    if (path.is_absolute())
        return p;
    std::error_code ec;
    const fs::path there = dir / path;
    // A file found only relative to the workspace stays as given; one found nowhere belongs in the spec's folder.
    if (!fs::exists(there, ec) && elsewhere && elsewhere(p))
        return p;
    const std::u8string u = there.lexically_normal().generic_u8string();
    return std::string(u.begin(), u.end());
}
} // namespace

const json& worldSpecSchema()
{
    static const json s = [] {
        json settlement = obj(
            "One settlement: from OSM (osm: 'remix' for the remix's output, or a file) on the world's terrain, else settlementCreate flags (create).",
            {{"name", str("Name of the settlement's mesh (required, unique).")},
             {"osm", str("'remix' (the remix's remixed OSM, needs remix) or an OSM file (.osm, Overpass .json, .osm.pbf).")},
             {"area", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 4}, {"maxItems", 4},
                       {"description", "[x0, z0, x1, z1] world cm: only the OSM inside goes to this settlement."}}},
             {"within", str("A terrain mask the settlement keeps to (at least 0.5): remixPresent_<source id> for a builtUp remix source, so the city "
                            "and its OSM end where the real built-up land does.")},
             {"profile", str("settlementFromOsm profile (usTown, mexicanCity, ...); settlementProfiles lists them.")},
             {"overrides", obj("Profile fields to change, such as {\"lotWidth\": 1500, \"maxFloors\": 5}.")},
             {"fill", {{"type", "string"}, {"enum", {"sparse", "all", "none"}}, {"description", "Settlement's own lots beside imported buildings."}}},
             {"coverage", num("Share of a block imported buildings must cover to count as dense.")},
             {"margin", num("Room round the imported streets, cm.")},
             {"culture", str("Naming culture; default the world's.")},
             {"create", obj("settlementCreate flags, for a settlement laid out by Settlement alone (outline, profile, seed, ...).")},
             {"buildings", {{"description", "true, or {style, cell, lod, districts: {district name: style}}: build it with the Buildings generator."}}},
             {"furniture", {{"description", "true or settlementFurniture flags: street furniture as slots."}}}});
        json site = obj("A site kit: siteCreate's spec (siteInfo()['spec']) plus buildings.",
                        {{"kind", str("port, cruise, lighthouse, cemetery, airstrip, farm, village, compound, island (siteKinds).")},
                         {"id", str("Stable site id (slot ids live under site:<id>).")},
                         {"name", str("Fictional name; default generated.")},
                         {"culture", str("Naming culture; default the world's.")},
                         {"seed", {{"type", "integer"}}},
                         {"at", pt2("[x, z] world cm.")},
                         {"heading", num("Degrees.")},
                         {"waterLevel", num("cm; default the terrain's sea level.")},
                         {"params", obj("The kind's settings (siteKinds).")},
                         {"buildings", boolean("Build its envelopes with the Buildings generator (siteBuildings) instead of greybox massing.", true)}});
        json out = {
            {"$schema", "https://json-schema.org/draft/2020-12/schema"},
            {"title", "Lila world spec"},
            {"description", "One JSON that builds a whole open-world map: worldBuild(spec=) runs the sections in order (remix, terrain, "
                            "settlements with their buildings, sites, slots, zones, check, export); worldSpec() reads a built world back. "
                            "Lengths cm, Y up, positions [x, z]."},
            {"type", "object"},
            {"properties",
             {{"name", str("World name: the world node and default output folder (openworld/world/<name>).")},
              {"description", str("Free text.")},
              {"version", {{"type", "integer"}, {"default", 1}}},
              {"culture", str("Default naming culture (namingCultures) for settlements, sites and zones; default mexican.")},
              {"seed", {{"type", "integer"}, {"description", "Default seed for sections that take one."}}},
              {"remix", obj("A World Remix spec (lila://schema/remix-spec). Its terrain is the world's; its remixed OSM feeds settlements with osm: 'remix'.")},
              {"terrain", obj("A terrain spec (lila://schema/terrain-spec). With a remix, its steps (water, coast, biomes, groundCover, roads, stamps, ...) "
                              "run on the remix terrain and source is ignored; without one it makes the terrain. Coast and water live here: "
                              "water {sea, rivers, lakes} and coast {bathymetry, masks, mangroves, reservoirs, channels, routes, lanes, airstrips, stash}. "
                              "materials (terrainMaterialRules flags) are laid last, over the finished ground; without them defaults fitted to it.")},
              {"settlements", {{"type", "array"}, {"items", settlement}}},
              {"buildings", obj("Buildings across the world.",
                                {{"edits", obj("{group name: {building id: edit}}: Buildings edits (style, floors, bays, palette, enterable, ...) by "
                                               "building id, seeded into each group before it builds, so they survive a rebuild.")},
                                 {"lod", {{"type", "string"}, {"enum", {"near", "hlod", "far"}}, {"description", "Level of detail new building cells start at (per settlement: buildings.lod); default near."}}},
                                 {"styles", {{"type", "array"},
                                             {"description", "buildingStyle calls run after everything is built: {style, polygon} or {style, building}."},
                                             {"items", {{"type", "object"}}}}},
                                 {"enterable", {{"description", "Buildings given real interiors (buildingMakeEnterable) once everything is built: one pick or a "
                                                                 "list of them, each {programs: [police, church, civic, shop, ...], landmarks: true, styles: [...], "
                                                                 "ids: [building ids], limit, program, furnish}. A building is picked when any filter matches."}}}})},
              {"slots", obj("Slot swap rules, per-slot overrides and pins (lila.slots).",
                            {{"rules", {{"type", "array"}, {"description", "Rules as slotRule(rule=) takes them, applied in order."}, {"items", {{"type", "object"}}}}},
                             {"overrides", obj("{slot id: payload}.")},
                             {"pins", {{"type", "array"}, {"description", "Slot ids or 'owner:<id>' keys pinned after the build."}, {"items", {{"type", "string"}}}}}})},
              {"sites", {{"type", "array"}, {"items", site}}},
              {"zones", obj("Zones: a zone set's spec (lila://schema/zones) and generators run after it.",
                            {{"name", str("Zone set name; default Zones.")},
                             {"spec", obj("zoneSpec(spec=): layers and zones.")},
                             {"fromDistricts", {{"description", "true (every settlement) or a list of settlement names: one district zone per Settlement district."}}},
                             {"fromWater", {{"description", "true or zoneFromWater flags (or a list of them): boat and air spawn zones along water routes."}}},
                             {"fromTerrain", {{"type", "array"}, {"description", "zoneFromTerrain flags per call."}, {"items", {{"type", "object"}}}}},
                             {"playArea", {{"type", "array"}, {"description", "[[x, z], ...] play area for gap checks."}}}})},
              {"check", {{"description", "true (default), false, or worldCheck flags."}}},
              {"export", obj("Where the built world goes.",
                             {{"target", {{"type", "string"}, {"enum", {"none", "files", "unreal"}}, {"default", "none"},
                                          {"description", "files: terrain, zones and the overview map as files; unreal: also sends terrain, settlements, buildings, site kits and zones over LilaLink."}}},
                              {"folder", str("Output folder; default the workspace's openworld/world/<name>.")},
                              {"map", {{"description", "true or worldMap flags (resolution): the overview map PNG + JSON."}}},
                              {"unreal", obj("target unreal: how the world goes into the open level. A rebuild sends only what changed and removes "
                                             "what the world no longer makes.",
                                             {{"folder", str("Content folder root; Buildings and Sites go under it. Default /Game/Lila.")},
                                              {"interiors", {{"type", "string"}, {"enum", {"levelInstance", "dataLayer", "none"}}, {"description", "buildingSend interiors."}}},
                                              {"roads", {{"type", "string"}, {"enum", {"landscape", "splines", "meshes", "none"}}, {"description", "settlementSendToUnreal roads."}}},
                                              {"roadMesh", str("settlementSendToUnreal roadMesh.")},
                                              {"pcgGraph", str("settlementSendToUnreal pcgGraph.")},
                                              {"landscapeMaterial", str("terrainSendToUnreal material; default the generated one.")},
                                              {"hlodLayer", str("buildingSend hlodLayer.")},
                                              {"hlodProxyLayer", str("buildingSend hlodProxyLayer.")}})}})}}}};
        return out;
    }();
    return s;
}

void validateSpec(const json& spec)
{
    need(spec.is_object(), "the spec is a dict (see lila://schema/world-spec)");
    for (const auto& [k, v] : spec.items())
        need(topKeys().count(k) > 0, "unknown key " + k + " (see lila://schema/world-spec)");
    if (spec.contains("name"))
        need(spec["name"].is_string() && !spec["name"].get<std::string>().empty(), "name is a non-empty string");
    for (const char* k : {"remix", "terrain", "buildings", "slots", "zones", "export"})
        if (spec.contains(k) && !spec[k].is_null())
            need(spec[k].is_object(), std::string(k) + " is a dict");
    need(spec.contains("remix") || spec.contains("terrain"), "give remix or terrain: the world needs a terrain");
    std::set<std::string> names;
    if (spec.contains("settlements"))
    {
        need(spec["settlements"].is_array(), "settlements is a list");
        for (size_t i = 0; i < spec["settlements"].size(); ++i)
        {
            const json& s = spec["settlements"][i];
            const std::string where = "settlements[" + std::to_string(i) + "]";
            need(s.is_object(), where + " is a dict");
            need(s.contains("name") && s["name"].is_string() && !s["name"].get<std::string>().empty(), where + " needs a name");
            need(names.insert(s["name"].get<std::string>()).second, where + ": the name " + s["name"].get<std::string>() + " is used twice");
            need(s.contains("osm") != s.contains("create"), where + " takes osm (a file or 'remix') or create (settlementCreate flags), one of them");
            if (s.contains("osm"))
            {
                need(s["osm"].is_string(), where + ".osm is 'remix' or a file path");
                if (s["osm"] == "remix")
                    need(spec.contains("remix"), where + ".osm is 'remix' but the spec has no remix");
            }
            if (s.contains("create"))
                need(s["create"].is_object(), where + ".create is a dict of settlementCreate flags");
            if (s.contains("area"))
                checkArea(s["area"], where);
            if (s.contains("within"))
                need(s["within"].is_string() && s.contains("osm"), where + ".within is a terrain mask name, for an osm settlement");
            if (s.contains("buildings"))
                need(s["buildings"].is_boolean() || s["buildings"].is_object(), where + ".buildings is true/false or a dict");
            if (s.contains("overrides"))
                need(s["overrides"].is_object() && s.contains("osm"), where + ".overrides is a dict of profile fields, for an osm settlement");
        }
    }
    if (spec.contains("sites"))
    {
        need(spec["sites"].is_array(), "sites is a list");
        std::set<std::string> ids;
        for (size_t i = 0; i < spec["sites"].size(); ++i)
        {
            const json& s = spec["sites"][i];
            const std::string where = "sites[" + std::to_string(i) + "]";
            need(s.is_object(), where + " is a dict");
            need(s.contains("kind") && s["kind"].is_string(), where + " needs a kind (siteKinds lists them)");
            if (s.contains("id"))
            {
                need(s["id"].is_string(), where + ".id is a string");
                need(ids.insert(s["id"].get<std::string>()).second, where + ": the id " + s["id"].get<std::string>() + " is used twice");
            }
            if (s.contains("at"))
                need(s["at"].is_array() && s["at"].size() == 2, where + ".at is [x, z]");
            if (s.contains("params"))
                need(s["params"].is_object(), where + ".params is a dict");
        }
    }
    if (spec.contains("slots"))
    {
        const json& sl = spec["slots"];
        if (sl.contains("rules"))
            need(sl["rules"].is_array(), "slots.rules is a list");
        if (sl.contains("overrides"))
            need(sl["overrides"].is_object(), "slots.overrides is a dict {slot id: payload}");
        if (sl.contains("pins"))
            need(sl["pins"].is_array(), "slots.pins is a list of keys");
    }
    if (spec.contains("buildings"))
    {
        const json& b = spec["buildings"];
        if (b.contains("edits"))
        {
            need(b["edits"].is_object(), "buildings.edits is {group name: {building id: edit}}");
            for (const auto& [g, e] : b["edits"].items())
                need(e.is_object(), "buildings.edits." + g + " is {building id: edit}");
        }
        if (b.contains("styles"))
            need(b["styles"].is_array(), "buildings.styles is a list of buildingStyle calls");
        if (b.contains("enterable"))
        {
            const json& en = b["enterable"];
            need(en.is_object() || en.is_array(), "buildings.enterable is a pick {programs, landmarks, styles, ids, limit} or a list of them");
            for (const json& p : en.is_array() ? en : json::array({en}))
            {
                need(p.is_object(), "buildings.enterable picks are dicts");
                for (const auto& [k, v] : p.items())
                    need(k == "programs" || k == "landmarks" || k == "styles" || k == "ids" || k == "limit" || k == "program" || k == "furnish",
                         "buildings.enterable has no key '" + k + "' (programs, landmarks, styles, ids, limit, program, furnish)");
            }
        }
    }
    if (spec.contains("export"))
    {
        const std::string t = spec["export"].value("target", std::string("none"));
        need(t == "none" || t == "files" || t == "unreal", "export.target is none, files or unreal");
        if (spec["export"].contains("unreal"))
        {
            const json& u = spec["export"]["unreal"];
            need(u.is_object(), "export.unreal is a dict");
            static const std::set<std::string> keys = {"folder", "interiors", "roads", "roadMesh", "pcgGraph", "landscapeMaterial", "hlodLayer", "hlodProxyLayer"};
            for (const auto& [k, v] : u.items())
            {
                need(keys.count(k) > 0, "unknown key export.unreal." + k + " (see lila://schema/world-spec)");
                need(v.is_string(), "export.unreal." + k + " is a string");
            }
        }
    }
}

json resolvePaths(json spec, const std::string& dir, const std::function<bool(const std::string&)>& elsewhere)
{
    const fs::path d = utf8Path(dir);
    if (spec.contains("remix") && spec["remix"].is_object() && spec["remix"].contains("sources") && spec["remix"]["sources"].is_array())
        for (json& src : spec["remix"]["sources"])
            for (const char* k : {"dem", "osm", "landcover", "bathymetry"})
                if (src.contains(k) && src[k].is_string())
                    src[k] = resolved(src[k].get<std::string>(), d, elsewhere);
    if (spec.contains("terrain") && spec["terrain"].is_object() && spec["terrain"].contains("source") && spec["terrain"]["source"].is_object() &&
        spec["terrain"]["source"].contains("file") && spec["terrain"]["source"]["file"].is_string())
        spec["terrain"]["source"]["file"] = resolved(spec["terrain"]["source"]["file"].get<std::string>(), d, elsewhere);
    if (spec.contains("terrain") && spec["terrain"].is_object() && spec["terrain"].contains("coast") && spec["terrain"]["coast"].is_object())
    {
        json& c = spec["terrain"]["coast"];
        if (c.contains("bathymetry") && c["bathymetry"].is_object() && c["bathymetry"].contains("file") && c["bathymetry"]["file"].is_string())
            c["bathymetry"]["file"] = resolved(c["bathymetry"]["file"].get<std::string>(), d, elsewhere);
    }
    if (spec.contains("settlements") && spec["settlements"].is_array())
        for (json& s : spec["settlements"])
            if (s.is_object() && s.contains("osm") && s["osm"].is_string())
                s["osm"] = resolved(s["osm"].get<std::string>(), d, elsewhere);
    return spec;
}

OsmDoc clipOsm(const OsmDoc& d, const std::vector<glm::dvec2>& world, glm::dvec2 lo, glm::dvec2 hi, const std::function<bool(glm::dvec2)>& keep)
{
    if (world.size() != d.nodes.size())
        throw std::runtime_error("clipOsm: one world position per node");
    std::unordered_map<int64_t, size_t> at;
    for (size_t i = 0; i < d.nodes.size(); ++i)
        at[d.nodes[i].id] = i;
    auto inBox = [&](glm::dvec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    auto inside = [&](glm::dvec2 p) { return inBox(p) && (!keep || keep(p)); };
    std::vector<char> in(d.nodes.size(), 0);
    for (size_t i = 0; i < d.nodes.size(); ++i)
        in[i] = inside(world[i]) ? 1 : 0;

    OsmDoc out;
    std::unordered_set<int64_t> keepNodes, keepWays;
    std::unordered_map<int64_t, std::vector<int64_t>> pieces; // original way -> the ways it became
    int64_t nextId = int64_t(1) << 52;
    // A node where the segment from a (inside) to b (outside) crosses the area's edge.
    auto edgeNode = [&](size_t a, size_t b) {
        const glm::dvec2 p = world[a], q = world[b];
        double t = 1.0;
        for (int k = 0; k < 2; ++k)
        {
            if (q[k] < lo[k] && q[k] != p[k])
                t = std::min(t, (lo[k] - p[k]) / (q[k] - p[k]));
            if (q[k] > hi[k] && q[k] != p[k])
                t = std::min(t, (hi[k] - p[k]) / (q[k] - p[k]));
        }
        if (keep && !keep(p + (q - p) * t))
        {
            // The line leaves through keep's edge before the box's: found by halving.
            double tIn = 0.0, tOut = t;
            for (int it = 0; it < 16; ++it)
            {
                const double m = 0.5 * (tIn + tOut);
                (keep(p + (q - p) * m) ? tIn : tOut) = m;
            }
            t = tIn;
        }
        OsmDoc::Node n;
        n.id = nextId++;
        n.at = {d.nodes[a].at.lat + (d.nodes[b].at.lat - d.nodes[a].at.lat) * t, d.nodes[a].at.lon + (d.nodes[b].at.lon - d.nodes[a].at.lon) * t};
        out.nodes.push_back(n);
        return n.id;
    };
    for (const OsmDoc::Way& w : d.ways)
    {
        std::vector<size_t> idx;
        for (int64_t n : w.nodes)
            if (auto it = at.find(n); it != at.end())
                idx.push_back(it->second);
        if (idx.size() < 2)
            continue;
        const bool closed = w.nodes.size() > 2 && w.nodes.front() == w.nodes.back();
        size_t inCount = 0;
        for (size_t i : idx)
            inCount += in[i];
        if (inCount == 0)
            continue;
        if (closed)
        {
            if (inCount * 2 < idx.size())
                continue;
            keepWays.insert(w.id);
            pieces[w.id] = {w.id};
            for (int64_t n : w.nodes)
                keepNodes.insert(n);
            out.ways.push_back(w);
            continue;
        }
        // Runs of inside nodes, each closed off by a node on the edge where the line leaves.
        std::vector<int64_t> run;
        auto flush = [&] {
            if (run.size() >= 2)
            {
                OsmDoc::Way piece = w;
                piece.id = pieces[w.id].empty() ? w.id : nextId++;
                piece.nodes = run;
                pieces[w.id].push_back(piece.id);
                keepWays.insert(piece.id);
                out.ways.push_back(std::move(piece));
            }
            run.clear();
        };
        for (size_t k = 0; k < idx.size(); ++k)
        {
            const size_t i = idx[k];
            if (in[i])
            {
                if (run.empty() && k > 0 && !in[idx[k - 1]])
                    run.push_back(edgeNode(i, idx[k - 1]));
                run.push_back(d.nodes[i].id);
                keepNodes.insert(d.nodes[i].id);
            }
            else if (!run.empty())
            {
                run.push_back(edgeNode(idx[k - 1], i));
                flush();
            }
        }
        flush();
    }
    for (const OsmDoc::Relation& r : d.relations)
    {
        OsmDoc::Relation kept = r;
        kept.members.clear();
        for (const OsmDoc::Member& m : r.members)
        {
            if (m.type == 'w')
            {
                if (auto it = pieces.find(m.ref); it != pieces.end())
                    for (int64_t id : it->second)
                        kept.members.push_back({'w', id, m.role});
            }
            else if (m.type == 'n')
            {
                if (auto it = at.find(m.ref); it != at.end() && in[it->second])
                {
                    kept.members.push_back(m);
                    keepNodes.insert(m.ref);
                }
            }
        }
        if (!kept.members.empty())
            out.relations.push_back(std::move(kept));
    }
    for (size_t i = 0; i < d.nodes.size(); ++i)
        if (keepNodes.count(d.nodes[i].id) || (in[i] && !d.nodes[i].tags.empty()))
            out.nodes.push_back(d.nodes[i]);
    out.index();
    return out;
}
std::string remixBathymetryFiles(const json& remix)
{
    std::string files;
    for (const json& src : remix.value("sources", json::array()))
        if (src.is_object() && src.contains("bathymetry") && src["bathymetry"].is_string() && !src["bathymetry"].get<std::string>().empty())
            files += (files.empty() ? "" : ", ") + src["bathymetry"].get<std::string>();
    return files;
}

} // namespace openworld::world
