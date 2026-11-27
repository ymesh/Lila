// The Terrain Bathymetry, Terrain Dams and Terrain Coast nodes and their commands: seabed from GEBCO, coast masks,
// mangroves, channels, reservoirs, boat and air routes, waterCheck, the terrain spec's coast step and the exports.

#include "Coast.h"
#include "Fetch.h"
#include "Http.h"
#include "LandCover.h"
#include "Materials.h"
#include "Stamps.h"
#include "TerrainEdit.h"
#include "Water.h"

#include <lila/Command.h>
#include <lila/DebugDraw.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/SpecSteps.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>

using namespace lila;

namespace terrain
{
namespace coast_cmd
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;
using namespace terrain::edit;
using Terrain = TerrainTarget;
using Edit = GroupEdit;

constexpr const char* kBathyType = "TerrainBathymetry";
constexpr const char* kBathyName = "Bathymetry";
constexpr const char* kDamsType = "TerrainDams";
constexpr const char* kDamsName = "Dams";
constexpr const char* kCoastType = "TerrainCoast";
constexpr const char* kCoastName = "Coast";
constexpr const char* kWaterType = "TerrainWaterFeatures";
constexpr const char* kPrefsFile = "terrain";
constexpr const char* kKeyPref = "/openTopography/apiKey";
/** Downloads past this need consent=True. */
constexpr double kConsentMB = 25.0;

/** Nodes that run after the coast: what stands on the finished ground and what paints it. */
const std::set<std::string>& lateTypes()
{
    static const std::set<std::string> s = {"TerrainStamps",     "TerrainRoads",    "TerrainVolume",   "TerrainBiomes",      "TerrainLayers", "TerrainGroundCover",
                                            "TerrainCliff",      "TerrainCliffWall", "TerrainCliffScatter", "TerrainCliffKit", "TerrainRegions"};
    return s;
}

const std::set<std::string>& damsAfter()
{
    static const std::set<std::string> s = [] {
        std::set<std::string> v = lateTypes();
        v.insert({kWaterType, kCoastType});
        return v;
    }();
    return s;
}

/** The bathymetry goes right after the source, before any shaping or erosion, so the seabed is sculpted and eroded with the land. */
const std::set<std::string>& bathyAfter()
{
    static const std::set<std::string> s = [] {
        std::set<std::string> v = damsAfter();
        v.insert({kDamsType, "TerrainConstraints", "TerrainSculpt", "TerrainErodePreset", "TerrainAnalyticErosion", "TerrainErosionFilter", "TerrainHydraulic",
                  "TerrainDebrisFlow", "TerrainDeposition", "TerrainSnow", "TerrainTerrace", "TerrainStrata", "TerrainFold", "TerrainWarp", "TerrainRidgeSharpen"});
        return v;
    }();
    return s;
}

gn::GNode* findOfType(gn::Group& grp, const char* type)
{
    for (gn::GNode& n : grp.nodes)
        if (n.type == type)
            return &n;
    return nullptr;
}

/** The water node's sea level (local), or NaN. */
double waterSeaLevel(gn::Group& grp)
{
    if (gn::GNode* w = findOfType(grp, kWaterType))
        return WaterSettings::fromJson(jsonProp(*w, "settings", json::object())).seaLevel;
    return std::numeric_limits<double>::quiet_NaN();
}

gn::GNode* coastNode(gn::Group& grp, bool create, const std::string& cmd)
{
    if (gn::GNode* n = findOfType(grp, kCoastType))
        return n;
    if (!create)
        return nullptr;
    gn::GNode& n = chainNode(grp, kCoastName, kCoastType, lateTypes(), 160.0, cmd);
    CoastSpec s;
    const double sea = waterSeaLevel(grp);
    s.seaLevel = std::isnan(sea) ? 0.0 : sea;
    n.values["settings"] = s.settingsJson().dump();
    return &n;
}

gn::GNode* damsNode(gn::Group& grp, bool create, const std::string& cmd)
{
    if (gn::GNode* n = findOfType(grp, kDamsType))
        return n;
    return create ? &chainNode(grp, kDamsName, kDamsType, damsAfter(), -160.0, cmd) : nullptr;
}

gn::GNode* bathyNode(gn::Group& grp, bool create, const std::string& cmd)
{
    if (gn::GNode* n = findOfType(grp, kBathyType))
        return n;
    return create ? &chainNode(grp, kBathyName, kBathyType, bathyAfter(), -160.0, cmd) : nullptr;
}

WorldMap worldMapOf(const Terrain& t)
{
    WorldMap w;
    w.m = t.m;
    w.up = upVector();
    w.across = t.across();
    w.vertical = t.vertical();
    return w;
}

std::string pathOf(const Terrain& t) { return t.path(t.modifier->path()); }

std::string label(const Terrain& t)
{
    std::string s = t.mesh ? t.mesh->name() : std::string("Terrain");
    for (char& ch : s)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
            ch = '_';
    return s;
}

fs::path dataDir()
{
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain";
    const fs::path& sceneFile = io::currentPath();
    if (!sceneFile.empty())
        return sceneFile.parent_path() / (sceneFile.stem().string() + "_terrain");
    return fs::temp_directory_path() / "lila_terrain";
}

fs::path exportDir()
{
    const fs::path& scene = io::currentPath();
    if (!scene.empty())
        return scene.parent_path() / (scene.stem().string() + "_terrain") / "export";
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain" / "export";
    return fs::temp_directory_path() / "lila_terrain" / "export";
}

fs::path resolved(const std::string& f)
{
    fs::path p = paths::fromUtf8(f);
    if (p.is_relative())
        if (const fs::path& root = Workspace::get().root(); !root.empty())
            p = root / p;
    return p;
}

json parsedProp(gn::ExecContext& c, const char* id, json fallback)
{
    const std::string text = c.prop(id);
    if (text.empty())
        return fallback;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded())
        throw gn::ExecError(c.node.name + ": " + id + " is not JSON");
    return j;
}

// ---------------------------------------------------------------------------------------------------- grid cache

struct GridKey
{
    std::string path;
    int64_t stamp = 0;
    std::array<int, 4> window{};
    auto operator<=>(const GridKey&) const = default;
};

/** The part of a bathymetry grid over the heightfield, read once per file version and window. */
std::shared_ptr<const Dem> loadGrid(const std::string& file, const Heightfield& h, const Georef& g, std::string* why)
{
    static std::mutex mutex;
    static std::map<GridKey, std::shared_ptr<const Dem>> cache;
    const fs::path p = paths::fromUtf8(file);
    std::error_code ec;
    if (!fs::exists(p, ec))
    {
        *why = "no file " + file;
        return nullptr;
    }
    GridKey key{file, int64_t(fs::last_write_time(p, ec).time_since_epoch().count()), {}};
    try
    {
        const std::string ext = p.extension().string();
        const bool tiff = ext == ".tif" || ext == ".tiff" || ext == ".TIF" || ext == ".TIFF";
        if (tiff)
        {
            GeoTiffWindowReader reader(fileByteRange(p), p.filename().string());
            key.window = rasterWindow(reader.info(), h, g);
            {
                std::lock_guard lock(mutex);
                if (auto it = cache.find(key); it != cache.end())
                    return it->second;
            }
            if (key.window[0] >= key.window[2] || key.window[1] >= key.window[3])
            {
                *why = p.filename().string() + " doesn't cover this terrain";
                return nullptr;
            }
            auto d = std::make_shared<Dem>(reader.window(key.window[0], key.window[1], key.window[2], key.window[3]));
            std::lock_guard lock(mutex);
            if (cache.size() > 8)
                cache.clear();
            cache[key] = d;
            return d;
        }
        {
            std::lock_guard lock(mutex);
            if (auto it = cache.find(key); it != cache.end())
                return it->second;
        }
        auto d = std::make_shared<Dem>(readDem(p));
        std::lock_guard lock(mutex);
        if (cache.size() > 8)
            cache.clear();
        cache[key] = d;
        return d;
    }
    catch (const std::exception& e)
    {
        *why = e.what();
        return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------- nodes

gn::NodeDecl bathyDecl()
{
    gn::NodeDecl d;
    d.type = kBathyType;
    d.label = "Terrain Bathymetry";
    d.category = "Terrain";
    d.doc = "A seabed under the flat sea a land DEM leaves: a GEBCO grid (GeoTIFF or any elevation file) placed by the terrain's "
            "georeference and blended in from a shoreline profile, so the shore stays continuous and the shelf smooth; without a "
            "grid, the profile alone (shelf, then the drop to deep water); a grid it can't place leaves the seabed as it was. Writes mask seabed. terrainBathymetry sets it; it "
            "goes right after the source.";
    d.props = {{.id = "settings", .defaultValue = "{}", .doc = "The bathymetry settings, a JSON object (see terrainBathymetry)."}};
    d.sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        gn::GeometryPtr g = c.geometry("heightfield");
        const Heightfield* in = heightfieldOf(*g);
        if (!in)
            throw gn::ExecError(c.node.name + ": the geometry has no heightfield");
        const BathymetrySpec s = BathymetrySpec::fromJson(parsedProp(c, "settings", json::object()));
        Heightfield h = *in;
        BathymetryResult r;
        Georef geo;
        const bool hasGeo = publishedGeoref(c.env.owner, &geo);
        // A real grid that can't be placed leaves the seabed as it is: a profile in its place would pass for real depths.
        std::string unused;
        std::shared_ptr<const Dem> grid;
        if (!s.file.empty() && !hasGeo)
            unused = "the terrain has no georeference (make it with terrainImportDem), so " + s.file + " can't be placed";
        else if (!s.file.empty() && !(grid = loadGrid(s.file, h, geo, &unused)))
            unused = "bathymetry grid " + s.file + " not used (" + unused + ")";
        if (!unused.empty())
        {
            r.source = "none";
            r.notes.push_back(unused + "; the seabed is left as it was");
        }
        else
            r = applyBathymetry(h, s, grid.get(), grid ? &geo : nullptr);
        for (const std::string& n : r.notes)
            log::warn("{}: {}", c.node.name, n);
        publishBathymetry(c.env.owner, c.node.name, std::move(r));
        gn::Geometry out = *g;
        out.heightfield = std::make_shared<const Heightfield>(std::move(h));
        c.out("heightfield", gn::makeGeometry(std::move(out)));
    };
    return d;
}

gn::NodeDecl damsDecl()
{
    gn::NodeDecl d;
    d.type = kDamsType;
    d.label = "Terrain Dams";
    d.category = "Terrain";
    d.doc = "Dams across valleys: a wall at crest height with sloped faces and a spillway notch at its sill, before the water so "
            "the reservoir fills behind it. Writes mask dam. terrainAddReservoir adds them with their lake and the river below.";
    d.props = {{.id = "dams", .defaultValue = "[]", .doc = "The dams, a JSON list in the heightfield's own space (see terrainAddReservoir)."}};
    d.sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        gn::GeometryPtr g = c.geometry("heightfield");
        const Heightfield* in = heightfieldOf(*g);
        if (!in)
            throw gn::ExecError(c.node.name + ": the geometry has no heightfield");
        std::vector<DamSpec> dams;
        for (const json& j : parsedProp(c, "dams", json::array()))
            dams.push_back(DamSpec::fromJson(j));
        Heightfield h = *in;
        publishDams(c.env.owner, c.node.name, applyDams(h, dams));
        gn::Geometry out = *g;
        out.heightfield = std::make_shared<const Heightfield>(std::move(h));
        c.out("heightfield", gn::makeGeometry(std::move(out)));
    };
    return d;
}

gn::NodeDecl coastDecl()
{
    gn::NodeDecl d;
    d.type = kCoastType;
    d.label = "Terrain Coast";
    d.category = "Terrain";
    d.doc = "The coast after the water: navigable channels carved for a craft class (braided, banks settled), beach, surf, "
            "shallows and reef masks, mangroves in the tidal band, stash-spot candidates, and the boat and air route graph "
            "(coast lanes on a depth contour, channels, river reaches, harbour approaches, airstrip corridors). Writes masks "
            "beach, surf, shallows, reef, tidal, mangrove and channel. Put it after Terrain Water Features.";
    d.props = {{.id = "settings", .defaultValue = "{}", .doc = "Sea level, masks, mangroves, stash and route settings, a JSON object."},
               {.id = "channels", .defaultValue = "[]", .doc = "Channels, a JSON list (see terrainAddChannel)."},
               {.id = "lanes", .defaultValue = "[]", .doc = "Drawn lanes and harbour approaches, a JSON list (see terrainAddLane)."},
               {.id = "airstrips", .defaultValue = "[]", .doc = "Airstrips, a JSON list (see terrainAddAirstrip)."}};
    d.sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        gn::GeometryPtr g = c.geometry("heightfield");
        const Heightfield* in = heightfieldOf(*g);
        if (!in)
            throw gn::ExecError(c.node.name + ": the geometry has no heightfield");
        CoastSpec spec;
        try
        {
            spec.readSettings(parsedProp(c, "settings", json::object()));
            for (const json& j : parsedProp(c, "channels", json::array()))
                spec.channels.push_back(ChannelSpec::fromJson(j));
            for (const json& j : parsedProp(c, "lanes", json::array()))
                spec.lanes.push_back(LaneSpec::fromJson(j));
            for (const json& j : parsedProp(c, "airstrips", json::array()))
                spec.airstrips.push_back(AirstripSpec::fromJson(j));
        }
        catch (const gn::ExecError&)
        {
            throw;
        }
        catch (const std::exception& e)
        {
            throw gn::ExecError(c.node.name + ": " + e.what());
        }
        Heightfield h = *in;
        WaterResult water;
        const bool hasWater = publishedWater(c.env.owner, "Water", &water);
        publishCoast(c.env.owner, c.node.name, applyCoast(h, spec, hasWater ? &water : nullptr));
        gn::Geometry out = *g;
        out.heightfield = std::make_shared<const Heightfield>(std::move(h));
        c.out("heightfield", gn::makeGeometry(std::move(out)));
    };
    return d;
}

// ---------------------------------------------------------------------------------------------------- reading

struct Current
{
    CoastSpec spec;
    CoastResult coast;
    bool hasCoast = false;
    std::vector<DamSpec> damSpecs;
    std::vector<DamResult> dams;
    bool hasBathy = false;
    BathymetrySpec bathy;
    BathymetryResult bathyResult;
};

Current currentCoast(Graph& g, const Terrain& t)
{
    Current c;
    Node* node = t.modifier ? gn::groupOfModifier(g, *t.modifier) : nullptr;
    if (!node)
        return c;
    gn::Group grp;
    try
    {
        grp = gn::readGroup(*node);
    }
    catch (const std::exception&)
    {
        return c;
    }
    if (gn::GNode* n = coastNode(grp, false, ""))
    {
        c.spec.readSettings(jsonProp(*n, "settings", json::object()));
        for (const json& j : jsonProp(*n, "channels", json::array()))
            c.spec.channels.push_back(ChannelSpec::fromJson(j));
        for (const json& j : jsonProp(*n, "lanes", json::array()))
            c.spec.lanes.push_back(LaneSpec::fromJson(j));
        for (const json& j : jsonProp(*n, "airstrips", json::array()))
            c.spec.airstrips.push_back(AirstripSpec::fromJson(j));
        c.hasCoast = publishedCoast(t.modifier->id(), n->name, &c.coast);
    }
    if (gn::GNode* n = damsNode(grp, false, ""))
    {
        for (const json& j : jsonProp(*n, "dams", json::array()))
            c.damSpecs.push_back(DamSpec::fromJson(j));
        publishedDams(t.modifier->id(), n->name, &c.dams);
    }
    if (gn::GNode* n = bathyNode(grp, false, ""))
    {
        c.bathy = BathymetrySpec::fromJson(jsonProp(*n, "settings", json::object()));
        c.hasBathy = publishedBathymetry(t.modifier->id(), n->name, &c.bathyResult);
    }
    return c;
}

/** Issue positions from the heightfield's space to world space. */
json worldIssues(const Terrain& t, json issues)
{
    const WorldMap w = worldMapOf(t);
    for (json& i : issues)
        if (i.contains("position") && i["position"].is_array())
        {
            const json& p = i["position"];
            i["position"] = w.point(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
        }
    return issues;
}

json stats(const Terrain& t, const Current& c)
{
    const WorldMap w = worldMapOf(t);
    const double a = t.across(), v = t.vertical();
    json out = json::object();
    const glm::dvec2 mid = t.centre();
    out["seaLevel"] = w.height(mid.x, c.spec.seaLevel, mid.y);
    if (c.hasBathy)
        out["bathymetry"] = {{"source", c.bathyResult.source},
                             {"file", c.bathy.file},
                             {"seaSamples", c.bathyResult.seaSamples},
                             {"gridSamples", c.bathyResult.gridSamples},
                             {"maxDepth", c.bathyResult.maxDepth * v},
                             {"meanDepth", c.bathyResult.meanDepth * v},
                             {"notes", c.bathyResult.notes}};
    const CoastResult& r = c.coast;
    out["masks"] = {{"beachM2", r.beachArea * a * a}, {"surfM2", r.surfArea * a * a}, {"shallowsM2", r.shallowArea * a * a},
                    {"reefM2", r.reefArea * a * a},   {"mangroveM2", r.mangroveArea * a * a}};
    out["mangroveSource"] = r.mangroveSource;
    json channels = json::array();
    for (const ChannelResult& ch : r.channels)
    {
        const CraftClass* craft = craftClass(ch.craft);
        json strands = json::array();
        bool meets = true;
        for (const ChannelStrand& s : ch.strands)
        {
            double len = 0.0;
            for (size_t k = 1; k < s.points.size(); ++k)
                len += glm::length(s.points[k] - s.points[k - 1]);
            strands.push_back({{"minWidth", s.minWidth * a}, {"minDepth", s.minDepth * v}, {"length", len * a}, {"points", s.points.size()}});
            meets = meets && craft && s.minWidth >= craft->minWidth && s.minDepth >= craft->minDepth;
        }
        channels.push_back({{"id", ch.id}, {"craft", ch.craft}, {"width", ch.width * a}, {"depth", ch.depth * v},
                            {"waterLevel", w.height(mid.x, ch.waterLevel, mid.y)}, {"cutM3", ch.cut * a * a * v * 1e-6}, {"strands", strands},
                            {"meetsCraft", meets}});
    }
    out["channels"] = channels;
    json dams = json::array();
    for (const DamResult& d : c.dams)
        dams.push_back({{"id", d.id}, {"crest", w.height(d.spillway.x, d.crest, d.spillway.y)}, {"sill", w.height(d.spillway.x, d.sill, d.spillway.y)},
                        {"height", d.height * v}, {"length", d.length * a}, {"fillM3", d.fill * a * a * v * 1e-6},
                        {"spillway", w.point(d.spillway.x, d.sill, d.spillway.y)}});
    out["dams"] = dams;
    std::map<std::string, int> byKind, stashKinds;
    int mainLanes = 0;
    for (const Lane& l : r.lanes)
    {
        ++byKind[l.kind];
        mainLanes += !l.air && l.component == r.mainComponent;
    }
    for (const StashSpot& s : r.stash)
        ++stashKinds[s.kind];
    out["lanes"] = {{"count", r.lanes.size()}, {"byKind", byKind}, {"nodes", r.nodes.size()}, {"mainNetwork", mainLanes}, {"runways", r.runways.size()}};
    out["stash"] = {{"count", r.stash.size()}, {"byKind", stashKinds}};
    out["issues"] = c.hasCoast ? routeIssues(*t.local, r).size() : 0;
    out["warnings"] = r.warnings;
    out["notes"] = r.notes;
    if (c.hasBathy)
        for (const std::string& n : c.bathyResult.notes)
            out["notes"].push_back(n);
    out["revision"] = revision(*t.local);
    return out;
}

json finish(Edit& e, const Terrain& t, const std::string& cmd)
{
    commitGroup(e, cmd);
    Graph& g = Graph::current();
    const Terrain after = findTerrain(g, pathOf(t), cmd);
    const Current c = currentCoast(g, after);
    for (const std::string& warning : c.coast.warnings)
        log::warn("{}: {}", cmd, warning);
    return stats(after, c);
}

/** A spec item placed off the terrain: its command refuses it, and terrainSpec's coast step skips it with a warning. */
struct OffMapError : CommandError
{
    using CommandError::CommandError;
};

/** "<what> <id>: <part> at x, z lies outside the map (x a to b, z c to d)" for the first point off t; empty when all are on it. */
std::string offMap(const Terrain& t, const std::string& what, const std::string& id, const std::vector<std::pair<std::string, glm::dvec2>>& points)
{
    const Heightfield& h = *t.local;
    for (const auto& [part, p] : points)
        if (!h.contains(p.x, p.y))
        {
            const glm::dvec3 at = t.world(p.x, 0.0, p.y), lo = t.world(h.origin.x, 0.0, h.origin.y),
                             hi = t.world(h.origin.x + h.extent().x, 0.0, h.origin.y + h.extent().y);
            return std::format("{} {}: {} at {:.0f}, {:.0f} lies outside the map (x {:.0f} to {:.0f}, z {:.0f} to {:.0f})", what, id, part, at.x, at.z,
                               std::min(lo.x, hi.x), std::max(lo.x, hi.x), std::min(lo.z, hi.z), std::max(lo.z, hi.z));
        }
    return {};
}

std::vector<std::pair<std::string, glm::dvec2>> pathPoints(const std::vector<glm::dvec2>& path)
{
    std::vector<std::pair<std::string, glm::dvec2>> out;
    for (size_t i = 0; i < path.size(); ++i)
        out.emplace_back("point " + std::to_string(i), path[i]);
    return out;
}

std::vector<glm::dvec2> localLine(const Terrain& t, const json& list, const std::string& cmd)
{
    if (!list.is_array())
        throw CommandError(cmd + ": expected a list of points");
    std::vector<glm::dvec2> out;
    for (const json& p : list)
        out.push_back(t.local2(worldPoint(p, cmd)));
    return out;
}

std::string freshId(const json& list, const std::string& base)
{
    for (int k = 1;; ++k)
    {
        const std::string candidate = base + std::to_string(k);
        if (std::none_of(list.begin(), list.end(), [&](const json& e) { return e.value("id", std::string()) == candidate; }))
            return candidate;
    }
}

bool upsert(json& list, const json& entry)
{
    for (json& e : list)
        if (e.value("id", std::string()) == entry["id"].get<std::string>())
        {
            e = entry;
            return true;
        }
    list.push_back(entry);
    return false;
}

std::string boatCraft(const Args& a, const char* flag, const char* fallback, const std::string& cmd)
{
    const std::string name = a.get<std::string>(flag, fallback);
    const CraftClass* c = craftClass(name);
    if (!c || c->kind != "boat")
    {
        std::string have;
        for (const CraftClass& k : craftClasses())
            if (k.kind == "boat")
                have += (have.empty() ? "" : ", ") + k.name;
        throw CommandError(cmd + ": craft is one of " + have + ", not " + name);
    }
    return name;
}

/** A curve's points in world space, through curveData. */
json curvePoints(const std::string& curve, const std::string& cmd)
{
    const json splines = ops::execute("curveData", {{"args", {curve}}, {"worldSpace", true}});
    if (!splines.is_array() || splines.empty() || !splines[0].contains("points"))
        throw CommandError(cmd + ": " + curve + " has no points");
    return splines[0]["points"];
}

// ---------------------------------------------------------------------------------------------------- bathymetry

GeoBox terrainBox(const Terrain& t, const std::string& cmd)
{
    Georef g;
    if (!t.modifier || !publishedGeoref(t.modifier->id(), &g) || !g.geographicOk())
        throw CommandError(cmd + ": the terrain has no georeference; give bbox=[west, south, east, north] or make it with terrainImportDem");
    const Heightfield& h = *t.local;
    GeoBox b{1e9, 1e9, -1e9, -1e9};
    for (glm::dvec2 c : {h.origin, h.origin + glm::dvec2(h.extent().x, 0.0), h.origin + glm::dvec2(0.0, h.extent().y), h.origin + h.extent()})
    {
        const LatLon ll = g.toGeographic(c);
        b.west = std::min(b.west, ll.lon);
        b.east = std::max(b.east, ll.lon);
        b.south = std::min(b.south, ll.lat);
        b.north = std::max(b.north, ll.lat);
    }
    // A margin of two grid cells so the blur near the edge has data.
    const double m = 2.0 / kGebcoPerDegree;
    return {b.west - m, b.south - m, b.east + m, b.north + m};
}

json estimateJson(const GeoBox& b)
{
    const uint64_t bytes = gebcoEstimateBytes(b.west, b.south, b.east, b.north);
    return {{"bbox", {b.west, b.south, b.east, b.north}},
            {"samples", {int((b.east - b.west) * kGebcoPerDegree) + 1, int((b.north - b.south) * kGebcoPerDegree) + 1}},
            {"bytes", bytes},
            {"megabytes", double(bytes) / 1048576.0}};
}

json runBathymetry(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainBathymetry";
    Graph& g = Graph::current();
    const bool estimate = a.get<bool>("estimate", false), fetch = a.get<bool>("fetch", false);
    // An area alone is enough to estimate or download: no terrain has to exist yet.
    const bool areaOnly = (estimate || fetch) && a.has("bbox") && a.positional.empty();
    std::optional<Terrain> found;
    if (!areaOnly || (fetch && !estimate))
    {
        try
        {
            found = oneTerrain(a, cmd);
        }
        catch (const CommandError&)
        {
            if (!areaOnly)
                throw;
        }
    }
    json fetched = nullptr;
    fs::path attributionFile;

    if (a.get<bool>("off", false))
    {
        if (!found)
            throw CommandError(cmd + ": off needs a terrain");
        const Terrain& t = *found;
        Edit e = readGroup(g, t, cmd);
        gn::GNode* n = bathyNode(e.group, false, cmd);
        if (!n)
            throw CommandError(cmd + ": the terrain has no bathymetry");
        const std::string name = n->name;
        if (const gn::GLink* in = linkInto(e.group, name, "heightfield"))
        {
            const std::string fromNode = in->fromNode, fromSocket = in->fromSocket;
            for (gn::GLink& l : e.group.links)
                if (l.fromNode == name)
                {
                    l.fromNode = fromNode;
                    l.fromSocket = fromSocket;
                }
        }
        std::erase_if(e.group.links, [&](const gn::GLink& l) { return l.toNode == name; });
        std::erase_if(e.group.nodes, [&](const gn::GNode& x) { return x.name == name; });
        json out = finish(e, t, cmd);
        out["removed"] = true;
        return out;
    }

    std::string file = a.get<std::string>("file", "");
    if (estimate || fetch)
    {
        GeoBox box;
        if (a.has("bbox"))
        {
            const json b = a.flags["bbox"];
            if (!b.is_array() || b.size() != 4)
                throw CommandError(cmd + ": bbox is [west, south, east, north] in degrees");
            box = {b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
        }
        else
            box = terrainBox(*found, cmd);
        if (!box.valid())
            throw CommandError(cmd + ": the area is not a valid box in degrees");
        FetchRequest r;
        r.source = DemSource::OpenTopography;
        r.box = box;
        r.dataset = a.get<std::string>("dataset", "GEBCOIceTopo");
        r.cache = dataDir() / "dem_cache";
        r.offline = a.get<bool>("offline", false);
        json est = estimateJson(box);
        const bool cached = fs::exists(openTopographyCacheFile(r));
        est["cached"] = cached;
        if (cached)
        {
            est["bytes"] = 0;
            est["megabytes"] = 0.0;
        }
        if (estimate)
            return {{"estimate", est}, {"dataset", r.dataset}, {"consentNeeded", est["megabytes"].get<double>() > kConsentMB}};
        if (est["megabytes"].get<double>() > kConsentMB && !a.get<bool>("consent", false))
            throw CommandError(std::format("{}: the GEBCO download for this area is about {:.0f} MB; pass consent=True to fetch it", cmd, est["megabytes"].get<double>()));
        std::string keyNote;
        try
        {
            r.apiKey = chooseApiKey(a.get<std::string>("apiKey", ""), PrefStore::get().value<std::string>(kPrefsFile, kKeyPref, std::string()), &keyNote);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what());
        }
        if (a.has("apiKey") && !r.apiKey.empty() && a.get<bool>("saveKey", false))
            PrefStore::get().set(kPrefsFile, kKeyPref, r.apiKey);
        FetchResult res;
        try
        {
            res = fetchDem(r, [](const std::string& url, const fs::path& dest, std::string* why) { return httpGet(url, dest, why); });
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what() + (r.apiKey.empty() && !keyNote.empty() ? "; " + keyNote : std::string()));
        }
        // file= with fetch names where the downloaded grid goes.
        const fs::path out = !file.empty() ? resolved(file)
                                           : dataDir() / "bathymetry" / std::format("{}_{:.4f}_{:.4f}_{:.4f}_{:.4f}.tif", r.dataset, box.west, box.south, box.east, box.north);
        try
        {
            writeGeoTiff(out, res.dem);
            attributionFile = writeAttribution(dataDir(), "GEBCO bathymetry", std::string(gebcoAttribution()) + "\n" + res.attribution);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what());
        }
        file = paths::toUtf8(out);
        fetched = {{"dataset", r.dataset}, {"downloaded", res.downloaded}, {"cached", res.cached}, {"samples", {res.dem.width, res.dem.height}},
                   {"estimate", est}, {"file", file}, {"notes", res.notes}};
        if (!found)
            return {{"fetched", fetched}, {"applied", false}, {"attributionFile", paths::toUtf8(attributionFile)},
                    {"notes", json::array({"no terrain: the grid is written to " + file + ", not applied; terrainBathymetry(<terrain>, file=...) applies it"})}};
    }
    else if (!file.empty())
    {
        const fs::path p = resolved(file);
        if (!fs::exists(p))
            throw CommandError(cmd + ": no file " + p.string());
        file = paths::toUtf8(p);
        if (a.get<bool>("credit", true))
            try
            {
                attributionFile = writeAttribution(dataDir(), "GEBCO bathymetry", gebcoAttribution());
            }
            catch (const std::exception& e)
            {
                log::warn("{}: {}", cmd, e.what());
            }
    }

    const Terrain& t = *found;
    const double ac = t.across(), v = t.vertical();
    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = bathyNode(e.group, true, cmd);
    BathymetrySpec s = BathymetrySpec::fromJson(jsonProp(*n, "settings", json::object()));
    if (a.has("file") || !fetched.is_null())
        s.file = file;
    if (a.get<bool>("profileOnly", false))
        s.file.clear();
    if (a.has("seaLevel"))
        s.seaLevel = t.localHeight(a.get<double>("seaLevel", 0.0), t.centre());
    else if (const double sea = waterSeaLevel(e.group); !std::isnan(sea))
        s.seaLevel = sea;
    auto len = [&](const char* f, double& field) {
        if (a.has(f))
            field = std::max(0.0, a.get<double>(f, 0.0)) / ac;
    };
    auto dep = [&](const char* f, double& field) {
        if (a.has(f))
            field = std::max(0.0, a.get<double>(f, 0.0)) / v;
    };
    dep("landTolerance", s.landTolerance);
    len("blendWidth", s.blendWidth);
    len("smoothing", s.smoothing);
    dep("shelfDepth", s.shelfDepth);
    len("shelfWidth", s.shelfWidth);
    dep("dropDepth", s.dropDepth);
    len("slopeWidth", s.slopeWidth);
    dep("minDepth", s.minDepth);
    if (a.has("shoreSlope"))
        s.shoreSlope = std::clamp(a.get<double>("shoreSlope", 1.5), 0.05, 60.0);
    if (a.has("depthScale"))
        s.depthScale = std::max(0.0, a.get<double>("depthScale", 1.0));
    s = BathymetrySpec::fromJson(s.toJson());
    n->values["settings"] = s.toJson().dump();
    json out = finish(e, t, cmd);
    if (!fetched.is_null())
        out["fetched"] = fetched;
    if (!attributionFile.empty())
        out["attributionFile"] = paths::toUtf8(attributionFile);
    return out;
}

// ---------------------------------------------------------------------------------------------------- settings commands

json runCoastMasks(const Args& a)
{
    const std::string cmd = "terrainCoastMasks";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = coastNode(e.group, true, cmd);
    CoastSpec s;
    s.readSettings(jsonProp(*n, "settings", json::object()));
    if (a.has("seaLevel"))
        s.seaLevel = t.localHeight(a.get<double>("seaLevel", 0.0), t.centre());
    CoastMasks& m = s.masks;
    m.enabled = !a.get<bool>("off", false);
    auto dep = [&](const char* f, double& field) {
        if (a.has(f))
            field = a.get<double>(f, 0.0) / v;
    };
    auto len = [&](const char* f, double& field) {
        if (a.has(f))
            field = a.get<double>(f, 0.0) / ac;
    };
    if (a.has("beachSlope"))
        m.beachSlope = a.get<double>("beachSlope", 10.0);
    dep("beachHeight", m.beachHeight);
    dep("beachBelow", m.beachBelow);
    dep("surfDepth", m.surfDepth);
    len("surfWidth", m.surfWidth);
    dep("shallowDepth", m.shallowDepth);
    dep("reefMinDepth", m.reefMinDepth);
    dep("reefMaxDepth", m.reefMaxDepth);
    len("reefMinDistance", m.reefMinDistance);
    len("reefMaxDistance", m.reefMaxDistance);
    if (a.has("reefCover"))
        m.reefCover = a.get<double>("reefCover", 0.35);
    len("reefScale", m.reefScale);
    if (a.has("seed"))
        m.seed = uint32_t(a.get<int64_t>("seed", 0));
    CoastSpec clean;
    clean.readSettings(s.settingsJson());
    n->values["settings"] = clean.settingsJson().dump();
    return finish(e, t, cmd);
}

/** A biomes spec with the default biome list filled in. */
json mergeDefaults(json spec)
{
    const json d = defaultBiomes().toJson();
    if (!spec.is_object())
        spec = json::object();
    spec["biomes"] = d["biomes"];
    return spec;
}

/** Adds the mangrove biome to the terrain's biomes, painted by the mangrove mask; true when it changed them. */
bool addMangroveBiome(Edit& e)
{
    gn::GNode* b = findOfType(e.group, "TerrainBiomes");
    if (!b)
        return false;
    json spec = jsonProp(*b, "biomes", json::object());
    if (!spec.contains("biomes"))
        spec = mergeDefaults(spec);
    for (const json& x : spec["biomes"])
        if (x.value("name", std::string()) == "mangrove")
            return false;
    spec["biomes"].push_back({{"name", "mangrove"}, {"when", {{"mangrove", {0.5, 1.0}}}}, {"paint", "mangrove"}});
    b->values["biomes"] = spec.dump();
    return true;
}

json runMangroves(const Args& a)
{
    const std::string cmd = "terrainMangroves";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = coastNode(e.group, true, cmd);
    CoastSpec s;
    s.readSettings(jsonProp(*n, "settings", json::object()));
    MangroveSettings& m = s.mangroves;
    m.enabled = !a.get<bool>("off", false);
    if (a.has("tidalLow"))
        m.tidalLow = a.get<double>("tidalLow", -80.0) / v;
    if (a.has("tidalHigh"))
        m.tidalHigh = a.get<double>("tidalHigh", 150.0) / v;
    if (a.has("maxSlope"))
        m.maxSlope = a.get<double>("maxSlope", 6.0);
    if (a.has("landcover"))
        m.landcover = a.get<std::string>("landcover", "landcover");
    if (a.has("code"))
        m.code = int(a.get<int64_t>("code", 95));
    if (a.has("fromWetland"))
        m.fromWetland = a.get<bool>("fromWetland", true);
    if (a.has("fromBand"))
        m.fromBand = a.get<bool>("fromBand", true);
    if (a.has("shelterRadius"))
        m.shelterRadius = a.get<double>("shelterRadius", 20000.0) / ac;
    if (a.has("shelterMax"))
        m.shelterMax = a.get<double>("shelterMax", 0.45);
    CoastSpec clean;
    clean.readSettings(s.settingsJson());
    n->values["settings"] = clean.settingsJson().dump();
    const bool biome = m.enabled && a.get<bool>("biome", true) && addMangroveBiome(e);
    json out = finish(e, t, cmd);
    out["biomeAdded"] = biome;
    return out;
}

json runRoutes(const Args& a)
{
    const std::string cmd = "terrainRoutes";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = coastNode(e.group, true, cmd);
    CoastSpec s;
    s.readSettings(jsonProp(*n, "settings", json::object()));
    RouteSettings& r = s.routes;
    if (a.has("coastLanes"))
        r.coastLanes = a.get<bool>("coastLanes", true);
    if (a.has("coastDepth"))
        r.coastDepth = a.get<double>("coastDepth", 800.0) / v;
    if (a.has("coastCraft"))
        r.coastCraft = boatCraft(a, "coastCraft", "goFast", cmd);
    if (a.has("minLaneLength"))
        r.minLaneLength = a.get<double>("minLaneLength", 20000.0) / ac;
    if (a.has("riverLanes"))
        r.riverLanes = a.get<bool>("riverLanes", true);
    if (a.has("channelLanes"))
        r.channelLanes = a.get<bool>("channelLanes", true);
    if (a.has("snap"))
        r.snap = a.get<double>("snap", 3000.0) / ac;
    if (a.has("linkReach"))
        r.linkReach = a.get<double>("linkReach", 300000.0) / ac;
    if (a.has("spacing"))
        r.spacing = a.get<double>("spacing", 2000.0) / ac;
    CoastSpec clean;
    clean.readSettings(s.settingsJson());
    n->values["settings"] = clean.settingsJson().dump();
    return finish(e, t, cmd);
}

// ---------------------------------------------------------------------------------------------------- features

json runAddChannel(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainAddChannel";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    json pathList;
    if (a.has("path"))
        pathList = a.flags["path"];
    else if (a.has("curve"))
        pathList = curvePoints(a.get<std::string>("curve", ""), cmd);
    else
        throw CommandError(cmd + ": give path=[[x, z], ...] or curve= (a curve the channel follows)");
    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = coastNode(e.group, true, cmd);
    json list = jsonProp(*n, "channels", json::array());
    ChannelSpec s;
    s.id = a.get<std::string>("id", "");
    if (s.id.empty())
        s.id = freshId(list, "channel");
    s.path = localLine(t, pathList, cmd);
    if (s.path.size() < 2)
        throw CommandError(cmd + ": the path needs at least two points");
    if (const std::string off = offMap(t, "channel", s.id, pathPoints(s.path)); !off.empty())
        throw OffMapError(cmd + ": " + off);
    s.craft = boatCraft(a, "craft", "skiff", cmd);
    s.braids = int(std::clamp<int64_t>(a.get<int64_t>("braids", 1), 1, 8));
    s.braidSpread = std::max(0.0, a.get<double>("braidSpread", 0.0)) / ac;
    s.width = std::max(0.0, a.get<double>("width", 0.0)) / ac;
    s.depth = std::max(0.0, a.get<double>("depth", 0.0)) / v;
    s.margin = std::clamp(a.get<double>("margin", 1.3), 1.0, 5.0);
    if (a.has("waterLevel"))
        s.waterLevel = t.localHeight(a.get<double>("waterLevel", 0.0), s.path.front());
    s.bankAngle = std::clamp(a.get<double>("bankAngle", 25.0), 5.0, 80.0);
    s.reErode = int(std::clamp<int64_t>(a.get<int64_t>("reErode", 4), 0, 50));
    s.meander = std::clamp(a.get<double>("meander", 0.6), 0.0, 5.0);
    s.seed = uint32_t(a.get<int64_t>("seed", 0));
    const bool replaced = upsert(list, s.toJson());
    n->values["channels"] = list.dump();
    json out = finish(e, t, cmd);
    out["id"] = s.id;
    out["replaced"] = replaced;
    return out;
}

json runAddLane(const Args& a)
{
    const std::string cmd = "terrainAddLane";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = coastNode(e.group, true, cmd);
    json list = jsonProp(*n, "lanes", json::array());
    LaneSpec s;
    s.id = a.get<std::string>("id", "");
    s.kind = a.get<std::string>("kind", "harbour");
    if (s.id.empty())
        s.id = freshId(list, s.kind == "harbour" ? "harbour" : "lane");
    s.craft = boatCraft(a, "craft", "goFast", cmd);
    if (a.has("path"))
        s.path = localLine(t, a.flags["path"], cmd);
    else if (a.has("curve"))
        s.path = localLine(t, curvePoints(a.get<std::string>("curve", ""), cmd), cmd);
    else if (a.has("at"))
        s.at = t.local2(worldPoint(a.flags["at"], cmd));
    else
        throw CommandError(cmd + ": give at= (a berth routed out to the boat lanes), path= or curve=");
    if (!s.at && s.path.size() < 2)
        throw CommandError(cmd + ": the path needs at least two points");
    const bool replaced = upsert(list, s.toJson());
    n->values["lanes"] = list.dump();
    json out = finish(e, t, cmd);
    out["id"] = s.id;
    out["replaced"] = replaced;
    return out;
}

json runAddAirstrip(const Args& a)
{
    const std::string cmd = "terrainAddAirstrip";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    if (!a.has("a") || !a.has("b"))
        throw CommandError(cmd + ": give the runway's ends a= and b=");
    AirstripSpec s;
    s.a = t.local2(worldPoint(a.flags["a"], cmd));
    s.b = t.local2(worldPoint(a.flags["b"], cmd));
    s.aircraft = a.get<std::string>("aircraft", "light");
    if (const CraftClass* c = craftClass(s.aircraft); !c || c->kind != "air")
        throw CommandError(cmd + ": aircraft is light or twin, not " + s.aircraft);
    if (a.has("elevation"))
        s.elevation = t.localHeight(a.get<double>("elevation", 0.0), (s.a + s.b) * 0.5);
    s.bothEnds = a.get<bool>("bothEnds", true);

    json padResult = nullptr;
    if (a.get<bool>("pad", true))
    {
        // The runway graded flat first, so the corridors start from the strip as built.
        const glm::dvec2 d = glm::normalize(s.b - s.a), side(-d.y, d.x);
        const double half = std::max(100.0, a.get<double>("width", 2500.0)) * 0.5 / t.across();
        json fp = json::array();
        for (glm::dvec2 p : {s.a - side * half, s.b - side * half, s.b + side * half, s.a + side * half})
            fp.push_back(vec3(t.world(p.x, t.local->heightAt(p.x, p.y), p.y)));
        json call = {{"args", {pathOf(t)}}, {"id", "airstrip_" + a.get<std::string>("id", "strip")}, {"footprint", fp}, {"falloff", 3000.0}};
        if (a.has("elevation"))
            call["height"] = a.get<double>("elevation", 0.0);
        padResult = ops::execute("terrainStampPad", call);
        if (std::isnan(s.elevation) && padResult.contains("padHeight"))
            s.elevation = t.localHeight(padResult["padHeight"].get<double>(), (s.a + s.b) * 0.5);
    }

    const Terrain t2 = findTerrain(g, pathOf(t), cmd);
    Edit e = readGroup(g, t2, cmd);
    gn::GNode* n = coastNode(e.group, true, cmd);
    json list = jsonProp(*n, "airstrips", json::array());
    s.id = a.get<std::string>("id", "");
    if (s.id.empty())
        s.id = freshId(list, "strip");
    const bool replaced = upsert(list, s.toJson());
    n->values["airstrips"] = list.dump();
    json out = finish(e, t2, cmd);
    out["id"] = s.id;
    out["replaced"] = replaced;
    if (!padResult.is_null())
        out["pad"] = {{"padHeight", padResult.value("padHeight", json())}, {"cutM3", padResult.value("cutM3", json())}, {"fillM3", padResult.value("fillM3", json())}};
    return out;
}

json runAddReservoir(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainAddReservoir";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    if (!a.has("a") || !a.has("b"))
        throw CommandError(cmd + ": give the dam's ends a= and b= (on the valley sides)");
    DamSpec s;
    s.a = t.local2(worldPoint(a.flags["a"], cmd));
    s.b = t.local2(worldPoint(a.flags["b"], cmd));
    if (const std::string off = offMap(t, "reservoir", a.get<std::string>("id", "dam"), {{"dam end a", s.a}, {"dam end b", s.b}}); !off.empty())
        throw OffMapError(cmd + ": " + off);
    if (a.has("crest"))
        s.crest = t.localHeight(a.get<double>("crest", 0.0), (s.a + s.b) * 0.5);
    s.thickness = std::max(100.0, a.get<double>("thickness", 800.0)) / ac;
    s.faceSlope = std::clamp(a.get<double>("faceSlope", 1.5), 0.1, 6.0);
    s.spillwayWidth = std::max(0.0, a.get<double>("spillwayWidth", 2000.0)) / ac;
    s.freeboard = std::max(0.0, a.get<double>("freeboard", 300.0)) / v;
    s.spillwayAt = std::clamp(a.get<double>("spillwayAt", 0.85), 0.0, 1.0);

    Edit e = readGroup(g, t, cmd);
    gn::GNode* n = damsNode(e.group, true, cmd);
    json list = jsonProp(*n, "dams", json::array());
    s.id = a.get<std::string>("id", "");
    if (s.id.empty())
        s.id = freshId(list, "dam");
    const bool replaced = upsert(list, s.toJson());
    n->values["dams"] = list.dump();
    commitGroup(e, cmd);

    Terrain now = findTerrain(g, pathOf(t), cmd);
    Current c = currentCoast(g, now);
    const auto it = std::find_if(c.dams.begin(), c.dams.end(), [&](const DamResult& d) { return d.id == s.id; });
    if (it == c.dams.end())
        throw CommandError(cmd + ": the dam didn't build");
    const DamResult dam = *it;
    const Heightfield& h = *now.local;
    json notes = json::array();
    json lake = nullptr, river = nullptr;
    const glm::dvec2 mid = (s.a + s.b) * 0.5;

    if (a.get<bool>("lake", true))
    {
        // The reservoir's seed: the lowest ground a little upstream of the dam.
        glm::dvec2 at = mid;
        double low = 1e30;
        for (double back = s.thickness + dam.height * s.faceSlope + 2.0 * h.cell; back <= std::max(dam.length, 40.0 * h.cell); back += h.cell)
            for (double f = 0.1; f <= 0.9; f += 0.1)
            {
                const glm::dvec2 p = s.a + (s.b - s.a) * f - dam.downstream * back;
                if (!h.contains(p.x, p.y))
                    continue;
                const double z = h.heightAt(p.x, p.y);
                if (z < low)
                {
                    low = z;
                    at = p;
                }
            }
        // The water's pit filling levels a basin no water holds yet to its spill height: behind a dam that is the sill, or
        // the crest when the spillway is narrower than a cell. Ground at that level is the basin to flood, so only ground
        // clearly above the crest stops the reservoir.
        if (low >= std::max(dam.sill, dam.crest) + 50.0)
            notes.push_back("the ground behind the dam is above its spillway sill, so no reservoir forms; raise crest= or move the dam downstream");
        else
            lake = ops::execute("terrainAddLake", {{"args", {pathOf(now)}},
                                                   {"id", s.id + "_reservoir"},
                                                   {"at", vec3(now.world(at.x, h.heightAt(at.x, at.y), at.y))},
                                                   {"level", worldMapOf(now).height(at.x, dam.sill, at.y)},
                                                   {"preview", a.get<bool>("preview", true)}});
    }
    if (a.get<bool>("river", true))
    {
        now = findTerrain(g, pathOf(t), cmd);
        const Heightfield& h2 = *now.local;
        const glm::dvec2 start = dam.spillway + dam.downstream * (s.thickness * 0.5 + std::max(0.0, dam.height) * s.faceSlope + 3.0 * h2.cell);
        const std::vector<glm::dvec2> path = traceDownhill(h2, start, std::max(0.0, a.get<double>("riverLength", 300000.0)) / ac);
        if (path.size() < 2)
            notes.push_back("no downhill path below the dam for the river");
        else
        {
            json pts = json::array();
            pts.push_back(vec3(now.world(dam.spillway.x, dam.sill, dam.spillway.y)));
            for (const glm::dvec2& p : path)
                pts.push_back(vec3(now.world(p.x, h2.heightAt(p.x, p.y), p.y)));
            const std::shared_ptr<const Analysis> an = analyse(now.local);
            const int cx = std::clamp(int(std::lround((dam.spillway.x - h2.origin.x) / h2.cell)), 0, h2.nx - 1);
            const int cz = std::clamp(int(std::lround((dam.spillway.y - h2.origin.y) / h2.cell)), 0, h2.nz - 1);
            json call = {{"args", {pathOf(now)}}, {"id", s.id + "_river"}, {"path", pts}, {"upstreamArea", an->areaM2(uint32_t(h2.index(cx, cz)))},
                         {"preview", a.get<bool>("preview", true)}};
            if (a.has("riverWidth"))
                call["width"] = a.get<double>("riverWidth", 0.0);
            river = ops::execute("terrainAddRiver", call);
        }
    }
    now = findTerrain(g, pathOf(t), cmd);
    json out = stats(now, currentCoast(g, now));
    out["id"] = s.id;
    out["replaced"] = replaced;
    const WorldMap w = worldMapOf(now);
    out["dam"] = {{"crest", w.height(dam.spillway.x, dam.crest, dam.spillway.y)}, {"sill", w.height(dam.spillway.x, dam.sill, dam.spillway.y)},
                  {"height", dam.height * v}, {"length", dam.length * ac}, {"spillway", w.point(dam.spillway.x, dam.sill, dam.spillway.y)}};
    if (!lake.is_null())
        for (const json& l : lake["lakes"])
            if (l.value("id", std::string()) == s.id + "_reservoir")
                out["reservoir"] = l;
    if (!river.is_null())
        for (const json& r : river["rivers"])
            if (r.value("feature", std::string()) == s.id + "_river")
                out["river"] = r;
    out["notes"].insert(out["notes"].end(), notes.begin(), notes.end());
    for (const json& note : notes)
        log::warn("{}: {}", cmd, note.get<std::string>());
    return out;
}

bool hasWaterIds(const Terrain& t, const json& ids)
{
    Node* node = t.modifier ? gn::groupOfModifier(Graph::current(), *t.modifier) : nullptr;
    if (!node)
        return false;
    gn::Group grp = gn::readGroup(*node);
    gn::GNode* w = findOfType(grp, kWaterType);
    if (!w)
        return false;
    for (const char* prop : {"rivers", "lakes"})
        for (const json& e : jsonProp(*w, prop, json::array()))
            if (std::find(ids.begin(), ids.end(), json(e.value("id", std::string()))) != ids.end())
                return true;
    return false;
}

json runCoastRemove(const Args& a)
{
    const std::string cmd = "terrainCoastRemove";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    Edit e = readGroup(g, t, cmd);
    json removed = json::array();
    std::set<std::string> ids;
    for (const json& id : a.flags.value("ids", json::array()))
        ids.insert(id.get<std::string>());
    const bool all = a.get<bool>("all", false);
    if (!all && ids.empty())
        throw CommandError(cmd + ": give ids=[...] or all=True");
    auto dropFrom = [&](gn::GNode* n, const char* prop) {
        if (!n)
            return;
        json keep = json::array();
        for (const json& s : jsonProp(*n, prop, json::array()))
            if (all || ids.count(s.value("id", std::string())))
                removed.push_back(s.value("id", std::string()));
            else
                keep.push_back(s);
        n->values[prop] = keep.dump();
    };
    gn::GNode* coast = coastNode(e.group, false, cmd);
    for (const char* prop : {"channels", "lanes", "airstrips"})
        dropFrom(coast, prop);
    dropFrom(damsNode(e.group, false, cmd), "dams");
    json out = finish(e, t, cmd);
    out["removed"] = removed;
    json water = json::array();
    for (const json& id : removed)
        for (const char* suffix : {"_reservoir", "_river"})
            water.push_back(id.get<std::string>() + suffix);
    // A reservoir's lake and river go with its dam.
    if (!water.empty() && hasWaterIds(t, water))
        ops::execute("terrainWaterRemove", {{"args", {pathOf(t)}}, {"ids", water}});
    return out;
}

// ---------------------------------------------------------------------------------------------------- reading commands

struct Loaded
{
    Terrain t;
    Current c;
};

Loaded loaded(const Args& a, const std::string& cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    Graph& g = Graph::current();
    Loaded l;
    l.t = findTerrain(g, names[0], cmd);
    l.c = currentCoast(g, l.t);
    return l;
}

json checkOf(const Terrain& t, const Current& c)
{
    json issues = c.hasCoast ? worldIssues(t, routeIssues(*t.local, c.coast)) : json::array();
    return {{"ok", issues.empty()}, {"issues", issues}, {"revision", revision(*t.local)}};
}

// ---------------------------------------------------------------------------------------------------- scene objects

void setStringAttr(const std::string& path, const char* attr, const std::string& value)
{
    Node* n = Graph::current().find(path);
    if (n && n->type().attrIndex(attr) < 0)
        ops::execute("addAttr", {{"args", {path}}, {"longName", attr}, {"dataType", "string"}});
    ops::execute("setAttr", {{"args", {path + "." + attr, value}}});
}

void setNumberAttr(const std::string& path, const char* attr, double value)
{
    Node* n = Graph::current().find(path);
    if (n && n->type().attrIndex(attr) < 0)
        ops::execute("addAttr", {{"args", {path}}, {"longName", attr}, {"attributeType", "double"}});
    ops::execute("setAttr", {{"args", {path + "." + attr, value}}});
}

/** Puts the selection back as it was when it goes: making scene objects selects them, and the terrain should stay picked. */
struct KeepSelection
{
    std::vector<scene::NodeId> ids = Graph::current().selection();
    ~KeepSelection()
    {
        Graph& g = Graph::current();
        std::erase_if(ids, [&](scene::NodeId id) { return !g.node(id); });
        g.select(ids);
    }
};

/** An empty group named name, made fresh (its old children deleted). */
std::string freshGroup(const std::string& name)
{
    Graph& g = Graph::current();
    if (Node* old = g.find(name))
        ops::execute("delete", {{"args", {old->path()}}});
    const json made = ops::execute("group", {{"empty", true}, {"name", name}});
    return made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
}

std::string asPath(const json& j) { return j.is_array() ? j[0].get<std::string>() : j.get<std::string>(); }

json makeStashLocators(const Terrain& t, const CoastResult& r)
{
    const KeepSelection keep;
    const std::string group = freshGroup(label(t) + "_stash");
    json made = json::array();
    for (const StashSpot& s : r.stash)
    {
        const glm::dvec3 p = t.world(s.at);
        const std::string loc = asPath(ops::execute("spaceLocator", {{"name", s.id}, {"position", {p.x, p.y, p.z}}, {"skipSelect", true}}));
        const std::string child = asPath(ops::execute("parent", {{"args", {loc, group}}}));
        setStringAttr(child, "stashKind", s.kind);
        setStringAttr(child, "channel", s.channel);
        setStringAttr(child, "stashId", s.id);
        setNumberAttr(child, "score", s.score);
        made.push_back(child);
    }
    return {{"group", group}, {"locators", made}};
}

json makeRouteCurves(const Terrain& t, const CoastResult& r)
{
    const KeepSelection keep;
    const std::string group = freshGroup(label(t) + "_routes");
    json made = json::array();
    for (const Lane& l : r.lanes)
    {
        if (l.points.size() < 2)
            continue;
        json pts = json::array();
        for (const glm::dvec3& p : l.points)
            pts.push_back(vec3(t.world(p)));
        const std::string curve = asPath(ops::execute("curve", {{"points", pts}, {"type", "poly"}, {"name", l.id}}));
        const std::string child = asPath(ops::execute("parent", {{"args", {curve, group}}}));
        setStringAttr(child, "laneId", l.id);
        setStringAttr(child, "laneKind", l.kind);
        setStringAttr(child, "craft", l.craft);
        setStringAttr(child, "laneSource", l.source);
        if (const CraftClass* c = craftClass(l.craft))
        {
            if (c->kind == "boat")
            {
                setNumberAttr(child, "minDepth", c->minDepth);
                setNumberAttr(child, "minWidth", c->minWidth);
            }
            else
                setNumberAttr(child, "clearance", c->clearance);
        }
        made.push_back(child);
    }
    return {{"group", group}, {"curves", made}};
}

// ---------------------------------------------------------------------------------------------------- overlay

debugDraw::Channel& overlayChannel()
{
    static debugDraw::Channel c;
    return c;
}

math::LinearColor craftColour(const std::string& craft)
{
    static const std::map<std::string, math::LinearColor> c = {
        {"skiff", {0.35f, 0.85f, 0.45f}},  {"airboat", {0.65f, 0.9f, 0.25f}}, {"goFast", {1.0f, 0.55f, 0.15f}}, {"semiSubmersible", {0.75f, 0.35f, 0.9f}},
        {"cutter", {0.25f, 0.55f, 1.0f}},  {"light", {0.95f, 0.95f, 0.4f}},   {"twin", {1.0f, 0.8f, 0.3f}}};
    auto it = c.find(craft);
    return it == c.end() ? math::LinearColor::white() : it->second;
}

json drawOverlay(const Terrain& t, const Current& c, const std::string& show, double depthStep)
{
    debugDraw::Channel& ch = overlayChannel();
    if (!ch)
        ch = debugDraw::channel("Coast and Water");
    debugDraw::flush(ch);
    int lines = 0, marks = 0;
    if (show == "off")
        return {{"show", show}, {"lines", 0}};
    const bool lanes = show == "lanes" || show == "all", depth = show == "depth" || show == "all", stash = show == "stash" || show == "all";
    if (lanes)
        for (const Lane& l : c.coast.lanes)
        {
            std::vector<math::Vector> pts;
            for (const glm::dvec3& p : l.points)
                pts.push_back(math::Vector(t.world(p.x, p.y + (l.air ? 0.0 : 20.0), p.z)));
            debugDraw::Style s;
            s.persistent = true;
            s.channel = ch;
            s.onTop = true;
            s.color = craftColour(l.craft);
            s.thickness = l.air ? 1.5f : l.kind == "coast" ? 3.0f : 2.0f;
            debugDraw::lines(pts, false, s);
            ++lines;
        }
    if (depth)
    {
        const Heightfield& h = *t.local;
        const std::vector<uint8_t> sea = seaOf(h, c.spec.seaLevel);
        std::vector<float> f(h.size());
        double deepest = 0.0;
        for (size_t i = 0; i < h.size(); ++i)
        {
            f[i] = sea[i] ? float(c.spec.seaLevel - h.heights[i]) : -1.0f;
            deepest = std::max(deepest, double(f[i]));
        }
        const double step = std::max(1.0, depthStep / t.vertical());
        for (int k = 1; k <= 24 && k * step < deepest; ++k)
        {
            const double level = k * step;
            const float shade = float(std::clamp(1.0 - double(k) / 12.0, 0.15, 1.0));
            for (const std::vector<glm::dvec2>& line : contourLines(f, h.nx, h.nz, float(level)))
            {
                std::vector<math::Vector> pts;
                for (const glm::dvec2& p : line)
                {
                    const glm::dvec2 q = h.origin + p * h.cell;
                    pts.push_back(math::Vector(t.world(q.x, c.spec.seaLevel + 10.0, q.y)));
                }
                debugDraw::Style s;
                s.persistent = true;
                s.channel = ch;
                s.color = math::LinearColor(0.2f * shade, 0.6f * shade + 0.2f, 1.0f);
                debugDraw::lines(pts, false, s);
                ++lines;
            }
        }
    }
    if (stash)
        for (const StashSpot& s : c.coast.stash)
        {
            const glm::dvec3 p = t.world(s.at);
            debugDraw::Style st;
            st.persistent = true;
            st.channel = ch;
            st.onTop = true;
            st.color = s.kind == "deadEnd" ? math::LinearColor(1.0f, 0.3f, 0.3f) : s.kind == "canopy" ? math::LinearColor(0.3f, 0.9f, 0.4f) : math::LinearColor(0.9f, 0.7f, 0.3f);
            debugDraw::point(math::Vector(p), 10.0f, st);
            debugDraw::text(math::Vector(p + upVector() * 300.0), s.kind, st);
            ++marks;
        }
    return {{"show", show}, {"lines", lines}, {"markers", marks}, {"channel", "Coast and Water"}};
}

// ---------------------------------------------------------------------------------------------------- spec step

json specStepSchema()
{
    return {{"type", "object"},
            {"description", "Coast and water for open worlds, after the water: bathymetry (terrainBathymetry flags: file or fetch with "
                            "consent, seaLevel, shoreSlope, blendWidth, shelfDepth ...), masks (terrainCoastMasks flags, or true), mangroves "
                            "(terrainMangroves flags, or true), reservoirs (terrainAddReservoir calls: {id, a, b, crest, ...}), channels "
                            "(terrainAddChannel calls: {id, path, craft, braids, ...}), routes (terrainRoutes flags), lanes (terrainAddLane "
                            "calls: {id, at | path, craft}), airstrips (terrainAddAirstrip calls: {id, a, b, aircraft}), stash (terrainStashSpots "
                            "flags, or true) and sceneObjects (true: route curves and stash locators in the scene). Runs waterCheck at the end."},
            {"properties",
             {{"bathymetry", {{"type", {"object", "boolean"}}}},
              {"masks", {{"type", {"object", "boolean"}}}},
              {"mangroves", {{"type", {"object", "boolean"}}}},
              {"reservoirs", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"channels", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"routes", {{"type", "object"}}},
              {"lanes", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"airstrips", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"stash", {{"type", {"object", "boolean"}}}},
              {"sceneObjects", {{"type", "boolean"}}}}}};
}

json runSpecStep(const std::string& target, const json& value)
{
    if (!value.is_object())
        throw CommandError("terrainSpec: coast is a dict (see lila://schema/terrain-spec)");
    json steps = json::array();
    auto call = [&](const char* step, const char* command, json flags) {
        if (!flags.is_object())
            flags = json::object();
        flags["args"] = json::array({target});
        json r = ops::execute(command, flags);
        steps.push_back({{"step", step}, {"command", command}, {"warnings", r.value("warnings", json::array())}});
    };
    auto on = [&](const char* k) { return value.contains(k) && !(value[k].is_boolean() && !value[k].get<bool>()) && !value[k].is_null(); };
    json warnings = json::array();
    auto placed = [&](const char* step, const char* command, const json& flags) {
        try
        {
            call(step, command, flags);
        }
        catch (const OffMapError& e)
        {
            log::warn("terrainSpec: {}; skipped", e.what());
            warnings.push_back(std::string(e.what()) + "; skipped");
        }
    };
    if (on("bathymetry"))
        call("bathymetry", "terrainBathymetry", value["bathymetry"]);
    for (const json& r : value.value("reservoirs", json::array()))
        placed("reservoirs", "terrainAddReservoir", r);
    for (const json& c : value.value("channels", json::array()))
        placed("channels", "terrainAddChannel", c);
    if (on("masks"))
        call("masks", "terrainCoastMasks", value["masks"]);
    if (on("mangroves"))
        call("mangroves", "terrainMangroves", value["mangroves"]);
    if (on("routes"))
        call("routes", "terrainRoutes", value["routes"]);
    for (const json& l : value.value("lanes", json::array()))
        call("lanes", "terrainAddLane", l);
    for (const json& s : value.value("airstrips", json::array()))
        call("airstrips", "terrainAddAirstrip", s);
    const bool scene = value.value("sceneObjects", true);
    if (on("stash") || scene)
    {
        json f = value.value("stash", json::object());
        if (!f.is_object())
            f = json::object();
        f["locators"] = scene;
        call("stash", "terrainStashSpots", f);
    }
    if (scene)
        call("routeCurves", "terrainRouteCurves", json::object());
    const json check = ops::execute("waterCheck", {{"args", {target}}});
    return {{"steps", steps}, {"check", check}, {"warnings", warnings}};
}

// ---------------------------------------------------------------------------------------------------- registration

std::vector<FlagDef> withPreview(std::vector<FlagDef> f)
{
    f.push_back({"preview", "pv", T::Bool, true, "Rebuild the flat water preview mesh <terrain>_water."});
    return f;
}

std::vector<std::string> boatNames()
{
    std::vector<std::string> v;
    for (const CraftClass& c : craftClasses())
        if (c.kind == "boat")
            v.push_back(c.name);
    return v;
}

void registerCoastCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainBathymetry",
        .doc = "Lay a seabed under a terrain's sea: from a GEBCO grid (file=, a GeoTIFF tile from gebco.net or any elevation file "
               "readDem reads) placed by the terrain's georeference, or fetched for the terrain's area (fetch=True: GEBCO through "
               "OpenTopography with your API key; estimate=True says the size first, and a download over 25 MB needs consent=True). "
               "With bbox= no terrain is needed to estimate or fetch: the grid goes to file= (else the terrain data's bathymetry "
               "folder) and is applied only when a terrain is found. "
               "The DEM's flat sea (ground at or below sea level plus landTolerance, joined to the edge or deep grid water) takes "
               "the grid's depths blended in from a shoreline profile over blendWidth cm, so the shore is continuous and the shelf "
               "smooth; with no grid the profile alone (shoreSlope off the beach, shelfDepth at shelfWidth, dropDepth beyond); a "
               "file the terrain can't place (no georeference) leaves the seabed as it was, source none, with a note. "
               "Writes the GEBCO credit into the terrain data's ATTRIBUTION.txt. off=True removes it. Returns the seabed source "
               "(grid, profile, grid+profile or none), samples, depths and notes.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"file", "f", T::String, nullptr, "A GEBCO GeoTIFF (or any elevation file); with fetch, where the download is written."},
                  {"fetch", "fe", T::Bool, false, "Download GEBCO for the terrain's area (or bbox) through OpenTopography."},
                  {"estimate", "es", T::Bool, false, "Only say how big the download would be."},
                  {"consent", "cn", T::Bool, false, "Allow a download over 25 MB."},
                  {"bbox", "bb", T::List, nullptr, "[west, south, east, north] in degrees; default the terrain's area."},
                  {"dataset", "ds", T::String, "GEBCOIceTopo", "OpenTopography dataset.", {"GEBCOIceTopo", "GEBCOSubIceTopo", "SRTM15Plus"}},
                  {"apiKey", "ak", T::String, "", "OpenTopography API key; default the saved one."},
                  {"saveKey", "sk", T::Bool, false, "Save apiKey for later fetches."},
                  {"offline", "off", T::Bool, false, "Use an area fetched before only."},
                  {"profileOnly", "po", T::Bool, false, "Drop the grid and use the profile alone."},
                  {"credit", "cr", T::Bool, true, "Write the GEBCO credit to ATTRIBUTION.txt."},
                  {"seaLevel", "sl", T::Float, nullptr, "Sea level along the up axis, cm; default the water's, else 0."},
                  {"landTolerance", "lt", T::Float, 50.0, "Ground this far above sea level still counts as the DEM's sea, cm.", {}, {}, 0.0},
                  {"shoreSlope", "ss", T::Float, 1.5, "Seabed slope off the shore, degrees.", {}, {}, 0.05, 60.0},
                  {"blendWidth", "bw", T::Float, 30000.0, "Distance from the shore over which the grid takes over, cm.", {}, {}, 0.0},
                  {"smoothing", "sm", T::Float, 0.0, "Blur of the grid into a shelf, cm; 0 is one grid cell.", {}, {}, 0.0},
                  {"depthScale", "dsc", T::Float, 1.0, "Multiplies the grid's depths.", {}, {}, 0.0},
                  {"shelfDepth", "sd", T::Float, 3000.0, "Profile: depth of the shelf, cm.", {}, {}, 1.0},
                  {"shelfWidth", "sw", T::Float, 200000.0, "Profile: how far out the shelf runs, cm.", {}, {}, 1.0},
                  {"dropDepth", "dd", T::Float, 20000.0, "Profile: depth past the shelf, cm.", {}, {}, 1.0},
                  {"slopeWidth", "slw", T::Float, 300000.0, "Profile: width of the drop off the shelf, cm.", {}, {}, 1.0},
                  {"minDepth", "mnd", T::Float, 30.0, "Shallowest sea off the shore, cm.", {}, {}, 0.0},
                  {"off", "of", T::Bool, false, "Remove the bathymetry."}},
        .run = [](const Args& a) -> json { return runBathymetry(a); },
        .examples = {"terrainBathymetry('Terrain', file='gebco/gebco_2024_n30_w120.tif')", "terrainBathymetry('Terrain', fetch=True, estimate=True)['estimate']['megabytes']",
                     "terrainBathymetry('Terrain', shelfDepth=2500, shelfWidth=150000)"},
    });

    ctx.registerCommand({
        .name = "terrainCoastMasks",
        .doc = "Coast masks on a terrain from the shore of its sea: beach (shore no steeper than beachSlope, up to beachHeight above "
               "the water and beachBelow under it), surf (depth under surfDepth within surfWidth of a beach), shallows (under "
               "shallowDepth) and reef (patches covering reefCover of the band reefMinDepth to reefMaxDepth deep and reefMinDistance "
               "to reefMaxDistance out). Lives in the terrain's Terrain Coast node. Returns the areas and everything the coast holds.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"seaLevel", "sl", T::Float, nullptr, "Sea level along the up axis, cm; default the water's."},
                  {"beachSlope", "bs", T::Float, 10.0, "Steepest beach, degrees.", {}, {}, 0.0, 90.0},
                  {"beachHeight", "bh", T::Float, 300.0, "Highest beach above the water, cm.", {}, {}, 0.0},
                  {"beachBelow", "bb", T::Float, 150.0, "How deep the beach runs under the water, cm.", {}, {}, 0.0},
                  {"surfDepth", "sd", T::Float, 250.0, "Deepest surf, cm.", {}, {}, 1.0},
                  {"surfWidth", "sw", T::Float, 4000.0, "Surf zone width from the shore, cm.", {}, {}, 1.0},
                  {"shallowDepth", "shd", T::Float, 600.0, "Deepest shallows, cm.", {}, {}, 1.0},
                  {"reefMinDepth", "rmd", T::Float, 100.0, "Shallowest reef, cm.", {}, {}, 0.0},
                  {"reefMaxDepth", "rxd", T::Float, 1500.0, "Deepest reef, cm.", {}, {}, 1.0},
                  {"reefMinDistance", "rmi", T::Float, 2000.0, "Nearest reef to the shore, cm.", {}, {}, 0.0},
                  {"reefMaxDistance", "rxi", T::Float, 60000.0, "Farthest reef from the shore, cm.", {}, {}, 1.0},
                  {"reefCover", "rc", T::Float, 0.35, "Share of the reef band that is reef.", {}, {}, 0.0, 1.0},
                  {"reefScale", "rs", T::Float, 6000.0, "Size of reef patches, cm.", {}, {}, 100.0},
                  {"seed", "sd2", T::Int, int64_t(0), "Reef pattern seed."},
                  {"off", "of", T::Bool, false, "No coast masks."}},
        .run = [](const Args& a) -> json { return runCoastMasks(a); },
        .examples = {"terrainCoastMasks('Terrain', reefCover=0.5)['masks']"},
    });

    ctx.registerCommand({
        .name = "terrainMangroves",
        .doc = "Mangroves on a terrain: where the land cover has the mangrove class (ESA WorldCover 95, from terrainImportLandCover), "
               "or with none, its wetland inside the tidal band, or with neither the tidal band (tidalLow to tidalHigh cm about sea "
               "level, no steeper than maxSlope) where it is sheltered (lagoons and estuaries, not open beach). Says which it used "
               "(mangroveSource). Writes masks tidal and mangrove, and adds a mangrove biome painted by them to the terrain's "
               "biomes (biome=True) for ground cover to plant. off=True clears them.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"tidalLow", "tl", T::Float, -80.0, "Bottom of the tidal band, cm from sea level."},
                  {"tidalHigh", "th", T::Float, 150.0, "Top of the tidal band, cm from sea level."},
                  {"maxSlope", "ms", T::Float, 6.0, "Steepest ground, degrees.", {}, {}, 0.1, 90.0},
                  {"landcover", "lc", T::String, "landcover", "The land cover mask (WorldCover codes / 255)."},
                  {"code", "cd", T::Int, int64_t(95), "Land cover code that is mangrove."},
                  {"fromWetland", "fw", T::Bool, true, "With no mangrove class, take wetland in the tidal band."},
                  {"fromBand", "fb", T::Bool, true, "With neither, take the sheltered tidal band."},
                  {"shelterRadius", "sr", T::Float, 20000.0, "How far round to look for open water, cm.", {}, {}, 100.0},
                  {"shelterMax", "sx", T::Float, 0.45, "Most open water around a sheltered spot, as a share.", {}, {}, 0.0, 1.0},
                  {"biome", "bi", T::Bool, true, "Add a mangrove biome to the terrain's biomes."},
                  {"off", "of", T::Bool, false, "No mangroves."}},
        .run = [](const Args& a) -> json { return runMangroves(a); },
        .examples = {"terrainMangroves('Terrain')['mangroveSource']"},
    });

    ctx.registerCommand({
        .name = "terrainAddChannel",
        .doc = "Carve a navigable channel through a lagoon, estuary or mangrove along path= or curve=, sized for a craft class "
               "(skiff, airboat, goFast, semiSubmersible, cutter): width and depth are the craft's minimum times margin unless "
               "given, braids adds strands that split off and rejoin, the banks rise at bankAngle and settle (reErode). The water "
               "is at sea level unless waterLevel is given. Returns each strand's measured narrowest clear width and shallowest "
               "depth and whether they meet the craft (meetsCraft), and everything the coast holds.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"path", "pa", T::List, nullptr, "The channel's line [[x, z], ...] in world space."},
                  nodeFlag({"curve", "cv", T::String, nullptr, "A curve the channel follows (read once)."}, "curve"),
                  {"id", "id", T::String, "", "Stable id; the same id again replaces it."},
                  {"craft", "cr", T::String, "skiff", "Craft class it is cut for.", boatNames()},
                  {"braids", "br", T::Int, int64_t(1), "Strands; 1 is one channel.", {}, {}, 1.0, 8.0},
                  {"braidSpread", "bsp", T::Float, 0.0, "How far braids wander, cm; 0 is three widths.", {}, {}, 0.0},
                  {"width", "w", T::Float, 0.0, "Width, cm; 0 is the craft's minimum times margin.", {}, {}, 0.0},
                  {"depth", "dp", T::Float, 0.0, "Depth, cm; 0 is the craft's minimum times margin.", {}, {}, 0.0},
                  {"margin", "mg", T::Float, 1.3, "Over the craft's minimums.", {}, {}, 1.0, 5.0},
                  {"waterLevel", "wl", T::Float, nullptr, "Water level along the up axis, cm; default sea level."},
                  {"bankAngle", "ba", T::Float, 25.0, "Bank slope, degrees.", {}, {}, 5.0, 80.0},
                  {"reErode", "re", T::Int, int64_t(4), "Bank settling passes.", {}, {}, 0.0, 50.0},
                  {"meander", "me", T::Float, 0.6, "Bends, in widths.", {}, {}, 0.0, 5.0},
                  {"seed", "sd", T::Int, int64_t(0), "Braid and bend seed."}},
        .run = [](const Args& a) -> json { return runAddChannel(a); },
        .examples = {"terrainAddChannel('Terrain', id='estuary', path=[[-30000, 5000], [0, 0], [20000, 6000]], craft='goFast', braids=3)"},
    });

    ctx.registerCommand({
        .name = "terrainAddReservoir",
        .doc = "Dam a valley into a reservoir: a dam from a= to b= (on the valley sides) at crest height (default the lower end's "
               "ground) with faces at faceSlope and a spillway notch spillwayWidth wide, freeboard below the crest; the lake behind "
               "it fills to the spillway sill (terrainAddLake, id <id>_reservoir) and a river runs from the spillway down the valley "
               "(terrainAddRiver, id <id>_river). Fords and bridges below come from the river and road tools. Returns the dam, the "
               "reservoir, the river and everything the coast holds.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withPreview({{"a", "a", T::List, nullptr, "One end of the dam [x, z] or [x, y, z]."},
                              {"b", "b", T::List, nullptr, "The other end."},
                              {"id", "id", T::String, "", "Stable id; the same id again replaces it."},
                              {"crest", "cs", T::Float, nullptr, "Crest height along the up axis, cm; default the lower end's ground."},
                              {"thickness", "tk", T::Float, 800.0, "Crest thickness, cm.", {}, {}, 100.0},
                              {"faceSlope", "fs", T::Float, 1.5, "Faces: horizontal per vertical.", {}, {}, 0.1, 6.0},
                              {"spillwayWidth", "sw", T::Float, 2000.0, "Spillway width, cm.", {}, {}, 0.0},
                              {"freeboard", "fb", T::Float, 300.0, "Spillway sill below the crest, cm.", {}, {}, 0.0},
                              {"spillwayAt", "sa", T::Float, 0.85, "Where along the dam the spillway is, 0 at a, 1 at b.", {}, {}, 0.0, 1.0},
                              {"lake", "lk", T::Bool, true, "Fill the reservoir behind it."},
                              {"river", "rv", T::Bool, true, "Run a river from the spillway down the valley."},
                              {"riverLength", "rl", T::Float, 300000.0, "Longest river below, cm.", {}, {}, 0.0},
                              {"riverWidth", "rw", T::Float, nullptr, "Fixed river width, cm; default from drainage."}}),
        .run = [](const Args& a) -> json { return runAddReservoir(a); },
        .examples = {"terrainAddReservoir('Terrain', id='presa', a=[-8000, 12000], b=[6000, 15000], crest=4200)"},
    });

    ctx.registerCommand({
        .name = "terrainRoutes",
        .doc = "Route graph settings: coast lanes along the coastDepth contour for coastCraft (pieces shorter than minLaneLength are "
               "left out), river reaches some boat can run, channel strands, lane ends within snap joined into nodes, and boat lanes "
               "ending in open water linked over water to the nearest lane within linkReach. Returns lane counts by kind and "
               "everything the coast holds; terrainWaterRoutes reads the graph.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"coastLanes", "cl", T::Bool, true, "Lanes along the shore."},
                  {"coastDepth", "cd", T::Float, 800.0, "Depth the coast lanes follow, cm.", {}, {}, 1.0},
                  {"coastCraft", "cc", T::String, "goFast", "Craft class of the coast lanes.", boatNames()},
                  {"minLaneLength", "ml", T::Float, 20000.0, "Shortest coast lane, cm.", {}, {}, 0.0},
                  {"riverLanes", "rl", T::Bool, true, "River reaches as lanes."},
                  {"channelLanes", "chl", T::Bool, true, "Channel strands as lanes."},
                  {"snap", "sn", T::Float, 3000.0, "Lane ends closer than this join, cm.", {}, {}, 0.0},
                  {"linkReach", "lr", T::Float, 300000.0, "Farthest over-water link from a loose lane end, cm.", {}, {}, 0.0},
                  {"spacing", "sp", T::Float, 2000.0, "Lane point spacing, cm.", {}, {}, 10.0}},
        .run = [](const Args& a) -> json { return runRoutes(a); },
        .examples = {"terrainRoutes('Terrain', coastDepth=1000, coastCraft='cutter')['lanes']"},
    });

    ctx.registerCommand({
        .name = "terrainAddLane",
        .doc = "Add a boat lane: a harbour approach from a berth (at=) routed over water deep and wide enough for the craft out to "
               "the nearest boat lane, or a drawn lane along path= or curve= (kind harbour, coast, channel or river). The same id "
               "again replaces it. waterCheck says when a lane runs over land, too shallow or too narrow, or joins nothing.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"at", "at", T::List, nullptr, "The berth [x, z] or [x, y, z]."},
                  {"path", "pa", T::List, nullptr, "A drawn lane [[x, z], ...]."},
                  nodeFlag({"curve", "cv", T::String, nullptr, "A curve the lane follows (read once)."}, "curve"),
                  {"id", "id", T::String, "", "Stable id."},
                  {"kind", "k", T::String, "harbour", "Lane kind.", {"harbour", "coast", "channel", "river"}},
                  {"craft", "cr", T::String, "goFast", "Craft class.", boatNames()}},
        .run = [](const Args& a) -> json { return runAddLane(a); },
        .examples = {"terrainAddLane('Terrain', id='marina', at=[4200, -1800], craft='cutter')"},
    });

    ctx.registerCommand({
        .name = "terrainAddAirstrip",
        .doc = "Add an airstrip from a= to b= for an aircraft class (light, twin): the runway graded flat (pad=True, a "
               "terrainStampPad), and air lanes for the approach and departure corridors off its ends (both ends unless "
               "bothEnds=False) climbing at the aircraft's angles, with the clearance over the ground along each. waterCheck flags "
               "corridors lower than the aircraft's clearance and runways shorter than it needs.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"a", "a", T::List, nullptr, "Runway end [x, z] or [x, y, z]."},
                  {"b", "b", T::List, nullptr, "The other end."},
                  {"id", "id", T::String, "", "Stable id."},
                  {"aircraft", "ac", T::String, "light", "Aircraft class.", {"light", "twin"}},
                  {"elevation", "el", T::Float, nullptr, "Runway height along the up axis, cm; default the graded pad's."},
                  {"width", "w", T::Float, 2500.0, "Runway width graded, cm.", {}, {}, 100.0},
                  {"pad", "pd", T::Bool, true, "Grade the runway flat."},
                  {"bothEnds", "be", T::Bool, true, "Corridors off both ends."}},
        .run = [](const Args& a) -> json { return runAddAirstrip(a); },
        .examples = {"terrainAddAirstrip('Terrain', id='rancho', a=[30000, 20000], b=[30000, 70000], aircraft='light')"},
    });

    ctx.registerCommand({
        .name = "terrainCoastRemove",
        .doc = "Remove channels, lanes, airstrips and reservoirs (with their lake and river) by id, or all=True.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"ids", "ids", T::List, nullptr, "Ids to remove."}, {"all", "al", T::Bool, false, "Remove them all."}},
        .run = [](const Args& a) -> json { return runCoastRemove(a); },
        .examples = {"terrainCoastRemove('Terrain', ids=['estuary'])"},
    });

    ctx.registerCommand({
        .name = "terrainCoast",
        .doc = "A terrain's coast as authored and as made: settings, channels, reservoirs, lanes, airstrips, the bathymetry, mask "
               "areas, the mangrove source, stash-spot counts and the route graph's size, with warnings and notes.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Loaded l = loaded(a, "terrainCoast");
            json out = stats(l.t, l.c);
            json channels = json::array(), lanes = json::array(), strips = json::array(), dams = json::array();
            for (const ChannelSpec& c : l.c.spec.channels)
                channels.push_back(c.toJson());
            for (const LaneSpec& s : l.c.spec.lanes)
                lanes.push_back(s.toJson());
            for (const AirstripSpec& s : l.c.spec.airstrips)
                strips.push_back(s.toJson());
            for (const DamSpec& s : l.c.damSpecs)
                dams.push_back(s.toJson());
            out["features"] = {{"settings", l.c.spec.settingsJson()}, {"channels", channels}, {"lanes", lanes}, {"airstrips", strips}, {"dams", dams},
                               {"bathymetry", l.c.bathy.toJson()}};
            return out;
        },
        .examples = {"terrainCoast('Terrain')['channels']"},
    });

    ctx.registerCommand({
        .name = "terrainCraftClasses",
        .doc = "The craft classes routes and channels are sized for: boats (draft, minDepth, minWidth, beam, speed) and aircraft "
               "(approach and climb angles, clearance, corridor size, shortest runway). Lengths cm, speeds m/s.",
        .flags = {},
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const CraftClass& c : craftClasses())
                out.push_back(craftJson(c));
            return out;
        },
        .examples = {"[c['name'] for c in terrainCraftClasses()]"},
    });

    ctx.registerCommand({
        .name = "terrainWaterRoutes",
        .doc = "A terrain's boat and air route graph in world space (format lila.waterRoutes): nodes, lanes (kind coast, channel, "
               "river, harbour, link, approach or departure; craft class; points with clear width and depth, or clearance), "
               "runways, the craft classes, and a ZoneGraph-style block (lane profiles per craft, shapes with links).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Loaded l = loaded(a, "terrainWaterRoutes");
            if (!l.c.hasCoast)
                throw CommandError("terrainWaterRoutes: the terrain has no coast (add one with terrainRoutes, terrainAddChannel or terrainCoastMasks)");
            return routesJson(l.c.coast, worldMapOf(l.t));
        },
        .examples = {"r = terrainWaterRoutes('Terrain')\n[(l['id'], l['craft']) for l in r['lanes']]"},
    });

    ctx.registerCommand({
        .name = "waterCheck",
        .doc = "What is wrong with a terrain's channels and routes: channelShallow and channelNarrow (a strand under its craft's "
               "minimum depth or width), laneShallow and laneNarrow, laneOverLand (points on dry ground), laneDisconnected (a boat "
               "lane not joined to the main network), harbourUnreachable, airClearance (a corridor lower over the ground than its "
               "aircraft needs) and shortRunway. Returns ok and the issues, each with type, about, position and message. "
               "terrainCheck includes them.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Loaded l = loaded(a, "waterCheck");
            return checkOf(l.t, l.c);
        },
        .examples = {"c = waterCheck('Terrain')\nc['ok'], [i['type'] for i in c['issues']]"},
    });

    ctx.registerCommand({
        .name = "terrainStashSpots",
        .doc = "Stash-spot candidates along a terrain's channels: dead ends away from open sea, overhung banks (ground bankHeight "
               "cm above the water within bankReach of the edge) and spots under mangrove canopy, at least spacing cm apart, best "
               "first, at most maxCount. locators=True makes them locators under <terrain>_stash with stashKind, channel and score "
               "attributes (replacing the old ones).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"bankHeight", "bh", T::Float, 250.0, "Overhang: bank height above the water, cm.", {}, {}, 0.0},
                  {"bankReach", "br", T::Float, 600.0, "Overhang and canopy: how far from the edge to look, cm.", {}, {}, 1.0},
                  {"canopy", "ca", T::Float, 0.5, "Canopy: mangrove density.", {}, {}, 0.0, 1.0},
                  {"spacing", "sp", T::Float, 8000.0, "Fewest cm between two.", {}, {}, 0.0},
                  {"maxCount", "mc", T::Int, int64_t(64), "Most candidates.", {}, {}, 0.0, 10000.0},
                  {"off", "of", T::Bool, false, "No stash spots."},
                  {"locators", "lo", T::Bool, true, "Make locators in the scene."}},
        .run = [](const Args& a) -> json {
            const std::string cmd = "terrainStashSpots";
            Graph& g = Graph::current();
            const Terrain t = oneTerrain(a, cmd);
            const double ac = t.across(), v = t.vertical();
            Edit e = readGroup(g, t, cmd);
            gn::GNode* n = coastNode(e.group, true, cmd);
            CoastSpec s;
            s.readSettings(jsonProp(*n, "settings", json::object()));
            s.stash.enabled = !a.get<bool>("off", false);
            if (a.has("bankHeight"))
                s.stash.bankHeight = a.get<double>("bankHeight", 250.0) / v;
            if (a.has("bankReach"))
                s.stash.bankReach = a.get<double>("bankReach", 600.0) / ac;
            if (a.has("canopy"))
                s.stash.canopy = a.get<double>("canopy", 0.5);
            if (a.has("spacing"))
                s.stash.spacing = a.get<double>("spacing", 8000.0) / ac;
            if (a.has("maxCount"))
                s.stash.maxCount = int(a.get<int64_t>("maxCount", 64));
            n->values["settings"] = s.settingsJson().dump();
            json out = finish(e, t, cmd);
            const Terrain now = findTerrain(g, pathOf(t), cmd);
            const Current c = currentCoast(g, now);
            json spots = json::array();
            for (const StashSpot& sp : c.coast.stash)
                spots.push_back({{"id", sp.id}, {"kind", sp.kind}, {"channel", sp.channel}, {"score", sp.score}, {"position", vec3(now.world(sp.at))}});
            out["spots"] = spots;
            if (a.get<bool>("locators", true))
                out["scene"] = makeStashLocators(now, c.coast);
            return out;
        },
        .examples = {"s = terrainStashSpots('Terrain')\n[(p['id'], p['kind']) for p in s['spots']]"},
    });

    ctx.registerCommand({
        .name = "terrainRouteCurves",
        .doc = "The route graph as curves in the scene, one poly curve per lane under <terrain>_routes (replacing the old ones), "
               "each with laneId, laneKind, craft and laneSource attributes and the craft's minDepth and minWidth (boats) or "
               "clearance (aircraft), for engines and tools that read splines.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {},
        .run = [](const Args& a) -> json {
            Loaded l = loaded(a, "terrainRouteCurves");
            if (!l.c.hasCoast)
                throw CommandError("terrainRouteCurves: the terrain has no coast");
            return makeRouteCurves(l.t, l.c.coast);
        },
        .examples = {"terrainRouteCurves('Terrain')['curves']"},
    });

    ctx.registerCommand({
        .name = "terrainExportRoutes",
        .doc = "Write a terrain's coast to <name>_coast.json (channels as water splines with width, depth and waterLevel; stash "
               "spots, dams and spillways as locators; format lila.terrainWater) and its routes to <name>_routes.json (format "
               "lila.waterRoutes, with the ZoneGraph-style block). terrainExport writes them too.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"folder", "f", T::String, "", "Output folder; defaults to the bake folder's export/."},
                  {"name", "n", T::String, "", "File name stem; defaults to the terrain's name."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainExportRoutes: give one terrain");
            fs::path folder = a.get<std::string>("folder", "");
            if (folder.empty())
                folder = exportDir();
            const Terrain t = findTerrain(Graph::current(), names[0], "terrainExportRoutes");
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = label(t);
            json out = exportTerrainCoast(names[0], folder, name);
            if (out.is_null())
                throw CommandError("terrainExportRoutes: " + names[0] + " has no coast");
            return out;
        },
        .examples = {"terrainExportRoutes('Terrain', folder='export')"},
    });

    ctx.registerCommand({
        .name = "terrainSendRoutesToUnreal",
        .doc = "Send a terrain's coast to the connected Unreal editor: channels, stash spots, dams and spillways as water spline "
               "and locator actors (as terrainSendWaterToUnreal sends rivers), and the route graph as spline actors with lane data "
               "(craft, kind, width, depth or clearance, links) under a LilaWaterRoutes folder. Keyed by the terrain's linkId, so "
               "sending again updates in place. terrainSendToUnreal sends them too.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"timeout", "to", T::Float, 120.0, "Seconds to wait for Unreal.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSendRoutesToUnreal: give one terrain");
            try
            {
                json out = sendTerrainCoast(names[0], a.get<double>("timeout", 120.0));
                if (out.is_null())
                    throw CommandError("terrainSendRoutesToUnreal: " + names[0] + " has no coast");
                return out;
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendRoutesToUnreal: ") + e.what());
            }
        },
        .examples = {"terrainSendRoutesToUnreal('Terrain')"},
    });

    ctx.registerCommand({
        .name = "terrainCoastOverlay",
        .doc = "Draw a terrain's coast over the viewport: lanes coloured by craft class (lanes), depth contours every depthStep cm "
               "(depth), stash spots (stash), all of them, or off. Stays until changed.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"show", "sh", T::String, "all", "What to draw.", {"all", "lanes", "depth", "stash", "off"}},
                  {"depthStep", "ds", T::Float, 500.0, "Depth contour spacing, cm.", {}, {}, 10.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string show = a.get<std::string>("show", "all");
            if (show == "off")
            {
                if (overlayChannel())
                    debugDraw::flush(overlayChannel());
                return {{"show", "off"}};
            }
            Loaded l = loaded(a, "terrainCoastOverlay");
            return drawOverlay(l.t, l.c, show, a.get<double>("depthStep", 500.0));
        },
        .examples = {"terrainCoastOverlay('Terrain', show='lanes')", "terrainCoastOverlay(show='off')"},
    });
}
} // namespace coast_cmd

// ---------------------------------------------------------------------------------------------------- hooks

json coastIssues(const std::string& terrain)
{
    using namespace coast_cmd;
    try
    {
        Graph& g = Graph::current();
        const Terrain t = findTerrain(g, terrain, "terrainCheck");
        const Current c = currentCoast(g, t);
        json issues = c.hasCoast ? worldIssues(t, routeIssues(*t.local, c.coast)) : json::array();
        // Spec items placed off the map (a terrain resized, or a spec written for another map): nothing of them is built.
        auto off = [&](const std::string& what, const std::string& id, const std::vector<std::pair<std::string, glm::dvec2>>& pts) {
            if (const std::string msg = offMap(t, what, id, pts); !msg.empty())
            {
                const glm::dvec2 p = pts.front().second;
                issues.push_back({{"type", "outsideMap"}, {"about", id}, {"position", vec3(t.world(p.x, 0.0, p.y))}, {"message", msg}});
            }
        };
        for (const DamSpec& d : c.damSpecs)
            off("reservoir", d.id, {{"dam end a", d.a}, {"dam end b", d.b}});
        for (const ChannelSpec& ch : c.spec.channels)
            off("channel", ch.id, pathPoints(ch.path));
        for (const LaneSpec& l : c.spec.lanes)
            off("lane", l.id, l.at ? std::vector<std::pair<std::string, glm::dvec2>>{{"berth", *l.at}} : pathPoints(l.path));
        for (const AirstripSpec& a : c.spec.airstrips)
            off("airstrip", a.id, {{"end a", a.a}, {"end b", a.b}});
        return issues;
    }
    catch (const std::exception&)
    {
        return json::array();
    }
}

json exportTerrainCoast(const std::string& terrain, const std::filesystem::path& folder, const std::string& name)
{
    using namespace coast_cmd;
    Graph& g = Graph::current();
    const Terrain t = findTerrain(g, terrain, "terrainExport");
    const Current c = currentCoast(g, t);
    if (!c.hasCoast && c.dams.empty())
        return nullptr;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const WorldMap w = worldMapOf(t);
    const json water = coastWaterJson(c.coast, c.dams, w);
    const fs::path coastFile = folder / (name + "_coast.json"), routesFile = folder / (name + "_routes.json");
    {
        std::ofstream f(coastFile, std::ios::binary);
        if (!f)
            throw std::runtime_error("can't write " + coastFile.string());
        f << water.dump(1);
    }
    json out = {{"coastFile", coastFile.generic_string()}, {"splines", water["splines"].size()}, {"locators", water["locators"].size()}};
    if (c.hasCoast)
    {
        const json routes = routesJson(c.coast, w);
        std::ofstream f(routesFile, std::ios::binary);
        if (!f)
            throw std::runtime_error("can't write " + routesFile.string());
        f << routes.dump(1);
        out["routesFile"] = routesFile.generic_string();
        out["lanes"] = routes["lanes"].size();
    }
    return out;
}

json sendTerrainCoast(const std::string& terrain, double timeoutSeconds)
{
    using namespace coast_cmd;
    Graph& g = Graph::current();
    const Terrain t = findTerrain(g, terrain, "terrainSendRoutesToUnreal");
    const Current c = currentCoast(g, t);
    if (!c.hasCoast && c.dams.empty())
        return nullptr;
    link::Link::get().ensureConnected();
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    const json features = link::Link::get().status().value("features", json::array());
    auto has = [&](const char* f) { return features.is_array() && std::find(features.begin(), features.end(), json(f)) != features.end(); };
    if (!client || !has("terrainWater"))
        throw std::runtime_error("the LilaLink plugin in Unreal predates terrain water; rebuild it from Lila's ue/LilaLink");
    // The terrain's linkId keys its water; the coast goes under the same key with a suffix so neither send replaces the other.
    std::string key;
    if (t.mesh)
    {
        const std::string* s = std::get_if<std::string>(&t.mesh->value(scene::Transform::kLinkId));
        key = s && !s->empty() ? *s : std::format("{:016x}", std::hash<std::string>{}(t.mesh->path() + std::to_string(t.mesh->id())));
        if (!s || s->empty())
            g.setValue({t.mesh->id(), scene::Transform::kLinkId, -1}, key);
    }
    else
        key = t.modifier->path();
    const WorldMap w = worldMapOf(t);
    const int timeoutMs = int(std::clamp(timeoutSeconds, 1.0, 3600.0) * 1000.0);
    json out = json::object();
    const json water = unrealWaterParams(coastWaterJson(c.coast, c.dams, w), key + "_coast", label(t) + "_Coast");
    link::RpcResult r = client->call("setWater", water, timeoutMs);
    if (!r.ok)
        throw std::runtime_error(r.error);
    out["water"] = r.result;
    if (c.hasCoast)
    {
        if (!has("waterRoutes"))
            throw std::runtime_error("the LilaLink plugin in Unreal predates water routes; rebuild it from Lila's ue/LilaLink");
        const json routes = routesJson(c.coast, w);
        json lanes = json::array();
        for (const json& l : routes["lanes"])
        {
            json q = l;
            json pts = json::array();
            for (const json& p : l["points"])
            {
                json pp = p;
                const math::Vector v = math::toUnreal(math::Vector(glm::dvec3(p["position"][0].get<double>(), p["position"][1].get<double>(), p["position"][2].get<double>())));
                pp["position"] = {v.x, v.y, v.z};
                pts.push_back(pp);
            }
            q["points"] = pts;
            lanes.push_back(q);
        }
        const json params = {{"id", key}, {"label", label(t)}, {"lanes", lanes}, {"crafts", routes["crafts"]}, {"laneProfiles", routes["zoneGraph"]["laneProfiles"]}};
        link::RpcResult rr = client->call("setWaterRoutes", params, timeoutMs);
        if (!rr.ok)
            throw std::runtime_error(rr.error);
        out["routes"] = rr.result;
        out["lanes"] = lanes.size();
    }
    return out;
}

void registerCoast(ext::Context& ctx)
{
    using namespace coast_cmd;
    for (gn::NodeDecl d : {bathyDecl(), damsDecl(), coastDecl()})
    {
        const std::string label = d.label;
        if (!gn::registerDecl(ctx, std::move(d)))
            log::warn("Terrain: the {} node didn't register", label);
    }
    registerCoastCommands(ctx);
    overlayChannel() = debugDraw::channel(ctx, "Coast and Water");
    ctx.add(lila::specs::steps(), "specStep", "terrain.coast",
            lila::specs::Step{.spec = "terrain", .key = "coast", .schema = specStepSchema(), .run = runSpecStep});
}
} // namespace terrain
