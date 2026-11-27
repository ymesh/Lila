// The declarative terrain spec: one JSON builds a whole terrain by calling the terrain commands in order (source,
// constraints, sculpt, erosion, landforms, water, regions, roads, stamps, volume, biomes, materials, ground cover,
// check, export), and terrainSpec(t) reads a terrain back as a spec that rebuilds it. Presets are saved specs.

#include "Spec.h"
#include "Stamps.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mcp.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/SpecSteps.h>

#include <chrono>
#include <fstream>
#include <sstream>

using namespace lila;

namespace terrain
{
namespace spec_
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;

constexpr const char* kSpecAttr = "terrainSpec";
constexpr const char* kSpecGraphAttr = "terrainSpecGraph";

fs::path gPresetDir;

/** Every key a spec may have, in the order the build runs them. */
const std::vector<std::string>& specKeys()
{
    static const std::vector<std::string> k = {"name",  "size",     "resolution", "seed",    "center",  "source",  "constraints", "sculpt",
                                               "erosion", "landforms", "water",    "regions", "roads",   "stamps",  "volume",      "cliffs",      "biomes",
                                               "materials", "groundCover", "check", "export",  "graph",   "inputs",  "transform",   "description"};
    return k;
}

json schemaJson()
{
    auto obj = [](const char* d) { return json{{"type", "object"}, {"description", d}}; };
    auto list = [](const char* d) { return json{{"type", "array"}, {"items", {{"type", "object"}}}, {"description", d}}; };
    auto flagsOr = [](const char* d) { return json{{"type", {"object", "boolean"}}, {"description", d}}; };
    json props = {
        {"name", {{"type", "string"}, {"default", "Terrain"}, {"description", "Name of the terrain mesh."}}},
        {"description", {{"type", "string"}, {"description", "What the terrain is, in words. Kept with the spec; not read by the build."}}},
        {"size", {{"type", "array"}, {"items", {{"type", "number"}}}, {"default", {200000, 200000}}, {"description", "[width, depth] on the ground plane, cm."}}},
        {"resolution", {{"type", "integer"}, {"default", 513}, {"description", "Samples along the longer side (noise and flat sources). Build at 257 or 513 to iterate."}}},
        {"seed", {{"type", "integer"}, {"default", 0}, {"description", "Seed for the noise and the erosion preset, unless their own seed is given."}}},
        {"center", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "Centre [x, y, z]; its up coordinate is the base height."}}},
        {"source",
         {{"type", "object"},
          {"description", "Where the heights start. type noise (terrainCreate flags: amplitude, featureSize, octaves, roughness, ridged, warp), "
                          "flat, image (terrainImport flags: file, mode guide|height|uplift|mask|sketch, low, high, erosion; spans size[0]) or "
                          "dem (terrainImportDem flags: place, bbox or file, source, projection, cellSize, bareEarth, amplify) or existing (mesh: a terrain already in the scene, such as a World Remix; the "
                          "steps run on it and nothing is created)."},
          {"properties", {{"type", {{"type", "string"}, {"enum", {"noise", "flat", "image", "dem", "existing"}}, {"default", "noise"}}}}}}},
        {"constraints", list("terrainConstrain calls: {kind: ridge|valley|cliff|river|flat, path, height, width, strength, id}. Held while erosion runs.")},
        {"sculpt", list("terrainStroke calls: {brush, points|path, radius, strength, stage, ...}. stage base (the default) erodes with the terrain.")},
        {"erosion", obj("terrainErode flags: {preset: natural|alpine|desert|hills, age: 0 to 1, iterations, device, seed, hardnessMask}.")},
        {"landforms", list("terrainLandform calls: {process: debris|thermal|hydraulic|deposition|snow|filter|terrace|strata|fold|warp|ridgeSharpen|cliff|amplify, settings, id, stage}.")},
        {"water",
         {{"type", "object"},
          {"description", "Water after erosion."},
          {"properties",
           {{"sea", obj("terrainSeaLevel flags: {level, shoreWidth, beachSlope, beachHeight, drainPits}.")},
            {"rivers", list("terrainAddRiver calls: {id, path | auto: true, threshold, width, depth, meander, ...}. Points may be 'region:<name>'.")},
            {"lakes", list("terrainAddLake calls: {id, at | polygon | auto: true, level, depth, ...}. at and polygon may be 'region:<name>'.")}}}}},
        {"regions", list("terrainQuery calls stored as named regions: {name, named: valleyFloor|passes|highestPeak|..., where: {...}, minArea, pick}. "
                         "Later steps refer to them as 'region:<name>'.")},
        {"roads", list("terrainAddRoad calls: {id, path, kind, profile, maxGrade, ...}; or {id, route: {start, end, maxGrade, weights}, kind, ...} "
                       "to find the way with terrainRoute. Path points, start and end may be 'region:<name>'.")},
        {"stamps", list("Footprint stamps: {type: pad|region|feature|quay, ...flags of terrainStampPad, terrainStampRegion, terrainAddFeature or terrainStampQuay}.")},
        {"volume", list("Volume edits: {op: cave|tunnel|overhang|arch|stamp|split, ...flags of terrainCave, terrainTunnel, terrainOverhang, terrainArch, "
                        "terrainVolumeStamp or terrainSplit}. Path points may be 'region:<name>'.")},
        {"cliffs",
         {{"type", {"object", "boolean"}},
          {"description", "Cliffs after the volume: terrainCliffs flags ({slope, minArea, minHeight, mergeDistance, strataDip}) plus mesh (true, or "
                          "terrainCliffMesh flags: {region, mode: proud|inset, depth, strataHeight, undercut, ledges, noise, overhang, ...}), "
                          "extrude (a list of terrainCliffExtrude calls: {curve | region, height, profile, ...}) and kits (a list of "
                          "terrainCliffKit calls: {region, kit, ...}); or true to find the cliffs and mesh them with the defaults. Later "
                          "steps refer to them as 'region:cliff_1', ..."}}},
        {"biomes", flagsOr("terrainBiomes flags, or true for the defaults.")},
        {"materials", flagsOr("terrainMaterialRules flags ({layers, set, seaLevel, seed}), or true for layers fitted to the terrain. Also sets the viewport's paint layers.")},
        {"groundCover", flagsOr("terrainGroundCover flags ({rules, items, density, seed}), or true for the default rules with placeholders.")},
        {"check", flagsOr("terrainCheck flags, or false to skip. Runs by default; the result's ok says whether the terrain passed.")},
        {"export", {{"type", {"object", "array"}}, {"description", "terrainExport flags {target: unreal|unity|godot|files|mesh, folder, ...}, or a list of them."}}},
        {"graph", obj("Read back only: the terrain's node group as saved (gnBuildGroup's format). With graph, the build restores it instead of running steps.")},
        {"inputs", obj("Read back only, with graph: the modifier's input values by name.")},
        {"transform", obj("Read back only, with graph: {translate, rotate, scale} of the mesh.")},
    };
    // Keys other extensions add (Seamwork's seams), run after every step above.
    for (const lila::specs::Step* s : lila::specs::stepsFor("terrain"))
        props[s->key] = s->schema;
    return {{"$schema", "http://json-schema.org/draft-07/schema#"},
            {"$id", "lila://schema/terrain-spec"},
            {"title", "Lila terrain spec"},
            {"description", "One JSON that builds a whole terrain with terrainSpec(spec=...). Lengths and heights are cm in world space; points are [x, z] "
                            "on the ground ([x, y] on a Z-up scene) or [x, y, z]. Every step is the flags of one terrain command, so that command's doc "
                            "(help(lila.ops.<command>)) is the full reference. Steps run in the order of the keys listed here."},
            {"type", "object"},
            {"properties", props},
            {"additionalProperties", false}};
}

json loadJsonFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw CommandError("terrainSpec: cannot read " + p.generic_string());
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded())
        throw CommandError("terrainSpec: " + p.generic_string() + " is not JSON");
    return j;
}

uint64_t fnv(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

/** Big arrays become their count, so a build's step results stay readable. */
json summarize(const json& j, int depth = 0)
{
    if (j.is_object())
    {
        if (depth > 4)
            return {{"keys", int64_t(j.size())}};
        json out = json::object();
        for (const auto& [k, v] : j.items())
            out[k] = summarize(v, depth + 1);
        return out;
    }
    if (j.is_array())
    {
        if (j.size() > 8 || depth > 4)
            return {{"count", int64_t(j.size())}};
        json out = json::array();
        for (const json& v : j)
            out.push_back(summarize(v, depth + 1));
        return out;
    }
    return j;
}

Node* nodeOf(const std::string& path) { return path.empty() ? nullptr : Graph::current().find(path); }

std::string stringAttr(const std::string& node, const char* attr)
{
    const Node* n = nodeOf(node);
    if (!n)
        return {};
    const int i = n->type().attrIndex(attr);
    if (i < 0)
        return {};
    const scene::PlugValue& v = n->value(i);
    return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string();
}

void setStringAttr(const std::string& node, const char* attr, const std::string& value)
{
    const Node* n = nodeOf(node);
    if (!n)
        return;
    if (n->type().attrIndex(attr) < 0)
        ops::execute("addAttr", {{"args", {node}}, {"longName", attr}, {"dataType", "string"}, {"hidden", true}});
    ops::execute("setAttr", {{"args", {node + "." + attr, value}}});
}

/** The group the terrain's modifier runs, read; null node when there is none. */
std::pair<Node*, gn::Group> terrainGroupOf(const TerrainRef& t)
{
    Graph& g = Graph::current();
    Node* mod = nodeOf(t.modifier);
    Node* node = mod ? gn::groupOfModifier(g, *mod) : nullptr;
    if (!node)
        return {nullptr, {}};
    return {node, gn::readGroup(*node)};
}

std::string graphHash(const TerrainRef& t)
{
    auto [node, grp] = terrainGroupOf(t);
    if (!node)
        return {};
    json j = grp.toJson();
    j.erase("name");
    return std::to_string(fnv(j.dump()));
}

/** The flags of one step, minus the keys the spec uses to pick the command. */
json callOf(const std::string& terrain, json flags, std::initializer_list<const char*> drop)
{
    for (const char* k : drop)
        flags.erase(k);
    flags["args"] = json::array({terrain});
    return flags;
}

/** One build: runs steps, records each one's result, stops at the first failure when asked. */
class Builder
{
public:
    Builder(bool stopOnError, bool full) : mStop(stopOnError), mFull(full) {}

    /** Runs another extension's spec step (lila::specs::Step) like a command. */
    bool runStep(const lila::specs::Step& s, const std::string& target, const json& value)
    {
        if (mFailed && mStop)
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        json entry = {{"step", s.key}, {"command", s.key}};
        try
        {
            json r = s.run(target, value);
            entry["ok"] = true;
            entry["result"] = mFull ? r : summarize(r);
            if (r.is_object() && r.contains("warnings") && r["warnings"].is_array() && !r["warnings"].empty())
                entry["warnings"] = r["warnings"];
        }
        catch (const std::exception& e)
        {
            entry["ok"] = false;
            entry["error"] = e.what();
            entry["call"] = value;
            mFailed = true;
            if (mFirstError.is_null())
                mFirstError = {{"step", s.key}, {"command", s.key}, {"error", e.what()}};
            log::warn("terrainSpec: {} failed: {}", s.key, e.what());
        }
        entry["ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        mSteps.push_back(entry);
        return entry["ok"].get<bool>();
    }

    /** Runs one command; false when it failed (and the build should stop). */
    bool run(const std::string& step, const std::string& command, const json& call, json* result = nullptr)
    {
        if (mFailed && mStop)
            return false;
        const auto t0 = std::chrono::steady_clock::now();
        json entry = {{"step", step}, {"command", command}};
        try
        {
            json r = ops::execute(command, call);
            entry["ok"] = true;
            entry["result"] = mFull ? r : summarize(r);
            if (r.is_object() && r.contains("warnings") && r["warnings"].is_array() && !r["warnings"].empty())
                entry["warnings"] = r["warnings"];
            if (result)
                *result = std::move(r);
        }
        catch (const std::exception& e)
        {
            entry["ok"] = false;
            entry["error"] = e.what();
            entry["call"] = call;
            mFailed = true;
            if (mFirstError.is_null())
                mFirstError = {{"step", step}, {"command", command}, {"error", e.what()}};
            log::warn("terrainSpec: {} ({}) failed: {}", step, command, e.what());
        }
        entry["ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        mSteps.push_back(entry);
        return entry["ok"].get<bool>();
    }

    bool failed() const { return mFailed; }
    bool stopped() const { return mFailed && mStop; }
    json steps() const { return mSteps; }
    json firstError() const { return mFirstError; }

private:
    bool mStop, mFull;
    bool mFailed = false;
    json mSteps = json::array();
    json mFirstError;
};

json asList(const json& j) { return j.is_array() ? j : j.is_object() ? json::array({j}) : json::array(); }

/** Each entry of a list step, checked to be an object. */
json entries(const json& spec, const char* key)
{
    const json v = spec.value(key, json());
    if (v.is_null())
        return json::array();
    if (!v.is_array())
        throw CommandError(std::string("terrainSpec: ") + key + " is a list of steps");
    for (const json& e : v)
        if (!e.is_object())
            throw CommandError(std::string("terrainSpec: every entry of ") + key + " is a dict of flags");
    return v;
}

json createCall(const json& spec)
{
    const json source = spec.value("source", json{{"type", "noise"}});
    if (!source.is_object())
        throw CommandError("terrainSpec: source is a dict with a type: noise, flat, image, dem or existing");
    const std::string type = source.value("type", std::string("noise"));
    if (type == "existing")
    {
        if (!source.contains("mesh") || !source["mesh"].is_string())
            throw CommandError("terrainSpec: an existing source needs mesh (the terrain the steps run on)");
        return {{"command", ""}, {"mesh", source["mesh"]}};
    }
    const std::string name = spec.value("name", std::string("Terrain"));
    json size = spec.value("size", json::array({200000.0, 200000.0}));
    if (!size.is_array() || size.size() != 2)
        throw CommandError("terrainSpec: size is [width, depth], cm");
    json call = source;
    call.erase("type");
    call["name"] = name;
    if (spec.contains("center"))
        call["center"] = spec["center"];
    if (type == "noise" || type == "flat")
    {
        call["size"] = size;
        call["resolution"] = spec.value("resolution", 513);
        call["seed"] = source.value("seed", spec.value("seed", 0));
        if (type == "flat")
            call["flat"] = true;
        return {{"command", "terrainCreate"}, {"call", call}};
    }
    if (type == "image")
    {
        if (!call.contains("file"))
            throw CommandError("terrainSpec: an image source needs file");
        call["args"] = json::array({call["file"]});
        call.erase("file");
        if (!call.contains("size"))
            call["size"] = size[0];
        return {{"command", "terrainImport"}, {"call", call}};
    }
    if (type == "dem")
    {
        if (call.contains("file"))
        {
            call["args"] = json::array({call["file"]});
            call.erase("file");
        }
        else if (!call.contains("size") && call.contains("place"))
            call["size"] = size;
        return {{"command", "terrainImportDem"}, {"call", call}};
    }
    throw CommandError("terrainSpec: source type is noise, flat, image, dem or existing, not " + type);
}

bool enabled(const json& spec, const char* key, bool byDefault)
{
    if (!spec.contains(key))
        return byDefault;
    const json& v = spec[key];
    return v.is_boolean() ? v.get<bool>() : !v.is_null();
}

json flagsOf(const json& spec, const char* key) { return spec.contains(key) && spec[key].is_object() ? spec[key] : json::object(); }

/** The graph path: a terrain whose group, inputs and placement are restored as read back. */
json buildFromGraph(const json& spec, Builder& b, std::string& mesh)
{
    json made;
    if (!b.run("create", "terrainCreate", {{"name", spec.value("name", std::string("Terrain"))}, {"flat", true}, {"resolution", 3}}, &made))
        return made;
    mesh = made["mesh"].get<std::string>();
    const TerrainRef t = findTerrainRef(mesh, "terrainSpec");
    auto [node, current] = terrainGroupOf(t);
    gn::Group next;
    try
    {
        next = gn::Group::fromJson(spec["graph"]);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("terrainSpec: graph is not a node group: ") + e.what());
    }
    next.name = current.name;
    gn::commitGroup(Graph::current(), *node, std::move(next));
    json notes = json::array();
    const json inputs = spec.value("inputs", json::object());
    for (const auto& [name, value] : inputs.items())
        try
        {
            ops::execute("setAttr", {{"args", {t.modifier + "." + name, value}}});
        }
        catch (const std::exception& e)
        {
            notes.push_back("input " + name + " not set: " + e.what());
        }
    const json transform = spec.value("transform", json::object());
    for (const auto& [name, value] : transform.items())
        ops::execute("setAttr", {{"args", {mesh + "." + name, value}}});
    made["notes"] = notes;
    return made;
}

json buildSteps(const json& spec, Builder& b, std::string& mesh)
{
    const json create = createCall(spec);
    json made;
    if (create["command"] == "")
    {
        if (!Graph::current().find(create["mesh"].get<std::string>()))
            throw CommandError("terrainSpec: no terrain " + create["mesh"].get<std::string>() + " for the existing source");
        made = {{"mesh", Graph::current().find(create["mesh"].get<std::string>())->path()}};
    }
    else if (!b.run("source", create["command"], create["call"], &made))
        return made;
    mesh = made["mesh"].get<std::string>();
    const std::string& t = mesh;

    for (const json& c : entries(spec, "constraints"))
        b.run("constraints", "terrainConstrain", callOf(t, c, {}));
    for (const json& s : entries(spec, "sculpt"))
        b.run("sculpt", "terrainStroke", callOf(t, s, {}));
    if (spec.contains("erosion") && !spec["erosion"].is_null() && spec["erosion"] != false)
    {
        json e = flagsOf(spec, "erosion");
        if (!e.contains("seed") && spec.contains("seed"))
            e["seed"] = spec["seed"];
        b.run("erosion", "terrainErode", callOf(t, e, {}));
    }
    for (const json& l : entries(spec, "landforms"))
        b.run("landforms", "terrainLandform", callOf(t, l, {}));

    const json water = flagsOf(spec, "water");
    if (water.contains("sea") && water["sea"].is_object())
        b.run("water.sea", "terrainSeaLevel", callOf(t, water["sea"], {}));
    for (const json& r : asList(water.value("rivers", json())))
        b.run("water.rivers", "terrainAddRiver", callOf(t, r, {}));
    for (const json& l : asList(water.value("lakes", json())))
        b.run("water.lakes", "terrainAddLake", callOf(t, l, {}));

    for (const json& r : entries(spec, "regions"))
    {
        if (!r.contains("name") || !r["name"].is_string())
            throw CommandError("terrainSpec: every region needs a name");
        json call = callOf(t, r, {"name"});
        call["store"] = r["name"];
        b.run("regions." + r["name"].get<std::string>(), "terrainQuery", call);
    }

    for (const json& r : entries(spec, "roads"))
    {
        if (r.contains("route"))
        {
            const json route = r["route"];
            json call = callOf(t, route, {});
            call["road"] = r.value("kind", std::string("road"));
            call["roadId"] = r.value("id", std::string());
            json settings = r;
            for (const char* k : {"route", "kind", "id"})
                settings.erase(k);
            if (!settings.empty())
                call["roadSettings"] = settings;
            b.run("roads", "terrainRoute", call);
        }
        else
            b.run("roads", "terrainAddRoad", callOf(t, r, {}));
    }

    static const std::map<std::string, std::string> stampCommands = {
        {"pad", "terrainStampPad"}, {"region", "terrainStampRegion"}, {"feature", "terrainAddFeature"}, {"quay", "terrainStampQuay"}};
    for (const json& s : entries(spec, "stamps"))
    {
        auto it = stampCommands.find(s.value("type", std::string("pad")));
        if (it == stampCommands.end())
            throw CommandError("terrainSpec: a stamp's type is pad, region, feature or quay");
        b.run("stamps", it->second, callOf(t, s, {"type"}));
    }

    static const std::map<std::string, std::string> volumeCommands = {{"cave", "terrainCave"},       {"tunnel", "terrainTunnel"},
                                                                      {"overhang", "terrainOverhang"}, {"arch", "terrainArch"},
                                                                      {"stamp", "terrainVolumeStamp"}, {"split", "terrainSplit"}};
    for (const json& v : entries(spec, "volume"))
    {
        auto it = volumeCommands.find(v.value("op", std::string()));
        if (it == volumeCommands.end())
            throw CommandError("terrainSpec: a volume entry's op is cave, tunnel, overhang, arch, stamp or split");
        b.run("volume", it->second, callOf(t, v, {"op"}));
    }

    if (enabled(spec, "cliffs", false))
    {
        const json c = flagsOf(spec, "cliffs");
        if (b.run("cliffs", "terrainCliffs", callOf(t, c, {"mesh", "extrude", "kits"})))
        {
            const json meshFlags = c.value("mesh", json(c.empty()));
            if (meshFlags.is_object() || (meshFlags.is_boolean() && meshFlags.get<bool>()))
                b.run("cliffs.mesh", "terrainCliffMesh", callOf(t, meshFlags.is_object() ? meshFlags : json::object(), {}));
            for (const json& e : c.value("extrude", json::array()))
                b.run("cliffs.extrude", "terrainCliffExtrude", callOf(t, e, {}));
            for (const json& k : c.value("kits", json::array()))
                b.run("cliffs.kits", "terrainCliffKit", callOf(t, k, {}));
        }
    }
    if (enabled(spec, "biomes", false))
        b.run("biomes", "terrainBiomes", callOf(t, flagsOf(spec, "biomes"), {}));
    if (enabled(spec, "materials", false))
        b.run("materials", "terrainMaterialRules", callOf(t, flagsOf(spec, "materials"), {}));
    if (enabled(spec, "groundCover", false))
        b.run("groundCover", "terrainGroundCover", callOf(t, flagsOf(spec, "groundCover"), {}));
    return made;
}

json readGraphSpec(const TerrainRef& t, const std::string& name)
{
    auto [node, grp] = terrainGroupOf(t);
    if (!node)
        throw CommandError("terrainSpec: " + t.mesh + " has no node group to read back");
    json spec = {{"name", name}, {"graph", grp.toJson()}};
    json inputs = json::object();
    for (const gn::GInput& in : grp.inputs)
        try
        {
            inputs[in.name] = ops::execute("getAttr", {{"args", {t.modifier + "." + in.name}}});
        }
        catch (const std::exception&)
        {
        }
    spec["inputs"] = inputs;
    if (!t.mesh.empty())
    {
        json transform = json::object();
        for (const char* a : {"translate", "rotate", "scale"})
            try
            {
                transform[a] = ops::execute("getAttr", {{"args", {t.mesh + "." + a}}});
            }
            catch (const std::exception&)
            {
            }
        spec["transform"] = transform;
    }
    return spec;
}
} // namespace spec_

using namespace spec_;

const nlohmann::json& terrainSpecSchema()
{
    // Rebuilt each time: extensions add and remove keys as they load.
    static json s;
    s = schemaJson();
    return s;
}

std::vector<std::string> terrainSpecPresets()
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!gPresetDir.empty())
        for (const auto& e : fs::directory_iterator(gPresetDir, ec))
            if (e.path().extension() == ".json")
                out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

nlohmann::json terrainSpecPreset(const std::string& name)
{
    const fs::path p = gPresetDir / (name + ".json");
    std::error_code ec;
    if (gPresetDir.empty() || !fs::exists(p, ec))
    {
        std::string have;
        for (const std::string& n : terrainSpecPresets())
            have += (have.empty() ? "" : ", ") + n;
        throw CommandError("terrainSpec: no preset " + name + " (presets: " + (have.empty() ? "none installed" : have) + ")");
    }
    return loadJsonFile(p);
}

nlohmann::json mergeSpec(nlohmann::json base, const nlohmann::json& overrides)
{
    if (!overrides.is_object() || !base.is_object())
        return overrides.is_null() ? base : overrides;
    for (const auto& [k, v] : overrides.items())
        base[k] = base.contains(k) && base[k].is_object() && v.is_object() ? mergeSpec(base[k], v) : v;
    return base;
}

nlohmann::json buildTerrainSpec(const nlohmann::json& spec, bool replace, bool stopOnError, bool full)
{
    LILA_ZONE();

    if (!spec.is_object())
        throw CommandError("terrainSpec: spec is a dict (see lila://schema/terrain-spec)");
    const std::vector<const lila::specs::Step*> added = lila::specs::stepsFor("terrain");
    for (const auto& [k, v] : spec.items())
        if (std::find(specKeys().begin(), specKeys().end(), k) == specKeys().end() &&
            std::none_of(added.begin(), added.end(), [&](const lila::specs::Step* s) { return s->key == k; }))
            throw CommandError("terrainSpec: unknown key " + k + " (see lila://schema/terrain-spec)");
    const std::string name = spec.value("name", std::string("Terrain"));
    Graph& g = Graph::current();
    const bool existing = spec.contains("source") && spec["source"].is_object() && spec["source"].value("type", std::string()) == "existing";
    if (Node* old = existing ? nullptr : g.find(name))
    {
        if (!replace)
            throw CommandError("terrainSpec: there is a node named " + name + " already; replace=True rebuilds it, or give the spec another name");
        std::vector<std::string> gone = {old->path()};
        for (const std::string& extra : {name + "_water", name + "_GroundCover"})
            if (Node* n = g.find(extra))
                gone.push_back(n->path());
        ops::execute("delete", {{"args", gone}});
    }

    Builder b(stopOnError, full);
    std::string mesh;
    const bool fromGraph = spec.contains("graph");
    json made = fromGraph ? buildFromGraph(spec, b, mesh) : buildSteps(spec, b, mesh);
    json out = {{"terrain", mesh}, {"from", fromGraph ? "graph" : "steps"}};
    if (mesh.empty())
    {
        out["ok"] = false;
        out["steps"] = b.steps();
        out["failed"] = b.firstError();
        return out;
    }

    for (const lila::specs::Step* s : added)
        if (spec.contains(s->key) && !(spec[s->key].is_boolean() && !spec[s->key].get<bool>()))
            b.runStep(*s, mesh, spec[s->key]);

    json check;
    if (!b.stopped() && enabled(spec, "check", true))
        b.run("check", "terrainCheck", callOf(mesh, flagsOf(spec, "check"), {}), &check);
    json exports = json::array();
    for (const json& e : asList(spec.value("export", json())))
    {
        json r;
        if (b.run("export." + e.value("target", std::string("unreal")), "terrainExport", callOf(mesh, e, {}), &r))
            exports.push_back({{"target", e.value("target", std::string("unreal"))}, {"folder", r.value("folder", json())}, {"files", r.value("files", json())}});
    }

    const TerrainRef t = findTerrainRef(mesh, "terrainSpec");
    if (!t.mesh.empty())
    {
        setStringAttr(t.mesh, kSpecAttr, spec.dump());
        setStringAttr(t.mesh, kSpecGraphAttr, graphHash(t));
    }
    const json info = ops::execute("terrainInfo", {{"args", {mesh}}});
    json masks = json::array();
    for (const auto& [m, v] : info["masks"].items())
        masks.push_back(m);
    out["stats"] = {{"resolution", info["resolution"]}, {"size", info["size"]},     {"cellSize", info["cellSize"]}, {"heightRange", info["heightRange"]},
                    {"masks", masks},                   {"revision", info["revision"]}};
    if (!check.is_null())
        out["check"] = {{"ok", check.value("ok", false)}, {"issues", check.value("issues", json::array())}};
    if (!exports.empty())
        out["exports"] = exports;
    out["steps"] = b.steps();
    out["ok"] = !b.failed() && (check.is_null() || check.value("ok", false));
    if (b.failed())
        out["failed"] = b.firstError();
    if (made.contains("notes") && !made["notes"].empty())
        out["notes"] = made["notes"];
    out["revision"] = info["revision"];
    return out;
}

nlohmann::json readTerrainSpec(const std::string& terrain)
{
    const TerrainRef t = findTerrainRef(terrain, "terrainSpec");
    const std::string name = t.mesh.empty() ? std::string("Terrain") : nodeOf(t.mesh)->name();
    const std::string authored = stringAttr(t.mesh, kSpecAttr);
    json notes = json::array();
    if (!authored.empty())
    {
        json spec = json::parse(authored, nullptr, false);
        if (!spec.is_discarded() && stringAttr(t.mesh, kSpecGraphAttr) == graphHash(t))
        {
            spec["name"] = name;
            return {{"spec", spec}, {"from", "spec"}, {"notes", notes}, {"revision", revision(*t.local)}};
        }
        notes.push_back("the terrain changed after it was built from its spec, so this is its node group as it stands");
    }
    else
        notes.push_back("the terrain was not built from a spec, so this is its node group as it stands");
    notes.push_back("objects the group follows (curves, meshes for stamps, roads and rivers) must exist in the scene to rebuild it");
    return {{"spec", readGraphSpec(t, name)}, {"from", "graph"}, {"notes", notes}, {"revision", revision(*t.local)}};
}

namespace spec_
{
const char* kMakeTerrainPrompt = R"(Build a terrain in Lila from this description, check it, and export it.

Description: {{description}}
Size: {{size}}. Style: {{style}}. Target engine: {{engine}}.

1. Read lila://guide/terrain (the recipes at the top) and lila://schema/terrain-spec.
2. Start from the closest preset (terrainSpec(list=True) lists them; terrainSpec(preset='alpineValley', build=False) shows one) or write a spec from scratch. Lengths are cm: 1 km is 100000.
3. Name the places the description talks about as regions (terrainQuery shortcuts: valleyFloor, passes, highestPeak, ridges, cliffs, northSlopes, flatAreas...) and use 'region:<name>' for roads, lakes, caves and stamps, so you never guess coordinates.
4. Build at resolution 257 first: r = terrainSpec(spec=..., replace=True). Read r['steps'] (every step's ok, warnings, error) and r['check']. Fix the spec and rebuild until r['ok'] is true.
5. Look at it: terrainMap(r['terrain'], file=...) writes a shaded top-down map; terrainQuery and terrainSample answer questions about places. To compare options, terrainWedge(spec=..., vary={'seed': [1, 2, 3]}) builds variants side by side.
6. Raise resolution to the final value (1025 or more), rebuild, and run terrainCheck once more.
7. Export: add export={'target': '{{engine}}'} to the spec (or call terrainExport), and report the files, the check result and the stats.
)";

std::string fillPrompt(std::string text, const json& args)
{
    const std::pair<const char*, const char*> keys[] = {
        {"description", "the requested landscape"}, {"size", "2 km by 2 km"}, {"style", "natural, eroded"}, {"engine", "unreal"}};
    for (const auto& [key, fallback] : keys)
    {
        const char* argName = std::string(key) == "engine" ? "target" : key;
        std::string value = args.contains(argName) && args[argName].is_string() && !args[argName].get<std::string>().empty() ? args[argName].get<std::string>() : fallback;
        const std::string tag = std::string("{{") + key + "}}";
        for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
            text.replace(at, tag.size(), value);
    }
    return text;
}
} // namespace spec_

void registerSpec(ext::Context& ctx)
{
    gPresetDir = ctx.resources() / "specs";
    if (std::error_code ec; !fs::exists(gPresetDir, ec))
    {
        log::warn("Terrain: no spec presets at {}; terrainSpec(preset=) will have none", gPresetDir.generic_string());
        gPresetDir.clear();
    }

    ctx.registerCommand({
        .name = "terrainSpec",
        .doc = "Build a whole terrain from one JSON spec, or read a terrain back as one.\n\n"
               "spec= (or preset= with overrides=) runs the terrain commands in order: source (noise, flat, image, dem, or existing: an existing terrain the steps run on), "
               "constraints, sculpt, erosion, landforms, water (sea, rivers, lakes), regions (terrainQuery, stored by name for "
               "'region:<name>' references), roads, stamps, volume, biomes, materials, ground cover, keys other extensions add "
               "(seams, with Seamwork), check, export. Each step is "
               "that command's flags; lila://schema/terrain-spec lists them. Returns ok (every step ran and terrainCheck passed), "
               "the terrain, each step's command, result (big lists counted), warnings, error and time, the check, the exports "
               "and the final stats with the revision. A failed step stops the build unless stopOnError=False.\n\n"
               "With a terrain and no spec it returns a spec that rebuilds it: the spec it was built from while the terrain is "
               "unchanged since, else its node group, inputs and placement (from: graph). list=True lists the presets; "
               "build=False returns the preset or spec without building.",
        .positionalDoc = "[terrain mesh or its modifier, to read back]",
        .flags = {{"spec", "s", T::Any, nullptr, "The spec, a dict (see lila://schema/terrain-spec)."},
                  {"preset", "p", T::String, nullptr, "Start from a saved spec: alpineValley, mesaDesert, coastalCliffs, rollingFarmland, volcanicIsland."},
                  {"overrides", "o", T::Any, nullptr, "Merged over the preset or spec key by key: {'seed': 4, 'resolution': 257, 'erosion': {'age': 0.8}}."},
                  {"replace", "r", T::Bool, false, "Delete a node with the spec's name (and its water and ground cover) first."},
                  {"build", "b", T::Bool, true, "False returns the merged spec without building."},
                  {"stopOnError", "soe", T::Bool, true, "Stop at the first failed step."},
                  {"full", "fu", T::Bool, false, "Return every step's result whole instead of with big lists counted."},
                  {"list", "ls", T::Bool, false, "List the presets with their descriptions."}},
        .run = [](const Args& a) -> json {
            if (a.get<bool>("list", false))
            {
                json out = json::array();
                for (const std::string& n : terrainSpecPresets())
                    out.push_back({{"name", n}, {"description", terrainSpecPreset(n).value("description", std::string())}});
                return out;
            }
            if (!a.has("spec") && !a.has("preset"))
            {
                const std::vector<std::string> objects = a.objects();
                if (objects.size() != 1)
                    throw CommandError("terrainSpec: give spec=, preset=, or one terrain to read back");
                return readTerrainSpec(objects[0]);
            }
            json spec = a.has("preset") ? terrainSpecPreset(a.get<std::string>("preset", "")) : json::object();
            if (a.has("spec"))
                spec = mergeSpec(spec, a.flags["spec"]);
            if (a.has("overrides"))
                spec = mergeSpec(spec, a.flags["overrides"]);
            if (!a.get<bool>("build", true))
                return {{"spec", spec}};
            return buildTerrainSpec(spec, a.get<bool>("replace", false), a.get<bool>("stopOnError", true), a.get<bool>("full", false));
        },
        .examples = {"r = terrainSpec(preset='alpineValley', overrides={'seed': 3, 'resolution': 257})\nr['ok'], r['stats'], [s['step'] for s in r['steps']]",
                     "terrainSpec(spec={'name': 'Hills', 'size': [200000, 200000], 'resolution': 513, 'seed': 2,\n"
                     "    'source': {'type': 'noise', 'amplitude': 20000, 'ridged': 0.3},\n"
                     "    'erosion': {'preset': 'hills', 'age': 0.5},\n"
                     "    'regions': [{'name': 'valley', 'named': 'valleyFloor', 'minArea': 50000, 'pick': 0}],\n"
                     "    'water': {'lakes': [{'id': 'pond', 'at': 'region:valley'}]},\n"
                     "    'materials': True, 'export': {'target': 'unreal'}})",
                     "s = terrainSpec('Hills')['spec']\ns['seed'] = 5\nterrainSpec(spec=s, replace=True)"},
    });

    using mcp::Server;
    Server& s = Server::get();
    s.addResource(ctx, {.uri = "lila://schema/terrain-spec",
                        .name = "Terrain spec schema",
                        .description = "JSON schema of terrainSpec's spec: every key, the command each step calls and its flags. Read before writing a terrain spec.",
                        .mimeType = "application/json",
                        .read = [] { return terrainSpecSchema().dump(2); }});
    s.addResource(ctx, {.uri = "lila://terrain/spec-presets",
                        .name = "Terrain spec presets",
                        .description = "The saved terrain specs terrainSpec(preset=) starts from (alpine valley, mesa desert, coastal cliffs, farmland, volcanic island), as JSON.",
                        .mimeType = "application/json",
                        .read = [] {
                            json out = json::object();
                            for (const std::string& n : terrainSpecPresets())
                                out[n] = terrainSpecPreset(n);
                            return out.dump(2);
                        }});
    s.addPrompt(ctx, {.name = "make_terrain",
                      .description = "Build a terrain from one description: write a spec, build and check it, then export it for an engine.",
                      .arguments = json::array({
                          {{"name", "description"}, {"description", "The landscape, e.g. \"a river valley between two ranges with a road over the pass\""}, {"required", true}},
                          {{"name", "size"}, {"description", "Extent, e.g. \"4 km by 4 km\""}, {"required", false}},
                          {{"name", "style"}, {"description", "Look, e.g. \"alpine, glacial, very eroded\""}, {"required", false}},
                          {{"name", "target"}, {"description", "Engine to export for: unreal, unity, godot, files or mesh"}, {"required", false}},
                      }),
                      .get = [](const json& args) { return fillPrompt(kMakeTerrainPrompt, args); }});
}
} // namespace terrain
