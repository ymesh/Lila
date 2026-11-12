// settlementRoadMesh builds road, sidewalk and crosswalk meshes; settlementFurniture places street furniture (through
// the Set Dressing extension's place command when realized); settlementNames names streets and districts;
// settlementLanes and settlementTransit return the lane graph, bus and tram routes and their checks.

#include "Settlement.h"
#include "Streetscape.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using scene::Node;
using StreetT = ArgType;

std::string settlementOf(const Args& a, const char* cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one settlement");
    return names[0];
}

json pointJson(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

std::string streetBaseName(const Rebuilt& r)
{
    const std::string& path = r.mesh.empty() ? r.modifier : r.mesh;
    std::string leaf = path.substr(path.find_last_of('|') == std::string::npos ? 0 : path.find_last_of('|') + 1);
    for (char& c : leaf)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    return leaf.empty() ? std::string("Settlement") : leaf;
}

/** A new mesh object holding m (face attributes kept). Returns its path. */
std::string meshObject(Graph& g, geo::Mesh m, const std::string& name)
{
    const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
    const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    g.setValue({g.find(path)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return path;
}

/** Terrain roads laid for this settlement's streets ("<settlement>/street/<street id>"), by street id. */
std::map<uint64_t, std::string> terrainStreetRoads(const Rebuilt& r, std::string* why)
{
    std::map<uint64_t, std::string> out;
    const std::string ground = r.values.value("ground", std::string());
    if (ground.empty())
    {
        *why = "flat ground: no terrain";
        return out;
    }
    if (!ops::find("terrainRoads") || !ops::find("terrainRoadBuild"))
    {
        *why = "the Terrain extension (terrainRoads, terrainRoadBuild) is not loaded";
        return out;
    }
    json roads;
    try
    {
        roads = ops::execute("terrainRoads", {{"args", {ground}}});
    }
    catch (const std::exception&)
    {
        *why = "the ground '" + ground + "' is not a terrain";
        return out;
    }
    const std::string prefix = (r.mesh.empty() ? r.modifier : r.mesh) + "/street/";
    for (const auto& [layer, list] : roads.value("layers", json::object()).items())
        for (const json& road : list)
        {
            const std::string id = road.value("id", std::string());
            if (id.rfind(prefix, 0) == 0)
                try
                {
                    out[std::stoull(id.substr(prefix.size()))] = id;
                }
                catch (const std::exception&)
                {
                }
        }
    if (out.empty())
        *why = "the terrain has no roads laid for this settlement's streets (ids " + prefix + "<street id>)";
    return out;
}

std::vector<FlagDef> laneFlags()
{
    return {{"busRoutes", "br", StreetT::Int, int64_t(-1), "Bus routes (each out and back); -1 from the style: none in medieval ones, else one per two districts, at most 4."},
            {"tramRoutes", "tr", StreetT::Int, int64_t(-1), "Tram routes on the arterials' inner lanes; -1: one in cities with arterials."},
            {"busStopSpacing", "bs", StreetT::Float, 40000.0, "cm between bus stops.", {}, {}, 2000.0},
            {"tramStopSpacing", "ts", StreetT::Float, 60000.0, "cm between tram stops.", {}, {}, 2000.0},
            {"lanes", "ln", StreetT::Bool, false, "Also every lane (points, mode, direction, street) and link (movement, phase)."}};
}

TransitOptions transitOptions(const Args& a)
{
    TransitOptions o;
    o.busRoutes = int(a.get<int64_t>("busRoutes", -1));
    o.tramRoutes = int(a.get<int64_t>("tramRoutes", -1));
    o.busStopSpacing = a.get<double>("busStopSpacing", 40000.0);
    o.tramStopSpacing = a.get<double>("tramStopSpacing", 60000.0);
    return o;
}

json namesJson(const Layout& l)
{
    std::map<uint64_t, json> streets;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
        {
            const StreetEdge& x = l.graph.edges[e];
            if (!streets.contains(x.street))
                streets[x.street] = {{"id", x.street}, {"name", streetName(l, x.street)}, {"class", className(x.cls)}};
        }
    json s = json::array(), d = json::array();
    for (auto& [id, j] : streets)
        s.push_back(std::move(j));
    for (const District& x : l.districts)
        d.push_back({{"id", x.id}, {"name", x.name}, {"areaM2", x.area}, {"center", json::array({x.center.x, x.center.y})}});
    return {{"culture", l.culture}, {"cultures", cultureNames()}, {"streets", s}, {"districts", d}};
}
} // namespace

void registerStreetscapeCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "settlementRoadMesh",
        .doc = "Builds a settlement's roads as meshes under a group <settlement>_Streetscape, made again on every call: <name>_Roads "
               "(carriageways swept along every road with its cross-section at the street's width, and a patch over each "
               "junction with rounded corners), <name>_Sidewalks (sidewalks raised by curb, kerb faces, medians, verges, and "
               "curb returns round each corner where both roads have a sidewalk) and <name>_Markings (crosswalk stripes at "
               "signalised and stop junctions). Medieval styles get cobble and packed earth edge to edge with a gutter down "
               "the middle and no kerbs or markings. Face attributes part, surface, line and junction.\n\n"
               "On a Terrain, streets already laid as terrain roads with ids <settlement>/street/<street id> are built by "
               "terrainRoadBuild instead (builder says which was used, and why). Returns the meshes, areas in m2 (roads, "
               "junctions, sidewalks, curb returns, markings), counts, and crosswalk decal placements.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"curb", "cb", StreetT::Float, 15.0, "Height of sidewalks and medians above the road, cm.", {}, {}, 0.0},
                  {"gutter", "gt", StreetT::Float, 8.0, "How far the middle of a kerbless street dips, cm.", {}, {}, 0.0},
                  {"segment", "sg", StreetT::Float, 500.0, "Longest step between cross-sections, cm.", {}, {}, 10.0},
                  {"markings", "mk", StreetT::String, "both", "Crosswalks as mesh stripes, decal placements (returned), both or none.",
                   {"both", "mesh", "decals", "none"}},
                  {"surface", "sf", StreetT::String, "auto", "Paving for every road; auto from the style and class.", {"auto", "asphalt", "cobble", "earth", "paving"}},
                  {"builder", "bd", StreetT::String, "auto", "auto: terrainRoadBuild for streets laid as terrain roads, the rest here; own: all here.",
                   {"auto", "own"}},
                  {"name", "n", StreetT::String, nullptr, "Base name of the meshes (default the settlement's)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementRoadMesh";
            Graph& g = Graph::current();
            const Rebuilt r = rebuildSettlement(g, settlementOf(a, cmd), cmd);
            RoadMeshOptions o;
            o.curb = a.get<double>("curb", 15.0);
            o.gutter = a.get<double>("gutter", 8.0);
            o.segment = a.get<double>("segment", 500.0);
            const std::string markings = a.get<std::string>("markings", "both");
            o.markings = markings == "both" || markings == "mesh";
            if (const std::string surface = a.get<std::string>("surface", "auto"); surface != "auto")
            {
                if (!parseSurface(surface, &o.surface))
                    throw CommandError(std::string(cmd) + ": unknown surface '" + surface + "'");
                o.forceSurface = true;
            }
            RoadNetwork net = roadNetwork(r.layout, r.ground);
            json out = json::object();
            std::string why;
            std::map<uint64_t, std::string> terrainRoads;
            if (a.get<std::string>("builder", "auto") == "auto")
                terrainRoads = terrainStreetRoads(r, &why);
            else
                why = "builder='own'";
            if (!terrainRoads.empty())
            {
                json ids = json::array();
                for (const auto& [street, id] : terrainRoads)
                    ids.push_back(id);
                out["terrainBuilt"] = ops::execute("terrainRoadBuild", {{"args", {r.values.value("ground", std::string())}}, {"ids", ids}});
                // The rest keep their junctions, renumbered; crosswalks and phases name ends by index, so they go.
                std::vector<int> remap(net.lines.size(), -1);
                std::vector<StreetLine> kept;
                for (size_t k = 0; k < net.lines.size(); ++k)
                    if (!terrainRoads.contains(net.lines[k].street))
                    {
                        remap[k] = int(kept.size());
                        kept.push_back(net.lines[k]);
                    }
                for (Junction& j : net.junctions)
                {
                    std::erase_if(j.ends, [&](const RoadEnd& e) { return remap[size_t(e.line)] < 0; });
                    for (RoadEnd& e : j.ends)
                        e.line = remap[size_t(e.line)];
                    j.crosswalks.clear();
                    j.phases.clear();
                    j.walks.clear();
                }
                net.lines = std::move(kept);
                net.parking.clear();
            }
            out["builder"] = terrainRoads.empty() ? "own" : net.lines.empty() ? "terrain" : "mixed";
            out["builderNote"] = terrainRoads.empty() ? "settlement mesher: " + why
                                                      : std::to_string(terrainRoads.size()) + " streets built by terrainRoadBuild, the rest here";
            if (!terrainRoads.empty() && !net.lines.empty())
                log::warn("{}: {} streets built by terrainRoadBuild; junctions they meet are patched here without crosswalks", cmd, terrainRoads.size());

            RoadMeshes m = roadMeshes(r.layout, net, r.ground, o);
            const std::string base = a.get<std::string>("name", streetBaseName(r));
            if (Node* old = g.find(base + "_Streetscape"))
                ops::execute("delete", {{"args", {old->path()}}});
            json children = json::array();
            const size_t roadFaces = m.roads.numFaces(), walkFaces = m.sidewalks.numFaces(), markFaces = m.markings.numFaces();
            children.push_back(meshObject(g, std::move(m.roads), base + "_Roads"));
            children.push_back(meshObject(g, std::move(m.sidewalks), base + "_Sidewalks"));
            if (markFaces > 0)
                children.push_back(meshObject(g, std::move(m.markings), base + "_Markings"));
            const json group = ops::execute("group", {{"args", children}, {"name", base + "_Streetscape"}});
            json decals = json::array();
            if (markings == "both" || markings == "decals")
                for (const DecalPlacement& d : m.crosswalks)
                    decals.push_back({{"point", pointJson(d.p)}, {"width", d.width}, {"depth", d.depth}, {"heading", d.heading}, {"junction", d.junction}});
            out["group"] = group;
            out["meshes"] = children;
            out["faces"] = {{"roads", roadFaces}, {"sidewalks", walkFaces}, {"markings", markFaces}};
            out["areaM2"] = {{"roads", m.roadArea}, {"junctions", m.junctionArea}, {"sidewalks", m.sidewalkArea},
                             {"curbReturns", m.curbReturnArea}, {"markings", m.markingArea}};
            out["roads"] = net.lines.size();
            out["junctions"] = net.junctions.size();
            out["curbReturns"] = m.curbReturns;
            out["crosswalks"] = decals;
            out["medieval"] = medievalStyle(r.layout);
            return out;
        },
        .examples = {"r = settlementRoadMesh('Settlement')\nr['areaM2']['roads'], r['curbReturns']",
                     "settlementRoadMesh('Village', surface='cobble', markings='none')"},
    });

    ctx.registerCommand({
        .name = "settlementFurniture",
        .doc = "Places street furniture: along every road from its road profile's set (lamps, street trees, benches, bins, "
               "hydrants; lanterns in medieval styles), signals on each approach of signalised junctions, stop and yield "
               "signs by junction control, a street-name sign at each junction, and squares furnished (medieval: a well, market "
               "stalls and carts; modern: benches, trees, and bollards along the main streets). Every placement has a stable id "
               "that survives unrelated edits.\n\n"
               "Sets are data: sets= replaces a profile's rules, {profile: [{kind, spacing, where, sides, minWidth}]} with "
               "where kerb, back, verge, median or edge and sides both, alternate, right or left. realize=True places a "
               "Set Dressing library asset for each kind in assets= (lamp defaults to Road/StreetLight) through its place "
               "command; kinds without an asset are listed in missingAssets. store=True keeps sets= with the settlement and "
               "engineAssets= names the engine asset per kind (default /Game/Settlement/Furniture/SM_<Kind>): the layout's "
               "placements, which exports and Send to Unreal read, use both. Returns counts by kind and by profile, the "
               "sets used and the placements.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"sets", "st", StreetT::Any, nullptr, "Furniture rules per road profile, over the built-in ones."},
                  {"kinds", "k", StreetT::List, nullptr, "Only these kinds."},
                  {"realize", "rz", StreetT::Bool, false, "Place library assets in the scene (needs the Set Dressing extension)."},
                  {"assets", "as", StreetT::Any, nullptr, "{kind: library asset id} for realize."},
                  {"limit", "lm", StreetT::Int, int64_t(2000), "Most placements returned in the list (counts cover all).", {}, {}, 0.0},
                  {"store", "sr", StreetT::Bool, false, "Keep sets= with the settlement, so its placements (exports, Send to Unreal) use them."},
                  {"engineAssets", "ea", StreetT::Any, nullptr, "{kind: engine asset path} the settlement's placements use, kept with it."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementFurniture";
            Graph& g = Graph::current();
            const std::string target = settlementOf(a, cmd);
            Rebuilt r = rebuildSettlement(g, target, cmd);
            const bool medieval = medievalStyle(r.layout);
            std::vector<FurnitureSet> sets;
            try
            {
                const json raw = ops::execute("getAttr", {{"args", {r.modifier + ".edits"}}});
                json edits = parseEdits(raw.is_string() ? raw.get<std::string>() : std::string());
                const bool storeSets = a.get<bool>("store", false) && a.has("sets");
                if (storeSets || a.has("engineAssets"))
                {
                    if (a.has("engineAssets") && !a.flags["engineAssets"].is_object())
                        throw std::runtime_error("engineAssets is {kind: asset path}");
                    applyFurnitureJson(furnitureSets(medieval), a.flags.value("sets", json()));
                    json& f = edits["furniture"];
                    if (!f.is_object())
                        f = json::object();
                    if (storeSets)
                        f["sets"] = a.flags["sets"];
                    if (a.has("engineAssets"))
                        f["assets"] = a.flags["engineAssets"];
                    ops::execute("setAttr", {{"args", {r.modifier + ".edits", edits.dump()}}});
                    r = rebuildSettlement(g, target, cmd);
                }
                sets = applyFurnitureJson(furnitureSetsFor(r.layout, edits), a.flags.value("sets", json()));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }
            const RoadNetwork net = roadNetwork(r.layout, r.ground);
            std::vector<FurniturePlacement> placed = placeFurniture(r.layout, net, r.ground, sets);
            if (a.has("kinds"))
            {
                std::set<std::string> keep;
                for (const json& k : a.flags["kinds"])
                    keep.insert(k.get<std::string>());
                std::erase_if(placed, [&](const FurniturePlacement& p) { return !keep.contains(p.kind); });
            }
            json byKind = json::object(), byProfile = json::object(), list = json::array();
            const size_t limit = size_t(std::max<int64_t>(0, a.get<int64_t>("limit", 2000)));
            for (const FurniturePlacement& p : placed)
            {
                byKind[p.kind] = byKind.value(p.kind, 0) + 1;
                json& prof = byProfile[p.profile];
                if (prof.is_null())
                    prof = json::object();
                prof[p.kind] = prof.value(p.kind, 0) + 1;
                if (list.size() < limit)
                {
                    json j = {{"id", p.id}, {"kind", p.kind}, {"point", pointJson(p.p)}, {"heading", p.heading}, {"profile", p.profile}};
                    if (p.street)
                        j["street"] = p.street;
                    if (p.junction >= 0)
                        j["junction"] = p.junction;
                    if (p.block >= 0)
                        j["block"] = p.block;
                    list.push_back(std::move(j));
                }
            }
            json setsJson = json::object();
            for (const FurnitureSet& s : sets)
                setsJson[s.profile] = toJson(s);
            json out = {{"count", placed.size()}, {"byKind", byKind}, {"byProfile", byProfile}, {"sets", setsJson},
                        {"medieval", medieval}, {"placements", list}, {"layoutPlacements", r.layout.placements.size()}};
            if (a.get<bool>("realize", false))
            {
                if (!ops::find("place"))
                    throw CommandError(std::string(cmd) + ": realize needs the place command; load the Set Dressing (Stagehand) extension");
                json assets = {{"lamp", "Road/StreetLight"}};
                if (a.has("assets"))
                {
                    if (!a.flags["assets"].is_object())
                        throw CommandError(std::string(cmd) + ": assets is {kind: asset id}");
                    assets.update(a.flags["assets"]);
                }
                std::set<std::string> missing;
                json roots = json::array();
                std::map<std::string, std::string> failed;
                for (const FurniturePlacement& p : placed)
                {
                    if (!assets.contains(p.kind) || !assets[p.kind].is_string())
                    {
                        missing.insert(p.kind);
                        continue;
                    }
                    if (failed.contains(p.kind))
                        continue;
                    try
                    {
                        const json made = ops::execute("place", {{"args", {assets[p.kind]}}, {"at", pointJson(p.p)}, {"snap", false}, {"rotate", p.heading}});
                        if (made.is_array() && !made.empty())
                            roots.push_back(made[0].value("root", made[0].value("name", json())));
                    }
                    catch (const std::exception& e)
                    {
                        failed[p.kind] = e.what();
                    }
                }
                out["placed"] = roots.size();
                out["missingAssets"] = missing;
                out["failedAssets"] = failed;
                if (!missing.empty())
                    log::warn("{}: no library asset for {} kinds (assets= names them); their placements are data only", cmd, missing.size());
            }
            return out;
        },
        .examples = {"f = settlementFurniture('Settlement')\nf['byKind']['lamp'], f['byProfile']['avenue']",
                     "settlementFurniture('Settlement', sets={'street': [{'kind': 'lamp', 'spacing': 2500, 'sides': 'both'}]}, kinds=['lamp'])",
                     "settlementFurniture('Settlement', realize=True, assets={'bench': 'Street/Bench'})"},
    });

    ctx.registerCommand({
        .name = "settlementNames",
        .doc = "Street and district names. Every street through its junctions has one name from the naming culture "
               "(oldEnglish, latin, french, spanish, usGrid with numbered streets and avenues, sector codes), stable by the "
               "ids of the streets in it; districts are the quarters between arterials. culture= sets the settlement's "
               "culture (default from its style); district= with name= renames a district. Street names change with "
               "settlementEditStreet(name=). Names save with the scene and go out in settlementExport. Returns the culture, "
               "every street's name by stable id and the districts.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"culture", "cu", StreetT::String, nullptr, "Naming culture; empty for the style's.", {"oldEnglish", "latin", "french", "spanish", "usGrid", "sector", ""}},
                  {"district", "d", StreetT::Int, nullptr, "A district's id, with name=."},
                  {"name", "n", StreetT::String, nullptr, "The district's new name; empty gives it back to the culture."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementNames";
            Graph& g = Graph::current();
            const std::string target = settlementOf(a, cmd);
            Rebuilt r = rebuildSettlement(g, target, cmd);
            if (a.has("culture") || a.has("district"))
            {
                const json raw = ops::execute("getAttr", {{"args", {r.modifier + ".edits"}}});
                json edits;
                try
                {
                    edits = parseEdits(raw.is_string() ? raw.get<std::string>() : std::string());
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string(cmd) + ": " + e.what());
                }
                json& naming = edits["naming"];
                if (!naming.is_object())
                    naming = json::object();
                if (a.has("culture"))
                {
                    const std::string c = a.get<std::string>("culture");
                    const std::vector<std::string> known = cultureNames();
                    if (!c.empty() && std::ranges::find(known, c) == known.end())
                        throw CommandError(std::string(cmd) + ": unknown culture '" + c + "'");
                    naming["culture"] = c;
                }
                if (a.has("district"))
                {
                    if (!a.has("name"))
                        throw CommandError(std::string(cmd) + ": district= needs name=");
                    const uint64_t id = uint64_t(a.get<int64_t>("district"));
                    if (std::ranges::find_if(r.layout.districts, [&](const District& d) { return d.id == id; }) == r.layout.districts.end())
                        throw CommandError(std::string(cmd) + ": no district with id " + std::to_string(id));
                    json& districts = naming["districts"];
                    if (!districts.is_object())
                        districts = json::object();
                    if (a.get<std::string>("name").empty())
                        districts.erase(std::to_string(id));
                    else
                        districts[std::to_string(id)] = a.get<std::string>("name");
                }
                ops::execute("setAttr", {{"args", {r.modifier + ".edits", edits.dump()}}});
                r = rebuildSettlement(g, target, cmd);
            }
            return namesJson(r.layout);
        },
        .examples = {"settlementNames('Settlement', culture='french')['streets'][0]['name']",
                     "d = settlementNames('Settlement')['districts'][0]['id']\nsettlementNames('Settlement', district=d, name='Old Town')"},
    });

    ctx.registerCommand({
        .name = "settlementLanes",
        .doc = "The lane graph a traffic system reads: lanes per road per direction, pedestrian lanes on sidewalks and "
               "footpaths, links for the movements junctions allow (through, left, right, U-turns at dead ends, corners, "
               "crosswalks, unmarked crossings) with each signalised movement's phase, and bus and tram routes. checks: per "
               "mode the strongly connected components, lanes that cannot reach an exit and exits that cannot be reached, "
               "conflicting greens (protected movements of one phase that cross or merge), unphased approaches and "
               "unreachable transit routes; ok when all are empty. Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = laneFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementLanes";
            const Rebuilt r = rebuildSettlement(Graph::current(), settlementOf(a, cmd), cmd);
            const RoadNetwork net = roadNetwork(r.layout, r.ground);
            LaneGraph lanes = laneGraph(r.layout, net);
            addTransit(lanes, r.layout, net, transitOptions(a));
            json out = toJson(lanes, a.get<bool>("lanes", false));
            out["checks"] = checkLaneGraph(lanes, net);
            return out;
        },
        .examples = {"c = settlementLanes('Settlement')['checks']\nc['ok'], c['modes']['vehicle']['components']"},
    });

    ctx.registerCommand({
        .name = "settlementTransit",
        .doc = "Bus and tram routes through the lane graph: buses along arterials and collectors, trams on the arterials' "
               "inner lanes, each between two districts across the centre, out and back, with stops by spacing named by "
               "their street. Each route says whether it is reachable (every lane leads to the next, and its far end leads "
               "back to the start) and notes any fallback (a bus route that had to use minor streets). Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = laneFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementTransit";
            const Rebuilt r = rebuildSettlement(Graph::current(), settlementOf(a, cmd), cmd);
            const RoadNetwork net = roadNetwork(r.layout, r.ground);
            LaneGraph lanes = laneGraph(r.layout, net);
            addTransit(lanes, r.layout, net, transitOptions(a));
            const json all = toJson(lanes, false);
            size_t stops = 0;
            for (const TransitRoute& t : lanes.routes)
                stops += t.stops.size();
            return {{"routes", all["routes"]}, {"stops", stops}, {"unreachable", checkLaneGraph(lanes, net)["unreachableRoutes"]}};
        },
        .examples = {"t = settlementTransit('Settlement', busRoutes=2, tramRoutes=1)\n[(r['name'], len(r['stops'])) for r in t['routes']]"},
    });
}
} // namespace settlement
