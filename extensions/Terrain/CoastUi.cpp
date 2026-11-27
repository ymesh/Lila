// Level > Coast and Water and the Level Toolkit's Coast and Water section, with the option boxes' fills and notes.

#include "Coast.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileDialog.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>
#include <lila/Transform.h>
#include <lila/ViewportTool.h>

#include <QApplication>

#include <format>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
namespace coast_ui
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;

/** The selected terrain and curve, either empty when none is selected. */
std::pair<std::string, std::string> terrainAndCurve()
{
    Graph& g = Graph::current();
    std::string terrain, curve;
    for (scene::NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        if (n->type().isA("curve"))
        {
            if (curve.empty())
                curve = n->path();
            continue;
        }
        if (terrain.empty())
            try
            {
                findTerrainRef(n->path(), "");
                terrain = n->path();
            }
            catch (const std::exception&)
            {
            }
    }
    return {terrain, curve};
}

/** The selected curve's first and last points in world space, as a= and b=. */
json endsOfSelectedCurve()
{
    const auto [terrain, curve] = terrainAndCurve();
    if (curve.empty())
        return json::object();
    try
    {
        const json s = ops::execute("curveData", {{"args", {curve}}, {"worldSpace", true}});
        const json& pts = s.at(0).at("points");
        if (pts.size() < 2)
            return json::object();
        return {{"a", pts.front()}, {"b", pts.back()}};
    }
    catch (const std::exception&)
    {
        return json::object();
    }
}

/** The selected locator's (or any transform's) world position as at=. */
json selectedPoint()
{
    Graph& g = Graph::current();
    for (scene::NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("locator"))
        {
            const glm::dmat4 m = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            return {{"at", {m[3].x, m[3].y, m[3].z}}};
        }
    return json::object();
}

void runWith(const std::string& cmd, bool curveFlag)
{
    const auto [terrain, curve] = terrainAndCurve();
    json call = json::object();
    if (!terrain.empty())
        call["args"] = {terrain};
    if (curveFlag && !curve.empty())
        call["curve"] = curve;
    actions::runWithOptions(cmd, call);
}

std::string fetchNote()
{
    const auto [terrain, curve] = terrainAndCurve();
    if (terrain.empty())
        return "Select a terrain to see the download size.";
    try
    {
        const json e = ops::execute("terrainBathymetry", {{"args", {terrain}}, {"estimate", true}});
        return std::format("GEBCO for this terrain: about {:.1f} MB", e["estimate"]["megabytes"].get<double>());
    }
    catch (const std::exception&)
    {
        return "Download size needs a georeferenced terrain (terrainImportDem).";
    }
}

void registerOptionBoxes()
{
    using ops::options::Override;
    Override bathy;
    bathy.order = {"file", "fetch", "consent", "dataset", "seaLevel", "shoreSlope", "blendWidth", "smoothing", "shelfDepth", "shelfWidth", "dropDepth", "slopeWidth"};
    bathy.hidden = {"estimate", "off", "apiKey", "saveKey", "bbox"};
    bathy.labels = {{"file", "GEBCO Grid"}, {"fetch", "Fetch for Area"}, {"consent", "Allow Large Download"}, {"seaLevel", "Sea Level"},
                    {"shoreSlope", "Shore Slope"}, {"blendWidth", "Blend Width"}, {"shelfDepth", "Shelf Depth"}, {"shelfWidth", "Shelf Width"},
                    {"dropDepth", "Deep Water"}, {"slopeWidth", "Drop Width"}, {"landTolerance", "Land Tolerance"}, {"minDepth", "Min Depth"},
                    {"profileOnly", "Profile Only"}, {"credit", "Write Credit"}};
    bathy.sections = {{"shoreSlope", "Shore"}, {"blendWidth", "Shore"}, {"smoothing", "Shore"}, {"shelfDepth", "Profile"}, {"shelfWidth", "Profile"},
                      {"dropDepth", "Profile"}, {"slopeWidth", "Profile"}};
    bathy.notes = {{"fetch", fetchNote}};
    bathy.docs = "terrain-coast?id=bathymetry";
    ops::options::setOverride("terrainBathymetry", bathy);

    Override channel;
    channel.order = {"curve", "craft", "braids", "margin", "width", "depth", "bankAngle", "meander", "reErode", "seed", "id"};
    channel.hidden = {"path", "waterLevel"};
    channel.labels = {{"braidSpread", "Braid Spread"}, {"bankAngle", "Bank Angle"}, {"reErode", "Settle Passes"}};
    channel.docs = "terrain-coast?id=channels";
    ops::options::setOverride("terrainAddChannel", channel);

    Override dam;
    dam.order = {"a", "b", "crest", "freeboard", "spillwayWidth", "spillwayAt", "thickness", "faceSlope", "lake", "river", "riverLength", "id"};
    dam.shown = {"a", "b"};
    dam.labels = {{"a", "End A"}, {"b", "End B"}, {"crest", "Crest Height"}, {"spillwayWidth", "Spillway Width"}, {"spillwayAt", "Spillway Position"},
                  {"faceSlope", "Face Slope"}, {"lake", "Fill Reservoir"}, {"river", "River Below"}};
    dam.fills = {{"From Curve", "Dam ends from the selected curve's first and last points.", endsOfSelectedCurve}};
    dam.docs = "terrain-coast?id=reservoirs";
    ops::options::setOverride("terrainAddReservoir", dam);

    Override strip;
    strip.order = {"a", "b", "aircraft", "width", "pad", "bothEnds", "elevation", "id"};
    strip.shown = {"a", "b"};
    strip.labels = {{"a", "End A"}, {"b", "End B"}, {"pad", "Grade Runway"}, {"bothEnds", "Both Ends"}};
    strip.fills = {{"From Curve", "Runway ends from the selected curve's first and last points.", endsOfSelectedCurve}};
    strip.docs = "terrain-coast?id=routes";
    ops::options::setOverride("terrainAddAirstrip", strip);

    Override lane;
    lane.order = {"at", "curve", "craft", "kind", "id"};
    lane.shown = {"at"};
    lane.hidden = {"path"};
    lane.labels = {{"at", "Berth"}};
    lane.fills = {{"From Locator", "Berth at the selected locator.", selectedPoint}};
    lane.docs = "terrain-coast?id=routes";
    ops::options::setOverride("terrainAddLane", lane);

    Override routes;
    routes.labels = {{"coastLanes", "Coast Lanes"}, {"coastDepth", "Coast Depth"}, {"coastCraft", "Coast Craft"}, {"minLaneLength", "Min Lane Length"},
                     {"riverLanes", "River Lanes"}, {"channelLanes", "Channel Lanes"}, {"linkReach", "Link Reach"}};
    routes.docs = "terrain-coast?id=routes";
    ops::options::setOverride("terrainRoutes", routes);

    Override mangroves;
    mangroves.hidden = {"off"};
    mangroves.labels = {{"tidalLow", "Tide Low"}, {"tidalHigh", "Tide High"}, {"maxSlope", "Max Slope"}, {"landcover", "Land Cover Mask"},
                        {"code", "Mangrove Code"}, {"fromWetland", "Wetland Fallback"}, {"fromBand", "Tidal Band Fallback"},
                        {"shelterRadius", "Shelter Radius"}, {"shelterMax", "Max Open Water"}, {"biome", "Add Biome"}};
    mangroves.docs = "terrain-coast?id=mangroves";
    ops::options::setOverride("terrainMangroves", mangroves);

    Override masks;
    masks.hidden = {"off"};
    masks.sections = {{"beachSlope", "Beach"},    {"beachHeight", "Beach"},  {"beachBelow", "Beach"},       {"surfDepth", "Surf"},
                      {"surfWidth", "Surf"},      {"shallowDepth", "Shallows"}, {"reefMinDepth", "Reef"},   {"reefMaxDepth", "Reef"},
                      {"reefMinDistance", "Reef"}, {"reefMaxDistance", "Reef"}, {"reefCover", "Reef"},       {"reefScale", "Reef"}, {"seed", "Reef"}};
    masks.docs = "terrain-coast?id=coast-masks";
    ops::options::setOverride("terrainCoastMasks", masks);

    Override overlay;
    overlay.labels = {{"depthStep", "Depth Step"}};
    ops::options::setOverride("terrainCoastOverlay", overlay);
}
} // namespace coast_ui

void registerCoastUi(ext::Context& ctx)
{
    using namespace coast_ui;
    struct Entry
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
        const char* icon;
        std::vector<std::string> keywords;
        bool curve = false;
    };
    const std::vector<Entry> entries = {
        {"terrain.bathymetry", "Bathymetry", "terrainBathymetry", "Lay a seabed under the selected terrain's sea from a GEBCO grid or a shelf profile.",
         "terrainBathymetry('Terrain', file='gebco.tif')", "ripple", {"bathymetry", "seabed", "gebco", "ocean depth", "sea floor", "underwater", "coastline"}},
        {"terrain.coastMasks", "Coast Masks", "terrainCoastMasks", "Beach, surf, shallows and reef masks along the selected terrain's shore.",
         "terrainCoastMasks('Terrain')", "beach", {"beach", "surf", "shallows", "reef", "coral", "shore", "coast"}},
        {"terrain.mangroves", "Mangroves", "terrainMangroves", "Mangroves in the tidal band of the selected terrain, from land cover where it has them.",
         "terrainMangroves('Terrain')", "plant-2", {"mangrove", "swamp", "estuary", "tidal", "wetland", "lagoon"}},
        {"terrain.addChannel", "Add Channel", "terrainAddChannel", "Carve a navigable channel sized for a boat class: along the selected curve, or click its line on the terrain.",
         "terrainAddChannel('Terrain', curve='ChannelCurve', craft='goFast', braids=3)", "git-branch", {"channel", "canal", "waterway", "lagoon", "estuary", "braided", "navigable", "draw channel"}, true},
        {"terrain.addReservoir", "Reservoir", "terrainAddReservoir", "Click a dam's two ends across a valley: a reservoir with a spillway and the river below.",
         "terrainAddReservoir('Terrain', a=[-8000, 12000], b=[6000, 15000])", "wall", {"dam", "reservoir", "spillway", "lake", "presa", "draw dam"}},
        {"terrain.routes", "Water Routes", "terrainRoutes", "Boat lanes along the coast, channels and rivers of the selected terrain.",
         "terrainRoutes('Terrain', coastDepth=800)", "route", {"boat lanes", "routes", "zonegraph", "navigation", "shipping lane", "sea lanes"}},
        {"terrain.addLane", "Harbour Lane", "terrainAddLane", "A harbour approach from a berth out to the boat lanes, or a drawn lane.",
         "terrainAddLane('Terrain', at=[4200, -1800], craft='cutter')", "anchor", {"harbour", "harbor", "port", "marina", "dock", "approach", "lane"}, true},
        {"terrain.addAirstrip", "Airstrip", "terrainAddAirstrip", "Click a runway's two ends on the terrain: an airstrip with approach and departure corridors.",
         "terrainAddAirstrip('Terrain', a=[30000, 20000], b=[30000, 70000])", "plane", {"airstrip", "runway", "airfield", "landing strip", "air lanes", "flight path", "draw runway"}},
        {"terrain.stashSpots", "Stash Spots", "terrainStashSpots", "Hiding places along the selected terrain's channels, as locators.",
         "terrainStashSpots('Terrain')", "map-pin", {"stash", "hideout", "hiding spot", "smuggling", "cache"}},
        {"terrain.routeCurves", "Route Curves", "terrainRouteCurves", "The selected terrain's routes as curves with craft attributes.",
         "terrainRouteCurves('Terrain')", "vector-spline", {"splines", "lanes", "curves", "routes"}},
        {"terrain.waterCheck", "Water Check", "waterCheck", "Channels and lanes too shallow or narrow for their craft, lanes over land or cut off, low air corridors.",
         "waterCheck('Terrain')", "checklist", {"water check", "validate", "navigable", "clearance"}},
        {"terrain.coastOverlay", "Coast Overlay", "terrainCoastOverlay", "Draw the selected terrain's lanes, depth contours and stash spots over the viewport.",
         "terrainCoastOverlay('Terrain', show='all')", "eye", {"overlay", "depth contours", "lanes", "bathymetry view"}},
        {"terrain.exportRoutes", "Export Routes", "terrainExportRoutes", "Write the selected terrain's channels, stash spots, dams and routes to JSON.",
         "terrainExportRoutes('Terrain')", "file-export", {"export", "routes", "json", "zonegraph"}},
        {"terrain.sendRoutes", "Send Routes to Unreal", "terrainSendRoutesToUnreal", "Send the selected terrain's channels and routes to the connected Unreal editor.",
         "terrainSendRoutesToUnreal('Terrain')", "send", {"unreal", "zonegraph", "splines", "routes"}},
    };
    for (const Entry& e : entries)
    {
        const std::string cmd = e.command;
        const bool curve = e.curve;
        // Drawn features start their viewport tool; a channel with a curve selected follows the curve instead.
        const std::string tool = cmd == "terrainAddChannel" ? "terrain.drawChannel" : cmd == "terrainAddReservoir" ? "terrain.drawDam"
                                 : cmd == "terrainAddAirstrip"                     ? "terrain.drawAirstrip"
                                                                                   : std::string();
        actions::add(ctx, {.id = e.id,
                           .label = e.label,
                           .category = "Terrain",
                           .run =
                               [cmd, curve, tool] {
                                   if (!tool.empty() && (!curve || terrainAndCurve().second.empty()))
                                       viewportTools::activate(tool);
                                   else
                                       runWith(cmd, curve);
                               },
                           .doc = e.doc,
                           .python = e.python,
                           .command = cmd,
                           .call = json::object(),
                           .icon = e.icon,
                           .keywords = e.keywords});
    }
    actions::add(ctx, {.id = "terrain.bathymetryFile",
                       .label = "Bathymetry from File",
                       .category = "Terrain",
                       .run =
                           [] {
                               const QString file = fileDialog::getOpenFileName(QApplication::activeWindow(), "Bathymetry Grid", {},
                                                                                "GEBCO and elevation grids (*.tif *.tiff *.asc *.xyz);;All files (*)");
                               if (file.isEmpty())
                                   return;
                               const auto [terrain, curve] = terrainAndCurve();
                               json call = {{"file", file.toStdString()}};
                               if (!terrain.empty())
                                   call["args"] = {terrain};
                               actions::runWithOptions("terrainBathymetry", call);
                           },
                       .doc = "Lay a seabed from a GEBCO GeoTIFF tile under the selected terrain's sea.",
                       .python = "terrainBathymetry('Terrain', file='gebco_2024_n30_w120.tif')",
                       .icon = "file-import",
                       .keywords = {"gebco", "bathymetry", "seabed", "netcdf", "geotiff"}});
    registerOptionBoxes();
    registerCoastTools(ctx);

    menus::addItem(ctx, {.id = "terrain.coastMenu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Coast and Water", .after = "terrain.menu"});
    int separators = 0;
    for (const char* a : {"terrain.bathymetry", "terrain.bathymetryFile", "terrain.coastMasks", "terrain.mangroves", "", "terrain.addChannel", "terrain.addReservoir", "", "terrain.routes",
                          "terrain.addLane", "terrain.addAirstrip", "terrain.stashSpots", "terrain.routeCurves", "", "terrain.waterCheck", "terrain.coastOverlay",
                          "terrain.exportRoutes", "terrain.sendRoutes"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("terrain.coastMenu.") + a, .menu = "Level/Coast and Water", .action = a});
        else
            menus::addItem(ctx, {.id = "terrain.coastMenu.sep" + std::to_string(separators++), .menu = "Level/Coast and Water", .kind = menus::Kind::Separator});
    }

    levelToolkit::add(ctx, {.id = "terrainCoast",
                            .title = "Coast and Water",
                            .items = {{.action = "terrain.bathymetry", .icon = "ripple", .text = "Seabed", .group = "Shore"},
                                      {.action = "terrain.coastMasks", .icon = "beach", .text = "Masks", .group = "Shore"},
                                      {.action = "terrain.mangroves", .icon = "plant-2", .text = "Mangroves", .group = "Shore"},
                                      {.action = "terrain.addChannel", .icon = "git-branch", .text = "Channel", .group = "Water"},
                                      {.action = "terrain.addReservoir", .icon = "wall", .text = "Reservoir", .group = "Water"},
                                      {.action = "terrain.stashSpots", .icon = "map-pin", .text = "Stash", .group = "Water"},
                                      {.action = "terrain.routes", .icon = "route", .text = "Routes", .group = "Routes"},
                                      {.action = "terrain.addLane", .icon = "anchor", .text = "Harbour", .group = "Routes"},
                                      {.action = "terrain.addAirstrip", .icon = "plane", .text = "Airstrip", .group = "Routes"},
                                      {.action = "terrain.waterCheck", .icon = "checklist", .text = "Check"},
                                      {.action = "terrain.coastOverlay", .icon = "eye", .text = "Overlay"},
                                      {.action = "terrain.routeCurves", .icon = "vector-spline", .text = "Curves"},
                                      {.action = "terrain.exportRoutes", .icon = "file-export", .text = "Export", .rare = true},
                                      {.action = "terrain.sendRoutes", .icon = "send", .text = "Unreal", .rare = true}},
                            .order = 12,
                            .docs = "terrain-coast"});
}
} // namespace terrain
