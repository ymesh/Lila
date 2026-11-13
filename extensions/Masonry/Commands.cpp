// Masonry commands: masonryWall and masonryBuilding build geometry-nodes groups anyone can open (a Masonry Wall node fed
// by the mesh's footprint edges, or a Masonry Building node holding a spec); ruin and age set their ruin and age inputs,
// or ruin hand-placed stones directly; masonryInfo, masonrySpec and masonryCheck read them back; masonryShip sends them
// to Unreal.

#include "Masonry.h"
#include "MasonryBake.h"
#include "MasonryCore.h"
#include "MasonryShip.h"
#include "MasonryTools.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Paths.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>

#include <tbb/parallel_for.h>

using namespace lila;

namespace masonry
{
using json = nlohmann::json;

namespace
{
using scene::Graph;
using scene::Node;
using T = ArgType;

/** The stones' custom data slots as placeInstances names them (MasonryShip's customData order), for the holder's LilaInstanceData. */
json masonryDataSlots()
{
    json out = json::array();
    int slot = 0;
    for (const char* name : {"random", "height", "age", "damage", "wet", "moss"})
        out.push_back({{"name", name}, {"slot", slot++}, {"width", 1}});
    return out;
}

/** Flags of a set of inputs; with noDefaults every flag is unset unless given (for commands that change only what is given). */
std::vector<FlagDef> flagsOf(InputSet set, bool noDefaults = false)
{
    std::vector<FlagDef> out;
    for (const Input* in : inputsOf(set))
    {
        FlagDef f;
        f.name = in->name;
        f.doc = in->doc;
        switch (in->type)
        {
        case Input::Float:
            f.type = T::Float;
            f.defaultValue = in->def;
            break;
        case Input::Int:
            f.type = T::Int;
            f.defaultValue = int64_t(in->def);
            break;
        case Input::Bool:
            f.type = T::Bool;
            f.defaultValue = in->def != 0.0;
            break;
        case Input::String:
            // Lists may come as JSON values as well as text.
            f.type = std::string_view(in->name) == "bond" ? T::String : T::Any;
            f.defaultValue = std::string(in->text ? in->text : "");
            break;
        }
        if (std::string_view(in->name) == "bond")
            f.choices = bondNames();
        if (in->max > in->min && in->type != Input::String && in->type != Input::Bool)
        {
            f.min = in->min;
            f.max = in->max;
        }
        if (noDefaults)
            f.defaultValue = nullptr;
        out.push_back(std::move(f));
    }
    return out;
}

json valueOfFlag(const Input& in, const json& v)
{
    if (in.type == Input::String && !v.is_string() && !v.is_null())
    {
        if (std::string_view(in.name) == "ruinRegions" && v.is_array())
        {
            std::string joined;
            for (const json& r : v)
                joined += (joined.empty() ? "" : ",") + (r.is_string() ? r.get<std::string>() : r.dump());
            return joined;
        }
        return v.dump();
    }
    return typedValue(in, v);
}

/** Every input of the set, from the flags given, else from fallback (a modifier's current values), else the default. */
json valuesFromArgs(const Args& a, InputSet set, const json& fallback = json::object())
{
    json v = json::object();
    for (const Input* in : inputsOf(set))
        if (a.has(in->name))
            v[in->name] = valueOfFlag(*in, a.flags[in->name]);
        else if (fallback.contains(in->name))
            v[in->name] = typedValue(*in, fallback[in->name]);
        else
            v[in->name] = typedValue(*in, json());
    return v;
}

size_t triangles(const geo::Mesh& m)
{
    size_t n = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            n += size_t(std::max<geo::Index>(m.faceSize(f), 2) - 2);
    return n;
}

/** What a build is made of, for agents and the status line. */
json statsOf(const Result& r, size_t walls)
{
    const Built& b = r.built;
    size_t stoneTris = 0, uniqueTris = 0;
    std::vector<size_t> perMesh(b.meshes.size());
    for (size_t m = 0; m < b.meshes.size(); ++m)
    {
        perMesh[m] = triangles(b.meshes[m]);
        uniqueTris += perMesh[m];
    }
    for (int m : b.reference)
        stoneTris += perMesh[size_t(m)];
    json roles = json::object();
    for (int k = 0; k < kRoleCount; ++k)
        roles[roleName(Role(k))] = 0;
    geo::Bounds bounds;
    std::vector<int64_t> perRegion(b.regions.size(), 0);
    for (const Stone& s : b.stones)
    {
        roles[roleName(s.role)] = roles[roleName(s.role)].get<int64_t>() + 1;
        const glm::dvec3 e = halfExtentAabb(s);
        bounds.extend(s.center - e);
        bounds.extend(s.center + e);
        if (s.region >= 0 && size_t(s.region) < perRegion.size())
            ++perRegion[size_t(s.region)];
    }
    json regions = json::object();
    for (size_t k = 0; k < b.regions.size(); ++k)
        regions[b.regions[k]] = perRegion[k];
    return {{"walls", int64_t(walls)},
            {"stones", int64_t(b.stones.size())},
            {"courses", b.courses},
            {"roles", roles},
            {"regions", regions},
            {"stoneMeshes", int64_t(b.meshes.size())},
            {"trimmed", b.trimmed},
            {"dropped", b.dropped},
            {"stoneTriangles", int64_t(stoneTris)},
            {"stoneMeshTriangles", int64_t(uniqueTris)},
            {"coreTriangles", int64_t(triangles(b.core))},
            {"collisionHulls", int64_t(b.coreHulls.size())},
            {"ruin", r.ruin.toJson()},
            {"bounds", bounds.valid() ? json{{"min", {bounds.min.x, bounds.min.y, bounds.min.z}}, {"max", {bounds.max.x, bounds.max.y, bounds.max.z}}} : json()},
            {"boundsMin", bounds.valid() ? json::array({bounds.min.x, bounds.min.y, bounds.min.z}) : json()},
            {"boundsMax", bounds.valid() ? json::array({bounds.max.x, bounds.max.y, bounds.max.z}) : json()}};
}

/** A masonry node in a mesh's stack (or the node itself): Masonry Wall or Masonry Building. */
bool isMasonry(const Node& m) { return (m.type().attrIndex("stoneLength") >= 0 && m.type().attrIndex("bond") >= 0) || m.type().attrIndex("spec") >= 0; }
bool isBuilding(const Node& m) { return m.type().attrIndex("spec") >= 0; }

Node* findMasonry(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n)
        return nullptr;
    if (isMasonry(*n))
        return n;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && isMasonry(*mod))
            return mod;
    return nullptr;
}

Node* masonryModifier(Graph& g, const std::string& name, const char* cmd)
{
    if (!g.find(name))
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (Node* n = findMasonry(g, name))
        return n;
    throw CommandError(std::string(cmd) + ": " + g.find(name)->path() + " has no masonry (make it with masonryWall or masonryBuilding)");
}

json modifierValues(const Node& mod)
{
    json values = json::object();
    for (const Input* in : inputsOf(isBuilding(mod) ? InputSet::Building : InputSet::Wall))
        values[in->name] = typedValue(*in, ops::execute("getAttr", {{"args", {mod.path() + "." + in->name}}}));
    return values;
}

/** A wall or building named on the command line: its modifier, the mesh holding it, its footprints and values. */
struct WallRef
{
    Node* mod = nullptr;
    Node* mesh = nullptr;
    std::vector<Footprint> footprints;
    json values;
};

WallRef refOf(Graph& g, const std::string& name, const char* cmd)
{
    WallRef w;
    w.mod = masonryModifier(g, name, cmd);
    const scene::NodeId meshId = scene::modifiers::meshOf(g, w.mod->id());
    w.mesh = meshId != scene::kNoNode ? g.node(meshId) : g.find(name);
    if (!isBuilding(*w.mod))
        if (auto m = w.mesh ? scene::modifiers::cage(g, w.mesh->id()) : nullptr)
            w.footprints = footprintsFromEdges(*m);
    w.values = modifierValues(*w.mod);
    return w;
}

WallRef wallRef(Graph& g, const Args& a, const char* cmd)
{
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one mesh");
    return refOf(g, names[0], cmd);
}

std::shared_ptr<const Result> buildOf(const WallRef& w, bool lods, const char* cmd)
{
    try
    {
        return buildMasonry(w.footprints, w.values, lods);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

std::string uniqueGroupName(const std::string& base)
{
    json existing = ops::execute("gnGroups", json::object());
    auto taken = [&](const std::string& n) {
        for (const json& e : existing)
            if ((e.is_string() && e.get<std::string>() == n) || (e.is_object() && e.value("name", std::string()) == n))
                return true;
        return false;
    };
    std::string name = base;
    for (int k = 2; taken(name); ++k)
        name = base + std::to_string(k);
    return name;
}

std::string identifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Wall" + out;
    return out;
}

Footprint footprintFromJson(const json& points, bool closed)
{
    if (!points.is_array() || points.size() < 2)
        throw CommandError("masonryWall: points takes at least two [x, y, z] points");
    Footprint f;
    for (const json& p : points)
    {
        if (!p.is_array() || p.size() != 3)
            throw CommandError("masonryWall: each point is [x, y, z]");
        f.points.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    }
    f.closed = closed;
    return f;
}

/** The mesh's own edges and points, under its modifiers: where the footprint lives. */
scene::MeshPtr baseMesh(Graph& g, const Node& n) { return scene::modifiers::cage(g, n.id()); }

/** The mesh's link id, given one (undoably) if it has none, so Unreal knows the wall again after a rename. */
std::string wallKey(Graph& g, Node& mesh)
{
    if (const std::string* s = std::get_if<std::string>(&mesh.value(scene::Transform::kLinkId)); s && !s->empty())
        return *s;
    static std::mt19937_64 rng{std::random_device{}()};
    const std::string fresh = std::format("{:016x}", rng());
    UndoChunk chunk("Unreal Link Id");
    g.setValue({mesh.id(), scene::Transform::kLinkId, -1}, fresh);
    return fresh;
}

/** Group keys a wall sent last time, so the ones it no longer has can be taken out of the level. */
std::filesystem::path sentFile(const std::string& key) { return link::cacheDir() / "masonry" / (key + ".json"); }

std::set<std::string> readSent(const std::string& key)
{
    std::set<std::string> out;
    std::ifstream in(sentFile(key));
    if (!in)
        return out;
    const json j = json::parse(in, nullptr, false);
    if (j.is_array())
        for (const json& k : j)
            if (k.is_string())
                out.insert(k.get<std::string>());
    return out;
}

void writeSent(const std::string& key, const json& groups)
{
    json keys = json::array();
    for (const json& g : groups)
        keys.push_back(g["key"]);
    std::error_code ec;
    std::filesystem::create_directories(sentFile(key).parent_path(), ec);
    std::ofstream(sentFile(key)) << keys.dump();
}

/** The Unreal texture asset each baked map of a group (or of every group when group is empty) became, by map. */
json bakedTextures(const json& baked, const std::string& folder, const std::string& group)
{
    json out = json::object();
    for (const json& o : baked.value("outputs", json::array()))
    {
        const json groups = o.value("groups", json::array());
        if (!group.empty() && std::ranges::find(groups, json(group)) == groups.end())
            continue;
        const std::string asset = link::textureAssetName(o.value("file", std::string()), o.value("udim", 1001));
        out[o.value("map", std::string())] = folder + "/" + asset + "." + asset;
    }
    return out;
}

/** One masonryMaterials instance: the slot's Lila material (when the scene has one) as tint, roughness and tiling maps. */
json materialInstance(const std::string& name, const std::string& slot, const json& lila, json scalars, const json& maps)
{
    json in = {{"name", name}, {"slot", slot}, {"scalars", std::move(scalars)}, {"vectors", json::object()}, {"textures", json::object()}, {"files", json::object()}};
    if (lila.contains("baseColor"))
        in["vectors"]["Tint"] = lila["baseColor"];
    if (lila.contains("roughness"))
        in["scalars"]["BaseRoughness"] = lila["roughness"];
    const std::string colour = lila.value("baseColorMap", std::string()), normal = lila.value("normalMap", std::string());
    in["scalars"]["HasBaseColorMap"] = colour.empty() ? 0.0 : 1.0;
    in["scalars"]["HasDetailNormal"] = normal.empty() ? 0.0 : 1.0;
    if (!colour.empty())
        in["files"]["BaseColorMap"] = {{"file", colour}};
    if (!normal.empty())
        in["files"]["DetailNormalMap"] = {{"file", normal}, {"normal", true}, {"flipGreen", lila.value("normalFormat", std::string()) == "opengl"}};
    in["scalars"]["HasBakedMaps"] = maps.empty() ? 0.0 : 1.0;
    const std::pair<const char*, const char*> params[] = {{"normal", "BakedNormalMap"}, {"ao", "BakedAOMap"}, {"curvature", "BakedCurvatureMap"}};
    for (const auto& [map, param] : params)
        if (maps.contains(map))
            in["textures"][param] = maps[map];
    return in;
}

/** Imports the plan's assets and places its groups, then removes the stale groups. Runs on the link worker. */
json sendPlan(const json& plan, const std::vector<std::filesystem::path>& files, const json& stale, const json& materials, const json& baked,
              json materialRequest, bool nanite)
{
    LILA_ZONE_N("Masonry to Unreal");
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    if (!client)
        throw std::runtime_error("not connected to Unreal");
    json errors = json::array();
    // Unreal rebuilds a Nanite Assembly each time one of its parts is reimported, seconds a stone; the old one goes first.
    json assemblies = json::array();
    for (const json& as : plan["assets"])
        if (as["kind"] == "assembly")
            assemblies.push_back(as["objectPath"]);
    if (!assemblies.empty())
        if (link::RpcResult r = client->call("deleteAssets", {{"paths", assemblies}}); !r.ok)
            errors.push_back("deleteAssets: " + r.error);
    std::vector<std::string> meshes;
    for (size_t k = 0; k < files.size(); ++k)
    {
        const json& as = plan["assets"][k];
        json mapping = {{"contentPath", as["contentPath"]}, {"kind", "static"}};
        if (nanite && (as["kind"] == "stone" || as["kind"] == "chunk" || as["kind"] == "assembly"))
            mapping["nanite"] = true;
        // Generated instances take the slots afterwards, so the import makes no plain materials of its own.
        if (materialRequest.is_object())
            mapping["importMaterials"] = false;
        link::RpcResult r = client->call("importAsset", link::importParams(files[k], mapping, materials), link::importCallOptions());
        std::string mesh = as["objectPath"].get<std::string>();
        if (!r.ok)
            errors.push_back(as["name"].get<std::string>() + ": " + r.error);
        else
        {
            for (const json& e : r.result.value("errors", json::array()))
                errors.push_back(as["name"].get<std::string>() + ": " + (e.is_string() ? e.get<std::string>() : e.dump()));
            if (json made = r.result.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
                mesh = made[0].get<std::string>();
        }
        meshes.push_back(mesh);
    }
    int64_t placed = 0;
    for (const json& grp : plan["groups"])
    {
        link::RpcResult r = client->call("placeInstances", {{"group", grp["key"]},
                                                            {"label", grp["label"]},
                                                            {"mesh", meshes[grp["asset"].get<size_t>()]},
                                                            {"mode", grp["mode"]},
                                                            {"customData", masonryDataSlots()},
                                                            {"placements", grp["placements"]}},
                                            link::placementTimeoutMs(grp["placements"].size()));
        if (r.ok)
            placed += int64_t(grp["placements"].size());
        else
            errors.push_back(grp["label"].get<std::string>() + ": " + r.error);
    }
    json textures = json::object();
    if (baked.is_object() && !baked.value("outputs", json::array()).empty())
    {
        json warnings = json::array();
        const json params = link::textureImportParams(baked, {{"folder", plan["folder"].get<std::string>() + "/Textures"}, {"materialMode", "none"}}, &warnings);
        link::RpcResult r = client->call("importTextures", params, link::importCallOptions());
        if (r.ok)
            textures = r.result;
        else
            errors.push_back("textures: " + r.error);
    }
    json made = json::object();
    if (materialRequest.is_object())
    {
        materialRequest["meshes"] = meshes;
        for (json& in : materialRequest["instances"])
            if (in.contains("assets"))
            {
                json only = json::array();
                for (const json& k : in["assets"])
                    only.push_back(meshes[k.get<size_t>()]);
                in["meshes"] = only;
                in.erase("assets");
            }
        link::RpcResult r = client->call("masonryMaterials", materialRequest, link::importCallOptions());
        if (r.ok)
            made = r.result;
        else
            errors.push_back("materials: " + r.error);
    }
    json removed = json::object();
    if (!stale.empty())
    {
        link::RpcResult r = client->call("removePlacements", {{"groups", stale}});
        if (r.ok)
            removed = r.result;
        else
            errors.push_back("removing old chunks: " + r.error);
    }
    return {{"imported", int64_t(files.size())}, {"placed", placed}, {"textures", textures}, {"materials", made}, {"removed", removed}, {"errors", errors}};
}

/** Every variant built in parallel. Throws CommandError with the first failure. */
std::vector<std::shared_ptr<const Result>> buildVariants(const std::vector<Footprint>& footprints, const std::vector<json>& values)
{
    std::vector<std::shared_ptr<const Result>> results(values.size());
    std::vector<std::string> errors(values.size());
    tbb::parallel_for(size_t(0), values.size(), [&](size_t k) {
        try
        {
            results[k] = buildMasonry(footprints, values[k], false);
        }
        catch (const std::exception& e)
        {
            errors[k] = e.what();
        }
    });
    for (const std::string& e : errors)
        if (!e.empty())
            throw CommandError("masonryWedge: " + e);
    return results;
}

/** A group around one masonry node with every input exposed, applied to host as a modifier with the given values. */
json applyGroup(const std::string& host, const char* nodeType, InputSet set, const json& values, const char* description)
{
    Graph& g = Graph::current();
    const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Masonry");
    json inputs = json::array();
    json links = json::array();
    if (set == InputSet::Wall)
        links.push_back("GroupInput.geometry -> Masonry.footprint");
    links.push_back("Masonry.geometry -> GroupOutput.geometry");
    for (const Input* in : inputsOf(set))
    {
        static const char* typeNames[] = {"float", "int", "string", "bool"};
        json gi = {{"name", in->name}, {"type", typeNames[in->type]}, {"default", values[in->name]}, {"panel", in->panel}, {"description", in->doc}};
        if (in->type == Input::Float || in->type == Input::Int)
        {
            if (in->max > in->min)
            {
                gi["min"] = in->min;
                gi["max"] = in->max;
            }
            if (in->softMax > in->min)
                gi["softMax"] = in->softMax;
        }
        inputs.push_back(gi);
        links.push_back(std::string("GroupInput.") + in->name + " -> Masonry." + in->name);
    }
    const json spec = {{"name", group},
                       {"description", description},
                       {"inputs", inputs},
                       {"nodes", json::array({{{"name", "Masonry"}, {"type", nodeType}}})},
                       {"links", links}};
    ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
    json mod = ops::execute("gnApply", {{"args", {host, group}}});
    // Set on the modifier itself: a modifier type kept from an earlier scene's group of this name has its defaults.
    const std::string modName = mod.is_string() ? mod.get<std::string>() : mod.at(0).get<std::string>();
    for (const Input* in : inputsOf(set))
        ops::execute("setAttr", {{"args", {modName + "." + in->name, values[in->name]}}});
    return {{"group", group}, {"modifier", mod}, {"modifierPath", modName}};
}

/** An empty mesh named name, for a building or a wall to live on. */
std::string newHost(const std::string& name)
{
    json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
    return made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
}

/** The footprint resampled every step cm and dropped onto a mesh (world space), so the wall stands on it. */
Footprint dropOnto(const Footprint& f, Graph& g, const std::string& groundName, double step)
{
    Node* ground = g.find(groundName);
    if (!ground || ground->type().attrIndex("outMesh") < 0)
        throw CommandError("masonryWall: ground '" + groundName + "' is not a mesh");
    g.evaluate();
    const scene::MeshPtr gm = ground->as<scene::MeshPtr>(ground->type().attrIndex("outMesh"));
    if (!gm || gm->numFaces() == 0)
        throw CommandError("masonryWall: ground '" + groundName + "' has no faces");
    geo::Mesh world = *gm;
    const glm::dmat4 xf = std::get<glm::dmat4>(g.value({ground->id(), scene::Transform::kWorldMatrix, -1}));
    for (glm::dvec3& q : world.positions())
        q = glm::dvec3(xf * glm::dvec4(q, 1.0));
    const geo::MeshBvh bvh(world);
    const geo::Bounds bb = world.bounds();
    Footprint out;
    out.closed = f.closed;
    const size_t n = f.points.size();
    const size_t segs = f.closed ? n : n - 1;
    auto heightAt = [&](size_t k) { return k < f.heights.size() ? f.heights[k] : -1.0; };
    auto add = [&](glm::dvec3 p, double h) {
        geo::BvhHit hit;
        if (bvh.raycast(glm::dvec3(p.x, bb.max.y + 1.0, p.z), glm::dvec3(0, -1, 0), hit, bb.max.y - bb.min.y + 2.0))
            p.y = bb.max.y + 1.0 - hit.t;
        out.points.push_back(p);
        if (h >= 0.0)
            out.heights.push_back(h);
    };
    for (size_t s = 0; s < segs; ++s)
    {
        const glm::dvec3 a = f.points[s], b = f.points[(s + 1) % n];
        const double len = glm::length(glm::dvec2(b.x - a.x, b.z - a.z));
        const int pieces = std::max(1, int(std::ceil(len / std::max(step, 1.0))));
        for (int i = 0; i < pieces; ++i)
        {
            const double t = double(i) / pieces;
            const double h = heightAt(s) >= 0.0 ? glm::mix(heightAt(s), heightAt((s + 1) % n), t) : -1.0;
            add(a + (b - a) * t, h);
        }
    }
    if (!f.closed)
        add(f.points.back(), heightAt(n - 1));
    return out;
}

/** Hand-placed stones (any transforms with meshes) ruined in place: fallen ones moved to the heap or deleted. */
json ruinPlaced(Graph& g, const std::vector<std::string>& names, const RuinParams& p)
{
    g.evaluate();
    std::vector<Stone> stones;
    std::vector<std::string> paths;
    std::vector<glm::dvec3> offsets, scales;
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n || !n->type().dag)
            throw CommandError("ruin: '" + name + "' is not an object");
        geo::Bounds b;
        if (int outMesh = n->type().attrIndex("outMesh"); outMesh >= 0)
            if (const scene::MeshPtr m = n->as<scene::MeshPtr>(outMesh))
                b = m->bounds();
        if (!b.valid())
        {
            b.extend(glm::dvec3(-0.5));
            b.extend(glm::dvec3(0.5));
        }
        const glm::dmat4 m = std::get<glm::dmat4>(g.value({n->id(), scene::Transform::kWorldMatrix, -1}));
        glm::dvec3 scale;
        glm::dvec3 axis[3];
        for (int k = 0; k < 3; ++k)
        {
            scale[k] = glm::length(glm::dvec3(m[k]));
            axis[k] = scale[k] > 1e-12 ? glm::dvec3(m[k]) / scale[k] : glm::dvec3(k == 0, k == 1, k == 2);
        }
        const glm::dvec3 mid = (b.min + b.max) * 0.5;
        Stone s;
        s.frame = glm::dmat3(axis[0], axis[1], axis[2]);
        s.center = glm::dvec3(m * glm::dvec4(mid, 1.0));
        s.size = glm::max((b.max - b.min) * scale, glm::dvec3(1e-3));
        s.face = Face::Through;
        s.random = float(unitRandom(stones.size()));
        stones.push_back(s);
        paths.push_back(n->path());
        offsets.push_back(mid);
        scales.push_back(scale);
    }
    std::vector<int> kept;
    const RuinStats st = ruinStones(stones, p, 0.0, {}, &kept);
    std::vector<char> stays(paths.size(), 0);
    json moved = json::array();
    for (size_t k = 0; k < kept.size(); ++k)
    {
        const size_t src = size_t(kept[k]);
        stays[src] = 1;
        const glm::dvec3& sc = scales[src];
        const glm::dmat4 m = stoneMatrix(stones[k]) *
                             glm::dmat4(glm::dvec4(sc.x, 0, 0, 0), glm::dvec4(0, sc.y, 0, 0), glm::dvec4(0, 0, sc.z, 0), glm::dvec4(-offsets[src] * sc, 1.0));
        json flat = json::array();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                flat.push_back(m[c][r]);
        ops::execute("xform", {{"args", {paths[src]}}, {"worldSpace", true}, {"matrix", flat}});
        if (stones[k].role == Role::Rubble || stones[k].loose)
            moved.push_back(paths[src]);
    }
    json deleted = json::array();
    for (size_t k = 0; k < paths.size(); ++k)
        if (!stays[k])
            deleted.push_back(paths[k]);
    if (!deleted.empty())
        ops::execute("delete", {{"args", deleted}});
    json out = st.toJson();
    out["moved"] = moved;
    out["deleted"] = deleted;
    return out;
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    std::vector<FlagDef> wallFlags = {
        {"points", "p", T::List, nullptr, "Footprint, [[x, y, z], ...] in cm; each point's y is the ground there, Y up."},
        {"closed", "c", T::Bool, false, "Join the last point back to the first."},
        {"heights", "", T::List, nullptr, "Wall height above each point, cm (one per point): sloping tops, gables, broken walls."},
        nodeFlag({"ground", "", T::String, nullptr, "A mesh to stand the wall on: the footprint is resampled and dropped onto it."}, "mesh"),
        {"groundStep", "", T::Float, 0.0, "Resampling step along the footprint for ground, cm; 0 is two stone lengths.", {}, {}, 0.0},
        {"name", "n", T::String, "MasonryWall", "Name of the mesh made to hold the wall."},
    };
    for (FlagDef& f : flagsOf(InputSet::Wall))
        wallFlags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "masonryWall",
        .doc = "Build a masonry wall along a line or loop: stones laid in courses, corners bonded, a mortar core behind them.\n\n"
               "Give points= (a new mesh holds them as edges) or a mesh whose loose edges are the footprint. Each point's y is the "
               "ground: courses step over slopes, and heights= (or the footprint's point float wallHeight) slopes or breaks the top; "
               "ground= drops the footprint onto a terrain mesh. openings= cuts doors, windows and niches with arches or lintels, "
               "sills and jambs. The wall is a geometry-nodes modifier around a Masonry Wall node; every flag becomes a keyable input "
               "on it, so change it later with setAttr on '<modifier>.<flag>', ruin it with ruin, weather it with age, and check it "
               "with masonryInfo or masonryCheck. Returns the mesh, modifier, group and stats (stones, courses, roles, regions, stone "
               "meshes, trimmed, dropped, triangle counts, collision hulls, ruin, bounds, seconds). Lengths are cm.",
        .positionalDoc = "mesh whose loose edges are the footprint (optional)",
        .flags = wallFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const auto t0 = std::chrono::steady_clock::now();
            Graph& g = Graph::current();
            const json values = valuesFromArgs(a, InputSet::Wall);
            try
            {
                optionsFromValues(values);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("masonryWall: ") + e.what());
            }

            std::vector<Footprint> fps;
            std::string host;
            if (!a.positional.empty())
            {
                Node* mesh = g.find(a.positional[0].get<std::string>());
                if (!mesh || !mesh->type().isA("mesh"))
                    throw CommandError("masonryWall: " + a.positional[0].dump() + " is not a mesh");
                if (auto m = scene::modifiers::cage(g, mesh->id()))
                    fps = footprintsFromEdges(*m);
                if (fps.empty())
                    throw CommandError("masonryWall: " + mesh->path() + " has no loose edges to build along");
                host = mesh->path();
            }
            else
            {
                if (!a.has("points"))
                    throw CommandError("masonryWall: give points= or a mesh with loose edges");
                Footprint f = footprintFromJson(a.flags["points"], a.get<bool>("closed", false));
                if (a.has("heights"))
                {
                    const json& hs = a.flags["heights"];
                    if (!hs.is_array() || hs.size() != f.points.size())
                        throw CommandError("masonryWall: heights takes one height per point");
                    for (const json& h : hs)
                        f.heights.push_back(h.get<double>());
                }
                if (a.has("ground"))
                {
                    const double step = a.get<double>("groundStep", 0.0);
                    f = dropOnto(f, g, a.get<std::string>("ground"), step > 0.0 ? step : values["stoneLength"].get<double>() * 2.0);
                }
                fps.push_back(std::move(f));
                host = newHost(a.get<std::string>("name", "MasonryWall"));
                Node* mesh = g.find(host);
                g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(edgesFromFootprints(fps))));
                // What the node will read back, so the build below is the one it finds in the cache.
                fps = footprintsFromEdges(edgesFromFootprints(fps));
            }

            const json made = applyGroup(host, "MasonryWall", InputSet::Wall, values, "Lays stones in courses along the mesh's loose edges, with a mortar core behind them.");
            std::shared_ptr<const Result> r;
            try
            {
                r = buildMasonry(fps, values, false);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("masonryWall: ") + e.what());
            }
            json out = statsOf(*r, fps.size());
            out["mesh"] = host;
            out["modifier"] = made["modifier"];
            out["group"] = made["group"];
            out["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return out;
        },
        .examples = {"masonryWall(points=[[0, 0, 0], [400, 0, 0], [400, 0, 300]], bond='flemish', height=250)",
                     "tower = masonryWall(points=[[100 * math.cos(a / 12 * math.tau), 0, 100 * math.sin(a / 12 * math.tau)] for a in range(12)],\n"
                     "                    closed=True, thickness=40, height=600, openings=[{'at': 50, 'width': 60, 'height': 160, 'arch': 'round'}])\n"
                     "setAttr(tower['modifier'] + '.bond', 'english')\n"
                     "masonryInfo(tower['mesh'])",
                     "masonryWall(points=[[0, 0, 0], [800, 120, 0]], heights=[300, 180], stoneLength=60, stoneHeight=30, stoneDepth=35, thickness=80, batter=3)"},
    });

    std::vector<FlagDef> buildingFlags = {
        {"spec", "", T::Any, nullptr, "The building: a JSON object (or its text). See the lila://masonry/spec-schema resource; masonrySpec reads one back."},
        {"name", "n", T::String, "MasonryBuilding", "Name of the mesh made to hold a new building."},
        nodeFlag({"node", "", T::String, nullptr, "An existing building (its mesh or modifier) to rebuild with this spec and these flags instead of making one."}, "mesh"),
    };
    for (FlagDef& f : flagsOf(InputSet::Building, true))
        if (f.name != "spec")
            buildingFlags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "masonryBuilding",
        .doc = "Build a whole masonry building from one JSON spec: walls (following the ground, sloped tops, batter, bands, "
               "cornices, buttresses) with doors, windows and niches under round, pointed, segmental or flat arches or lintels, "
               "plus columns and colonnades, stairs, barrel vaults, domes, freestanding arches and floors. Every part is a named "
               "region (north_wall, north_wall/door_1, portico/column_3) that ruin, masonryInfo and masonryCheck address. The "
               "spec's scale multiplies every length, stones included, for buildings far larger than human ones.\n\n"
               "The building is a geometry-nodes modifier around a Masonry Building node holding the spec, so it saves with the "
               "scene; change it by editing what masonrySpec returns and calling masonryBuilding again with node=. Ruin and age "
               "flags work as on masonryWall. Returns the mesh, modifier, group and stats (stones, roles, regions, ruin, triangle "
               "counts, collision hulls, bounds, seconds).",
        .flags = buildingFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const auto t0 = std::chrono::steady_clock::now();
            Graph& g = Graph::current();
            json fallback = json::object();
            Node* existing = nullptr;
            if (a.has("node"))
            {
                existing = masonryModifier(g, a.get<std::string>("node"), "masonryBuilding");
                if (!isBuilding(*existing))
                    throw CommandError("masonryBuilding: " + existing->path() + " is a wall, not a building");
                fallback = modifierValues(*existing);
            }
            else if (!a.has("spec"))
                throw CommandError("masonryBuilding: give spec= (see the lila://masonry/spec-schema resource)");
            json values = valuesFromArgs(a, InputSet::Building, fallback);
            if (a.has("spec"))
            {
                const json& s = a.flags["spec"];
                const json parsed = s.is_string() ? json::parse(s.get<std::string>(), nullptr, false) : s;
                if (!parsed.is_object())
                    throw CommandError("masonryBuilding: spec is a JSON object");
                values["spec"] = parsed.dump();
            }
            std::shared_ptr<const Result> r;
            try
            {
                r = buildMasonry({}, values, false);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("masonryBuilding: ") + e.what());
            }
            json out = statsOf(*r, 0);
            if (existing)
            {
                for (const Input* in : inputsOf(InputSet::Building))
                    if (values[in->name] != fallback.value(in->name, json()))
                        ops::execute("setAttr", {{"args", {existing->path() + "." + in->name, values[in->name]}}});
                const scene::NodeId meshId = scene::modifiers::meshOf(g, existing->id());
                out["mesh"] = meshId != scene::kNoNode ? g.node(meshId)->path() : std::string();
                out["modifier"] = existing->path();
            }
            else
            {
                const std::string host = newHost(a.get<std::string>("name", "MasonryBuilding"));
                const json made = applyGroup(host, "MasonryBuilding", InputSet::Building, values, "A masonry building from a spec.");
                out["mesh"] = host;
                out["modifier"] = made["modifier"];
                out["group"] = made["group"];
            }
            out["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return out;
        },
        .examples = {"masonryBuilding(spec={'stone': {'length': 60, 'height': 30, 'depth': 40, 'joint': 1.5},\n"
                     "    'walls': [{'name': 'nave', 'points': [[0, 0, 0], [2000, 0, 0], [2000, 0, 900], [0, 0, 900]], 'closed': True,\n"
                     "               'height': 1200, 'thickness': 120, 'cornice': 2,\n"
                     "               'openings': [{'name': 'door', 'at': 1000, 'width': 300, 'height': 600, 'arch': 'pointed'}]}],\n"
                     "    'floors': [{'name': 'roof', 'points': [[0, 0, 0], [2000, 0, 0], [2000, 0, 900], [0, 0, 900]], 'y': 1200}]},\n"
                     "    left=0.5, age=0.7)",
                     "b = masonrySpec('MasonryBuilding')\nb['spec']['scale'] = 8\nmasonryBuilding(node='MasonryBuilding', spec=b['spec'])"},
    });

    ctx.registerCommand({
        .name = "masonrySpec",
        .doc = "A masonry building's spec and settings, to edit and pass back to masonryBuilding with node=; for a wall, its "
               "settings and footprints. Returns {spec, settings, regions, footprints}.",
        .positionalDoc = "mesh or its masonry modifier",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const WallRef w = wallRef(g, a, "masonrySpec");
            json settings = w.values;
            json spec = json::object();
            if (settings.contains("spec"))
            {
                spec = json::parse(settings["spec"].get<std::string>(), nullptr, false);
                settings.erase("spec");
            }
            json footprints = json::array();
            for (const Footprint& f : w.footprints)
            {
                json pts = json::array();
                for (const glm::dvec3& p : f.points)
                    pts.push_back({p.x, p.y, p.z});
                footprints.push_back({{"points", pts}, {"closed", f.closed}, {"heights", f.heights}});
            }
            const std::shared_ptr<const Result> r = buildOf(w, false, "masonrySpec");
            return {{"spec", spec}, {"settings", settings}, {"regions", r->built.regions}, {"footprints", footprints}, {"modifier", w.mod->path()}};
        },
        .examples = {"masonrySpec('MasonryBuilding')['spec']"},
    });

    ctx.registerCommand({
        .name = "masonryInfo",
        .doc = "What a masonry wall or building is made of, and whether it is clean.\n\n"
               "Returns the same stats masonryWall does (stones, courses, roles, regions with stone counts, stone meshes, trimmed, "
               "dropped, triangle counts, collision hulls, what the ruin did, bounds), the current settings, and checks: overlaps "
               "(standing stone pairs intersecting by more than 0.01 cm, 0 when clean) and ok.",
        .positionalDoc = "mesh or its masonry modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const WallRef w = wallRef(g, a, "masonryInfo");
            const std::shared_ptr<const Result> r = buildOf(w, false, "masonryInfo");
            json out = statsOf(*r, w.footprints.size());
            std::vector<int> standing;
            for (size_t k = 0; k < r->built.stones.size(); ++k)
                if (r->built.stones[k].role != Role::Rubble && !r->built.stones[k].loose)
                    standing.push_back(int(k));
            const size_t overlapping = overlappingPairs(r->built, standing, 0.01).size();
            out["settings"] = w.values;
            out["checks"] = {{"overlaps", int64_t(overlapping)}, {"ok", overlapping == 0 && !r->built.stones.empty()}};
            out["mesh"] = w.mesh ? w.mesh->path() : std::string();
            out["modifier"] = w.mod->path();
            return out;
        },
        .examples = {"masonryInfo('MasonryWall')['checks']"},
    });

    std::vector<FlagDef> ruinFlags = flagsOf(InputSet::Ruin, true);
    for (FlagDef& f : ruinFlags)
        if (f.name == "ruinRegions")
        {
            f.name = "regions";
            f.doc = "Only these regions ruin: a list or comma separated (north_wall, tower_1/door_1). Empty is all.";
        }
        else if (f.name == "ruinSeed")
        {
            f.name = "seed";
            f.doc = "Another seed breaks it differently.";
        }
    ctx.registerCommand({
        .name = "ruin",
        .doc = "Ruin masonry: stones above a ragged break fall, lower toward the collapse side and cut by gaps; a few just above it "
               "hang on; nothing is left floating; the tops tip; a share of what fell lies heaped at the foot (settle=True drops it "
               "with the rigid-body solver); floors break into pieces that fall where nothing holds them. left is the plain knob: "
               "0.75, 0.5, 0.25 of the stone left standing. regions= limits it to named parts, such as the top of one wall.\n\n"
               "On a masonry wall or building it sets the modifier's ruin inputs (only the flags given; left defaults to 0.5 when "
               "none are), so it stays editable and saves with the scene. On other objects (stones placed by hand, any meshes) it "
               "ruins them in place: fallen ones move to the heap or are deleted. Returns the ruin stats (removed, survivors, "
               "loose, unsupported, rubble, slab pieces, fallen pieces, left).",
        .positionalDoc = "a masonry mesh or modifier, or the objects to ruin (default the selection)",
        .flags = ruinFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.empty())
                throw CommandError("ruin: give a masonry wall or building, or the stones to ruin");
            auto ruinValue = [&](const Input* in) -> std::optional<json> {
                const char* flag = std::string_view(in->name) == "ruinRegions" ? "regions" : (std::string_view(in->name) == "ruinSeed" ? "seed" : in->name);
                if (!a.has(flag))
                    return std::nullopt;
                return valueOfFlag(*in, a.flags[flag]);
            };
            bool any = false;
            for (const Input* in : inputsOf(InputSet::Ruin))
                any = any || ruinValue(in).has_value();
            if (Node* mod = findMasonry(g, names[0]); mod && names.size() == 1)
            {
                for (const Input* in : inputsOf(InputSet::Ruin))
                {
                    std::optional<json> v = ruinValue(in);
                    if (!v && !any && std::string_view(in->name) == "left")
                        v = 0.5;
                    if (v)
                        ops::execute("setAttr", {{"args", {mod->path() + "." + in->name, *v}}});
                }
                const WallRef w = refOf(g, names[0], "ruin");
                const std::shared_ptr<const Result> r = buildOf(w, false, "ruin");
                json out = r->ruin.toJson();
                out["stones"] = int64_t(r->built.stones.size());
                out["modifier"] = mod->path();
                return out;
            }
            json v = json::object();
            for (const Input* in : inputsOf(InputSet::Ruin))
                if (std::optional<json> given = ruinValue(in))
                    v[in->name] = *given;
            if (!any)
                v["left"] = 0.5;
            return ruinPlaced(g, names, ruinFromValues(v));
        },
        .examples = {"ruin('MasonryWall', left=0.5, collapse=90, gaps=2)", "ruin('Chapel', regions=['north_wall'], left=0.66)",
                     "ruin(['stone1', 'stone2', 'stone3'], left=0.4, settle=True)"},
    });

    ctx.registerCommand({
        .name = "age",
        .doc = "Weather masonry: sets the age inputs of a masonry wall or building (only the flags given; age defaults to 0.7 "
               "when none are). Every stone gets age, wet (near the ground and on exposed tops) and moss (where wet and exposed), "
               "sent to Unreal per instance for the masonry material. Returns how many stones carry moss and the averages.",
        .positionalDoc = "masonry mesh or modifier (default the selection)",
        .flags = flagsOf(InputSet::Age, true),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("age: give one masonry wall or building");
            Node* mod = masonryModifier(g, names[0], "age");
            bool any = false;
            for (const Input* in : inputsOf(InputSet::Age))
                any = any || a.has(in->name);
            for (const Input* in : inputsOf(InputSet::Age))
                if (a.has(in->name))
                    ops::execute("setAttr", {{"args", {mod->path() + "." + in->name, typedValue(*in, a.flags[in->name])}}});
                else if (!any && std::string_view(in->name) == "age")
                    ops::execute("setAttr", {{"args", {mod->path() + ".age", 0.7}}});
            const WallRef w = refOf(g, names[0], "age");
            const std::shared_ptr<const Result> r = buildOf(w, false, "age");
            double age = 0.0, wet = 0.0, moss = 0.0;
            int64_t mossy = 0;
            for (const Stone& s : r->built.stones)
            {
                age += s.age;
                wet += s.wet;
                moss += s.moss;
                mossy += s.moss > 0.25f ? 1 : 0;
            }
            const double n = double(std::max<size_t>(1, r->built.stones.size()));
            return {{"stones", int64_t(r->built.stones.size())}, {"mossy", mossy}, {"meanAge", age / n}, {"meanWet", wet / n}, {"meanMoss", moss / n}, {"modifier", mod->path()}};
        },
        .examples = {"age('MasonryWall', age=0.8, moss=0.6)"},
    });

    ctx.registerCommand({
        .name = "masonryCheck",
        .doc = "Check a masonry wall or building before shipping it: floating stones (not on the ground and touching nothing "
               "under them), standing stones interpenetrating by more than tolerance, rubble heaped higher than half the walls "
               "(stacked), missing collision, and an optional budget. Returns {ok, issues: [text], floating, floatingStones (the "
               "first ten: region, role, at), overlaps, overlapRegions (pairs per region, 'a + b' across two), overlapPairs (the "
               "first ten: regions, roles, at), rubbleOverlaps, stacked, rubbleHeight, collisionHulls, stones, triangles}.",
        .positionalDoc = "masonry mesh or modifier (default the selection)",
        .flags = {{"tolerance", "", T::Float, 0.1, "Interpenetration allowed, and the gap a resting stone may have under it, cm.", {}, {}, 0.0},
                  {"maxStones", "", T::Int, int64_t(0), "Budget: most stones; 0 for none.", {}, {}, 0.0},
                  {"maxTriangles", "", T::Int, int64_t(0), "Budget: most stone triangles, instanced; 0 for none.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const WallRef w = wallRef(g, a, "masonryCheck");
            const std::shared_ptr<const Result> r = buildOf(w, false, "masonryCheck");
            const Built& b = r->built;
            const double tol = std::max(0.0, a.get<double>("tolerance", 0.1));
            std::vector<int> standing, rubble;
            for (size_t k = 0; k < b.stones.size(); ++k)
                (isRubble(b.stones[k]) ? rubble : standing).push_back(int(k));
            const std::vector<std::pair<int, int>> pairs = overlappingPairs(b, standing, tol);
            const size_t overlaps = pairs.size();
            const size_t rubbleOverlaps = overlappingPairs(b, rubble, std::max(tol, 1.0)).size();
            auto regionOf = [&](const Stone& s) { return s.region >= 0 && size_t(s.region) < b.regions.size() ? b.regions[size_t(s.region)] : std::string(); };
            std::map<std::string, int64_t> perRegion;
            json overlapPairs = json::array();
            for (const auto& [i, j] : pairs)
            {
                const Stone& s = b.stones[size_t(i)];
                const Stone& t = b.stones[size_t(j)];
                std::string ra = regionOf(s), rb = regionOf(t);
                if (rb < ra)
                    std::swap(ra, rb);
                ++perRegion[ra == rb ? ra : ra + " + " + rb];
                if (overlapPairs.size() < 10)
                {
                    const glm::dvec3 at = (s.center + t.center) * 0.5;
                    overlapPairs.push_back({{"regions", {regionOf(s), regionOf(t)}}, {"roles", {roleName(s.role), roleName(t.role)}}, {"at", {at.x, at.y, at.z}}});
                }
            }
            json overlapRegions = json::object();
            for (const auto& [name, count] : perRegion)
                overlapRegions[name] = count;
            std::vector<int> which;
            const int floating = countFloating(b.stones, std::max(tol, b.pitch * 0.15), &which);
            json floatingStones = json::array();
            for (size_t i = 0; i < which.size() && i < 10; ++i)
            {
                const Stone& s = b.stones[size_t(which[i])];
                floatingStones.push_back({{"region", regionOf(s)}, {"role", roleName(s.role)}, {"at", {s.center.x, s.center.y, s.center.z}}});
            }
            // Rubble from a wall spreads at its foot: a heap higher than half the walls is a column, not a heap.
            const double stackLimit = std::max(3.0 * b.pitch, 0.5 * b.courses * b.pitch);
            double rubbleHeight = 0.0;
            const int stacked = countStacked(b.stones, stackLimit, nullptr, &rubbleHeight);
            const json stats = statsOf(*r, w.footprints.size());
            json issues = json::array();
            if (b.stones.empty())
                issues.push_back("no stones");
            if (floating > 0)
                issues.push_back(std::format("{} stones float", floating));
            if (overlaps > 0)
            {
                std::vector<std::pair<int64_t, std::string>> ranked;
                for (const auto& [name, count] : perRegion)
                    ranked.push_back({count, name});
                std::sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first > y.first || (x.first == y.first && x.second < y.second); });
                std::string where;
                for (size_t i = 0; i < ranked.size() && i < 4; ++i)
                    where += std::format("{}{} {}", i ? ", " : "", ranked[i].second, ranked[i].first);
                if (ranked.size() > 4)
                    where += ", ...";
                issues.push_back(std::format("{} pairs of standing stones interpenetrate by more than {} cm ({})", overlaps, tol, where));
            }
            if (stacked > 0)
                issues.push_back(std::format("{} rubble stones are stacked up to {:.0f} cm above the ground, over half the walls' height ({:.0f} cm)", stacked, rubbleHeight, stackLimit));
            if (b.coreHulls.empty() && !b.stones.empty())
                issues.push_back("no collision: the core is empty");
            if (const int64_t m = a.get<int64_t>("maxStones", 0); m > 0 && int64_t(b.stones.size()) > m)
                issues.push_back(std::format("{} stones, over the budget of {}", b.stones.size(), m));
            if (const int64_t m = a.get<int64_t>("maxTriangles", 0); m > 0 && stats["stoneTriangles"].get<int64_t>() > m)
                issues.push_back(std::format("{} stone triangles, over the budget of {}", stats["stoneTriangles"].get<int64_t>(), m));
            return {{"ok", issues.empty()},
                    {"issues", issues},
                    {"floating", floating},
                    {"floatingStones", floatingStones},
                    {"overlaps", int64_t(overlaps)},
                    {"overlapRegions", overlapRegions},
                    {"overlapPairs", overlapPairs},
                    {"rubbleOverlaps", int64_t(rubbleOverlaps)},
                    {"stacked", stacked},
                    {"rubbleHeight", rubbleHeight},
                    {"collisionHulls", int64_t(b.coreHulls.size())},
                    {"stones", int64_t(b.stones.size())},
                    {"triangles", stats["stoneTriangles"]}};
        },
        .examples = {"masonryCheck('MasonryBuilding', maxStones=50000)['issues']"},
    });

    ctx.registerCommand({
        .name = "masonryShip",
        .doc = "Send a masonry wall or building to Unreal's open level, as it stands (ruin, rubble and age included), instanced: "
               "each stone mesh is imported once as a static mesh with three LODs (worn, clean chamfer, plain box), and the stones "
               "are placed as one instanced component (HISM by default) per stone mesh per ground chunk, so streaming and culling "
               "work per chunk. mode='merged' instead makes each chunk's stones one static mesh with the same three LODs. The core "
               "goes as its own mesh carrying the collision as convex UCX pieces; stones have none. mode='lowPoly' sends each chunk "
               "as a box shell with the stones and core baked onto it (normal, AO, curvature) and its own UCX collision, for "
               "background walls and HLOD (an intact wall's shape). Nanite is off unless asked: nanite=True builds Nanite data for "
               "the stone meshes, and mode='assembly' (UE 5.8 Nanite Assemblies) also writes the stones as one USD assembly of "
               "those meshes and places that instead of instanced components.\n\n"
               "With textures=True (the default) the stones also get a shared atlas: every stone mesh's worn LOD0 is baked onto its "
               "box into one normal, AO and curvature set, in uv0 of all three LODs (the tiling projection moves to uv1), and the "
               "textures go to <folder>/<mesh>/Textures. Slots: MasonryStone (LOD0), MasonryStoneFar (LOD1 and 2), MasonryMortar "
               "(core), MasonryShell (low poly); Unreal finds or makes materials by these names, taking a Lila material's values "
               "when the scene has one. Instances carry custom primitive data 0 random, 1 height up the wall (0 bottom, 1 top), 2 age, "
               "3 damage, 4 wet, 5 moss; merged meshes carry random, height, moss and wet as vertex colour and age and damage in uv2. "
               "Sending again replaces the wall's chunks in place and removes the ones it no longer has.\n\n"
               "Returns stats (stones, assets, groups, chunks, average triangles per stone mesh at each LOD, collisionHulls, "
               "atlasTexelsPerCm, shellTriangles, textures, bakeSeconds, removedGroups) plus, with wait=True, what Unreal imported "
               "and placed. dryRun=True returns the plan without baking, writing files or connecting.",
        .positionalDoc = "mesh or its masonry modifier (default the selection)",
        .flags = {{"folder", "f", T::String, "/Game/Lila/Masonry", "Content folder; the wall's assets go in <folder>/<mesh name>."},
                  {"chunkSize", "cs", T::Float, 400.0, "Ground cell per instanced component, cm.", {}, {}, 10.0},
                  {"mode", "m", T::String, "hism",
                   "hism or ism: stones instanced per stone mesh per chunk. merged: each chunk's stones as one static mesh with three LODs. "
                   "lowPoly: each chunk as a box shell with the stones baked onto it. assembly: a Nanite Assembly (UE 5.8) through USD.",
                   {"hism", "ism", "merged", "lowPoly", "assembly"}},
                  {"nanite", "na", T::Bool, false, "Build Nanite data for the stone meshes (always on for assembly)."},
                  {"textures", "tx", T::Bool, true,
                   "Bake and send textures: the stone atlas (instanced and merged) or each shell's maps (lowPoly, which always bakes)."},
                  {"textureSize", "ts", T::Int, int64_t(2048), "Atlas or shell texture size, texels.", {}, {}, 64.0, 8192.0},
                  {"backend", "", T::String, "auto", "Bake backend: auto (gpu when there is one), gpu or cpu.", {"auto", "gpu", "cpu"}},
                  {"material", "mt", T::String, "generate",
                   "generate: instances of Lila's masonry master (M_LilaMasonry, built on MF_LilaMasonry once, then yours to edit) per slot. "
                   "none: materials matched or made by slot name. Or a material path to parent the instances to, which must have the master's parameters."},
                  {"regenerateMaster", "", T::Bool, false, "Rebuild M_LilaMasonry and MF_LilaMasonry from this version of Lila, discarding edits to them."},
                  {"dryRun", "", T::Bool, false, "Return the plan, placements included, without writing files or contacting Unreal."},
                  {"wait", "w", T::Bool, false, "Block until Unreal has imported and placed everything."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const WallRef w = wallRef(g, a, "masonryShip");
            if (!w.mesh)
                throw CommandError("masonryShip: the masonry modifier is on no mesh");
            const std::shared_ptr<const Result> r = buildOf(w, true, "masonryShip");
            Built b = r->built;
            if (b.stones.empty())
                throw CommandError("masonryShip: " + w.mesh->path() + " has no stones");
            const BuildOptions o = optionsFromValues(w.values);

            ShipOptions so;
            so.folder = a.get<std::string>("folder", so.folder);
            so.chunkSize = std::max(10.0, a.get<double>("chunkSize", so.chunkSize));
            so.mode = a.get<std::string>("mode", so.mode);
            if (so.mode != "hism" && so.mode != "ism" && so.mode != "merged" && so.mode != "lowPoly" && so.mode != "assembly")
                throw CommandError("masonryShip: mode is hism, ism, merged, lowPoly or assembly, not '" + so.mode + "'");
            const bool nanite = so.mode == "assembly" || a.get<bool>("nanite", false);
            g.evaluate();
            const glm::dmat4 world = std::get<glm::dmat4>(g.value({w.mesh->id(), scene::Transform::kWorldMatrix, -1}));
            const bool dry = a.get<bool>("dryRun", false);
            const std::string key = dry ? std::string("dryRun") : wallKey(g, *w.mesh);
            const std::string name = identifier(w.mesh->name());
            const bool lowPoly = so.mode == "lowPoly";
            const bool textures = lowPoly || a.get<bool>("textures", true);
            const int textureSize = int(std::clamp<int64_t>(a.get<int64_t>("textureSize", 2048), 64, 8192));
            constexpr int kPadding = 8;
            const double texelsPerCm = textures && !lowPoly ? atlasStones(b, textureSize, kPadding) : 0.0;
            if (lowPoly && b.shellCells.empty())
                throw CommandError("masonryShip: lowPoly needs walls; nothing in this one is a wall");
            // The shells follow the walls as they stand, ruin and all.
            const std::vector<Shell> shells = lowPoly ? shellsFromCells(b.shellCells, so.chunkSize, textureSize, kPadding) : std::vector<Shell>();
            json plan = shipPlan(b, world, key, name, so, shells);

            std::array<size_t, 3> lodTris{};
            for (const auto& chain : b.lods)
                for (size_t l = 0; l < chain.size(); ++l)
                    lodTris[l] += triangles(chain[l]);
            json stats = {{"stones", int64_t(b.stones.size())},
                          {"assets", int64_t(plan["assets"].size())},
                          {"groups", int64_t(plan["groups"].size())},
                          {"chunks", int64_t(plan["chunks"].size())},
                          {"lodTriangles", json::array()},
                          {"collisionHulls", int64_t(b.coreHulls.size())},
                          {"atlasTexelsPerCm", texelsPerCm},
                          {"nanite", nanite},
                          {"folder", plan["folder"]},
                          {"mesh", w.mesh->path()}};
            for (size_t t : lodTris)
                stats["lodTriangles"].push_back(double(t) / double(std::max<size_t>(1, b.lods.size())));
            if (lowPoly)
            {
                size_t shellTris = 0, hullCount = 0;
                for (const Shell& sh : shells)
                {
                    shellTris += triangles(sh.mesh);
                    hullCount += sh.hulls.size();
                }
                stats["shellTriangles"] = int64_t(shellTris);
                stats["collisionHulls"] = int64_t(hullCount);
            }
            if (dry)
            {
                stats["plan"] = std::move(plan);
                return stats;
            }

            json baked;
            if (textures)
            {
                BakeSettings bs;
                bs.folder = link::cacheDir() / "masonry" / name / "textures";
                bs.size = textureSize;
                bs.padding = kPadding;
                bs.backend = a.get<std::string>("backend", "auto");
                try
                {
                    baked = lowPoly ? bakeShells(b, shells, o.wall, name, bs) : bakeAtlas(b, o.bevel, name + "_Atlas", bs);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("masonryShip: baking failed: ") + e.what());
                }
                json files = json::array();
                for (const json& out : baked.value("outputs", json::array()))
                    files.push_back(out.value("file", std::string()));
                stats["textures"] = files;
                stats["bakeSeconds"] = baked.value("seconds", 0.0);
                stats["bakeBackend"] = baked.value("backend", std::string());
            }

            const std::filesystem::path dir = link::cacheDir() / "masonry" / name;
            std::vector<geo::Mesh> hulls = b.coreHulls;
            for (const Stone& s : b.stones)
                if (s.role == Role::Column || s.role == Role::Step)
                    hulls.push_back(hexMesh(stoneCorners(s)));
            std::vector<std::filesystem::path> files;
            for (const json& as : plan["assets"])
            {
                const std::string kind = as["kind"].get<std::string>(), assetName = as["name"].get<std::string>();
                const size_t index = as["index"].get<size_t>();
                std::filesystem::path file = dir / (assetName + ".fbx");
                std::string err;
                bool ok = false;
                if (kind == "stone")
                    ok = writeLodFbx(b.lods[index], assetName, file, &err);
                else if (kind == "assembly")
                {
                    file = dir / (assetName + ".usda");
                    ok = writeAssemblyUsda(b, plan, file, &err);
                }
                else if (kind == "shell")
                {
                    const Shell& sh = shells[index];
                    auto local = [&](geo::Mesh m) {
                        for (glm::dvec3& q : m.positions())
                            q -= sh.origin;
                        return m;
                    };
                    const geo::Mesh mesh = local(sh.mesh);
                    std::vector<geo::Mesh> pieces;
                    for (const geo::Mesh& h : sh.hulls)
                        pieces.push_back(local(h));
                    ok = writeLodFbx(std::span<const geo::Mesh>(&mesh, 1), assetName, file, &err, pieces);
                }
                else if (kind == "chunk")
                {
                    const std::vector<int> members = plan["members"][index].get<std::vector<int>>();
                    const json& at = plan["origins"][index];
                    const glm::dvec3 origin(at[0].get<double>(), at[1].get<double>(), at[2].get<double>());
                    std::array<geo::Mesh, 3> levels;
                    for (int l = 0; l < 3; ++l)
                        levels[size_t(l)] = mergeStones(b, members, l, origin);
                    ok = writeLodFbx(levels, assetName, file, &err);
                }
                else if (kind == "dressing")
                    ok = writeLodFbx(std::span<const geo::Mesh>(&b.dressing, 1), assetName, file, &err);
                else
                    ok = writeLodFbx(std::span<const geo::Mesh>(&b.core, 1), assetName, file, &err, hulls);
                if (!ok)
                    throw CommandError("masonryShip: writing " + file.string() + " failed: " + err);
                files.push_back(file);
            }
            std::set<std::string> now;
            for (const json& grp : plan["groups"])
                now.insert(grp["key"].get<std::string>());
            json stale = json::array();
            for (const std::string& old : readSent(key))
                if (!now.count(old))
                    stale.push_back(old);
            stats["removedGroups"] = int64_t(stale.size());

            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("masonryShip: not connected to Unreal: ") + e.what());
            }
            writeSent(key, plan["groups"]);
            json materials = json::array();
            for (const char* slot : {kStoneSlot, kStoneFarSlot, kMortarSlot, kShellSlot, kPlasterSlot, kIvySlot})
                materials.push_back(link::materialInputs(g, slot));

            json materialRequest;
            if (const std::string material = a.get<std::string>("material", "generate"); material != "none")
            {
                const json features = link::Link::get().status().value("features", json::array());
                if (!features.is_array() || std::ranges::find(features, json("masonryMaterials")) == features.end())
                    throw CommandError("masonryShip: the LilaLink plugin in this editor is too old for masonry materials; rebuild it from "
                                       "Lila's ue/LilaLink, or send with material='none'");
                const std::string texFolder = plan["folder"].get<std::string>() + "/Textures";
                const json stone = link::materialInputs(g, kStoneSlot), mortar = link::materialInputs(g, kMortarSlot);
                double height = 0.0;
                for (const Stone& s : b.stones)
                    height = std::max(height, s.center.y);
                json instances = json::array();
                if (lowPoly)
                {
                    for (size_t k = 0; k < plan["assets"].size(); ++k)
                    {
                        const json& as = plan["assets"][k];
                        if (as["kind"] != "shell")
                            continue;
                        const Shell& sh = shells[as["index"].get<size_t>()];
                        const std::string group = std::format("{}_{}_{}", name, sh.cell.x, sh.cell.y);
                        json in = materialInstance("MI_" + group, kShellSlot, stone,
                                                   {{"StoneData", 0.0}, {"WorldAligned", 1.0}, {"UseBakedNormal", 1.0}, {"WallHeight", height}},
                                                   bakedTextures(baked, texFolder, group));
                        in["assets"] = json::array({k});
                        instances.push_back(std::move(in));
                    }
                }
                else
                {
                    const json atlas = textures ? bakedTextures(baked, texFolder, std::string()) : json::object();
                    const double data = so.mode == "merged" ? 2.0 : 1.0;
                    instances.push_back(materialInstance("MI_" + name + "_Stone", kStoneSlot, stone,
                                                         {{"StoneData", data}, {"WorldAligned", 0.0}, {"UseBakedNormal", 0.0}, {"WallHeight", height}}, atlas));
                    instances.push_back(materialInstance("MI_" + name + "_StoneFar", kStoneFarSlot, stone,
                                                         {{"StoneData", data}, {"WorldAligned", 0.0}, {"UseBakedNormal", 1.0}, {"WallHeight", height}}, atlas));
                }
                instances.push_back(materialInstance("MI_" + name + "_Mortar", kMortarSlot, mortar,
                                                     {{"StoneData", 0.0}, {"WorldAligned", 1.0}, {"UseBakedNormal", 0.0}, {"WallHeight", height}}, json::object()));
                if (b.dressing.numFaces() > 0)
                    instances.push_back(materialInstance("MI_" + name + "_Plaster", kPlasterSlot, link::materialInputs(g, kPlasterSlot),
                                                         {{"StoneData", 0.0}, {"WorldAligned", 1.0}, {"UseBakedNormal", 0.0}, {"WallHeight", height}},
                                                         json::object()));
                materialRequest = {{"folder", plan["folder"].get<std::string>() + "/Materials"},
                                   {"regenerate", a.get<bool>("regenerateMaster", false)},
                                   {"instances", std::move(instances)}};
                if (material != "generate")
                    materialRequest["parent"] = material;
            }

            if (a.get<bool>("wait", false))
            {
                stats["unreal"] = sendPlan(plan, files, stale, materials, baked, materialRequest, nanite);
                return stats;
            }
            link::Link::get().post([plan = std::move(plan), files, stale, materials, baked, materialRequest, name, nanite] {
                json done;
                try
                {
                    done = sendPlan(plan, files, stale, materials, baked, materialRequest, nanite);
                }
                catch (const std::exception& e)
                {
                    done = {{"errors", json::array({e.what()})}};
                }
                mainq::post([done, name] {
                    for (const json& e : done.value("errors", json::array()))
                        log::error("masonryShip {}: {}", name, e.get<std::string>());
                    if (done.contains("placed"))
                        log::info("masonryShip: placed {} stones of {} in Unreal", done["placed"].get<int64_t>(), name);
                });
            });
            stats["queued"] = true;
            return stats;
        },
        .examples = {"masonryShip('MasonryWall', dryRun=True)['chunks']", "masonryShip('MasonryWall', wait=True)['unreal']",
                     "masonryShip('Temple', mode='assembly', wait=True)"},
    });
    ctx.registerCommand({
        .name = "masonryFromMesh",
        .doc = "Lay masonry walls over a blockout mesh: it is sliced with level planes, each section's outline moved in to the wall's "
               "centre line, and walls laid there, so towers, tapered keeps and irregular blockouts become stone. A hollow blockout "
               "(walls with an inside) takes its wall thickness from the blockout; a solid one uses thickness. Sections with the same "
               "outline stack into one wall; a tapering blockout steps in band by band. Every masonryWall flag works the same. "
               "Returns what masonryWall returns, plus the blockout, the footprints' count and the thickness used.",
        .positionalDoc = "blockout mesh",
        .flags = [] {
            std::vector<FlagDef> f = {{"band", "", T::Float, 0.0, "Height of each slice, cm; 0 slices the blockout into eight bands.", {}, {}, 0.0},
                                      {"hideBlockout", "", T::Bool, true, "Hide the blockout once the walls stand on it."},
                                      {"name", "n", T::String, "MasonryFromMesh", "Name of the mesh made to hold the walls."}};
            for (FlagDef& d : flagsOf(InputSet::Wall))
                if (d.name != "height")
                    f.push_back(std::move(d));
            for (FlagDef& d : f)
                if (d.name == "thickness")
                {
                    d.defaultValue = 0.0;
                    d.min = 0.0;
                    d.doc = "Wall thickness for a solid blockout, cm; 0 takes it from the blockout (a tenth of its narrower side when solid).";
                }
            return f;
        }(),
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("masonryFromMesh: give one blockout mesh");
            Node* blockout = g.find(names[0]);
            if (!blockout || blockout->type().attrIndex("outMesh") < 0)
                throw CommandError("masonryFromMesh: " + names[0] + " is not a mesh");
            g.evaluate();
            const scene::MeshPtr src = blockout->as<scene::MeshPtr>(blockout->type().attrIndex("outMesh"));
            if (!src || src->numFaces() == 0)
                throw CommandError("masonryFromMesh: " + blockout->path() + " has no faces");
            geo::Mesh world = *src;
            const glm::dmat4 xf = std::get<glm::dmat4>(g.value({blockout->id(), scene::Transform::kWorldMatrix, -1}));
            for (glm::dvec3& q : world.positions())
                q = glm::dvec3(xf * glm::dvec4(q, 1.0));
            double used = 0.0;
            const std::vector<Footprint> fps = footprintsFromBlockout(world, a.get<double>("thickness", 0.0), a.get<double>("band", 0.0), &used);
            if (fps.empty())
                throw CommandError("masonryFromMesh: no closed outline cuts " + blockout->path() + " (is it a closed mesh?)");
            const std::string host = newHost(a.get<std::string>("name", "MasonryFromMesh"));
            g.setValue({g.find(host)->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(edgesFromFootprints(fps))));
            json call = {{"args", {host}}};
            for (const Input* in : inputsOf(InputSet::Wall))
                if (a.has(in->name) && std::string_view(in->name) != "thickness")
                    call[in->name] = a.flags[in->name];
            call["thickness"] = used;
            json out = ops::execute("masonryWall", call);
            if (a.get<bool>("hideBlockout", true))
                ops::execute("hide", {{"args", {blockout->path()}}});
            out["blockout"] = blockout->path();
            out["footprints"] = int64_t(fps.size());
            out["thickness"] = used;
            return out;
        },
        .examples = {"masonryFromMesh('TowerBlockout', bond='ashlar', stoneLength=60, stoneHeight=30, stoneDepth=35)"},
    });

    ctx.registerCommand({
        .name = "ruinPaint",
        .doc = "Paint damage onto a masonry wall or building, as the Ruin Brush does: stones within radius of the stroke fall (a "
               "negative strength holds them up instead). Strokes are kept on the modifier's ruinStrokes input, so they save with the "
               "scene and stay editable; clear=True removes them. Returns the ruin stats with painted, the stones the strokes took.",
        .positionalDoc = "masonry mesh or modifier",
        .flags = {{"points", "p", T::List, nullptr, "The stroke, [[x, y, z], ...] in the mesh's space, cm."},
                  {"radius", "r", T::Float, 50.0, "Brush radius, cm.", {}, {}, 0.1},
                  {"strength", "s", T::Float, 1.0, "1 knocks stones out; -1 holds them up.", {}, {}, -1.0, 1.0},
                  {"clear", "", T::Bool, false, "Remove every stroke first."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("ruinPaint: give one masonry wall or building");
            Node* mod = masonryModifier(g, names[0], "ruinPaint");
            json strokes = a.get<bool>("clear", false) ? json::array() : json::parse(modifierValues(*mod).value("ruinStrokes", std::string()), nullptr, false);
            if (!strokes.is_array())
                strokes = json::array();
            if (a.has("points"))
            {
                const json& pts = a.flags["points"];
                if (!pts.is_array() || pts.empty())
                    throw CommandError("ruinPaint: points takes at least one [x, y, z]");
                strokes.push_back({{"points", pts}, {"radius", a.get<double>("radius", 50.0)}, {"strength", a.get<double>("strength", 1.0)}});
            }
            ops::execute("setAttr", {{"args", {mod->path() + ".ruinStrokes", strokes.dump()}}});
            const WallRef w = refOf(g, names[0], "ruinPaint");
            json out = buildOf(w, false, "ruinPaint")->ruin.toJson();
            out["strokes"] = int64_t(strokes.size());
            out["modifier"] = mod->path();
            return out;
        },
        .examples = {"ruinPaint('Chapel', points=[[200, 400, 0], [600, 380, 0]], radius=120)", "ruinPaint('Chapel', clear=True)"},
    });

    ctx.registerCommand({
        .name = "masonryWedge",
        .doc = "Try variants of a masonry wall or building side by side: count builds, each with the varied settings changed (by "
               "default another ruinSeed each), built in parallel, drawn in elevation into one contact sheet PNG. Returns each "
               "variant's settings and stats and the sheet's path; apply=<index> then sets that variant's settings on the modifier. "
               "vary is {setting: [values]} (cycled per variant) or a list of settings to step by one per variant.",
        .positionalDoc = "masonry mesh or modifier",
        .flags = {{"count", "c", T::Int, int64_t(9), "How many variants.", {}, {}, 1.0, 64.0},
                  {"vary", "", T::Any, nullptr, "What changes: {\"left\": [0.75, 0.5, 0.25]} or [\"ruinSeed\", \"seed\"]. Default ruinSeed."},
                  {"view", "", T::Vec3, nullptr, "Direction the sheet looks along, horizontal; default -Z."},
                  {"tile", "", T::Int, int64_t(256), "Tile size in the sheet, pixels.", {}, {}, 64.0, 2048.0},
                  {"file", "f", T::String, nullptr, "Where to write the sheet; default a temporary PNG."},
                  {"apply", "", T::Int, nullptr, "Set this variant's settings on the modifier instead of wedging."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const WallRef w = wallRef(g, a, "masonryWedge");
            const int count = int(std::clamp<int64_t>(a.get<int64_t>("count", 9), 1, 64));
            std::vector<json> values(size_t(count), w.values);
            const json vary = a.has("vary") ? a.flags["vary"] : json::array({"ruinSeed"});
            for (int k = 0; k < count; ++k)
            {
                json& v = values[size_t(k)];
                if (vary.is_object())
                    for (const auto& [name, list] : vary.items())
                    {
                        const Input* in = findInput(name);
                        if (!in || !list.is_array() || list.empty())
                            throw CommandError("masonryWedge: vary '" + name + "' is not a masonry setting with a list of values");
                        v[name] = typedValue(*in, list[size_t(k) % list.size()]);
                    }
                else
                    for (const json& name : vary)
                    {
                        const Input* in = name.is_string() ? findInput(name.get<std::string>()) : nullptr;
                        if (!in || (in->type != Input::Int && in->type != Input::Float))
                            throw CommandError("masonryWedge: vary " + name.dump() + " is not a numeric masonry setting");
                        v[in->name] = typedValue(*in, v[in->name].get<double>() + k);
                    }
                // Jolt runs on the main thread only; the wedge heaps rubble without it.
                v["settle"] = false;
            }
            if (a.has("apply"))
            {
                const int64_t pick = a.get<int64_t>("apply", 0);
                if (pick < 0 || pick >= count)
                    throw CommandError("masonryWedge: apply is a variant index below count");
                json changed = json::object();
                for (const auto& [name, value] : values[size_t(pick)].items())
                    if (name != "settle" && value != w.values.value(name, json()))
                    {
                        ops::execute("setAttr", {{"args", {w.mod->path() + "." + name, value}}});
                        changed[name] = value;
                    }
                return {{"applied", pick}, {"settings", changed}, {"modifier", w.mod->path()}};
            }
            const std::vector<std::shared_ptr<const Result>> results = buildVariants(w.footprints, values);
            std::vector<std::vector<Stone>> stones;
            json variants = json::array();
            for (int k = 0; k < count; ++k)
            {
                stones.push_back(results[size_t(k)]->built.stones);
                json vary1 = json::object();
                for (const auto& [name, value] : values[size_t(k)].items())
                    if (name != "settle" && value != w.values.value(name, json()))
                        vary1[name] = value;
                variants.push_back({{"index", k},
                                    {"settings", vary1},
                                    {"stones", int64_t(results[size_t(k)]->built.stones.size())},
                                    {"ruin", results[size_t(k)]->ruin.toJson()}});
            }
            const glm::dvec3 view = a.has("view") ? glm::dvec3(a.flags["view"][0].get<double>(), a.flags["view"][1].get<double>(), a.flags["view"][2].get<double>())
                                                  : glm::dvec3(0, 0, -1);
            std::filesystem::path file = a.has("file") ? std::filesystem::path(a.get<std::string>("file"))
                                                       : std::filesystem::temp_directory_path() / "lila_masonry" / (identifier(w.mod->name()) + "_wedge.png");
            std::string err;
            if (!writeContactSheet(stones, view, int(a.get<int64_t>("tile", 256)), file, &err))
                throw CommandError("masonryWedge: writing the sheet failed: " + err);
            return {{"variants", variants}, {"sheet", file.string()}, {"columns", int64_t(std::ceil(std::sqrt(double(count))))}, {"modifier", w.mod->path()}};
        },
        .examples = {"w = masonryWedge('Tower', count=9)\nmasonryWedge('Tower', apply=4)", "masonryWedge('Tower', vary={'left': [0.75, 0.5, 0.25]}, count=3)"},
    });

    ctx.registerCommand({
        .name = "makeDestructible",
        .doc = "Make a masonry wall or building destructible: its stones become the Destruction extension's pieces, held together by "
               "links where they touch (mortar strength times contact area, weaker with age) and to the ground, and the Rigid Body "
               "Solver runs them from startFrame. Its group gains Masonry Links, Rigid Body Solver and a Join with the core; open it "
               "in the Node Editor to add explosions or colliders, or use addExplosion. Needs the Destruction extension. Returns the "
               "group, links and pieces.",
        .positionalDoc = "masonry mesh or modifier",
        .flags = {{"strength", "s", T::Float, 30.0, "Mortar strength, N per cm2 of contact.", {}, {}, 0.0},
                  {"ageWeakens", "", T::Float, 0.6, "How much of its strength the oldest mortar has lost, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"impact", "i", T::Vec3, nullptr, "Where a blast hits, in the mesh's space, cm."},
                  {"blast", "b", T::Float, 0.0, "How hard the blast hits, cm/s; 0 lets it stand until something else breaks it.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (!gn::findDecl("RigidBodySolver"))
                throw CommandError("makeDestructible: the Destruction extension isn't loaded");
            const WallRef w = wallRef(g, a, "makeDestructible");
            scene::Node* groupNode = gn::groupOfModifier(g, *w.mod);
            if (!groupNode)
                throw CommandError("makeDestructible: " + w.mod->path() + " has no group");
            const std::string group = groupNode->path();
            const json info = ops::execute("gnGroupInfo", {{"args", {group}}});
            if (info.dump().find("MasonryLinks") != std::string::npos)
                throw CommandError("makeDestructible: " + w.mod->path() + " is destructible already");
            auto add = [&](const char* type, const char* name) { return ops::execute("gnAddNode", {{"args", {group, type}}, {"name", name}}).get<std::string>(); };
            auto connect = [&](const std::string& from, const std::string& to) { ops::execute("gnConnect", {{"args", {group, from, to}}}); };
            auto set = [&](const std::string& plug, const json& v) { ops::execute("gnSetValue", {{"args", {group, plug, v}}}); };
            const std::string links = add("MasonryLinks", "Links"), solver = add("RigidBodySolver", "Solver"), join = add("JoinGeometry", "Join");
            connect("Masonry.stones", links + ".stones");
            connect("Masonry.stones", solver + ".pieces");
            connect(links + ".links", solver + ".links");
            connect(solver + ".pieces", join + ".geometry");
            connect("Masonry.core", join + ".geometry");
            connect("Masonry.dressing", join + ".geometry");
            connect(join + ".geometry", "GroupOutput.geometry");
            set(links + ".strength", a.get<double>("strength", 30.0));
            set(links + ".ageWeakens", a.get<double>("ageWeakens", 0.6));
            if (a.has("impact"))
                set(solver + ".blastCenter", a.flags["impact"]);
            set(solver + ".blastSpeed", a.get<double>("blast", 0.0));
            const std::shared_ptr<const Result> r = buildOf(w, false, "makeDestructible");
            double lowest = 1e300, typical = 0.0;
            for (const Stone& s : r->built.stones)
            {
                lowest = std::min(lowest, s.center.y - halfExtentAabb(s).y);
                typical += std::min({s.size.x, s.size.y, s.size.z});
            }
            typical /= double(std::max<size_t>(1, r->built.stones.size()));
            std::vector<Stone> stones = r->built.stones;
            for (Stone& s : stones)
                s.grounded = s.center.y - halfExtentAabb(s).y <= lowest + std::max(0.3, typical * 0.08) * 2.0;
            const std::vector<StoneLink> made = stoneLinks(stones, std::max(0.3, typical * 0.08));
            int64_t anchors = 0;
            for (const StoneLink& l : made)
                anchors += l.b < 0 ? 1 : 0;
            return {{"group", group}, {"pieces", int64_t(stones.size())}, {"links", int64_t(made.size())}, {"anchors", anchors}, {"modifier", w.mod->path()}};
        },
        .examples = {"makeDestructible('Tower', strength=20, impact=[0, 300, 0], blast=1500)"},
    });
}
} // namespace masonry
