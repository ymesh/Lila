// The Terrain Water Features node and the water commands: rivers, lakes and sea level authored on a terrain, their
// preview surface, and their export as splines and locators to files and to Unreal.

#include "Stamps.h"
#include "TerrainEdit.h"
#include "Water.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>

using namespace lila;

namespace terrain
{
namespace water_cmd
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;

constexpr const char* kNodeType = "TerrainWaterFeatures";
constexpr const char* kNodeName = "Water";
constexpr const char* kSourcePrefix = "WaterSource";
constexpr const char* kWaterMaterial = "TerrainWater";

using namespace terrain::edit;
using Terrain = TerrainTarget;
using Edit = GroupEdit;

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

/** terrainExport's default folder: the bake folder's export/. */
fs::path exportDir()
{
    const fs::path& scene = io::currentPath();
    if (!scene.empty())
        return scene.parent_path() / (scene.stem().string() + "_terrain") / "export";
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain" / "export";
    return fs::temp_directory_path() / "lila_terrain" / "export";
}

/** The group's water node, added on the heightfield chain before the trailing stamp layers when missing. */
gn::GNode* waterNode(gn::Group& grp, bool create, const std::string& cmd)
{
    // Water sits right after the ground forms: stamps, roads (which bridge it), the volume and surface layers come after.
    static const std::set<std::string> after = {"TerrainStamps", "TerrainRoads", "TerrainVolume", "TerrainBiomes", "TerrainLayers", "TerrainCoast"};
    if (nodeNamed(grp, kNodeName))
        return &chainNode(grp, kNodeName, kNodeType, after, -160.0, cmd);
    for (gn::GNode& n : grp.nodes)
        if (n.type == kNodeType)
            return &n;
    return create ? &chainNode(grp, kNodeName, kNodeType, after, -160.0, cmd) : nullptr;
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

/** The water a terrain carries: its spec, what it made last, and the node it lives in. */
struct Current
{
    WaterSpec spec;
    WaterResult result;
    bool has = false;
};

Current currentWater(Graph& g, const Terrain& t)
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
    gn::GNode* n = waterNode(grp, false, "");
    if (!n)
        return c;
    for (const json& j : jsonProp(*n, "rivers", json::array()))
        c.spec.rivers.push_back(RiverSpec::fromJson(j));
    for (const json& j : jsonProp(*n, "lakes", json::array()))
        c.spec.lakes.push_back(LakeSpec::fromJson(j));
    c.spec.settings = WaterSettings::fromJson(jsonProp(*n, "settings", json::object()));
    c.has = publishedWater(t.modifier->id(), n->name, &c.result);
    return c;
}

json worldWater(const Terrain& t, const Current& c)
{
    return waterJson(c.result, worldMapOf(t), c.spec.settings.seaLevel);
}

/** Counts and volumes an agent reads after every water call. */
json stats(const Terrain& t, const Current& c)
{
    const WaterResult& r = c.result;
    const double a = t.across(), v = t.vertical(), vol = a * a * v;
    json lakes = json::array();
    for (const LakeResult& l : r.lakes)
        lakes.push_back({{"id", l.id},
                         {"kind", l.kind},
                         {"level", l.level * v},
                         {"spillHeight", l.spillHeight * v},
                         {"area", l.area * a * a},
                         {"maxDepth", l.maxDepth * v},
                         {"volume", l.volume * vol},
                         {"damHeight", l.damHeight * v}});
    json rivers = json::array();
    for (const RiverResult& rr : r.rivers)
    {
        double len = rr.points.empty() ? 0.0 : rr.points.back().distance;
        rivers.push_back({{"id", rr.id},
                          {"feature", rr.spec},
                          {"mouth", rr.mouth},
                          {"length", len * a},
                          {"points", rr.points.size()},
                          {"widthMax", rr.points.empty() ? 0.0 : rr.points.back().width * a},
                          {"depthMax", rr.points.empty() ? 0.0 : rr.points.back().depth * v},
                          {"discharge", rr.points.empty() ? 0.0 : rr.points.back().discharge},
                          {"maxCut", rr.maxCut * v},
                          {"reversed", rr.reversed}});
    }
    return {{"rivers", rivers},
            {"lakes", lakes},
            {"waterfalls", r.waterfalls.size()},
            {"springs", r.springs.size()},
            {"pitsFilled", r.pitsFilled},
            {"pitVolume", r.pitVolume * vol},
            {"cutM3", r.cut * vol},
            {"fillM3", r.fill * vol},
            {"warnings", r.warnings}};
}

/** Rebuilds the flat water preview mesh beside the terrain; returns its path, or empty when there is no water. */
std::string refreshPreview(Graph& g, const Terrain& t, const WaterResult& r)
{
    const std::string name = (t.mesh ? t.mesh->name() : std::string("Terrain")) + "_water";
    Node* existing = nullptr;
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->name() == name && n->type().isA("mesh"))
            existing = n;
    if (r.rivers.empty() && r.lakes.empty())
    {
        if (existing)
            ops::execute("delete", {{"args", {existing->path()}}});
        return {};
    }
    geo::Mesh m = waterPreviewMesh(*t.local, r, t.m);
    // Water in its own material, so the sea reads apart from the seabed and the land under the default white.
    if (!g.find(kWaterMaterial))
        ops::execute("createMaterial", {{"args", {kWaterMaterial}}, {"baseColor", {0.02, 0.16, 0.24}}, {"roughness", 0.08}, {"blendMode", "translucent"}, {"opacity", 0.8}});
    m.materialNames() = {kWaterMaterial};
    std::span<int32_t> slot = m.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    std::fill(slot.begin(), slot.end(), 0);
    if (!existing)
    {
        json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
        existing = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
    }
    g.setValue({existing->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return existing->path();
}

/** Commits the edited water, re-reads it and builds the answer: the named feature's data, the stats and the revision. */
json finish(Edit& e, const Terrain& t, const std::string& cmd, const std::string& id, bool preview)
{
    dropUnusedSources(e.group, kSourcePrefix, kNodeType, {"rivers", "lakes"});
    commitGroup(e, cmd);
    Graph& g = Graph::current();
    const Terrain after = findTerrain(g, pathOf(t), cmd);
    const Current c = currentWater(g, after);
    const json w = worldWater(after, c);
    json out = stats(after, c);
    if (!id.empty())
    {
        json splines = json::array(), locators = json::array();
        for (const json& s : w["splines"])
            if (s.value("feature", std::string()) == id || s.value("id", std::string()) == id)
                splines.push_back(s);
        for (const json& l : w["locators"])
            if (l.value("id", std::string()).starts_with(id))
                locators.push_back(l);
        out["id"] = id;
        out["splines"] = splines;
        out["locators"] = locators;
    }
    for (const std::string& warning : c.result.warnings)
        log::warn("{}: {}", cmd, warning);
    if (preview)
        out["preview"] = refreshPreview(g, after, c.result);
    out["revision"] = revision(*after.local);
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

/** Replaces the entry with the same id, or adds it. True when it replaced one. */
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

// ---------------------------------------------------------------------------------------------------- node

gn::NodeDecl waterDecl()
{
    gn::NodeDecl d;
    d.type = kNodeType;
    d.label = "Terrain Water Features";
    d.category = "Terrain";
    d.doc = "Rivers, lakes and sea level on a heightfield. Rivers are carved with a bed that falls all the way to their mouth, sized "
            "by their drainage, with banks re-eroded into the ground, gravel bars inside bends and waterfalls with plunge pools at "
            "cliffs; lakes stand at their spill level or a set level; basins no water holds are filled so everything drains. Writes "
            "the masks riverBed, riverBank, gravelBar, waterfall, lakeBed, sea, water, shore and beach. terrainAddRiver, "
            "terrainAddLake and terrainSeaLevel write its lists; put it after Erode Terrain.";
    d.props = {{.id = "rivers", .defaultValue = "[]", .doc = "The rivers, a JSON list in the heightfield's own space (see terrainAddRiver)."},
               {.id = "lakes", .defaultValue = "[]", .doc = "The lakes, a JSON list in the heightfield's own space (see terrainAddLake)."},
               {.id = "settings", .defaultValue = "{}", .doc = "Sea level, shore and beach settings, a JSON object (see terrainSeaLevel)."}};
    d.sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}};
    d.exec = [](gn::ExecContext& c) {
        LILA_ZONE();

        gn::GeometryPtr g = c.geometry("heightfield");
        const Heightfield* in = heightfieldOf(*g);
        if (!in)
            throw gn::ExecError(c.node.name + ": the geometry has no heightfield (start from Heightfield Grid or Mesh to Heightfield)");
        auto parsed = [&](const char* id, json fallback) {
            const std::string text = c.prop(id);
            if (text.empty())
                return fallback;
            json j = json::parse(text, nullptr, false);
            if (j.is_discarded())
                throw gn::ExecError(c.node.name + ": " + id + " is not JSON");
            return j;
        };
        WaterSpec spec;
        try
        {
            for (const json& j : parsed("rivers", json::array()))
                spec.rivers.push_back(RiverSpec::fromJson(j));
            for (const json& j : parsed("lakes", json::array()))
                spec.lakes.push_back(LakeSpec::fromJson(j));
            spec.settings = WaterSettings::fromJson(parsed("settings", json::object()));
        }
        catch (const std::exception& e)
        {
            throw gn::ExecError(c.node.name + ": " + e.what());
        }
        WaterObjects objects;
        for (const auto& [name, obj] : c.env.objects)
        {
            if (!name.starts_with(kSourcePrefix) || !obj.curves)
                continue;
            const Footprint f = footprintFromCurves(*obj.curves, glm::inverse(c.env.ownerMatrix) * obj.matrix);
            objects.lines[name] = f.lines;
            objects.rings[name] = f.rings;
        }
        Heightfield h = *in;
        WaterResult r = applyWater(h, spec, objects);
        publishWater(c.env.owner, c.node.name, std::move(r));
        gn::Geometry out = *g;
        out.heightfield = std::make_shared<const Heightfield>(std::move(h));
        c.out("heightfield", gn::makeGeometry(std::move(out)));
    };
    return d;
}

// ---------------------------------------------------------------------------------------------------- commands

std::vector<FlagDef> commonFlags(std::vector<FlagDef> flags)
{
    flags.push_back({"id", "id", T::String, "", "Stable id; the same id again replaces it. Empty makes one."});
    flags.push_back({"preview", "pv", T::Bool, true, "Rebuild the flat water preview mesh <terrain>_water."});
    return flags;
}

json addRiver(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainAddRiver";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    Edit e = readGroup(g, t, cmd);
    gn::GNode* node = waterNode(e.group, true, cmd);
    json list = jsonProp(*node, "rivers", json::array());

    RiverSpec s;
    s.id = a.get<std::string>("id", "");
    if (s.id.empty())
        s.id = freshId(list, "river");
    if (a.get<bool>("auto", false))
        s.source = "auto";
    else if (a.has("path"))
    {
        s.source = "path";
        s.path = localLine(t, a.flags["path"], cmd);
        if (s.path.size() < 2)
            throw CommandError(cmd + ": path needs at least two points");
    }
    else if (a.has("object"))
    {
        Node* src = g.find(a.get<std::string>("object", ""));
        if (!src || !src->type().isA("curve"))
            throw CommandError(cmd + ": object must name a curve");
        s.source = "object";
        s.object = sourceNode(e.group, kSourcePrefix, src->path());
        node = waterNode(e.group, false, cmd);
    }
    else
        throw CommandError(cmd + ": give path=, object= (a curve) or auto=True");
    s.threshold = std::max(1.0, a.get<double>("threshold", 1.0e6)) / (ac * ac);
    s.minLength = std::max(0.0, a.get<double>("minLength", 0.0)) / ac;
    s.upstreamArea = std::max(0.0, a.get<double>("upstreamArea", 0.0)) / (ac * ac);
    s.width = std::max(0.0, a.get<double>("width", 0.0)) / ac;
    s.depth = std::max(0.0, a.get<double>("depth", 0.0)) / v;
    s.widthScale = std::max(0.0, a.get<double>("widthScale", 1.0));
    s.depthScale = std::max(0.0, a.get<double>("depthScale", 1.0));
    s.minWidth = std::max(1.0, a.get<double>("minWidth", 150.0)) / ac;
    s.minDepth = std::max(1.0, a.get<double>("minDepth", 40.0)) / v;
    s.meander = std::max(0.0, a.get<double>("meander", 0.0));
    s.meanderLength = std::max(2.0, a.get<double>("meanderLength", 14.0));
    s.seed = uint32_t(a.get<int64_t>("seed", 0));
    s.bankAngle = std::clamp(a.get<double>("bankAngle", 30.0), 5.0, 85.0);
    s.bankWidth = std::max(0.0, a.get<double>("bankWidth", 0.0)) / ac;
    s.reErode = int(std::clamp<int64_t>(a.get<int64_t>("reErode", 6), 0, 100));
    s.gravelBars = a.get<bool>("gravelBars", true);
    s.waterfalls = a.get<bool>("waterfalls", true);
    s.waterfallHeight = std::max(1.0, a.get<double>("waterfallHeight", 300.0)) / v;
    s.cliffAngle = std::clamp(a.get<double>("cliffAngle", 55.0), 5.0, 89.0);
    s.plungeDepth = std::clamp(a.get<double>("plungeDepth", 0.4), 0.0, 2.0);
    s.flowSpeed = std::max(0.0, a.get<double>("flowSpeed", 0.0));
    s.keepDirection = a.get<bool>("keepDirection", false);
    const bool replaced = upsert(list, s.toJson());
    node->values["rivers"] = list.dump();
    json out = finish(e, t, cmd, s.id, a.get<bool>("preview", true));
    out["replaced"] = replaced;
    return out;
}

json addLake(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainAddLake";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    const double ac = t.across(), v = t.vertical();
    Edit e = readGroup(g, t, cmd);
    gn::GNode* node = waterNode(e.group, true, cmd);
    json list = jsonProp(*node, "lakes", json::array());

    LakeSpec s;
    s.id = a.get<std::string>("id", "");
    if (s.id.empty())
        s.id = freshId(list, "lake");
    glm::dvec2 centre = t.centre();
    if (a.get<bool>("auto", false))
        s.source = "auto";
    else if (a.has("at"))
    {
        s.source = "point";
        s.at = t.local2(worldPoint(a.flags["at"], cmd));
        centre = s.at;
    }
    else if (a.has("polygon"))
    {
        s.source = "region";
        s.polygon = localLine(t, a.flags["polygon"], cmd);
        if (s.polygon.size() < 3)
            throw CommandError(cmd + ": polygon needs at least three points");
        centre = glm::dvec2(0.0);
        for (const glm::dvec2& p : s.polygon)
            centre += p / double(s.polygon.size());
    }
    else if (a.has("object"))
    {
        Node* src = g.find(a.get<std::string>("object", ""));
        if (!src || !src->type().isA("curve"))
            throw CommandError(cmd + ": object must name a closed curve");
        s.source = "object";
        s.object = sourceNode(e.group, kSourcePrefix, src->path());
        node = waterNode(e.group, false, cmd);
        centre = t.local2(glm::dvec3(src->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]));
    }
    else
        throw CommandError(cmd + ": give at=, polygon=, object= (a closed curve) or auto=True");
    if (a.has("level"))
        s.level = t.localHeight(a.get<double>("level", 0.0), centre);
    s.minDepth = std::max(0.0, a.get<double>("minDepth", 50.0)) / v;
    s.minArea = std::max(0.0, a.get<double>("minArea", 2000.0)) / (ac * ac);
    s.depth = std::max(0.0, a.get<double>("depth", 300.0)) / v;
    s.shoreSlope = std::clamp(a.get<double>("shoreSlope", 12.0), 1.0, 80.0);
    s.dam = a.get<bool>("dam", true);
    const bool replaced = upsert(list, s.toJson());
    node->values["lakes"] = list.dump();
    json out = finish(e, t, cmd, s.id, a.get<bool>("preview", true));
    out["replaced"] = replaced;
    return out;
}

json seaLevel(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainSeaLevel";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    Edit e = readGroup(g, t, cmd);
    gn::GNode* node = waterNode(e.group, true, cmd);
    WaterSettings s = WaterSettings::fromJson(jsonProp(*node, "settings", json::object()));
    if (a.get<bool>("off", false))
        s.seaLevel = std::numeric_limits<double>::quiet_NaN();
    else if (a.has("level"))
        s.seaLevel = t.localHeight(a.get<double>("level", 0.0), t.centre());
    if (a.has("shoreWidth"))
        s.shoreWidth = std::max(0.0, a.get<double>("shoreWidth", 1500.0)) / t.across();
    if (a.has("beachSlope"))
        s.beachSlope = std::clamp(a.get<double>("beachSlope", 10.0), 0.0, 90.0);
    if (a.has("beachHeight"))
        s.beachHeight = std::max(0.0, a.get<double>("beachHeight", 300.0)) / t.vertical();
    if (a.has("drainPits"))
        s.drainPits = a.get<bool>("drainPits", true);
    node->values["settings"] = s.toJson().dump();
    json out = finish(e, t, cmd, std::isnan(s.seaLevel) ? std::string() : std::string("sea"), a.get<bool>("preview", true));
    out["seaLevel"] = std::isnan(s.seaLevel) ? json(nullptr) : json(s.seaLevel * t.vertical());
    return out;
}

json waterRemove(const Args& a)
{
    const std::string cmd = "terrainWaterRemove";
    Graph& g = Graph::current();
    const Terrain t = oneTerrain(a, cmd);
    Edit e = readGroup(g, t, cmd);
    gn::GNode* node = waterNode(e.group, false, cmd);
    if (!node)
        throw CommandError(cmd + ": the terrain has no water");
    json removed = json::array();
    if (a.get<bool>("all", false))
    {
        // The node leaves the chain: what fed it feeds what it fed.
        const std::string name = node->name;
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
        std::erase_if(e.group.nodes, [&](const gn::GNode& n) { return n.name == name; });
        removed.push_back("all");
    }
    else
    {
        if (!a.has("ids") || !a.flags["ids"].is_array())
            throw CommandError(cmd + ": give ids=[...] or all=True");
        std::set<std::string> ids;
        for (const json& id : a.flags["ids"])
            ids.insert(id.get<std::string>());
        for (const char* prop : {"rivers", "lakes"})
        {
            json keep = json::array();
            for (const json& s : jsonProp(*node, prop, json::array()))
            {
                const std::string id = s.value("id", std::string());
                if (ids.count(id))
                    removed.push_back(id);
                else
                    keep.push_back(s);
            }
            node->values[prop] = keep.dump();
        }
    }
    json out = finish(e, t, cmd, "", a.get<bool>("preview", true));
    out["removed"] = removed;
    return out;
}

json waterOf(const std::string& terrain, const std::string& cmd, Terrain* outT = nullptr, Current* outC = nullptr)
{
    Graph& g = Graph::current();
    const Terrain t = findTerrain(g, terrain, cmd);
    Current c = currentWater(g, t);
    if (outT)
        *outT = t;
    json w = c.has ? worldWater(t, c) : json(nullptr);
    if (outC)
        *outC = std::move(c);
    return w;
}

std::string label(const Terrain& t)
{
    std::string s = t.mesh ? t.mesh->name() : std::string("Terrain");
    for (char& ch : s)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
            ch = '_';
    return s;
}

/** The terrain mesh's linkId, assigned on first use, which keys its water in Unreal across renames. */
std::string linkKey(const Terrain& t)
{
    if (!t.mesh)
        return t.modifier->path();
    const std::string* s = std::get_if<std::string>(&t.mesh->value(scene::Transform::kLinkId));
    if (s && !s->empty())
        return *s;
    const std::string fresh = std::format("{:016x}", std::hash<std::string>{}(t.mesh->path() + std::to_string(t.mesh->id())));
    Graph::current().setValue({t.mesh->id(), scene::Transform::kLinkId, -1}, fresh);
    return fresh;
}

void registerWaterCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainAddRiver",
        .doc = "Add a river or stream to a terrain, or replace the one with the same id: along path=, along a curve (object=, "
               "drawn or imported), or auto=True for every channel whose drainage passes threshold m2. Width and depth follow the "
               "drainage area (or width=, depth=); the bed falls all the way from source to mouth (a drawn line is turned to run "
               "downhill unless keepDirection=True), the channel and banks are carved and re-eroded (reErode steps) so they settle "
               "into the ground, gravel bars build inside bends, and where the ground drops steeper than cliffAngle by waterfallHeight "
               "or more it keeps a waterfall with a plunge pool below. meander adds bends a few widths wide on flat ground. A river "
               "ending inland gets a pond. Lives in the terrain's Terrain Water Features node, so it re-applies as the terrain "
               "changes. Returns its splines (per point width, depth, bedHeight, waterLevel, flowSpeed, discharge), locators "
               "(spring, waterfall top and bottom), every river and lake, warnings, cut and fill, and the revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags({{"path", "pa", T::List, nullptr, "The line [[x, z] or [x, y, z], ...] in world space (x, y on a Z-up scene)."},
                              nodeFlag({"object", "obj", T::String, nullptr, "A curve the river follows live; each of its curves is a river."}, "curve"),
                              {"auto", "au", T::Bool, false, "Every channel past threshold becomes a river."},
                              {"threshold", "th", T::Float, 1.0e6, "auto: drainage area, m2, past which a channel is a river.", {}, {}, 1.0},
                              {"minLength", "ml", T::Float, 0.0, "auto: leave out springs shorter than this, cm.", {}, {}, 0.0},
                              {"upstreamArea", "ua", T::Float, 0.0, "Drawn: catchment above its first point, m2, for a river entering from off the map.", {}, {}, 0.0},
                              {"width", "w", T::Float, 0.0, "Fixed width, cm; 0 follows drainage.", {}, {}, 0.0},
                              {"depth", "dp", T::Float, 0.0, "Fixed depth, cm; 0 follows drainage.", {}, {}, 0.0},
                              {"widthScale", "ws", T::Float, 1.0, "Scales the width drainage gives.", {}, {}, 0.0},
                              {"depthScale", "ds", T::Float, 1.0, "Scales the depth drainage gives.", {}, {}, 0.0},
                              {"minWidth", "mw", T::Float, 150.0, "Narrowest, cm.", {}, {}, 1.0},
                              {"minDepth", "md", T::Float, 40.0, "Shallowest, cm.", {}, {}, 1.0},
                              {"meander", "me", T::Float, 0.0, "Bend amplitude in widths; 0 is as drawn.", {}, {}, 0.0, 5.0},
                              {"meanderLength", "mle", T::Float, 14.0, "Bend wavelength in widths.", {}, {}, 2.0, 50.0},
                              {"seed", "sd", T::Int, int64_t(0), "Meander seed."},
                              {"bankAngle", "ba", T::Float, 30.0, "Bank slope from the water's edge, degrees.", {}, {}, 5.0, 85.0},
                              {"bankWidth", "bw", T::Float, 0.0, "How far out banks are cut, cm; 0 is four widths.", {}, {}, 0.0},
                              {"reErode", "re", T::Int, int64_t(6), "Erosion steps around the channel; 0 is off.", {}, {}, 0.0, 100.0},
                              {"gravelBars", "gb", T::Bool, true, "Gravel bars inside bends."},
                              {"waterfalls", "wf", T::Bool, true, "Keep waterfalls with plunge pools where it crosses a cliff."},
                              {"waterfallHeight", "wh", T::Float, 300.0, "Smallest drop that is a waterfall, cm.", {}, {}, 1.0},
                              {"cliffAngle", "ca", T::Float, 55.0, "Ground steeper than this along the river is a cliff, degrees.", {}, {}, 5.0, 89.0},
                              {"plungeDepth", "pd", T::Float, 0.4, "Plunge pool depth as a fraction of the drop.", {}, {}, 0.0, 2.0},
                              {"flowSpeed", "fs", T::Float, 0.0, "Fixed flow speed, m/s; 0 works it out from depth and slope.", {}, {}, 0.0},
                              {"keepDirection", "kd", T::Bool, false, "Keep a drawn line's direction even uphill."}}),
        .run = [](const Args& a) -> json { return addRiver(a); },
        .examples = {"terrainAddRiver('Terrain', id='main', auto=True, threshold=2e6)",
                     "terrainAddRiver('Terrain', id='mill', path=[[-20000, 0], [0, 4000], [20000, 0]], meander=1.5)",
                     "terrainAddRiver('Terrain', id='creek', object='CreekCurve', width=300, depth=60)"},
    });

    ctx.registerCommand({
        .name = "terrainAddLake",
        .doc = "Add a lake or pond to a terrain, or replace the one with the same id. at= fills the basin under that point to its "
               "spill level (or to level=); polygon= or object= (a closed curve) digs a basin inside the outline, depth below the "
               "level with the bed sloping at shoreSlope from the shore, the level being the lowest ground around it unless given, "
               "and a higher level raises a dam round it (dam=False lowers the level instead and says so); auto=True makes a lake of "
               "every basin at least minDepth deep and minArea m2. Returns its shoreline spline, level locator, every river and "
               "lake (level, spillHeight, area m2, maxDepth, volume m3), warnings and the revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = commonFlags({{"at", "at", T::List, nullptr, "A point in the basin, [x, z] or [x, y, z]."},
                              {"polygon", "pg", T::List, nullptr, "The lake's outline [[x, z], ...]."},
                              nodeFlag({"object", "obj", T::String, nullptr, "A closed curve outlining the lake, followed live."}, "curve"),
                              {"auto", "au", T::Bool, false, "Every basin past minDepth and minArea becomes a lake."},
                              {"level", "lv", T::Float, nullptr, "Water level along the up axis, cm; default the spill level."},
                              {"minDepth", "mnd", T::Float, 50.0, "auto: shallowest basin, cm.", {}, {}, 0.0},
                              {"minArea", "ma", T::Float, 2000.0, "auto: smallest lake, m2.", {}, {}, 0.0},
                              {"depth", "dp", T::Float, 300.0, "Outlined lake: depth dug below the level, cm.", {}, {}, 0.0},
                              {"shoreSlope", "ss", T::Float, 12.0, "Outlined lake: bed slope from the shore, degrees.", {}, {}, 1.0, 80.0},
                              {"dam", "dm", T::Bool, true, "Outlined lake: raise a dam when the level is above the ground around it."}}),
        .run = [](const Args& a) -> json { return addLake(a); },
        .examples = {"terrainAddLake('Terrain', id='tarn', at=[12000, -8000])", "terrainAddLake('Terrain', id='millPond', polygon=[[0, 0], [3000, 0], [3000, 2000], [0, 2000]], depth=250)",
                     "terrainAddLake('Terrain', id='lakes', auto=True, minDepth=100)"},
    });

    ctx.registerCommand({
        .name = "terrainSeaLevel",
        .doc = "Set a terrain's sea level (ground below it and joined to the edge is sea) and its shore settings: shoreWidth cm of "
               "shore mask round lakes and sea, beaches where that shore is no steeper than beachSlope degrees and at most beachHeight "
               "cm above the water. drainPits fills basins no water holds so every sample drains (on by default). off=True removes "
               "the sea. Returns the sea's shoreline and level locator, every river and lake, and the revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"level", "lv", T::Float, nullptr, "Sea level along the up axis, cm."},
                  {"off", "of", T::Bool, false, "No sea."},
                  {"shoreWidth", "sw", T::Float, 1500.0, "Shore band, cm.", {}, {}, 0.0},
                  {"beachSlope", "bs", T::Float, 10.0, "Steepest beach, degrees.", {}, {}, 0.0, 90.0},
                  {"beachHeight", "bh", T::Float, 300.0, "Highest beach above the water, cm.", {}, {}, 0.0},
                  {"drainPits", "dpt", T::Bool, true, "Fill basins no water holds."},
                  {"preview", "pv", T::Bool, true, "Rebuild the flat water preview mesh <terrain>_water."}},
        .run = [](const Args& a) -> json { return seaLevel(a); },
        .examples = {"terrainSeaLevel('Terrain', level=0, beachSlope=8)"},
    });

    ctx.registerCommand({
        .name = "terrainWaterRemove",
        .doc = "Remove rivers and lakes from a terrain by id, or all=True to drop its water node altogether. Returns what was "
               "removed and what is left.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"ids", "ids", T::List, nullptr, "River and lake ids."},
                  {"all", "al", T::Bool, false, "Remove all the water, sea included."},
                  {"preview", "pv", T::Bool, true, "Rebuild the flat water preview mesh."}},
        .run = [](const Args& a) -> json { return waterRemove(a); },
        .examples = {"terrainWaterRemove('Terrain', ids=['mill'])"},
    });

    ctx.registerCommand({
        .name = "terrainWater",
        .doc = "A terrain's authored water: the river and lake specs, and what they made in world space (cm): splines (rivers "
               "with per point width, depth, bedHeight, waterLevel, flowSpeed, discharge and flowDirection; lake and sea "
               "shorelines) and locators (lakeLevel, seaLevel, spring, waterfallTop, waterfallBottom), with counts, volumes and "
               "warnings. For the water the ground itself implies, see terrainWaterInfo.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainWater: give one terrain");
            Terrain t;
            Current c;
            json w = waterOf(names[0], "terrainWater", &t, &c);
            json out = stats(t, c);
            json rivers = json::array(), lakes = json::array();
            for (const RiverSpec& r : c.spec.rivers)
                rivers.push_back(r.toJson());
            for (const LakeSpec& l : c.spec.lakes)
                lakes.push_back(l.toJson());
            out["features"] = {{"rivers", rivers}, {"lakes", lakes}, {"settings", c.spec.settings.toJson()}};
            out["water"] = w;
            out["revision"] = revision(*t.local);
            return out;
        },
        .examples = {"w = terrainWater('Terrain')\n[(s['id'], len(s['points'])) for s in w['water']['splines']]"},
    });

    ctx.registerCommand({
        .name = "terrainWaterPreview",
        .doc = "Rebuild the flat preview of a terrain's water, the mesh <terrain>_water: lake and sea cells at their level and a "
               "ribbon along each river at its water level. Water commands rebuild it themselves; this is for after other edits.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Terrain t = oneTerrain(a, "terrainWaterPreview");
            const Current c = currentWater(g, t);
            const std::string path = refreshPreview(g, t, c.result);
            return {{"mesh", path.empty() ? json(nullptr) : json(path)}};
        },
        .examples = {"terrainWaterPreview('Terrain')"},
    });

    ctx.registerCommand({
        .name = "terrainExportWater",
        .doc = "Write a terrain's water to <name>_water.json: splines with per point width, depth, bedHeight, waterLevel, "
               "flowSpeed, discharge and flowDirection, and locators for lake and sea levels, springs and waterfall tops and "
               "bottoms, in world space, cm. Any water system can be driven from it. terrainExport writes it too.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"folder", "f", T::String, "", "Output folder; defaults to the bake folder's export/."},
                  {"name", "n", T::String, "", "File name stem; defaults to the terrain's name."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainExportWater: give one terrain");
            fs::path folder = a.get<std::string>("folder", "");
            if (folder.empty())
                folder = exportDir();
            Terrain t = findTerrain(Graph::current(), names[0], "terrainExportWater");
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = label(t);
            json out = exportTerrainWater(names[0], folder, name);
            if (out.is_null())
                throw CommandError("terrainExportWater: " + names[0] + " has no water (add some with terrainAddRiver, terrainAddLake or terrainSeaLevel)");
            return out;
        },
        .examples = {"terrainExportWater('Terrain', folder='export')['file']"},
    });

    ctx.registerCommand({
        .name = "terrainSendWaterToUnreal",
        .doc = "Send a terrain's water to the connected Unreal editor as plain actors: a spline actor per river and shoreline "
               "(a USplineComponent with a LilaWaterData component holding per point width, depth, water level, bed height, flow "
               "speed and direction) and a locator actor per lake level, sea level, spring and waterfall top and bottom. Never "
               "Water plugin bodies, so any water system can be driven from them. Keyed by the terrain's linkId: sending again "
               "updates them in place and removes what is gone. terrainSendToUnreal sends them too.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"timeout", "to", T::Float, 120.0, "Seconds to wait for Unreal.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSendWaterToUnreal: give one terrain");
            try
            {
                json out = sendTerrainWater(names[0], a.get<double>("timeout", 120.0));
                if (out.is_null())
                    throw CommandError("terrainSendWaterToUnreal: " + names[0] + " has no water");
                return out;
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendWaterToUnreal: ") + e.what());
            }
        },
        .examples = {"terrainSendWaterToUnreal('Terrain')"},
    });
}
} // namespace water_cmd

json exportTerrainWater(const std::string& terrain, const std::filesystem::path& folder, const std::string& name)
{
    using namespace water_cmd;
    water_cmd::Terrain t;
    Current c;
    const json w = waterOf(terrain, "terrainExport", &t, &c);
    if (w.is_null() || (w["splines"].empty() && w["locators"].empty()))
        return nullptr;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path file = folder / (name + "_water.json");
    std::ofstream f(file, std::ios::binary);
    if (!f)
        throw std::runtime_error("can't write " + file.string());
    f << w.dump(1);
    return {{"file", file.generic_string()}, {"splines", w["splines"].size()}, {"locators", w["locators"].size()}};
}

json sendTerrainWater(const std::string& terrain, double timeoutSeconds)
{
    using namespace water_cmd;
    water_cmd::Terrain t;
    Current c;
    const json w = waterOf(terrain, "terrainSendWaterToUnreal", &t, &c);
    if (w.is_null())
        return nullptr;
    link::Link::get().ensureConnected();
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    const json features = link::Link::get().status().value("features", json::array());
    if (!client || !features.is_array() || std::find(features.begin(), features.end(), json("terrainWater")) == features.end())
        throw std::runtime_error("the LilaLink plugin in Unreal predates terrain water; rebuild it from Lila's ue/LilaLink");
    const json params = unrealWaterParams(w, linkKey(t), label(t));
    const int timeoutMs = int(std::clamp(timeoutSeconds, 1.0, 3600.0) * 1000.0);
    link::RpcResult r = client->call("setWater", params, timeoutMs);
    if (!r.ok)
        throw std::runtime_error(r.error);
    json out = r.result;
    out["splines"] = params["splines"].size();
    out["locators"] = params["locators"].size();
    return out;
}

void registerWater(ext::Context& ctx)
{
    if (!gn::registerDecl(ctx, water_cmd::waterDecl()))
        log::warn("Terrain: the Terrain Water Features node didn't register");
    water_cmd::registerWaterCommands(ctx);
}
} // namespace terrain
