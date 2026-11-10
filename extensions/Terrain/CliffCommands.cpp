// The cliff commands: terrainCliffs (slope regions), terrainCliffMesh (cliff meshes from regions or blockouts),
// terrainCliffExtrude (walls from curves), terrainCliffKit (kit pieces along a region), terrainCliffScatter (tiered scatter
// on cliffs), terrainSendCliffs (World Partition tiles to Unreal) and the cliff checks terrainCheck runs.

#include "CliffScene.h"
#include "GroundCover.h"
#include "Query.h"
#include "Stamps.h"
#include "TerrainEdit.h"
#include "Volume.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/TextureGraph.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>

using namespace lila;

namespace terrain::cliffs
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;
using edit::TerrainTarget;

const std::set<std::string> kCliffsAfter = {"TerrainBiomes", "TerrainLayers", "TerrainGroundCover"};
constexpr const char* kCurvePrefix = "CliffCurve";

std::string identifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    return out;
}

// ---------------------------------------------------------------------------------------------------- state

/** The Terrain Cliffs node's lists, read for editing. */
struct CliffState
{
    json cliffs = json::array();
    json walls = json::array();
};

CliffState readState(const TerrainTarget& t, const std::string& cmd)
{
    CliffState s;
    if (!t.modifier)
        return s;
    edit::GroupEdit e = edit::readGroup(Graph::current(), t, cmd);
    if (gn::GNode* n = edit::nodeNamed(e.group, kCliffsNode))
    {
        s.cliffs = edit::jsonProp(*n, "cliffs", json::array());
        s.walls = edit::jsonProp(*n, "walls", json::array());
    }
    return s;
}

/** Writes the lists back; wallSources maps a wall's name to the curve it follows (made into an Object Info node). */
void writeState(const TerrainTarget& t, CliffState s, const std::map<std::string, std::string>& wallSources, const std::string& cmd)
{
    edit::GroupEdit e = edit::readGroup(Graph::current(), t, cmd);
    gn::GNode& n = edit::chainNode(e.group, kCliffsNode, kCliffsType, kCliffsAfter, 160.0, cmd);
    for (json& w : s.walls)
        if (auto it = wallSources.find(w.value("name", std::string())); it != wallSources.end())
            w["object"] = edit::sourceNode(e.group, kCurvePrefix, it->second);
    n.values["cliffs"] = s.cliffs.dump();
    n.values["walls"] = s.walls.dump();
    edit::dropUnusedSources(e.group, kCurvePrefix, kCliffsType, {"walls"});
    edit::commitGroup(e, cmd);
}

json* entryNamed(json& list, const std::string& name)
{
    for (json& e : list)
        if (e.value("name", std::string()) == name)
            return &e;
    return nullptr;
}

void putEntry(json& list, const json& entry)
{
    if (json* e = entryNamed(list, entry.value("name", std::string())))
        *e = entry;
    else
        list.push_back(entry);
}

/** The ground before any cliff edit: what the Terrain Cliffs node saw, or the terrain itself when it has none. */
std::shared_ptr<const Heightfield> groundOf(const TerrainTarget& t, const std::string& cmd)
{
    Graph::current().evaluate();
    if (t.modifier)
    {
        edit::GroupEdit e = edit::readGroup(Graph::current(), t, cmd);
        if (edit::nodeNamed(e.group, kCliffsNode))
            if (auto h = publishedGround(t.modifier->id()))
                return h;
    }
    return t.local;
}

std::string terrainPath(const TerrainTarget& t, const Args& a) { return t.path(a.objects().empty() ? std::string() : a.objects()[0]); }

// ---------------------------------------------------------------------------------------------------- scene meshes

scene::MeshPtr meshOf(Graph& g, Node* n)
{
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    return std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
}

glm::dmat4 worldOf(Node* n) { return n->as<glm::dmat4>(scene::Transform::kWorldMatrix); }

/** The mesh node name under parent, made or refilled with m. */
Node* putMesh(Graph& g, const std::string& name, Node* parent, geo::Mesh m)
{
    Node* n = nullptr;
    if (parent)
        n = g.find(parent->path() + "|" + name);
    if (!n)
        n = g.find(name);
    if (n && !n->type().isA("mesh"))
        throw CommandError(name + " is not a mesh");
    if (!n)
        n = g.createNode(*scene::types::find("mesh"), name, parent ? parent->id() : scene::kNoNode);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n;
}

Node* putGroup(Graph& g, const std::string& name)
{
    if (Node* n = g.find(name); n && n->type().isA("transform") && !n->type().isA("mesh"))
        return n;
    return g.createNode(*scene::types::find("transform"), name, scene::kNoNode);
}

// ---------------------------------------------------------------------------------------------------- inputs

RockMap loadRock(const std::string& file, const std::string& cmd)
{
    RockMap r;
    if (file.empty())
        return r;
    const fs::path path = paths::resolvePath(file);
    if (path.extension() == ".lilamat")
    {
        try
        {
            std::ifstream in(path);
            if (!in)
                throw std::runtime_error("can't open " + path.string());
            const json j = json::parse(in);
            const tex::Graph graph = tex::Graph::fromJson(j.contains("graph") ? j["graph"] : j);
            tex::PlanRequest req;
            req.width = req.height = 512;
            req.bufferFormat = "rgba32f";
            for (tex::OutputRequest& o : tex::materialRequests(graph, tex::standardChannels()))
                if (o.name == "height")
                    req.outputs.push_back(std::move(o));
            if (req.outputs.empty())
                throw std::runtime_error("the graph has no height channel");
            const tex::Plan plan = tex::buildPlan(graph, req);
            if (!plan.error.empty())
                throw std::runtime_error(plan.error);
            tex::EvalOptions opts;
            opts.imageRoot = path.parent_path();
            const tex::EvalResult res = tex::evaluate(plan, opts, std::string());
            const auto it = res.outputs.find("height");
            if (it == res.outputs.end())
                throw std::runtime_error("the graph gave no height image");
            r.width = it->second.width;
            r.height = it->second.height;
            for (size_t k = 0; k < size_t(r.width) * size_t(r.height); ++k)
                r.v.push_back(it->second.rgba[k * 4]);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": rockMap " + file + ": " + e.what());
        }
        return r;
    }
    std::string err;
    const image::Image img = image::read(path, &err);
    if (img.width <= 0)
        throw CommandError(cmd + ": rockMap " + file + ": " + (err.empty() ? "unreadable" : err));
    r.width = img.width;
    r.height = img.height;
    r.v.resize(size_t(r.width) * size_t(r.height));
    for (size_t k = 0; k < r.v.size(); ++k)
        r.v[k] = img.rgba[k * 4];
    return r;
}

std::vector<FlagDef> shapeFlags(std::vector<FlagDef> flags)
{
    const std::vector<FlagDef> more = {
        {"depth", "dp", T::Float, 150.0, "How far the face is pushed in before the shape brings it back out, cm: the deepest cut.", {}, {}, 0.0},
        {"strataHeight", "sh", T::Float, 250.0, "Thickness of a rock layer, cm.", {}, {}, 1.0},
        {"strataJitter", "sj", T::Float, 0.35, "How much layer thickness varies, 0 to 1.", {}, {}, 0.0, 1.0},
        {"strataDip", "sdp", T::Float, 0.0, "Tilt of the layers toward the face, degrees.", {}, {}, -60.0, 60.0},
        {"undercut", "uc", T::Float, 0.55, "How deep the bottom of each layer is cut back, 0 to 1 of depth.", {}, {}, 0.0, 1.0},
        {"ledges", "le", T::Float, 0.3, "How often a layer stands out over the next, leaving a ledge, 0 to 1.", {}, {}, 0.0, 1.0},
        {"noise", "nz", T::Float, 0.3, "Surface roughness, 0 to 1.", {}, {}, 0.0, 1.0},
        {"noiseSize", "ns", T::Float, 300.0, "Size of the bumps, cm.", {}, {}, 1.0},
        {"overhang", "oh", T::Float, 0.0, "0 an even face, 1 the top leaning out over a deeply cut base.", {}, {}, 0.0, 1.0},
        {"rock", "rk", T::Float, 0.0, "How much the rock height map shapes the face, 0 to 1.", {}, {}, 0.0, 1.0},
        {"rockMap", "rm", T::String, "", "A rock height map: an image (red channel) or a texture graph (.lilamat, its height channel), projected from three sides."},
        {"rockSize", "rs", T::Float, 400.0, "Rock map repeat, cm.", {}, {}, 1.0},
        {"uvScale", "uv", T::Float, 400.0, "uv0 repeat, cm.", {}, {}, 1.0},
        {"seed", "sd", T::Int, int64_t(0), "Another seed gives other layers and bumps.", {}, {}, 0.0}};
    flags.insert(flags.end(), more.begin(), more.end());
    return flags;
}

/** The shape flags in the terrain's own units (horizontal lengths over across, heights over vertical). */
Shape shapeFrom(const Args& a, double across)
{
    Shape s;
    s.depth = a.get<double>("depth", s.depth) / across;
    s.strataHeight = a.get<double>("strataHeight", s.strataHeight) / across;
    s.strataJitter = a.get<double>("strataJitter", s.strataJitter);
    s.undercut = a.get<double>("undercut", s.undercut);
    s.ledges = a.get<double>("ledges", s.ledges);
    s.noise = a.get<double>("noise", s.noise);
    s.noiseSize = a.get<double>("noiseSize", s.noiseSize) / across;
    s.overhang = a.get<double>("overhang", s.overhang);
    s.rock = a.get<double>("rock", s.rock);
    s.rockSize = a.get<double>("rockSize", s.rockSize) / across;
    s.seed = uint32_t(a.get<int64_t>("seed", 0));
    return s;
}

json shapeJson(const Args& a)
{
    json j = json::object();
    for (const char* k : {"depth", "strataHeight", "strataJitter", "strataDip", "undercut", "ledges", "noise", "noiseSize", "overhang", "uvScale"})
        if (a.has(k))
            j[k] = a.get<double>(k, 0.0);
    if (a.has("seed"))
        j["seed"] = a.get<int64_t>("seed", 0);
    return j;
}

json statsJson(const MeshStats& s, double vertical)
{
    return {{"vertices", s.vertices},
            {"triangles", s.triangles},
            {"layers", s.layers},
            {"ledgeVertices", s.ledgeVertices},
            {"deepestCut", s.maxOut * vertical},
            {"baseClearance", s.baseClearance * vertical},
            {"hidden", s.hidden},
            {"detail", s.detail},
            {"warnings", s.warnings}};
}

/** A region of the terrain by name ('cliff_1' or 'region:cliff_1'), from its stored regions. */
json regionNamed(const std::string& terrain, std::string name, const std::string& cmd)
{
    if (name.starts_with("region:"))
        name = name.substr(7);
    const json stored = storedRegions(terrain);
    if (!stored.contains(name))
    {
        std::string have;
        for (const auto& [n, r] : stored.items())
            have += (have.empty() ? "" : ", ") + n;
        throw CommandError(cmd + ": no region named " + name + " (stored: " + (have.empty() ? "none; run terrainCliffs first" : have) + ")");
    }
    json r = stored[name];
    r["name"] = name;
    return r;
}

std::vector<std::string> regionNames(const std::string& terrain, const json& flag, const std::string& cmd)
{
    std::vector<std::string> out;
    if (flag.is_string() && flag.get<std::string>() == "all")
    {
        const json stored = storedRegions(terrain);
        for (const auto& [n, r] : stored.items())
            if (n.starts_with("cliff_"))
                out.push_back(n);
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        if (out.empty())
            throw CommandError(cmd + ": the terrain has no cliff regions; run terrainCliffs first");
    }
    else if (flag.is_string())
        out.push_back(flag.get<std::string>());
    else if (flag.is_array())
        for (const json& n : flag)
            out.push_back(n.get<std::string>());
    for (std::string& n : out)
        if (n.starts_with("region:"))
            n = n.substr(7);
    return out;
}

// ---------------------------------------------------------------------------------------------------- terrainCliffs

json runCliffs(const Args& a)
{
    LILA_ZONE();
    const std::string cmd = "terrainCliffs";
    const TerrainTarget t = edit::oneTerrain(a, cmd);
    const std::string path = terrainPath(t, a);
    if (a.get<bool>("list", false))
    {
        const CliffState s = readState(t, cmd);
        json regions = json::object();
        const json stored = storedRegions(path);
        for (const auto& [n, r] : stored.items())
            if (n.starts_with("cliff_"))
                regions[n] = r.value("cliff", json::object());
        return {{"regions", regions}, {"cliffs", s.cliffs}, {"walls", s.walls}};
    }
    const auto ground = groundOf(t, cmd);
    const Heightfield& h = *ground;
    const double across = t.across(), vertical = t.vertical();
    RegionParams p;
    const double worldSlope = a.get<double>("slope", p.slope);
    p.slope = std::atan(std::tan(worldSlope * 3.141592653589793 / 180.0) * across / vertical) * 180.0 / 3.141592653589793;
    p.minArea = a.get<double>("minArea", p.minArea) / (across * across);
    p.minHeight = a.get<double>("minHeight", p.minHeight) / vertical;
    p.mergeDistance = a.get<double>("mergeDistance", p.mergeDistance) / across;
    p.maxRegions = int(a.get<int64_t>("maxRegions", p.maxRegions));
    p.strataDip = a.get<double>("strataDip", 0.0);
    size_t dropped = 0;
    const std::vector<Region> regions = findRegions(h, p, &dropped);

    json out = json::array(), stored = json::object();
    double totalArea = 0.0;
    for (const Region& r : regions)
    {
        json polygon = json::array();
        for (const glm::dvec2& q : r.rings.front())
            polygon.push_back(edit::vec3(t.world(q.x, h.heightAt(q.x, q.y), q.y)));
        const glm::dvec3 facing = t.direction({r.facing.x, r.facing.z});
        const glm::dvec3 strata = glm::normalize(glm::dvec3(t.m * glm::dvec4(r.strata, 0.0)));
        const double area = r.areaM2 * across * across;
        totalArea += area;
        json cliff = {{"facing", edit::vec3(facing)},
                      {"facingDegrees", r.facingDegrees},
                      {"strata", edit::vec3(strata)},
                      {"low", edit::upOf(t.world(r.point.x, r.low, r.point.y))},
                      {"high", edit::upOf(t.world(r.point.x, r.high, r.point.y))},
                      {"localLow", r.low},
                      {"localHigh", r.high},
                      {"localFacing", edit::vec3(r.facing)},
                      {"localStrata", edit::vec3(r.strata)},
                      {"areaM2", area},
                      {"meanSlope", r.meanSlope},
                      {"maxSlope", r.maxSlope}};
        out.push_back({{"name", r.name},
                       {"reference", "region:" + r.name},
                       {"mask", "region_" + r.name},
                       {"areaM2", area},
                       {"heightCm", (r.high - r.low) * vertical},
                       {"low", cliff["low"]},
                       {"high", cliff["high"]},
                       {"meanSlope", std::atan(std::tan(r.meanSlope * 3.141592653589793 / 180.0) * vertical / across) * 180.0 / 3.141592653589793},
                       {"facing", cliff["facing"]},
                       {"facingDegrees", r.facingDegrees},
                       {"strata", cliff["strata"]},
                       {"point", edit::vec3(t.world(r.point.x, h.heightAt(r.point.x, r.point.y), r.point.y))},
                       {"polygon", polygon}});
        stored[r.name] = {{"rings", ringsJson(r.rings)},
                          {"parts", json::array({{{"point", {r.point.x, r.point.y}}, {"ring", ringsJson({r.rings.front()})[0]}}})},
                          {"point", {r.point.x, r.point.y}},
                          {"query", {{"named", "cliffs"}, {"slope", worldSlope}}},
                          {"cliff", cliff}};
    }
    if (a.get<bool>("store", true))
        replaceRegions(path, "cliff_", stored, cmd);
    json result = {{"regions", out}, {"count", out.size()}, {"dropped", dropped}, {"areaM2", totalArea}, {"slope", worldSlope}};
    if (regions.empty())
        result["warnings"] = json::array({"no slope passed " + std::format("{:.0f}", worldSlope) + " degrees over a large enough area; lower slope, minArea or minHeight"});
    if (a.get<bool>("mesh", false) && !regions.empty())
        result["meshes"] = ops::execute("terrainCliffMesh", {{"args", {path}}, {"region", "all"}});
    return result;
}

// ---------------------------------------------------------------------------------------------------- terrainCliffMesh

MeshParams meshParams(const Args& a, double across, double vertical)
{
    MeshParams p;
    p.mode = a.get<std::string>("mode", "proud") == "inset" ? Mode::Inset : Mode::Proud;
    p.detail = a.get<double>("detail", p.detail);
    p.margin = int(a.get<int64_t>("margin", p.margin));
    p.blend = a.get<double>("blend", p.blend) / across;
    p.baseSink = a.get<double>("baseSink", p.baseSink) / vertical;
    p.lipSink = a.get<double>("lipSink", p.lipSink) / vertical;
    p.recess = a.get<double>("recess", p.recess);
    p.skirt = a.get<double>("skirt", p.skirt) / vertical;
    p.talusWidth = a.get<double>("talusWidth", p.talusWidth) / across;
    p.uvScale = a.get<double>("uvScale", p.uvScale) / across;
    p.maxTriangles = size_t(std::max<int64_t>(1000, a.get<int64_t>("maxTriangles", int64_t(p.maxTriangles))));
    p.shape = shapeFrom(a, across);
    return p;
}

/** A reduced copy with uv0 kept, its normal and AO baked from the full cliff when baking works (said in the result when not). */
json makeLowPoly(Graph& g, Node* high, Node* parent, double ratio, int resolution, const std::string& cmd)
{
    const scene::MeshPtr m = meshOf(g, high);
    if (!m)
        return nullptr;
    geo::ReduceOptions ro;
    ro.ratio = std::clamp(ratio, 0.001, 1.0);
    ro.keepBorders = true;
    geo::Mesh low = geo::reduceMesh(*m, ro).mesh;
    const std::string name = high->name() + "_low";
    Node* n = putMesh(g, name, parent, std::move(low));
    json out = {{"mesh", n->path()}, {"triangles", int64_t(geo::triangulate(*meshOf(g, n)).faces.size())}};
    try
    {
        const std::string project = identifier(high->name()) + "_bake";
        ops::execute("createBakeProject", {{"name", project}});
        ops::execute("bakeGroup", {{"args", {identifier(high->name()) + "_group"}}, {"project", project}, {"lows", {n->path()}}, {"highs", {high->path()}}});
        ops::execute("setBakeOutput", {{"project", project}, {"resolution", resolution}});
        out["bake"] = ops::execute("bake", {{"project", project}, {"maps", {"normal", "ao"}}});
    }
    catch (const std::exception& e)
    {
        const std::string w = std::string("the low poly cliff has no baked maps: ") + e.what();
        log::warn("{}: {}", cmd, w);
        out["warning"] = w;
    }
    return out;
}

json runCliffMesh(const Args& a)
{
    LILA_ZONE();
    const std::string cmd = "terrainCliffMesh";
    Graph& g = Graph::current();
    const TerrainTarget t = edit::oneTerrain(a, cmd);
    const std::string path = terrainPath(t, a);
    const double across = t.across(), vertical = t.vertical();
    const auto ground = groundOf(t, cmd);
    MeshParams p = meshParams(a, across, vertical);
    const RockMap rock = loadRock(a.get<std::string>("rockMap", ""), cmd);
    const std::string stem = t.mesh ? t.mesh->name() : std::string("Terrain");
    Node* group = putGroup(g, identifier(stem + "_Cliffs"));
    CliffState state = readState(t, cmd);
    json made = json::array();
    std::vector<std::string> warnings;
    const double dip = a.get<double>("strataDip", 0.0) * 3.141592653589793 / 180.0;

    if (a.has("blockout"))
    {
        Node* src = g.find(a.get<std::string>("blockout", ""));
        const scene::MeshPtr bm = meshOf(g, src);
        if (!bm)
            throw CommandError(cmd + ": blockout must name a mesh");
        geo::Mesh local = *bm;
        gn::transformMesh(local, t.inv * worldOf(src));
        // The blockout's facing: its area-weighted horizontal normal.
        glm::dvec3 facing(0.0);
        for (geo::Index f = 0; f < local.faceCapacity(); ++f)
            if (local.faceValid(f))
                facing += local.faceAreaVector(f) * glm::dvec3(1.0, 0.0, 1.0);
        p.shape.facing = glm::length(facing) > 1e-9 ? glm::normalize(facing) : glm::dvec3(1.0, 0.0, 0.0);
        p.shape.strata = glm::normalize(glm::dvec3(0.0, std::cos(dip), 0.0) + p.shape.facing * std::sin(dip));
        CliffMesh c = buildFromMesh(local, ground.get(), p, rock.empty() ? nullptr : &rock);
        geo::Mesh m = toMesh(c.mesh);
        gn::transformMesh(m, t.m);
        const std::string name = identifier(a.get<std::string>("name", src->name() + "_cliff"));
        Node* n = putMesh(g, name, group, std::move(m));
        putEntry(state.cliffs, {{"name", name}, {"kind", "blockout"}, {"mesh", n->path()}, {"source", src->path()}});
        json one = {{"name", name}, {"mesh", n->path()}, {"kind", "blockout"}, {"stats", statsJson(c.stats, vertical)}};
        if (a.get<bool>("lowPoly", false))
            one["lowPoly"] = makeLowPoly(g, n, group, a.get<double>("lowPolyRatio", 0.05), int(a.get<int64_t>("bakeResolution", 2048)), cmd);
        made.push_back(one);
        warnings.insert(warnings.end(), c.stats.warnings.begin(), c.stats.warnings.end());
    }
    else
    {
        const std::vector<std::string> names = regionNames(path, a.flags.value("region", json("all")), cmd);
        for (const std::string& rn : names)
        {
            const json r = regionNamed(path, rn, cmd);
            const auto rings = ringsFrom(r.value("rings", json::array()));
            if (rings.empty())
                throw CommandError(cmd + ": region " + rn + " has no outline");
            const json cliff = r.value("cliff", json::object());
            glm::dvec3 facing(1.0, 0.0, 0.0);
            if (cliff.contains("localFacing"))
                facing = {cliff["localFacing"][0].get<double>(), cliff["localFacing"][1].get<double>(), cliff["localFacing"][2].get<double>()};
            p.shape.facing = facing;
            p.shape.strata = glm::normalize(glm::dvec3(0.0, std::cos(dip), 0.0) + facing * std::sin(dip));
            CliffMesh c = buildFromRegion(*ground, rings, p, rock.empty() ? nullptr : &rock);
            if (c.mesh.t.empty())
            {
                warnings.push_back(rn + ": no mesh (the region covers no cells)");
                continue;
            }
            geo::Mesh m = toMesh(c.mesh);
            gn::transformMesh(m, t.m);
            const std::string name = identifier(stem + "_" + rn);
            Node* n = putMesh(g, name, group, std::move(m));
            GroundEdit e;
            e.name = rn;
            e.mode = p.mode;
            e.rings = rings;
            e.depth = p.shape.depth;
            e.recess = p.recess;
            e.blend = p.blend;
            e.margin = p.margin;
            e.talusWidth = p.talusWidth;
            e.detail = p.detail;
            json entry = e.toJson();
            entry["kind"] = "region";
            entry["mesh"] = n->path();
            entry["shape"] = shapeJson(a);
            putEntry(state.cliffs, entry);
            json one = {{"name", rn}, {"mesh", n->path()}, {"mode", p.mode == Mode::Inset ? "inset" : "proud"}, {"stats", statsJson(c.stats, vertical)}};
            if (a.get<bool>("lowPoly", false))
                one["lowPoly"] = makeLowPoly(g, n, group, a.get<double>("lowPolyRatio", 0.05), int(a.get<int64_t>("bakeResolution", 2048)), cmd);
            made.push_back(one);
            for (const std::string& w : c.stats.warnings)
                warnings.push_back(rn + ": " + w);
        }
    }
    writeState(t, state, {}, cmd);
    g.evaluate();
    for (const std::string& w : warnings)
        log::warn("{}: {}", cmd, w);
    json ground2 = json::object();
    edit::findTerrain(g, path, cmd);
    if (t.modifier)
        if (json r = cliffNodeResult(t.modifier->id(), kCliffsNode); !r.is_null())
            ground2 = r;
    return {{"cliffs", made}, {"group", group->path()}, {"ground", ground2}, {"warnings", warnings}};
}

// ---------------------------------------------------------------------------------------------------- terrainCliffExtrude

json runCliffExtrude(const Args& a)
{
    LILA_ZONE();
    const std::string cmd = "terrainCliffExtrude";
    Graph& g = Graph::current();
    const TerrainTarget t = edit::oneTerrain(a, cmd);
    const std::string path = terrainPath(t, a);
    if (!t.mesh)
        throw CommandError(cmd + ": the terrain has no mesh for the wall to stand on");
    // The curve: given, made from mesh edges, or from a region's outline.
    std::string curve = a.get<std::string>("curve", "");
    if (curve.empty() && a.has("edges"))
    {
        const json e = a.flags["edges"];
        if (!e.is_object() || !e.contains("mesh") || !e.contains("ids"))
            throw CommandError(cmd + ": edges is {mesh, ids}");
        const json made = ops::execute("polyEdgeToCurve", {{"args", {e["mesh"]}}, {"edges", e["ids"]}, {"type", "poly"}, {"constructionHistory", true}});
        curve = made.is_array() ? made.at(0).get<std::string>() : made.get<std::string>();
    }
    if (curve.empty() && a.has("region"))
    {
        const json r = regionNamed(path, a.get<std::string>("region", ""), cmd);
        const auto rings = ringsFrom(r.value("rings", json::array()));
        if (rings.empty())
            throw CommandError(cmd + ": the region has no outline");
        json pts = json::array();
        for (const glm::dvec2& q : rings.front())
            pts.push_back(edit::vec3(t.world(q.x, t.local->heightAt(q.x, q.y), q.y)));
        const json made = ops::execute("curve", {{"points", pts}, {"type", "poly"}, {"closed", true}, {"name", identifier(r["name"].get<std::string>() + "_edge")}});
        curve = made.get<std::string>();
    }
    Node* cn = curve.empty() ? nullptr : g.find(curve);
    if (!cn || !cn->type().isA("curve"))
        throw CommandError(cmd + ": give curve= (a curve), edges= {mesh, ids} or region=");
    const std::string name = identifier(a.get<std::string>("name", cn->name() + "_cliff"));
    const double height = a.get<double>("height", 1500.0);
    json settings = {{"height", height},
                     {"profile", a.get<std::string>("profile", "vertical")},
                     {"batter", a.get<double>("batter", 0.2)},
                     {"steps", a.get<int64_t>("steps", 3)},
                     {"sink", a.get<double>("sink", 150.0)},
                     {"detail", a.get<double>("detail", 4.0)},
                     {"flip", a.get<bool>("flip", false)},
                     {"shape", shapeJson(a)}};
    if (a.has("profileCurve"))
    {
        // A profile curve drawn in its own XY plane: X out from the face, Y up; normalised to the wall's height.
        Node* pc = g.find(a.get<std::string>("profileCurve", ""));
        if (!pc || !pc->type().isA("curve"))
            throw CommandError(cmd + ": profileCurve must name a curve");
        const curve::Splines& splines = curve::shapeSplines(g, pc->id());
        if (splines.empty())
            throw CommandError(cmd + ": the profile curve is empty");
        const std::vector<glm::dvec3>& pts = splines.front().points;
        json custom = json::array();
        double lo = 1e300, hi = -1e300;
        for (const glm::dvec3& q : pts)
        {
            lo = std::min(lo, q.y);
            hi = std::max(hi, q.y);
        }
        for (const glm::dvec3& q : pts)
            custom.push_back({(q.x - pts[0].x) / std::max(1.0, hi - lo), (q.y - lo) / std::max(1.0, hi - lo)});
        if (custom.size() < 2)
            throw CommandError(cmd + ": the profile curve needs two points");
        settings["custom"] = custom;
        settings["profile"] = "custom";
    }
    json nodes = json::array({{{"name", "Curve"}, {"type", "ObjectInfo"}, {"position", json::array({-400, 0})}, {"props", {{"object", cn->path()}, {"transformSpace", "world"}}}},
                              {{"name", "Terrain"}, {"type", "ObjectInfo"}, {"position", json::array({-400, 140})}, {"props", {{"object", t.mesh->path()}, {"transformSpace", "world"}}}},
                              {{"name", "Wall"}, {"type", "TerrainCliffWall"}, {"position", json::array({0, 0})},
                               {"props", {{"curve", "Curve"}, {"terrain", "Terrain"}, {"settings", settings.dump()}}}}});
    json links = json::array({"Wall.geometry -> GroupOutput.geometry"});
    const std::string groupName = name + "_Wall";
    Node* host = g.find(name);
    if (host && !host->type().isA("mesh"))
        throw CommandError(cmd + ": " + name + " is not a mesh");
    if (!host)
    {
        const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
        host = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
    }
    ops::execute("gnBuildGroup", {{"args", {json{{"name", groupName}, {"description", "A cliff wall along " + cn->name() + "."}, {"nodes", nodes}, {"links", links}}}},
                                  {"replace", true}});
    bool applied = false;
    for (scene::NodeId m : scene::modifiers::stack(g, host->id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            if (Node* grpNode = gn::groupOfModifier(g, *mod); grpNode && gn::readGroup(*grpNode).name == groupName)
                applied = true;
    if (!applied)
        ops::execute("gnApply", {{"args", {host->path(), groupName}}});
    Node* group = putGroup(g, identifier((t.mesh ? t.mesh->name() : std::string("Terrain")) + "_Cliffs"));
    if (g.parent(host->id()) != group->id())
        g.setParent(host->id(), group->id());

    CliffState state = readState(t, cmd);
    const bool stamp = a.get<bool>("stamp", true);
    putEntry(state.walls, {{"name", name},
                           {"height", height / t.vertical()},
                           {"plateau", a.get<double>("plateau", 1500.0) / t.across()},
                           {"cut", a.get<double>("cut", 600.0) / t.across()},
                           {"falloff", a.get<double>("falloff", 800.0) / t.across()},
                           {"flip", a.get<bool>("flip", false)},
                           {"stamp", stamp},
                           {"mesh", host->path()},
                           {"curve", cn->path()}});
    putEntry(state.cliffs, {{"name", name}, {"kind", "wall"}, {"mesh", host->path()}, {"curve", cn->path()}});
    writeState(t, state, {{name, cn->path()}}, cmd);
    g.evaluate();
    json out = {{"name", name}, {"mesh", host->path()}, {"curve", cn->path()}, {"group", groupName}, {"stamp", stamp}};
    for (scene::NodeId m : scene::modifiers::stack(g, host->id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            out["wall"] = cliffNodeResult(mod->id(), "Wall");
    // Reading the terrain makes it evaluate with the new stamp before its result is read.
    const TerrainTarget after = edit::findTerrain(g, path, cmd);
    if (after.modifier)
        if (json r = cliffNodeResult(after.modifier->id(), kCliffsNode); !r.is_null())
            out["ground"] = r;
    return out;
}

// ---------------------------------------------------------------------------------------------------- kits, scatter

/** A mesh host with a one-node geometry-nodes group, made or rebuilt; returns its modifier. */
Node* liveHost(Graph& g, const std::string& name, const std::string& groupName, const json& nodes, const json& links, const json& inputs,
               const std::string& description, const std::string& cmd)
{
    Node* host = g.find(name);
    if (host && !host->type().isA("mesh"))
        throw CommandError(cmd + ": " + name + " is not a mesh");
    if (!host)
    {
        const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
        host = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
    }
    ops::execute("gnBuildGroup", {{"args", {json{{"name", groupName}, {"description", description}, {"inputs", inputs}, {"nodes", nodes}, {"links", links}}}},
                                  {"replace", true}});
    bool applied = false;
    for (scene::NodeId m : scene::modifiers::stack(g, host->id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            if (Node* grpNode = gn::groupOfModifier(g, *mod); grpNode && gn::readGroup(*grpNode).name == groupName)
                applied = true;
    if (!applied)
        ops::execute("gnApply", {{"args", {host->path(), groupName}}});
    g.evaluate();
    Node* modifier = nullptr;
    for (scene::NodeId m : scene::modifiers::stack(g, host->id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            modifier = mod;
    return modifier ? modifier : host;
}

fs::path assemblyDir() { return paths::userConfigDir() / "cliffAssemblies"; }

json runCliffKit(const Args& a)
{
    LILA_ZONE();
    const std::string cmd = "terrainCliffKit";
    Graph& g = Graph::current();
    const TerrainTarget t = edit::oneTerrain(a, cmd);
    const std::string path = terrainPath(t, a);
    if (!t.mesh)
        throw CommandError(cmd + ": the terrain has no mesh");
    const std::string rn = a.get<std::string>("region", "");
    const json r = regionNamed(path, rn, cmd);
    std::vector<std::string> pieces;
    json kit = a.flags.value("kit", json::array());
    if (a.has("assembly"))
    {
        const fs::path file = assemblyDir() / (identifier(a.get<std::string>("assembly", "")) + ".json");
        std::ifstream in(file);
        if (!in)
            throw CommandError(cmd + ": no saved assembly " + a.get<std::string>("assembly", "") + " (in " + assemblyDir().generic_string() + ")");
        kit = json::parse(in).value("pieces", json::array());
    }
    if (kit.is_string())
        kit = json::array({kit});
    for (const json& k : kit)
    {
        Node* n = g.find(k.get<std::string>());
        if (!n)
            throw CommandError(cmd + ": no node named " + k.get<std::string>());
        // A group stands for its meshes.
        if (n->type().isA("mesh"))
            pieces.push_back(n->path());
        else
            for (scene::NodeId c : g.children(n->id()))
                if (Node* cn = g.node(c); cn && cn->type().isA("mesh"))
                    pieces.push_back(cn->path());
    }
    if (pieces.empty())
        throw CommandError(cmd + ": give kit= (kit meshes, or a group of them) or assembly=");
    if (a.has("saveAssembly"))
    {
        fs::create_directories(assemblyDir());
        std::ofstream(assemblyDir() / (identifier(a.get<std::string>("saveAssembly", "")) + ".json")) << json{{"pieces", pieces}}.dump(2);
    }
    const json cliff = r.value("cliff", json::object());
    json region = {{"rings", r.value("rings", json::array())}, {"low", cliff.value("localLow", 0.0)}, {"high", cliff.value("localHigh", 1000.0)}};
    json settings = {{"overlap", a.get<double>("overlap", 0.8)},
                     {"rotate", a.get<double>("rotate", 15.0)},
                     {"scaleJitter", a.get<double>("scaleJitter", 0.2)},
                     {"sink", a.get<double>("sink", 0.1)},
                     {"floatTolerance", a.get<double>("floatTolerance", 25.0)},
                     {"maxPenetration", a.get<double>("maxPenetration", 0.8)},
                     {"gap", a.get<double>("gap", 150.0)},
                     {"seed", a.get<int64_t>("seed", 1)}};
    json names = json::array();
    json nodes = json::array({{{"name", "Terrain"}, {"type", "ObjectInfo"}, {"position", json::array({-400, 0})}, {"props", {{"object", t.mesh->path()}, {"transformSpace", "world"}}}}});
    int row = 1;
    for (const std::string& pth : pieces)
    {
        const std::string item = identifier(g.find(pth)->name());
        names.push_back(item);
        nodes.push_back({{"name", "Item_" + item}, {"type", "ObjectInfo"}, {"position", json::array({-400, 140 * row++})}, {"props", {{"object", pth}}}});
    }
    nodes.push_back({{"name", "Kit"}, {"type", "TerrainCliffKit"}, {"position", json::array({0, 0})},
                     {"props", {{"terrain", "Terrain"}, {"region", region.dump()}, {"pieces", names.dump()}, {"settings", settings.dump()}}}});
    const std::string name = identifier(a.get<std::string>("name", (t.mesh->name()) + "_" + r["name"].get<std::string>() + "_Kit"));
    Node* mod = liveHost(g, name, name + "_Fit", nodes, json::array({"Kit.instances -> GroupOutput.geometry"}), json::array(),
                         "Cliff kit pieces fitted along " + r["name"].get<std::string>() + ".", cmd);
    Node* host = g.find(name);
    Node* group = putGroup(g, identifier(t.mesh->name() + "_Cliffs"));
    if (host && g.parent(host->id()) != group->id())
        g.setParent(host->id(), group->id());
    CliffState state = readState(t, cmd);
    putEntry(state.cliffs, {{"name", name}, {"kind", "kit"}, {"mesh", g.find(name)->path()}, {"region", r["name"]}, {"pieces", pieces}});
    writeState(t, state, {}, cmd);
    g.evaluate();
    json out = cliffNodeResult(mod->id(), "Kit");
    if (out.is_null())
        out = json::object();
    out["mesh"] = g.find(name)->path();
    out["region"] = r["name"];
    out["kit"] = pieces;
    // Export: HISM through unrealPlaceInstances, or a Nanite Assembly (UE 5.7+) built from the same placements.
    const std::string send = a.get<std::string>("send", "none");
    if (send == "hism")
        out["unreal"] = ops::execute("unrealPlaceInstances", {{"args", {g.find(name)->path()}}, {"mode", "hism"}});
    else if (send == "assembly")
        out["unreal"] = ops::execute("terrainSendCliffs", {{"args", {path}}, {"only", json::array({name})}, {"kitAs", "assembly"}});
    return out;
}

std::vector<Tier> defaultTiers()
{
    return {{"boulders", 3200.0, 0.004, "talus", 0.3, 0.8, 1.6, {"boulder"}},
            {"rocks", 1600.0, 0.03, "scatter", 0.6, 0.6, 1.4, {"rock"}},
            {"debris", 400.0, 0.4, "ledge", 0.9, 0.5, 1.5, {"debris"}}};
}

json runCliffScatter(const Args& a)
{
    LILA_ZONE();
    const std::string cmd = "terrainCliffScatter";
    Graph& g = Graph::current();
    const TerrainTarget t = edit::oneTerrain(a, cmd);
    const CliffState state = readState(t, cmd);
    json sources = json::array(), nodes = json::array();
    int row = 0;
    for (const json& e : state.cliffs)
    {
        const std::string kind = e.value("kind", std::string());
        if (kind != "region" && kind != "blockout" && kind != "wall")
            continue;
        Node* n = g.find(e.value("mesh", std::string()));
        if (!n)
            continue;
        const std::string info = "Cliff" + std::to_string(row);
        sources.push_back(info);
        nodes.push_back({{"name", info}, {"type", "ObjectInfo"}, {"position", json::array({-600, 140 * row++})}, {"props", {{"object", n->path()}, {"transformSpace", "world"}}}});
    }
    if (sources.empty())
        throw CommandError(cmd + ": the terrain has no cliff meshes; run terrainCliffMesh or terrainCliffExtrude first");
    json tiers = json::array();
    if (a.has("tiers"))
        tiers = a.flags["tiers"];
    else
        for (const Tier& tr : defaultTiers())
            tiers.push_back({{"name", tr.name}, {"grid", tr.grid}, {"density", tr.density}, {"mask", tr.mask}, {"align", tr.align},
                             {"scale", {tr.scaleMin, tr.scaleMax}}, {"items", tr.items}});
    const json given = a.flags.value("items", json::object());
    std::set<std::string> items;
    for (const json& tr : tiers)
        for (const json& i : tr.value("items", json::array()))
            items.insert(i.get<std::string>());
    json placeholders = json::array();
    int irow = 0;
    for (const std::string& item : items)
    {
        std::string mesh;
        if (given.contains(item))
            mesh = given[item].get<std::string>();
        else if (Node* n = g.find(item); n && n->type().isA("mesh"))
            mesh = n->path();
        if (mesh.empty())
        {
            placeholders.push_back(item);
            continue;
        }
        nodes.push_back({{"name", "Item_" + item}, {"type", "ObjectInfo"}, {"position", json::array({-300, 140 * irow++})}, {"props", {{"object", mesh}}}});
    }
    for (const json& p : placeholders)
        log::warn("{}: no mesh for item {}; its pieces are empty instances (give items={{'{}': '<mesh>'}})", cmd, p.get<std::string>(), p.get<std::string>());
    nodes.push_back({{"name", "Scatter"}, {"type", "TerrainCliffScatter"}, {"position", json::array({0, 0})}, {"props", {{"tiers", tiers.dump()}, {"sources", sources.dump()}}}});
    const std::string name = identifier(a.get<std::string>("name", (t.mesh ? t.mesh->name() : std::string("Terrain")) + "_CliffScatter"));
    json inputs = json::array({{{"name", "seed"}, {"type", "int"}, {"default", a.get<int64_t>("seed", 1)}, {"description", "Another seed gives another scatter."}}});
    Node* mod = liveHost(g, name, name + "_Tiers", nodes, json::array({"GroupInput.seed -> Scatter.seed", "Scatter.instances -> GroupOutput.geometry"}), inputs,
                         "Tiered scatter on cliffs.", cmd);
    json out = cliffNodeResult(mod->id(), "Scatter");
    if (out.is_null())
        out = json::object();
    out["mesh"] = g.find(name)->path();
    out["tiers"] = tiers;
    out["placeholders"] = placeholders;
    if (a.get<bool>("sendToUnreal", false))
        out["unreal"] = ops::execute("unrealPlaceInstances", {{"args", {g.find(name)->path()}}, {"mode", "hism"}});
    return out;
}

// ---------------------------------------------------------------------------------------------------- terrainSendCliffs

/** Lila's ground plane as X and Z: a Z-up scene swaps Y and Z for the cut and back after. */
glm::dmat4 groundFrame()
{
    if (!edit::zUp())
        return glm::dmat4(1.0);
    glm::dmat4 m(0.0);
    m[0][0] = 1.0;
    m[1][2] = 1.0;
    m[2][1] = 1.0;
    m[3][3] = 1.0;
    return m;
}

void writeTileFbx(const Tile& tile, const fs::path& file, const std::string& name)
{
    fbx::ExportScene scene;
    const glm::dmat4 frame = groundFrame();
    auto placed = [&](const geo::Mesh& m) {
        geo::Mesh out = m;
        if (frame != glm::dmat4(1.0))
        {
            gn::transformMesh(out, frame);
            for (geo::Index f = 0; f < out.faceCapacity(); ++f)
                if (out.faceValid(f))
                    out.flipFace(f);
        }
        return std::make_shared<const geo::Mesh>(std::move(out));
    };
    if (tile.lods.size() == 1)
    {
        fbx::ExportNode node;
        node.name = name;
        node.mesh = placed(tile.lods[0]);
        scene.nodes.push_back(std::move(node));
    }
    else
    {
        fbx::ExportNode group;
        group.name = name;
        group.lodGroup = true;
        scene.nodes.push_back(std::move(group));
        for (size_t l = 0; l < tile.lods.size(); ++l)
        {
            fbx::ExportNode node;
            node.name = name + "_LOD" + std::to_string(l);
            node.parent = 0;
            node.mesh = placed(tile.lods[l]);
            scene.nodes.push_back(std::move(node));
        }
    }
    std::string err;
    fs::create_directories(file.parent_path());
    if (!fbx::writeFbx(scene, file, fbx::ExportOptions{}, &err))
        throw std::runtime_error("writing " + file.string() + ": " + err);
}

/**
 * Sends one cliff kit: hism through unrealPlaceInstances, or assembly: its pieces imported with Nanite and one Nanite
 * Assembly (USD, UE 5.7+) drawing every fitted piece, placed as a single actor.
 */
json sendKit(Graph& g, const json& entry, const std::string& kitAs, const std::string& id, const std::string& content, const std::string& hlod,
             const std::function<json(const char*, const json&)>& call, const std::string& cmd)
{
    LILA_ZONE();
    Node* host = g.find(entry.value("mesh", std::string()));
    const std::string name = identifier(entry.value("name", std::string("Kit")));
    if (!host)
        throw std::runtime_error(name + ": the kit's mesh is gone");
    const std::string group = id + "/kits/" + name;
    if (kitAs == "hism")
    {
        call("removePlacements", {{"groups", json::array({group})}});
        json place = {{"args", {host->path()}}, {"mode", "hism"}, {"folder", content + "/Kits"}, {"wait", true}};
        if (!hlod.empty())
            place["hlodLayer"] = hlod;
        return ops::execute("unrealPlaceInstances", place);
    }
    ops::execute("unrealRemovePlacements", {{"args", {host->path()}}});

    link::PlacementOptions o;
    o.folder = content + "/Kits/" + name;
    o.mode = "hism";
    o.collision = "complexAsSimple";
    o.nanite = true;
    o.lightingChannels = false;
    std::vector<scene::MeshPtr> meshes;
    const json plan = link::placementPlan(g, {host->id()}, o, meshes);
    std::vector<size_t> partGroups;
    const std::vector<link::AssemblyInstance> instances = link::assemblyInstances(plan["groups"], partGroups);
    if (instances.empty())
        throw std::runtime_error(name + ": the kit has no fitted pieces");
    const fs::path dir = link::cacheDir() / "terrain" / (name + "_kit");
    std::vector<std::string> parts;
    json errors = json::array();
    for (size_t gi : partGroups)
    {
        const json& grp = plan["groups"][gi];
        const std::string contentPath = grp["contentPath"].get<std::string>();
        const std::string asset = contentPath.substr(contentPath.rfind('/') + 1);
        const fs::path file = dir / (asset + ".fbx");
        std::string err;
        const scene::MeshPtr mesh = grp.contains("gnMesh") ? meshes[grp["gnMesh"].get<size_t>()] : nullptr;
        if (!mesh || !link::writeMeshFbx(mesh, asset, file, &err))
            throw std::runtime_error(name + ": writing " + asset + " failed: " + err);
        const json mapping = {{"contentPath", contentPath}, {"kind", "static"}, {"collision", "complexAsSimple"}, {"nanite", true}, {"reapplyOptions", true}};
        const json imported = call("importAsset", link::importParams(file, mapping));
        std::string objectPath = grp["objectPath"].get<std::string>();
        for (const json& made : imported.value("assets", json::array()))
            if (made.is_string() && made.get<std::string>().ends_with("/" + asset + "." + asset))
                objectPath = made.get<std::string>();
        parts.push_back(objectPath);
        std::error_code ec;
        fs::remove(file, ec);
    }
    const std::string root = "SM_" + name + "_Assembly";
    const std::string assemblyPath = content + "/Kits/" + root;
    // A Nanite Assembly rebuilds whenever a part is reimported; replacing it outright is quicker and never races that build.
    try
    {
        call("deleteAssets", {{"paths", json::array({assemblyPath + "." + root})}});
    }
    catch (const std::exception& e)
    {
        errors.push_back(e.what());
    }
    const fs::path usd = dir / (root + ".usda");
    std::string err;
    if (!link::writeNaniteAssemblyUsda(usd, root, parts, instances, &err))
        throw std::runtime_error(name + ": " + err);
    const json imported = call("importAsset", link::importParams(usd, {{"contentPath", assemblyPath}, {"kind", "static"}, {"nanite", true}}));
    for (const json& e : imported.value("errors", json::array()))
        errors.push_back(e.is_string() ? e.get<std::string>() : e.dump());
    std::string assembly = assemblyPath + "." + root;
    for (const json& made : imported.value("assets", json::array()))
        if (made.is_string() && made.get<std::string>().ends_with("/" + root + "." + root))
            assembly = made.get<std::string>();
    json params = {{"group", group},
                   {"label", root},
                   {"mesh", assembly},
                   {"mode", "actors"},
                   {"placements", json::array({{{"id", group}, {"name", root}, {"location", json::array({0, 0, 0})},
                                                {"rotation", json::array({0, 0, 0, 1})}, {"scale", json::array({1, 1, 1})}}})}};
    if (!hlod.empty())
        params["hlodLayer"] = hlod;
    call("placeInstances", params);
    log::info("{}: kit {} sent as a Nanite Assembly of {} pieces ({} parts)", cmd, name, instances.size(), parts.size());
    return {{"name", name}, {"as", "assembly"}, {"assembly", assembly}, {"parts", parts}, {"instances", instances.size()}, {"errors", errors}};
}

json runSendCliffs(const Args& a)
{
    LILA_ZONE();
    const std::string cmd = "terrainSendCliffs";
    Graph& g = Graph::current();
    const TerrainTarget t = edit::oneTerrain(a, cmd);
    const std::string id = terrainPath(t, a);
    const CliffState state = readState(t, cmd);
    std::set<std::string> only;
    for (const json& o : a.flags.value("only", json::array()))
        only.insert(o.get<std::string>());
    std::vector<std::pair<std::string, Node*>> meshes;
    json kits = json::array();
    for (const json& e : state.cliffs)
    {
        const std::string name = e.value("name", std::string());
        if (!only.empty() && !only.count(name))
            continue;
        Node* n = g.find(e.value("mesh", std::string()));
        if (!n)
            continue;
        if (e.value("kind", std::string()) == "kit")
            kits.push_back(e);
        else
            meshes.push_back({name, n});
    }
    for (const json& x : a.flags.value("extra", json::array()))
    {
        Node* n = g.find(x.get<std::string>());
        if (!n || !n->type().isA("mesh"))
            throw CommandError(cmd + ": extra " + x.get<std::string>() + " is not a mesh");
        meshes.push_back({identifier(n->name()), n});
    }
    if (meshes.empty() && kits.empty())
        throw CommandError(cmd + ": nothing to send: the terrain has no cliffs (terrainCliffMesh, terrainCliffExtrude, terrainCliffKit) and no extra= meshes");
    TileParams tp;
    tp.cell = a.get<double>("cellSize", tp.cell);
    tp.lods = int(a.get<int64_t>("lods", tp.lods));
    tp.lodRatio = a.get<double>("lodRatio", tp.lodRatio);
    tp.collisionRatio = a.get<double>("collisionRatio", tp.collisionRatio);
    tp.nanite = a.get<std::string>("nanite", tp.nanite);
    tp.naniteTriangles = size_t(a.get<int64_t>("naniteTriangles", int64_t(tp.naniteTriangles)));
    const bool dryRun = a.get<bool>("dryRun", false);
    const std::string label = identifier(t.mesh ? t.mesh->name() : std::string("Terrain"));
    const fs::path folder = dryRun && a.has("folder") ? paths::resolvePath(a.get<std::string>("folder", "")) : link::cacheDir() / "terrain" / (label + "_cliffs");
    const std::string content = a.get<std::string>("contentFolder", "/Game/LilaLink/Landscapes/" + label + "/Cliffs");
    const std::string hlod = a.get<std::string>("hlodLayer", "");
    const glm::dmat4 frame = groundFrame();

    struct Planned
    {
        std::string key, name;
        Tile tile;
    };
    std::vector<Planned> planned;
    json perMesh = json::array();
    for (const auto& [name, n] : meshes)
    {
        const scene::MeshPtr m = meshOf(g, n);
        if (!m || m->empty())
            continue;
        geo::Mesh world = *m;
        gn::transformMesh(world, frame * worldOf(n));
        std::vector<Tile> tiles = cutTiles(fromMesh(world), tp);
        size_t tris = 0, nanite = 0;
        for (Tile& tile : tiles)
        {
            tris += tile.triangles;
            nanite += tile.nanite;
            const std::string suffix = std::format("x{}_y{}", tile.cell.first, tile.cell.second);
            planned.push_back({id + "/cliffs/" + name + "/" + suffix, "SM_" + label + "_" + identifier(name) + "_" + suffix, std::move(tile)});
        }
        perMesh.push_back({{"name", name}, {"mesh", n->path()}, {"tiles", tiles.size()}, {"triangles", tris}, {"naniteTiles", nanite}});
    }
    json tilesJson = json::array();
    for (const Planned& p : planned)
    {
        const glm::dvec3 c = glm::dvec3(frame * glm::dvec4(p.tile.centre, 1.0));
        tilesJson.push_back({{"key", p.key}, {"name", p.name}, {"cell", {p.tile.cell.first, p.tile.cell.second}}, {"centre", edit::vec3(c)},
                             {"triangles", p.tile.triangles}, {"lods", p.tile.lods.size()}, {"nanite", p.tile.nanite}});
    }
    json out = {{"cellSize", tp.cell}, {"meshes", perMesh}, {"tiles", tilesJson}, {"tileCount", planned.size()}};
    const std::string kitAs = a.get<std::string>("kitAs", "assembly");
    json kitNames = json::array();
    for (const json& k : kits)
        kitNames.push_back(k.value("name", std::string()));
    out["kits"] = {{"names", kitNames}, {"as", kitAs}};
    if (dryRun)
    {
        if (a.has("folder"))
            for (const Planned& p : planned)
                writeTileFbx(p.tile, folder / (p.name + ".fbx"), p.name);
        out["dryRun"] = true;
        return out;
    }

    try
    {
        link::Link::get().ensureConnected();
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": not connected to Unreal: " + e.what());
    }
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    if (!client)
        throw CommandError(cmd + ": not connected to Unreal");
    auto call = [&](const char* method, const json& params) {
        link::RpcResult r = client->call(method, params, 300000);
        if (!r.ok)
            throw std::runtime_error(std::string(method) + ": " + r.error);
        return r.result;
    };
    std::vector<link::TileItem> items;
    std::vector<std::string> assets(planned.size());
    // The cliff material: MI_<terrain>_Cliffs on M_LilaTerrainVolume (MF_LilaTerrainVolume).
    bool material = a.get<bool>("material", true);
    json materialResult;
    if (material)
    {
        const json features = link::Link::get().status().value("features", json::array());
        if (!features.is_array() || std::find(features.begin(), features.end(), json("cliffMaterial")) == features.end())
        {
            material = false;
            out["warnings"].push_back("the LilaLink plugin in Unreal predates cliff materials: the tiles keep their imported material; rebuild LilaLink from Lila's ue/LilaLink");
        }
    }
    for (size_t i = 0; i < planned.size(); ++i)
    {
        const Planned& p = planned[i];
        link::TileItem it;
        it.key = p.key;
        it.hash = p.tile.hash ^ link::hashBytes(hlod.data(), hlod.size());
        it.group = p.key;
        json options = {{"nanite", p.tile.nanite}, {"collision", "complexAsSimple"}, {"existing", "replace"}, {"reapplyOptions", true}};
        // Without Nanite the collision comes from the last LOD: simplified, and its borders still meet the neighbours'.
        options["lodForCollision"] = !p.tile.nanite && p.tile.lods.size() > 1 ? int64_t(p.tile.lods.size() - 1) : int64_t(0);
        it.mapping = options;
        it.mapping["contentPath"] = content + "/" + p.name;
        it.mapping["kind"] = "static";
        it.write = [&p, &folder]() {
            const fs::path file = folder / (p.name + ".fbx");
            writeTileFbx(p.tile, file, p.name);
            return file;
        };
        const glm::dvec3 c = glm::dvec3(frame * glm::dvec4(p.tile.centre, 1.0));
        it.after = [&call, &p, c, hlod, i, &assets, material, &label, &materialResult](const std::string& asset) {
            assets[i] = asset;
            // Per tile, so a long send never loads every tile at the end to give them their material.
            if (material)
                materialResult = call("setCliffMaterial", {{"label", label}, {"meshes", json::array({asset})}});
            const math::Vector u = math::toUnreal(math::Vector(c.x, c.y, c.z));
            json params = {{"group", p.key},
                           {"label", p.name},
                           {"mesh", asset},
                           {"mode", "actors"},
                           {"placements", json::array({{{"id", p.key}, {"name", p.name}, {"location", json::array({u.x, u.y, u.z})},
                                                        {"rotation", json::array({0, 0, 0, 1})}, {"scale", json::array({1, 1, 1})}}})}};
            if (!hlod.empty())
                params["hlodLayer"] = hlod;
            call("placeInstances", params);
        };
        items.push_back(std::move(it));
    }
    link::TileSendOptions so;
    so.manifest = identifier(id) + "_cliffs";
    so.force = a.get<bool>("force", false);
    so.collectEvery = int(a.get<int64_t>("collectEvery", 8));
    const int64_t stopAfter = a.get<int64_t>("stopAfter", -1);
    so.progress = [stopAfter](size_t index, size_t total, const std::string& key) {
        log::info("terrainSendCliffs: tile {} of {} ({})", index + 1, total, key);
        return stopAfter < 0 || int64_t(index) < stopAfter;
    };
    const link::TileSendResult r = items.empty() ? link::TileSendResult{} : link::sendTiles(*client, items, so);
    json kitResults = json::array();
    if (!r.cancelled)
        for (const json& k : kits)
        {
            try
            {
                kitResults.push_back(sendKit(g, k, kitAs, id, content, hlod, call, cmd));
            }
            catch (const std::exception& e)
            {
                kitResults.push_back({{"name", k.value("name", std::string())}, {"error", e.what()}});
                log::warn("{}: kit {}: {}", cmd, k.value("name", std::string()), e.what());
            }
        }
    out["kits"]["sent"] = kitResults;
    json sentAssets = json::array();
    for (const std::string& s : r.assets)
        if (!s.empty())
            sentAssets.push_back(s);
    if (!r.stale.empty())
    {
        json prefixes = json::array();
        for (const std::string& k : r.stale)
            prefixes.push_back(k);
        try
        {
            call("removePlacements", {{"ids", prefixes}, {"groups", prefixes}});
        }
        catch (const std::exception& e)
        {
            log::warn("{}: stale tiles stay in Unreal: {}", cmd, e.what());
        }
    }
    if (!materialResult.is_null())
        out["material"] = materialResult.value("material", json());
    out["sent"] = r.sent;
    out["skipped"] = r.skipped;
    out["restored"] = r.restored;
    out["replaced"] = r.replaced;
    if (!r.warnings.empty())
        for (const std::string& w : r.warnings)
            out["warnings"].push_back(w);
    out["failed"] = r.failed;
    out["cancelled"] = r.cancelled;
    out["stale"] = r.stale;
    out["errors"] = r.errors;
    out["assets"] = sentAssets;
    if (r.cancelled)
        out["resume"] = "send again with the same flags to carry on from the next tile";
    return out;
}

// ---------------------------------------------------------------------------------------------------- registration

std::vector<FlagDef> meshFlags()
{
    return shapeFlags({{"region", "r", T::Any, "all", "Region(s) to build: 'cliff_1' (or 'region:cliff_1'), a list, or 'all'."},
                       nodeFlag({"blockout", "bo", T::String, nullptr, "Build from this mesh instead of a region: any blockout, refined and shaped along its normals."}, "mesh"),
                       {"mode", "m", T::String, "proud", "proud: the cliff stands on the terrain, its base sunk below the ground and its top tucked under the plateau, the ground set back behind it. inset: holes under the cliff and the mesh fills them, its edge on the ground's heights and normals.", {"proud", "inset"}},
                       {"detail", "dt", T::Float, 4.0, "Mesh edges per heightfield cell.", {}, {}, 0.25, 32.0},
                       {"margin", "mg", T::Int, int64_t(1), "Samples beyond the region the mesh covers.", {}, {}, 0.0, 16.0},
                       {"blend", "bl", T::Float, 300.0, "Width of the band where the shape fades onto the ground at the mesh's edge, cm.", {}, {}, 1.0},
                       {"baseSink", "bs", T::Float, 150.0, "Proud: how far the bottom edge sinks below the ground, cm.", {}, {}, 0.0},
                       {"lipSink", "ls", T::Float, 15.0, "Proud: how far the top edge tucks under the plateau, cm.", {}, {}, 0.0},
                       {"recess", "rc", T::Float, 1.2, "Proud: the ground behind the cliff is set back this many times depth.", {}, {}, 0.0, 4.0},
                       {"skirt", "sk", T::Float, 200.0, "Inset: how far a skirt drops below the ground at the edge, cm.", {}, {}, 0.0},
                       {"talusWidth", "tw", T::Float, 800.0, "Band below the cliff written to the cliffTalus mask for scree, cm.", {}, {}, 0.0},
                       {"maxTriangles", "mt", T::Int, int64_t(4000000), "Most triangles per cliff; past it the detail drops, with a warning.", {}, {}, 1000.0},
                       {"lowPoly", "lp", T::Bool, false, "Also make <cliff>_low, reduced, with a normal and AO map baked from the full cliff (for non-Nanite targets)."},
                       {"lowPolyRatio", "lpr", T::Float, 0.05, "Triangles the low poly keeps.", {}, {}, 0.001, 1.0},
                       {"bakeResolution", "br", T::Int, int64_t(2048), "Low poly map size.", {}, {}, 64.0, 16384.0},
                       {"name", "n", T::String, nullptr, "Blockout: the cliff mesh's name; defaults to <blockout>_cliff."}});
}
} // namespace

void registerCliffCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainCliffs",
        .doc = "Find the cliffs on a terrain: regions steeper than slope degrees, at least minArea m2 and minHeight cm from foot "
               "to top, with patches closer than mergeDistance joined. Named cliff_1, cliff_2, ... largest first and stored "
               "as regions, so every terrain command takes 'region:cliff_1' and material rules read the mask region_cliff_1. "
               "Each has its facing (outward, downhill), bearing (0 facing -Z, 90 facing +X, like the aspect field), strata "
               "direction (strataDip tilts it toward the face), height range, area and slope. mesh=True builds every cliff "
               "mesh at once (terrainCliffMesh). list=True lists the stored cliffs and what was built from them.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"slope", "sl", T::Float, 50.0, "Steepest slope that is still ground, degrees.", {}, {}, 1.0, 89.0},
                  {"minArea", "ma", T::Float, 40.0, "Smallest cliff face, m2.", {}, {}, 0.0},
                  {"minHeight", "mh", T::Float, 400.0, "Smallest drop from foot to top, cm.", {}, {}, 0.0},
                  {"mergeDistance", "md", T::Float, 400.0, "Steep patches closer than this are one cliff, cm.", {}, {}, 0.0},
                  {"maxRegions", "mr", T::Int, int64_t(64), "Most cliffs, largest first.", {}, {}, 1.0, 1024.0},
                  {"strataDip", "sdp", T::Float, 0.0, "Tilt of the rock layers toward each face, degrees.", {}, {}, -60.0, 60.0},
                  {"store", "st", T::Bool, true, "Keep them as regions cliff_1, ... (replacing the last set)."},
                  {"mesh", "me", T::Bool, false, "Also build every cliff's mesh with terrainCliffMesh's defaults."},
                  {"list", "ls", T::Bool, false, "List the stored cliffs and the meshes, walls and kits built from them."}},
        .run = [](const Args& a) { return runCliffs(a); },
        .examples = {"c = terrainCliffs('Terrain', slope=55, minHeight=800)\n[(r['name'], r['heightCm'], r['facingDegrees']) for r in c['regions']]",
                     "terrainCliffs('Terrain', mesh=True)"},
    });

    ctx.registerCommand({
        .name = "terrainCliffMesh",
        .doc = "Build cliff meshes from terrain regions (terrainCliffs) or from a blockout mesh: the face at detail edges per "
               "heightfield cell, pushed in by depth, then shaped back out by rock layers (each cut back at its base by "
               "undercut, some standing out over the next as ledges), noise, overhang and a rock height map, never past "
               "where the terrain or blockout was. The masks go into the mesh's vertex colour (R strata, G 1 - cavity, B up-facing, "
               "A scatter zones) and as point attributes (strata, cavity, upFacing, ledge, scatter, talus) for materials, "
               "PCG and scatterProps(mask=); uv0 is projected along the cliff's facing, one island per layer.\n\n"
               "mode='proud' (default) keeps the landscape whole: the ground behind the cliff is set back (recess) so the mesh "
               "stands in front, the bottom edge sinks baseSink below the ground and the top tucks lipSink under the plateau. "
               "mode='inset' opens holes under the cliff (the holes mask, sent as the landscape's visibility layer) and the "
               "mesh fills them exactly, its edge on the ground's heights and normals with a skirt below. Either way the "
               "terrain gains masks cliff, cliffTalus (the band below each cliff; ground cover's scree preset reads it) and "
               "cliffTop. Meshes are <terrain>_<region> under <terrain>_Cliffs; building again replaces them. lowPoly=True "
               "adds a reduced copy with normal and AO maps baked from the full cliff. Returns per cliff the mesh, vertices, "
               "triangles, layers, deepestCut, baseClearance (proud: the highest bottom vertex relative to the ground, "
               "negative is buried) and hidden (the part of a proud cliff the ground still covers).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = meshFlags(),
        .run = [](const Args& a) { return runCliffMesh(a); },
        .examples = {"terrainCliffs('Terrain')\nterrainCliffMesh('Terrain', region='cliff_1', depth=200, strataHeight=300, ledges=0.5)",
                     "terrainCliffMesh('Terrain', region='all', mode='inset')", "terrainCliffMesh('Terrain', blockout='CliffBlock', depth=120)"},
    });

    ctx.registerCommand({
        .name = "terrainCliffExtrude",
        .doc = "Raise a cliff wall along a curve on a terrain, selected mesh edges (edges={mesh, ids}) or a region's outline "
               "(region=): from the ground up height cm, with a profile (vertical, battered: leaning back by batter, overhang: "
               "leaning out, stepped: steps terraces, or profileCurve= drawn in its XY plane, X out and Y up), the same strata, "
               "ledges and noise as terrainCliffMesh, and its masks. The face looks to the right of the curve's direction "
               "(flip=True turns it). With stamp=True the terrain is raised to a plateau behind the wall (plateau cm wide) and "
               "cut down to its base in front (cut cm). Live: edit the curve and the wall and terrain follow. Returns the wall "
               "mesh, its group and the ground edit.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = shapeFlags({nodeFlag({"curve", "cv", T::String, nullptr, "The curve the wall follows."}, "curve"),
                             {"edges", "ed", T::Any, nullptr, "Mesh edges to follow: {mesh, ids} (made into a live curve)."},
                             {"region", "r", T::String, nullptr, "A region whose outline the wall follows ('cliff_1')."},
                             {"height", "ht", T::Float, 1500.0, "Wall height, cm.", {}, {}, 1.0},
                             {"profile", "pf", T::String, "vertical", "Cross-section.", {"vertical", "battered", "overhang", "stepped"}},
                             nodeFlag({"profileCurve", "pc", T::String, nullptr, "A curve for the cross-section, in its XY plane: X out from the face, Y up."}, "curve"),
                             {"batter", "bt", T::Float, 0.2, "Battered and stepped: how far the top sits back (overhang: out), as a fraction of height.", {}, {}, 0.0, 2.0},
                             {"steps", "stp", T::Int, int64_t(3), "Stepped: terraces.", {}, {}, 1.0, 32.0},
                             {"sink", "snk", T::Float, 150.0, "How far the wall's foot goes below the ground, cm.", {}, {}, 0.0},
                             {"detail", "dt", T::Float, 4.0, "Mesh edges per heightfield cell.", {}, {}, 0.25, 32.0},
                             {"flip", "fl", T::Bool, false, "Face the other side of the curve."},
                             {"stamp", "sm", T::Bool, true, "Raise a plateau behind the wall and cut the base in front."},
                             {"plateau", "pl", T::Float, 1500.0, "Width of the plateau behind, cm.", {}, {}, 0.0},
                             {"cut", "ct", T::Float, 600.0, "Width of the cut in front, cm.", {}, {}, 0.0},
                             {"falloff", "fo", T::Float, 800.0, "Blend back to the ground past plateau and cut, cm.", {}, {}, 1.0},
                             {"name", "n", T::String, nullptr, "The wall mesh; defaults to <curve>_cliff."}}),
        .run = [](const Args& a) { return runCliffExtrude(a); },
        .examples = {"terrainCliffExtrude('Terrain', curve='Escarpment', height=2500, profile='battered', strataHeight=350)",
                     "terrainCliffExtrude('Terrain', region='cliff_2', height=1200, profile='stepped', steps=4)"},
    });

    ctx.registerCommand({
        .name = "terrainCliffKit",
        .doc = "Dress a cliff region with your own kit pieces instead of a generated mesh: each piece faces out from the cliff, "
               "scaled to its height (jittered), overlapped (overlap under 1) and turned up to rotate degrees to hide repeats, "
               "its base sunk by sink of its height. Pieces floating more than floatTolerance cm above the ground or buried "
               "past maxPenetration are dropped, and gaps wider than gap cm get smaller pieces. Kit pieces stand on their "
               "base with their face toward +Z. Live: a mesh <terrain>_<region>_Kit of instances (piece, filler) that follows "
               "the terrain and the pieces. saveAssembly= keeps the kit as a named assembly, assembly= reuses one. send='hism' "
               "sends it as HISM instances, send='assembly' as a Nanite Assembly (UE 5.7+, r.Nanite.AllowAssemblies=1). "
               "Returns placed, fillers, rejectedFloating, rejectedPenetrating and coverage (of the region's outline).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"region", "r", T::String, nullptr, "The cliff region ('cliff_1' or 'region:cliff_1')."},
                  nodeFlag({"kit", "k", T::Any, nullptr, "Kit meshes, or a group holding them."}, "mesh|transform"),
                  {"assembly", "as", T::String, nullptr, "Use a saved assembly's pieces."},
                  {"saveAssembly", "sa", T::String, nullptr, "Save these pieces as a named assembly."},
                  {"overlap", "ov", T::Float, 0.8, "Spacing as a fraction of a piece's width; under 1 overlaps.", {}, {}, 0.2, 2.0},
                  {"rotate", "ro", T::Float, 15.0, "Random turn either way, degrees.", {}, {}, 0.0, 90.0},
                  {"scaleJitter", "sj", T::Float, 0.2, "Scale variation, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"sink", "sk", T::Float, 0.1, "Base sunk into the ground, as a fraction of a piece's height.", {}, {}, 0.0, 1.0},
                  {"floatTolerance", "ft", T::Float, 25.0, "A piece this far above the ground is floating, cm.", {}, {}, 0.0},
                  {"maxPenetration", "mp", T::Float, 0.8, "How far into a piece's depth the cliff face behind it may reach, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"gap", "gp", T::Float, 150.0, "Gaps wider than this get smaller pieces, cm.", {}, {}, 1.0},
                  {"seed", "sd", T::Int, int64_t(1), "Another seed gives another arrangement."},
                  {"send", "se", T::String, "none", "Send it to Unreal: none, hism or assembly.", {"none", "hism", "assembly"}},
                  {"name", "n", T::String, nullptr, "The kit mesh; defaults to <terrain>_<region>_Kit."}},
        .run = [](const Args& a) { return runCliffKit(a); },
        .examples = {"terrainCliffKit('Terrain', region='cliff_1', kit=['Rock_A', 'Rock_B', 'Rock_C'], overlap=0.7)",
                     "terrainCliffKit('Terrain', region='cliff_2', kit='CliffKit', saveAssembly='granite', send='hism')"},
    });

    ctx.registerCommand({
        .name = "terrainCliffScatter",
        .doc = "Scatter on a terrain's cliff meshes by tiers, each tier on its own grid with a seed per grid cell (big rocks on a "
               "coarse grid, debris on a fine one, as PCG's hierarchical generation does), so editing one cliff only changes "
               "the cells it touches. A tier is {name, grid (cm), density (per m2 where its mask is 1), mask (a cliff mesh "
               "attribute: scatter, ledge, talus, upFacing, cavity), align (0 upright, 1 on the surface), scale: [min, max], "
               "items}; the default tiers put boulders on talus, rocks on scatter zones and debris on ledges. Instances carry "
               "tier, item, slope and downhill. Live: a mesh <terrain>_CliffScatter. Returns the count per tier.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"tiers", "t", T::Any, nullptr, "The tiers; default boulders, rocks and debris."},
                  {"items", "it", T::Any, nullptr, "{item name: mesh}."},
                  {"seed", "sd", T::Int, int64_t(1), "Another seed gives another scatter."},
                  {"sendToUnreal", "ue", T::Bool, false, "Then send it to Unreal as HISM instances."},
                  {"name", "n", T::String, nullptr, "The scatter mesh; defaults to <terrain>_CliffScatter."}},
        .run = [](const Args& a) { return runCliffScatter(a); },
        .examples = {"terrainCliffScatter('Terrain', items={'boulder': 'SM_Boulder', 'rock': 'SM_Rock', 'debris': 'SM_Pebble'})"},
    });

    ctx.registerCommand({
        .name = "terrainSendCliffs",
        .doc = "Send a terrain's cliffs (and extra= meshes, such as contact skirts or strips) to Unreal cut on the World "
               "Partition grid (cellSize cm, lines at whole multiples from the origin): tiles share their border vertices "
               "exactly, every LOD and the collision are reduced with their borders kept so neighbours meet at any LOD, each "
               "tile's pivot is its centre so its actor lands in its own cell, Nanite per tile (nanite: auto turns it on past "
               "naniteTriangles), and without Nanite the collision comes from the last LOD. Tiles go one at a time "
               "through LilaLink with progress; only tiles whose content changed, or whose asset or actor Unreal lost, are sent "
               "again (a lost actor is placed again without reimporting); an interrupted send "
               "carries on where it stopped. hlodLayer= puts every actor on that HLOD layer. Tiles get MI_<terrain>_Cliffs "
               "(MF_LilaTerrainVolume). dryRun=True plans the tiles (and writes their FBX to folder= when given) without "
               "Unreal. Returns the tiles, sent, skipped, restored, replaced, failed, stale and assets.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"cellSize", "cs", T::Float, 12800.0, "World Partition cell size, cm.", {}, {}, 100.0},
                  {"lods", "ld", T::Int, int64_t(3), "Levels per tile.", {}, {}, 1.0, 8.0},
                  {"lodRatio", "lr", T::Float, 0.35, "Triangles each level keeps of the one before.", {}, {}, 0.001, 1.0},
                  {"collisionRatio", "cr", T::Float, 0.1, "Triangles the collision keeps (Nanite tiles).", {}, {}, 0.001, 1.0},
                  {"nanite", "na", T::String, "auto", "Nanite per tile: on, off or auto.", {"on", "off", "auto"}},
                  {"naniteTriangles", "nt", T::Int, int64_t(50000), "auto: Nanite for tiles over this many triangles.", {}, {}, 0.0},
                  {"hlodLayer", "hl", T::String, "", "HLOD layer asset path for every tile's actor."},
                  nodeFlag({"extra", "ex", T::List, json::array(), "More meshes to tile and send (contact skirts, strips)."}, "mesh"),
                  {"only", "on", T::List, json::array(), "Send only these cliffs, by name."},
                  {"kitAs", "ka", T::String, "assembly", "Kits: assembly (a Nanite Assembly) or hism.", {"assembly", "hism"}},
                  {"contentFolder", "cf", T::String, nullptr, "Where the tile assets go in Unreal; defaults to /Game/LilaLink/Landscapes/<terrain>/Cliffs."},
                  {"material", "mat", T::Bool, true, "Give the tiles MI_<terrain>_Cliffs."},
                  {"force", "fc", T::Bool, false, "Send every tile, changed or not."},
                  {"collectEvery", "ce", T::Int, int64_t(8), "Ask Unreal to collect garbage after this many tiles.", {}, {}, 0.0},
                  {"stopAfter", "sa", T::Int, int64_t(-1), "Stop after this many tiles (a resumable partial send); -1 sends all.", {}, {}, -1.0},
                  {"dryRun", "dr", T::Bool, false, "Plan the tiles without Unreal."},
                  {"folder", "fo", T::String, nullptr, "dryRun: write the tile FBX files here."}},
        .undoable = false,
        .run = [](const Args& a) { return runSendCliffs(a); },
        .examples = {"terrainSendCliffs('Terrain', cellSize=12800, hlodLayer='/Game/HLOD/HLOD_Cliffs')",
                     "t = terrainSendCliffs('Terrain', dryRun=True)\n[(x['name'], x['triangles'], x['nanite']) for x in t['tiles']]"},
    });
}

// ---------------------------------------------------------------------------------------------------- checks

json cliffIssues(const std::string& terrain, double tolerance, double triangleBudget, double cellSize)
{
    json issues = json::array();
    Graph& g = Graph::current();
    const TerrainTarget t = edit::findTerrain(g, terrain, "terrainCheck");
    const CliffState state = readState(t, "terrainCheck");
    if (state.cliffs.empty())
        return issues;
    const Heightfield& h = *t.local;
    auto issue = [&](const char* type, const std::string& about, const json& at, const std::string& msg) {
        issues.push_back({{"type", type}, {"about", about}, {"position", at}, {"message", msg}});
    };
    const double vertical = t.vertical();
    for (const json& e : state.cliffs)
    {
        const std::string name = e.value("name", std::string()), kind = e.value("kind", std::string());
        Node* n = g.find(e.value("mesh", std::string()));
        if (!n)
        {
            issue("cliffMissing", name, nullptr, "the cliff's mesh " + e.value("mesh", std::string()) + " is gone");
            continue;
        }
        if (kind == "kit")
        {
            const scene::MeshPtr km = meshOf(g, n);
            (void)km;
            continue;
        }
        const scene::MeshPtr m = meshOf(g, n);
        if (!m || m->empty())
            continue;
        geo::Mesh local = *m;
        gn::transformMesh(local, t.inv * worldOf(n));
        TriMesh tm = fromMesh(local);
        const std::vector<uint8_t> border = borderVertices(tm);
        const int hf = tm.vertexIndex("heightFraction");
        // Floating: the bottom edge above the ground; cracks: the top edge above the plateau (proud), or a seam gap (inset).
        double worstFloat = 0.0, worstLip = 0.0;
        glm::dvec3 floatAt(0.0), lipAt(0.0);
        for (size_t v = 0; v < tm.p.size(); ++v)
        {
            if (!border[v])
                continue;
            const double gap = (tm.p[v].y - h.heightAt(tm.p[v].x, tm.p[v].z)) * vertical;
            const bool low = hf >= 0 ? tm.vattr(v)[hf] < 0.5 : true;
            if (low && gap > worstFloat)
            {
                worstFloat = gap;
                floatAt = tm.p[v];
            }
            if (!low && gap > worstLip)
            {
                worstLip = gap;
                lipAt = tm.p[v];
            }
        }
        if (kind != "region" || e.value("mode", std::string("proud")) == "proud")
        {
            if (worstFloat > tolerance)
                issue("floatingCliff", name, edit::vec3(t.world(floatAt)), std::format("bottom edge {:.0f} cm above the ground", worstFloat));
            if (kind == "region" && worstLip > tolerance)
                issue("cliffCrack", name, edit::vec3(t.world(lipAt)), std::format("top edge {:.0f} cm above the plateau: a gap shows", worstLip));
        }
        else
        {
            std::vector<std::shared_ptr<const geo::Mesh>> meshes{std::make_shared<const geo::Mesh>(local)};
            const SeamReport s = measureSeams(h, meshes);
            if (s.uncovered > 0.0 || s.maxGap * vertical > tolerance)
                issue("cliffCrack", name, edit::vec3(t.world(s.worst)),
                      std::format("seam gap {:.1f} cm, {:.0f} cm of the hole edge uncovered", s.maxGap * vertical, s.uncovered * t.across()));
        }
        // Tiles over budget: triangles per World Partition cell.
        geo::Mesh world = *m;
        gn::transformMesh(world, worldOf(n));
        std::map<std::pair<int, int>, size_t> perCell;
        const geo::Triangulation tris = geo::triangulate(world);
        for (size_t k = 0; k < tris.faces.size(); ++k)
        {
            const glm::dvec3 c = (world.position(world.cornerPoint(tris.corners[k * 3])) + world.position(world.cornerPoint(tris.corners[k * 3 + 1])) +
                                  world.position(world.cornerPoint(tris.corners[k * 3 + 2]))) / 3.0;
            const double gx = c.x, gz = edit::zUp() ? c.y : c.z;
            ++perCell[{int(std::floor(gx / cellSize)), int(std::floor(gz / cellSize))}];
        }
        for (const auto& [cell, count] : perCell)
            if (double(count) > triangleBudget)
                issue("tileOverBudget", name, json::array({cell.first, cell.second}),
                      std::format("{} triangles in World Partition cell x{} y{}, over {:.0f}; lower detail or send with nanite='on'", count, cell.first, cell.second, triangleBudget));
    }
    return issues;
}

json sendCliffsWith(const std::string& terrain, const json& flags)
{
    Graph& g = Graph::current();
    const TerrainTarget t = edit::findTerrain(g, terrain, "terrainSendToUnreal");
    if (readState(t, "terrainSendToUnreal").cliffs.empty())
        return nullptr;
    json call = flags;
    call["args"] = json::array({terrain});
    return ops::execute("terrainSendCliffs", call);
}
} // namespace terrain::cliffs
