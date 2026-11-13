// Terrain commands. A terrain is a mesh whose geometry-nodes modifier makes a heightfield; the commands build that group,
// read what it made (in world space, whatever the mesh's placement and the scene's up axis), stamp, sculpt and route on
// it, and bake and export it.

#include "Analysis.h"
#include "Checks.h"
#include "CliffScene.h"
#include "Erosion.h"
#include "ErosionGpu.h"
#include "Export.h"
#include "ExportEngines.h"
#include "Inputs.h"
#include "LandformCommands.h"
#include "MaterialNodes.h"
#include "Stamps.h"
#include "Terrain.h"
#include "TerrainEdit.h"
#include "Water.h"
#include "Volume.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;

bool zUp() { return math::upAxis() == math::UpAxis::Z; }
glm::dvec3 upVector() { return zUp() ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0); }
double upOf(const glm::dvec3& w) { return zUp() ? w.z : w.y; }
/** Ground-plane coordinates: x and z on a Y-up scene, x and y on a Z-up one. */
glm::dvec2 groundOf(const glm::dvec3& w) { return zUp() ? glm::dvec2(w.x, w.y) : glm::dvec2(w.x, w.z); }
glm::dvec3 fromGround(glm::dvec2 g, double up = 0.0) { return zUp() ? glm::dvec3(g.x, g.y, up) : glm::dvec3(g.x, up, g.y); }
const char* secondAxis() { return zUp() ? "y" : "z"; }

/** A terrain mesh's placement: world from the heightfield's own space (X, Z across, Y up) and back. */
struct Frame
{
    glm::dmat4 m{1.0};
    glm::dmat4 inv{1.0};

    Frame() = default;
    explicit Frame(const glm::dmat4& world) : m(world), inv(glm::inverse(world)) {}

    /** The heightfield XZ under a world point, along the terrain's own up. */
    glm::dvec2 local(const glm::dvec3& w) const
    {
        const glm::dvec4 q = inv * glm::dvec4(w, 1.0);
        return {q.x, q.z};
    }
    glm::dvec3 world(double x, double y, double z) const { return glm::dvec3(m * glm::dvec4(x, y, z, 1.0)); }
    /** The local height whose world point over local xz sits at this height along the scene's up axis. */
    double localHeight(double up, glm::dvec2 xz) const
    {
        const double k = glm::dot(glm::dvec3(m[1]), upVector());
        const double base = upOf(world(xz.x, 0.0, xz.y));
        return std::abs(k) > 1e-12 ? (up - base) / k : up;
    }
    /** World length of one unit across the heightfield, and along its up. */
    double across() const { return 0.5 * (glm::length(glm::dvec3(m[0])) + glm::length(glm::dvec3(m[2]))); }
    double vertical() const { return glm::length(glm::dvec3(m[1])); }
    glm::dvec3 worldNormal(const glm::dvec3& n) const { return glm::normalize(glm::transpose(glm::inverse(glm::dmat3(m))) * n); }
};

/** A terrain as the commands see it: its mesh, the modifier making it, its heightfield in its own space and the placement. */
struct Found
{
    Node* mesh = nullptr;
    Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> local;
    Frame frame;
};

Found findTerrain(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    Found f;
    for (bool evaluated = !edit::EditBatch::active(); ; evaluated = true)
    {
        if (evaluated)
            g.evaluate();
        f = {};
        if (gn::isGnModifier(*n))
        {
            f.modifier = n;
            f.local = gn::modifierHeightfield(n->id());
            const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
            f.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
        }
        else
        {
            f.mesh = n;
            const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
            for (auto it = stack.rbegin(); it != stack.rend() && !f.local; ++it)
                if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                    if (auto h = gn::modifierHeightfield(m->id()))
                    {
                        f.modifier = m;
                        f.local = std::move(h);
                    }
        }
        // In a batch the heightfield from the last evaluation does; a terrain never evaluated yet is evaluated now.
        if (evaluated || (f.local && !f.local->empty()))
            break;
    }
    if (!f.local || f.local->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    f.frame = Frame(f.mesh ? f.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0));
    return f;
}

std::string uniqueGroupName(const std::string& base)
{
    const json existing = ops::execute("gnGroups", json::object());
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
        out = "Terrain" + out;
    return out;
}

json vec2(glm::dvec2 v) { return json::array({v.x, v.y}); }

void remember(const struct Found& f, const std::string& rev);

/** What a terrain is, for agents: grid, area, height range and masks, in world units on the scene's ground plane. */
json describe(const Found& f)
{
    const Heightfield& h = *f.local;
    const glm::dvec2 r = h.range(), e = h.extent();
    glm::dvec3 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
    for (double x : {h.origin.x, h.origin.x + e.x})
        for (double z : {h.origin.y, h.origin.y + e.y})
            for (double y : {r.x, r.y})
            {
                const glm::dvec3 w = f.frame.world(x, y, z);
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
    const glm::dvec3 center = f.frame.world(h.origin.x + e.x * 0.5, (r.x + r.y) * 0.5, h.origin.y + e.y * 0.5);
    json masks = json::object();
    for (const auto& [name, values] : h.masks)
    {
        if (values.empty())
            continue;
        auto [least, most] = std::minmax_element(values.begin(), values.end());
        double sum = 0.0;
        for (float v : values)
            sum += v;
        masks[name] = {{"min", *least}, {"max", *most}, {"mean", sum / double(values.size())}};
    }
    const glm::dvec3 c0(f.frame.m[0]), c2(f.frame.m[2]);
    const std::string rev = revision(h);
    remember(f, rev);
    return {{"mesh", f.mesh ? f.mesh->path() : std::string()},
            {"modifier", f.modifier ? f.modifier->path() : std::string()},
            {"resolution", json::array({h.nx, h.nz})},
            {"cellSize", h.cell * f.frame.across()},
            {"size", json::array({e.x * glm::length(c0), e.y * glm::length(c2)})},
            {"min", vec2(groundOf(lo))},
            {"max", vec2(groundOf(hi))},
            {"center", vec2(groundOf(center))},
            {"heightRange", json::array({upOf(lo), upOf(hi)})},
            {"upAxis", zUp() ? "z" : "y"},
            {"samples", int64_t(h.size())},
            {"masks", masks},
            {"revision", rev}};
}

/** The surface under a world point: ground coordinates, height along the scene's up, world normal and slope. */
json sampleAt(const Found& f, const glm::dvec3& at, const std::vector<std::string>& masks, const Analysis* an = nullptr)
{
    const Heightfield& h = *f.local;
    const glm::dvec2 q = f.frame.local(at);
    const double y = h.heightAt(q.x, q.y);
    const glm::dvec3 p = f.frame.world(q.x, y, q.y);
    const glm::dvec3 n = f.frame.worldNormal(h.normalAt(q.x, q.y));
    const glm::dvec2 g = groundOf(p);
    json s = {{"x", g.x},
              {secondAxis(), g.y},
              {"height", upOf(p)},
              {"position", json::array({p.x, p.y, p.z})},
              {"normal", json::array({n.x, n.y, n.z})},
              {"slope", std::acos(std::clamp(glm::dot(n, upVector()), -1.0, 1.0)) * 57.29577951308232},
              {"inside", h.contains(q.x, q.y)}};
    for (const std::string& m : masks)
        s[m] = h.findMask(m) || !an ? h.maskAt(m, q.x, q.y) : an->sample(m, q.x, q.y);
    return s;
}

/** A point given as [x, z] on the ground (x, y on a Z-up scene) or [x, y, z] in world space. */
glm::dvec3 worldPoint(const json& p, const std::string& cmd)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(cmd + std::string(": points are [x, ") + secondAxis() + "] or [x, y, z]");
    return p.size() == 2 ? fromGround({p[0].get<double>(), p[1].get<double>()}) : glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
}

std::vector<glm::dvec3> worldPoints(const json& list, const std::string& cmd)
{
    std::vector<glm::dvec3> out;
    if (!list.is_array())
        throw CommandError(cmd + ": expected a list of points");
    for (const json& p : list)
        out.push_back(worldPoint(p, cmd));
    return out;
}

json worldJson(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

/** The flags every analysis read takes: what counts as a river, the sea level, and how big floods get. */
std::vector<FlagDef> withAnalysisFlags(std::vector<FlagDef> flags)
{
    flags.push_back({"riverArea", "ra", T::Float, 250000.0, "Drainage area, m2, past which water is a river.", {}, {}, 1.0});
    flags.push_back({"seaLevel", "sea", T::Float, nullptr, "Sea level along the up axis, cm; ground below it joined to the edge is sea. Default no sea."});
    flags.push_back({"floodReturn", "fr", T::String, "rare", "Flood size for floodRisk: frequent, rare or extreme.", {"frequent", "rare", "extreme"}});
    return flags;
}

AnalysisParams analysisParams(const Args& a)
{
    AnalysisParams p;
    p.riverArea = std::max(1.0, a.get<double>("riverArea", 250000.0));
    if (a.has("seaLevel"))
        p.seaLevel = a.get<double>("seaLevel", 0.0);
    const std::string fr = a.get<std::string>("floodReturn", "rare");
    p.floodScale = fr == "frequent" ? 1.0 : fr == "extreme" ? 4.0 : 2.0;
    return p;
}

WorldMap worldMapOf(const Found& f)
{
    WorldMap w;
    w.m = f.frame.m;
    w.up = upVector();
    w.across = f.frame.across();
    w.vertical = f.frame.vertical();
    return w;
}

fs::path bakeDir()
{
    const fs::path& scene = io::currentPath();
    if (!scene.empty())
        return scene.parent_path() / (scene.stem().string() + "_terrain");
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain";
    return fs::temp_directory_path() / "lila_terrain";
}

/** Exposed inputs of a new terrain's group, in attribute editor order: name, type, panel, min, soft max, description. */
struct Exposed
{
    const char* name;
    const char* type;
    const char* panel;
    double min;
    double softMax;
    const char* node;
    const char* socket;
    const char* description;
};

constexpr Exposed kInputs[] = {
    {"sizeX", "float", "Grid", 100, 1000000, "Grid", "sizeX", "Width along X, cm."},
    {"sizeZ", "float", "Grid", 100, 1000000, "Grid", "sizeZ", "Depth along Z, cm."},
    {"resolution", "int", "Grid", 2, 4097, "Grid", "resolution", "Samples along the longer side."},
    {"amplitude", "float", "Shape", 0, 100000, "Noise", "amplitude", "Height of the tallest features, cm."},
    {"featureSize", "float", "Shape", 100, 200000, "Noise", "featureSize", "Size of the largest features, cm."},
    {"octaves", "int", "Shape", 1, 12, "Noise", "octaves", "Layers of detail."},
    {"roughness", "float", "Shape", 0, 1, "Noise", "roughness", "How strong each smaller layer is."},
    {"ridged", "float", "Shape", 0, 1, "Noise", "ridged", "0 rolling hills, 1 sharp ridges."},
    {"warp", "float", "Shape", 0, 2, "Noise", "warp", "Bends shapes so they don't line up."},
    {"seed", "int", "Shape", 0, 0, "Noise", "seed", "Another seed gives other shapes."},
    {"erosion", "int", "Erosion", 0, 300, "Erode", "iterations", "Erosion steps: rivers carve valleys; 0 is off, more is older."},
    {"erosionStrength", "float", "Erosion", 0, 0.005, "Erode", "strength", "How fast rivers cut."},
    {"uplift", "float", "Erosion", 0, 100, "Erode", "uplift", "Rise per step, cm, building ranges as they wear."},
    {"talusAngle", "float", "Erosion", 5, 60, "Erode", "talusAngle", "Slopes steeper than this shed scree, degrees."},
    {"age", "float", "Erosion", 0, 1, "Preset", "age", "Erode preset age: 0 is off, 1 is very old."},
    {"erosionPreset", "string", "Erosion", 0, 0, "Preset", "preset", "Erode preset: natural, alpine, desert or hills."},
};

/** A new terrain mesh stood on the scene's ground: on a Z-up scene the heightfield's Y turns to world Z and its Z to world X. */
void placeOnGround(const std::string& host)
{
    if (zUp())
        ops::execute("setAttr", {{"args", {host + ".rotate", json::array({90.0, 0.0, 90.0})}}});
}

/** A world centre in the heightfield's own space, for a mesh placed by placeOnGround. */
json localCenter(const json& c)
{
    if (!zUp() || !c.is_array() || c.size() != 3)
        return c;
    return json::array({c[1], c[2], c[0]});
}

std::string createHost(const std::string& name)
{
    json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
    const std::string host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    placeOnGround(host);
    return host;
}

std::string pathOf(const Found& f, const std::string& fallback) { return f.mesh ? f.mesh->path() : fallback; }

// ---------------------------------------------------------------------------------------------------- group layers

struct TerrainGroup
{
    Node* node = nullptr;
    gn::Group group;
};

TerrainGroup terrainGroup(Graph& g, const Found& f, const std::string& cmd)
{
    Node* node = f.modifier ? gn::groupOfModifier(g, *f.modifier) : nullptr;
    if (!node)
        throw CommandError(cmd + ": " + (f.modifier ? f.modifier->path() : std::string("the terrain")) + " has no group");
    try
    {
        TerrainGroup t{node, {}};
        if (!edit::batchedGroup(*node, t.group))
            t.group = gn::readGroup(*node);
        return t;
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void commitTerrainGroup(TerrainGroup& t, const std::string& cmd)
{
    if (edit::batchGroup(*t.node, t.group))
        return;
    try
    {
        gn::commitGroup(Graph::current(), *t.node, std::move(t.group));
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

gn::GNode* nodeNamed(gn::Group& grp, const std::string& name)
{
    for (gn::GNode& n : grp.nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

gn::GLink* linkInto(gn::Group& grp, const std::string& node, const std::string& socket)
{
    for (gn::GLink& l : grp.links)
        if (l.toNode == node && l.toSocket == socket)
            return &l;
    return nullptr;
}

/**
 * The layer node named name, added when missing on the heightfield chain into the group output: before the run of
 * trailing nodes whose types are in skip, so a base sculpt lands before erosion and a final one before the stamps.
 */
gn::GNode& ensureLayer(gn::Group& grp, const std::string& type, const std::string& name, const std::set<std::string>& skip, const std::string& cmd)
{
    if (gn::GNode* n = nodeNamed(grp, name))
    {
        if (n->type != type)
            throw CommandError(cmd + ": the terrain's group has a " + n->type + " node named " + name + " already");
        return *n;
    }
    std::string to = "GroupOutput", socket = "geometry";
    for (gn::GLink* l = linkInto(grp, to, socket); l; l = linkInto(grp, to, socket))
    {
        const gn::GNode* from = nodeNamed(grp, l->fromNode);
        // The volume, then biomes and material layers, always stay last: they split and read the finished heights.
        static const std::set<std::string> trailing = {"TerrainVolume", "TerrainBiomes", "TerrainLayers"};
        if (!from || !(skip.count(from->type) || trailing.count(from->type)))
            break;
        to = from->name;
        socket = "heightfield";
    }
    gn::GLink* l = linkInto(grp, to, socket);
    if (!l)
        throw CommandError(cmd + ": the terrain's group has no heightfield running into " + to);
    const gn::GNode* toNode = nodeNamed(grp, to);
    const gn::GNode* fromNode = nodeNamed(grp, l->fromNode);
    const glm::dvec2 at = toNode && fromNode ? (toNode->position + fromNode->position) * 0.5 + glm::dvec2(0.0, 160.0) : glm::dvec2(0.0);
    const std::string fromName = l->fromNode, fromSocket = l->fromSocket;
    l->fromNode = name;
    l->fromSocket = "heightfield";
    grp.links.push_back({fromName, fromSocket, name, "heightfield"});
    gn::GNode n;
    n.name = name;
    n.type = type;
    n.position = at;
    grp.nodes.push_back(std::move(n));
    return grp.nodes.back();
}

json listOf(const gn::GNode& n, const char* prop)
{
    auto it = n.values.find(prop);
    if (it == n.values.end() || !it->is_string() || it->get<std::string>().empty())
        return json::array();
    json j = json::parse(it->get<std::string>(), nullptr, false);
    return j.is_array() ? j : json::array();
}

void setList(gn::GNode& n, const char* prop, const json& list) { n.values[prop] = list.dump(); }

/** Object Info nodes this command made for stamp sources, dropped once no stamp in any layer reads them. */
constexpr const char* kSourcePrefix = "StampSource";

void dropUnusedSources(gn::Group& grp)
{
    std::set<std::string> used;
    for (const gn::GNode& n : grp.nodes)
        if (n.type == "TerrainStamps")
            for (const json& s : listOf(n, "stamps"))
                if (s.contains("object"))
                    used.insert(s["object"].get<std::string>());
    std::erase_if(grp.nodes, [&](const gn::GNode& n) { return n.type == "ObjectInfo" && n.name.starts_with(kSourcePrefix) && !used.count(n.name); });
}

/** The Object Info node following object, made when the group has none. */
std::string sourceNode(gn::Group& grp, const std::string& object)
{
    for (const gn::GNode& n : grp.nodes)
        if (n.type == "ObjectInfo" && n.name.starts_with(kSourcePrefix) && n.values.value("object", std::string()) == object)
            return n.name;
    gn::GNode n;
    n.name = grp.uniqueNodeName(kSourcePrefix);
    n.type = "ObjectInfo";
    n.position = {-600.0, 400.0 + 120.0 * double(grp.nodes.size())};
    n.values["object"] = object;
    grp.nodes.push_back(n);
    return n.name;
}

/** pad, minScaleZ and layers= (a list of masks, or {mask: layer name}) as export options. */
UnrealOptions unrealOptions(const Args& a)
{
    UnrealOptions o;
    o.pad = a.get<bool>("pad", false);
    o.minScaleZ = std::max(0.01, a.get<double>("minScaleZ", 1.0));
    if (a.has("layers"))
    {
        const json l = a.flags["layers"];
        if (l.is_object())
            for (const auto& [mask, layer] : l.items())
            {
                o.masks.push_back(mask);
                o.layerNames[mask] = layer.get<std::string>();
            }
        else if (l.is_array())
            for (const json& m : l)
                o.masks.push_back(m.get<std::string>());
        else
            throw CommandError("layers is a list of masks or {mask: layer name}");
        if (o.masks.empty())
            o.masks.push_back("");
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------- stamps

/** A stamp's result in world terms: heights along the scene's up, volumes in cm3 (and m3), points in world space. */
json stampJson(const Found& f, const Stamp& s, const StampResult& r, const std::string& layer)
{
    const Frame& fr = f.frame;
    const double a = fr.across(), v = fr.vertical();
    auto upAt = [&](glm::dvec2 xz, double y) { return upOf(fr.world(xz.x, y, xz.y)); };
    const double cut = r.cut * a * a * v, fill = r.fill * a * a * v;
    json walls = json::array();
    for (const auto& run : r.walls)
    {
        json pts = json::array(), heights = json::array();
        for (const StampResult::WallPoint& w : run)
        {
            pts.push_back(worldJson(fr.world(w.at.x, w.top, w.at.y)));
            heights.push_back((w.top - w.bottom) * v);
        }
        walls.push_back({{"points", pts}, {"heights", heights}});
    }
    json piles = json::array();
    for (const StampResult::Pile& p : r.piles)
        piles.push_back({{"position", worldJson(fr.world(p.at.x, p.top, p.at.y))}, {"length", (p.top - p.ground) * v}});
    json levels = json::array();
    for (double l : r.levels)
        levels.push_back(upAt(r.centroid, l));
    json out = {{"id", s.id},
            {"layer", layer},
            {"kind", s.kind},
            {"mode", stampModeNames()[size_t(s.mode)]},
            {"applied", r.samples > 0},
            {"padHeight", upAt(r.centroid, r.padHeight)},
            {"center", worldJson(fr.world(r.centroid.x, r.padHeight, r.centroid.y))},
            {"groundMin", upAt(r.centroid, r.groundMin)},
            {"groundMax", upAt(r.centroid, r.groundMax)},
            {"groundMean", upAt(r.centroid, r.groundMean)},
            {"plinthDepth", r.plinthDepth * v},
            {"cut", cut},
            {"fill", fill},
            {"cutM3", cut * 1e-6},
            {"fillM3", fill * 1e-6},
            {"area", r.area * a * a},
            {"levels", levels},
            {"walls", walls},
            {"piles", piles}};
    if (s.isLine())
    {
        json line = json::array(), spans = json::array(), culverts = json::array();
        for (const glm::dvec3& p : r.centreline)
            line.push_back(worldJson(fr.world(p.x, p.y, p.z)));
        for (const StampResult::Span& sp : r.spans)
        {
            json pts = json::array();
            for (size_t k = sp.from; k <= sp.to && k < r.centreline.size(); ++k)
                pts.push_back(line[k]);
            spans.push_back({{"kind", sp.kind}, {"points", pts}, {"length", double(sp.to - sp.from) * r.length / double(std::max<size_t>(1, r.centreline.size() - 1)) * a}});
        }
        for (const glm::dvec2& c : r.culverts)
            culverts.push_back(worldJson(fr.world(c.x, r.padHeight, c.y)));
        out["centreline"] = line;
        out["maxGrade"] = r.maxGrade * v / a;
        out["length"] = r.length * a;
        out["spans"] = spans;
        out["culverts"] = culverts;
        if (s.kind == "canal" || s.kind == "moat")
            out["waterLevel"] = upAt(r.centroid, r.waterLevel);
    }
    return out;
}

std::vector<FlagDef> stampFlags(bool region)
{
    std::vector<FlagDef> flags = {
        {"footprint", "fp", T::List, nullptr, "Polygon [[x, z] or [x, y, z], ...] in world space (x, y on a Z-up scene)."},
        nodeFlag({"object", "obj", T::String, nullptr, "A mesh (its faces seen from above) or closed curve the stamp follows live."}, "mesh|curve"),
        {"id", "id", T::String, "", "Stable id; stamping an id again replaces that stamp. Empty makes one."},
        {"layer", "ly", T::String, "Stamps", "Stamp layer (a Terrain Stamps node after erosion); the city uses City."},
        {"mode", "m", T::String, "flatten", region ? "flatten, carve, raise, blend, or dredge (lowered by depth)." : "flatten, carve, raise, blend, plinth (no grading, a raised base), terraced (steps along the slope) or stilts (no grading, piles).",
         region ? std::vector<std::string>{"flatten", "carve", "raise", "blend", "dredge"} : stampModeNames()},
        {"level", "lv", T::String, "", "Target height from the ground under it: mean, min, max, median, or object (the source's lowest point). Default mean; max for plinth and stilts.",
         {"", "mean", "min", "max", "median", "object"}},
        {"height", "h", T::Float, nullptr, "Target height along the scene's up axis, cm; overrides level."},
        {"offset", "of", T::Float, 0.0, "Added to the target height, cm."},
        {"falloff", "fo", T::Float, 500.0, "Skirt width back to the ground, cm.", {}, {}, 0.0},
        {"strength", "st", T::Float, 1.0, "Blend: how far toward the target, 0 to 1.", {}, {}, 0.0, 1.0},
        {"masks", "mk", T::List, json::array({"cityFootprint"}), "Masks its coverage is written to."},
        {"maskValue", "mv", T::Float, 1.0, "Mask value inside.", {}, {}, 0.0, 1.0},
        {"wallHeight", "wh", T::Float, 50.0, "Report retaining walls where pad and ground part by more than this, cm.", {}, {}, 0.0}};
    if (region)
    {
        flags.push_back({"grade", "gr", T::Float, 0.0, "Fall across it, percent, toward direction."});
        flags.push_back({"direction", "dir", T::List, nullptr, "Downhill [x, z] (x, y on Z-up); default the ground's own fall."});
        flags.push_back({"edge", "ed", T::String, "slope", "slope (blend over falloff), wall (sheer edge, reported as walls) or steps.", stampEdgeNames()});
        flags.push_back({"stepHeight", "sh", T::Float, 150.0, "Steps edge: riser height, cm.", {}, {}, 1.0});
        flags.push_back({"depth", "dp", T::Float, 200.0, "Dredge: how far it lowers the ground, cm.", {}, {}, 0.0});
    }
    else
    {
        flags.push_back({"steps", "sp", T::Int, int64_t(0), "Terraced: number of steps; 0 picks from stepHeight.", {}, {}, 0.0, 64.0});
        flags.push_back({"stepHeight", "sh", T::Float, 150.0, "Terraced: height per step when steps is 0, cm.", {}, {}, 1.0});
    }
    return flags;
}

json runStamp(const Args& a, bool region, const std::string& cmd, const std::string& lineKind = {})
{
    LILA_ZONE();

    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    Graph& g = Graph::current();
    const Found f = findTerrain(g, names[0], cmd);
    const Frame& fr = f.frame;
    const std::string layer = identifier(a.get<std::string>("layer", "Stamps"));

    Stamp s;
    s.kind = region ? "region" : "pad";
    const bool dredge = region && a.get<std::string>("mode", "flatten") == "dredge";
    try
    {
        json spec = {{"mode", lineKind.empty() && !dredge ? a.get<std::string>("mode", "flatten") : std::string("carve")}};
        if (region)
            spec["edge"] = a.get<std::string>("edge", "slope");
        s = Stamp::fromJson(spec);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
    s.kind = !lineKind.empty() ? lineKind : dredge ? "dredge" : region ? "region" : "pad";
    if (!lineKind.empty() || dredge)
    {
        s.width = std::max(1.0, a.get<double>("width", 600.0)) / fr.across();
        s.depth = std::max(0.0, a.get<double>("depth", 200.0)) / fr.vertical();
        s.rise = a.get<double>("rise", 200.0) / fr.vertical();
        s.side = a.get<std::string>("side", "left");
        s.maxGrade = std::max(0.0, a.get<double>("maxGrade", 8.0)) * fr.across() / fr.vertical();
        s.clearance = std::max(0.0, a.get<double>("clearance", 500.0)) / fr.vertical();
    }
    std::string level = a.get<std::string>("level", "");
    if (level.empty())
        level = s.mode == StampMode::Plinth || s.mode == StampMode::Stilts ? "max" : "mean";
    s.level = StampLevel(std::find(stampLevelNames().begin(), stampLevelNames().end(), level) - stampLevelNames().begin());
    s.offset = a.get<double>("offset", 0.0) / fr.vertical();
    s.falloff = std::max(0.0, a.get<double>("falloff", 500.0)) / fr.across();
    s.strength = std::clamp(a.get<double>("strength", 1.0), 0.0, 1.0);
    s.stepHeight = std::max(1.0, a.get<double>("stepHeight", 150.0)) / fr.vertical();
    s.steps = int(std::clamp<int64_t>(a.get<int64_t>("steps", 0), 0, 64));
    s.wallHeight = std::max(0.0, a.get<double>("wallHeight", 50.0)) / fr.vertical();
    s.maskValue = std::clamp(a.get<double>("maskValue", 1.0), 0.0, 1.0);
    s.masks.clear();
    const json masks = a.has("masks") ? a.flags["masks"] : json(defaultMasks(s.kind));
    if (masks.is_string())
        s.masks.push_back(masks.get<std::string>());
    else
        for (const json& m : masks)
            s.masks.push_back(m.get<std::string>());
    if (region)
    {
        s.grade = a.get<double>("grade", 0.0) * fr.across() / fr.vertical();
        if (a.has("direction"))
        {
            const json d = a.flags["direction"];
            if (!d.is_array() || d.size() != 2)
                throw CommandError(cmd + std::string(": direction is [x, ") + secondAxis() + "]");
            const glm::dvec3 w = fromGround({d[0].get<double>(), d[1].get<double>()});
            const glm::dvec4 l = fr.inv * glm::dvec4(w, 0.0);
            s.direction = {l.x, l.z};
        }
    }

    TerrainGroup t = terrainGroup(g, f, cmd);
    edit::ListEdit stamps(*t.node, ensureLayer(t.group, "TerrainStamps", layer, {}, cmd), "stamps");
    json& list = stamps.list();
    std::string id = a.get<std::string>("id", "");
    if (id.empty())
        for (int k = 1; id.empty(); ++k)
        {
            const std::string candidate = s.kind + std::to_string(k);
            if (std::none_of(list.begin(), list.end(), [&](const json& e) { return e.value("id", std::string()) == candidate; }))
                id = candidate;
        }
    s.id = id;

    glm::dvec2 centroid(0.0);
    if (!lineKind.empty() && a.has("path"))
    {
        for (const glm::dvec3& p : worldPoints(a.flags["path"], cmd))
            s.path.push_back(fr.local(p));
        if (s.path.size() < 2)
            throw CommandError(cmd + ": path needs at least two points");
        for (const glm::dvec2& p : s.path)
            centroid += p / double(s.path.size());
    }
    else if (!lineKind.empty() && !a.has("object"))
        throw CommandError(cmd + ": give path= or object= (a curve)");
    else if (a.has("footprint"))
    {
        for (const glm::dvec3& p : worldPoints(a.flags["footprint"], cmd))
            s.polygon.push_back(fr.local(p));
        if (s.polygon.size() < 3)
            throw CommandError(cmd + ": footprint needs at least three points");
        for (const glm::dvec2& p : s.polygon)
            centroid += p / double(s.polygon.size());
    }
    else if (a.has("object"))
    {
        Node* src = g.find(a.get<std::string>("object", ""));
        if (!src || !(src->type().isA("mesh") || src->type().isA("curve")))
            throw CommandError(cmd + ": object must name a mesh or a curve");
        if (f.mesh && src->id() == f.mesh->id())
            throw CommandError(cmd + ": a terrain can't stamp itself");
        const glm::dvec3 at(src->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        centroid = fr.local(at);
        // Moving the object afterwards moves the stamp; the node keeps the path it had now.
        s.object = sourceNode(t.group, src->path());
    }
    else
        throw CommandError(cmd + ": give footprint= or object=");
    if (a.has("height"))
    {
        s.level = StampLevel::Value;
        s.height = fr.localHeight(a.get<double>("height", 0.0), centroid);
    }

    bool replaced = false;
    for (json& e : list)
        if (e.value("id", std::string()) == id)
        {
            e = s.toJson();
            replaced = true;
        }
    if (!replaced)
        list.push_back(s.toJson());
    stamps.store(*nodeNamed(t.group, layer));
    // A batch leaves unused sources for the next edit to drop: its lists are only written back when it commits.
    if (!edit::EditBatch::active())
        dropUnusedSources(t.group);
    commitTerrainGroup(t, cmd);
    if (edit::EditBatch::active())
        return {{"id", id}, {"layer", layer}, {"replaced", replaced}, {"batched", true}};

    const Found after = findTerrain(g, pathOf(f, names[0]), cmd);
    const auto results = stampResults(after.modifier->id(), layer);
    auto it = results.find(id);
    json out = stampJson(after, s, it != results.end() ? it->second : StampResult{}, layer);
    out["replaced"] = replaced;
    out["revision"] = revision(*after.local);
    return out;
}

/** A curve object's shape as world-space polylines. */
std::vector<std::vector<glm::dvec3>> curvePolylines(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError(cmd + ": no curve named '" + name + "'");
    const int attr = n->type().attrIndex("worldSpace");
    gn::CurveSet set;
    for (const curve::Spline& s : curve::splinesOf(g.value({n->id(), attr, -1})))
        set.addSpline(s);
    const gn::CurveSet e = set.isPoly() ? set : set.evaluated();
    std::vector<std::vector<glm::dvec3>> out;
    for (size_t c = 0; c < e.numCurves(); ++c)
    {
        const auto pts = e.curvePoints(c);
        out.emplace_back(pts.begin(), pts.end());
        if (e.cyclic[c] && !out.back().empty())
            out.back().push_back(out.back().front());
    }
    return out;
}

json gridJson(const Found& f, const std::vector<float>& values, int resolution)
{
    const Heightfield& h = *f.local;
    const int longest = std::max(h.nx, h.nz);
    const int res = std::clamp(resolution, 2, longest);
    const double cell = h.cell * double(longest - 1) / double(res - 1);
    const int nx = std::max(2, int(h.extent().x / cell) + 1), nz = std::max(2, int(h.extent().y / cell) + 1);
    Heightfield src = h;
    src.masks.clear();
    src.heights = values;
    const Heightfield g = geo::resampled(src, nx, nz, cell, h.origin);
    const glm::dvec3 o = f.frame.world(g.origin.x, 0.0, g.origin.y);
    const glm::dvec3 ax = f.frame.world(g.origin.x + g.cell, 0.0, g.origin.y) - o;
    const glm::dvec3 az = f.frame.world(g.origin.x, 0.0, g.origin.y + g.cell) - o;
    return {{"resolution", json::array({nx, nz})},
            {"origin", vec2(groundOf(o))},
            {"stepX", vec2(groundOf(ax))},
            {"stepZ", vec2(groundOf(az))},
            {"values", g.heights}};
}

void registerStampCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainStampPad",
        .doc = "Stamp a building pad into a terrain: flatten (cut and fill to one level), carve (cut only), raise (fill only), "
               "blend, terraced (steps down the slope), plinth (no grading: the building gets a raised base) or stilts (no "
               "grading: piles).\n\n"
               "The footprint is a polygon or follows a mesh (its faces seen from above) or closed curve, live: moving that object "
               "moves the pad. A skirt of falloff cm blends back to the ground. Stamps sit in a layer after erosion, so erosion "
               "never re-cuts them; stamping the same id again replaces it, and the coverage goes into masks (cityFootprint by "
               "default). Returns padHeight, ground min/max/mean, cut and fill (cm3 and m3), area, plinthDepth, terraced levels, "
               "retaining walls where pad and ground part, piles for stilts and plinths, and the terrain revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = stampFlags(false),
        .run = [](const Args& a) -> json { return runStamp(a, false, "terrainStampPad"); },
        .examples = {"terrainStampPad('Terrain', id='temple', footprint=[[-2000, -1500], [2000, -1500], [2000, 1500], [-2000, 1500]], falloff=1500)",
                     "terrainStampPad('Terrain', id='house12', object='House12_base', mode='plinth')['plinthDepth']"},
    });

    ctx.registerCommand({
        .name = "terrainStampRegion",
        .doc = "Stamp a plaza, park or yard into a terrain: graded to a level (or falling at grade percent toward direction), "
               "with a slope skirt, a sheer wall edge (reported as walls) or steps. Footprint, layers, ids, masks and the result "
               "are as terrainStampPad's; masks such as paved or gardens tell materials and scatter what it is.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = stampFlags(true),
        .run = [](const Args& a) -> json { return runStamp(a, true, "terrainStampRegion"); },
        .examples = {"terrainStampRegion('Terrain', id='forum', footprint=[[0, 0], [6000, 0], [6000, 4000], [0, 4000]], grade=2, masks=['cityFootprint', 'paved'])",
                     "terrainStampRegion('Terrain', id='terrace', object='TerraceOutline', edge='wall')['walls']"},
    });

    ctx.registerCommand({
        .name = "terrainStamps",
        .doc = "The stamps on a terrain, by layer: each stamp's settings and what it did on the last evaluation (as "
               "terrainStampPad returns), and the terrain revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainStamps: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainStamps");
            TerrainGroup t = terrainGroup(g, f, "terrainStamps");
            json layers = json::object();
            for (const gn::GNode& n : t.group.nodes)
            {
                if (n.type != "TerrainStamps")
                    continue;
                const auto results = stampResults(f.modifier->id(), n.name);
                json list = json::array();
                for (const json& j : listOf(n, "stamps"))
                {
                    const Stamp s = Stamp::fromJson(j);
                    auto it = results.find(s.id);
                    json e = stampJson(f, s, it != results.end() ? it->second : StampResult{}, n.name);
                    if (!s.object.empty())
                        if (const gn::GNode* src = nodeNamed(t.group, s.object))
                            e["object"] = src->values.value("object", std::string());
                    list.push_back(e);
                }
                layers[n.name] = list;
            }
            return {{"layers", layers}, {"revision", revision(*f.local)}};
        },
        .examples = {"terrainStamps('Terrain')['layers']['City']"},
    });

    ctx.registerCommand({
        .name = "terrainStampRemove",
        .doc = "Remove stamps from a terrain by id, or a whole layer with all=True. Returns the ids removed.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"ids", "i", T::List, json::array(), "Stamp ids."},
                  {"layer", "ly", T::String, "Stamps", "Stamp layer."},
                  {"all", "a", T::Bool, false, "Remove the layer with every stamp in it."}},
        .run = [](const Args& a) -> json {
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainStampRemove: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainStampRemove");
            TerrainGroup t = terrainGroup(g, f, "terrainStampRemove");
            const std::string layer = identifier(a.get<std::string>("layer", "Stamps"));
            gn::GNode* node = nodeNamed(t.group, layer);
            if (!node || node->type != "TerrainStamps")
                throw CommandError("terrainStampRemove: no stamp layer " + layer);
            json removed = json::array();
            json list = listOf(*node, "stamps");
            std::set<std::string> ids;
            for (const json& i : a.flags.value("ids", json::array()))
                ids.insert(i.get<std::string>());
            json kept = json::array();
            for (const json& s : list)
            {
                const std::string id = s.value("id", std::string());
                if (a.get<bool>("all", false) || ids.count(id))
                    removed.push_back(id);
                else
                    kept.push_back(s);
            }
            if (a.get<bool>("all", false))
            {
                // Splice the layer out of the chain.
                gn::GLink* in = linkInto(t.group, layer, "heightfield");
                const std::string fromNode = in ? in->fromNode : std::string(), fromSocket = in ? in->fromSocket : std::string();
                for (gn::GLink& l : t.group.links)
                    if (l.fromNode == layer)
                    {
                        l.fromNode = fromNode;
                        l.fromSocket = fromSocket;
                    }
                std::erase_if(t.group.links, [&](const gn::GLink& l) { return l.toNode == layer || l.fromNode.empty(); });
                std::erase_if(t.group.nodes, [&](const gn::GNode& n) { return n.name == layer; });
            }
            else
                setList(*node, "stamps", kept);
            dropUnusedSources(t.group);
            commitTerrainGroup(t, "terrainStampRemove");
            return removed;
        },
        .examples = {"terrainStampRemove('Terrain', ids=['house12'], layer='City')"},
    });

    ctx.registerCommand({
        .name = "terrainSculpt",
        .doc = "Sculpt a terrain by command: raise, lower, smooth, flatten, noise, ramp, terrace, stamp, erode, uplift, relax or "
               "mask, in dabs at points=, along a path= polyline or along a curve object, each dab radius cm wide with a smooth "
               "falloff (or falloff=, alpha=), weighted per point by pressure= and mirrored by mirror=.\n\n"
               "Raise, lower, noise and stamp move the ground by strength cm at the centre; the rest go strength (0 to 1) of the "
               "way. Flatten levels to height (along the up axis), the plane under the stroke (plane=True) or the mean under it. "
               "Ramp runs from the first point to the last. Terrace steps every step cm. Stamp lays a library shape or image. "
               "Erode runs erosion under the brush; uplift paints the uplift mask Erode Terrain lifts by (base stage); relax "
               "evens out bumps; mask paints any mask toward value. stage='base' sculpts before "
               "erosion, which then shapes it; stage='final' after erosion, before the stamps. Strokes are kept in a Terrain "
               "Sculpt node and replay on every evaluation, so they save with the scene and undo like any edit. clear=True drops "
               "the stage's strokes. Returns the stroke count, height range and revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"op", "o", T::String, "raise", "The brush.", sculptOpNames()},
                  {"points", "p", T::List, nullptr, "Dab centres [[x, z] or [x, y, z], ...]."},
                  {"path", "pa", T::List, nullptr, "A polyline the stroke runs along."},
                  nodeFlag({"curve", "cv", T::String, nullptr, "A curve object the stroke runs along."}, "curve"),
                  {"radius", "r", T::Float, 2000.0, "Dab radius, cm.", {}, {}, 1.0},
                  {"strength", "s", T::Float, 100.0, "cm for raise, lower and noise; 0 to 1 for smooth and flatten."},
                  {"spacing", "sp", T::Float, 0.0, "Between dabs along a path, cm; 0 is a quarter of the radius.", {}, {}, 0.0},
                  {"height", "h", T::Float, nullptr, "Flatten: target height along the up axis, cm; default the mean under the stroke."},
                  {"noiseSize", "ns", T::Float, 1000.0, "Noise: feature size, cm.", {}, {}, 1.0},
                  {"seed", "sd", T::Int, int64_t(0), "Noise: seed."},
                  {"pressure", "pr", T::List, nullptr, "Weight per point, 0 to 1 (pen pressure)."},
                  {"falloff", "fo", T::String, nullptr, "Brush falloff shape: smooth, sphere, root, sharp, linear, constant, bell, or a soft select preset."},
                  {"mirror", "mi", T::String, nullptr, "Mirror across the terrain's centre along its own x, z or both.", {"off", "x", "z", "xz"}},
                  {"plane", "pl", T::Bool, nullptr, "Flatten: level to the plane fitted under the stroke."},
                  {"endHeight", "eh", T::Float, nullptr, "Ramp: height at the last point along the up axis, cm; default the ground there (height= sets the first)."},
                  {"step", "stp", T::Float, nullptr, "Terrace: step height, cm (default 1000).", {}, {}, 1.0},
                  {"sharpness", "sh", T::Float, nullptr, "Terrace: 0 soft to 1 sheer risers (default 0.8).", {}, {}, 0.0, 1.0},
                  {"stamp", "stm", T::String, nullptr, "Stamp: a library stamp (peak, crater, dune, rocks, mesa, ridge, volcano) or an image file."},
                  {"angle", "an", T::Float, nullptr, "Stamp and alpha: turn, degrees."},
                  {"blend", "bl", T::String, nullptr, "Stamp: add (default), raise (never below the stamp) or lower.", {"add", "raise", "lower"}},
                  {"alpha", "al", T::String, nullptr, "A stamp or image (alpha:// or a file) shaping the brush tip."},
                  {"mask", "mk", T::String, nullptr, "Mask and uplift brushes: the mask to paint (default paint, uplift)."},
                  {"value", "va", T::Float, nullptr, "Mask and uplift brushes: value painted toward, 0 to 1 (default 1).", {}, {}, 0.0, 1.0},
                  {"iterations", "it", T::Int, nullptr, "Erode and relax: steps (default 10 and 4).", {}, {}, 1.0, 200.0},
                  {"patch", "pt", T::Any, nullptr, "Patch: the block of height changes terrainSculptRegion bakes."},
                  {"stage", "sg", T::String, "base", "base (before erosion) or final (after it).", {"base", "final"}},
                  {"clear", "cl", T::Bool, false, "Remove every stroke of this stage instead of adding one."},
                  {"replaceLast", "rl", T::Bool, false, "Replace the stage's last stroke instead of adding one (a stroke growing as it is drawn)."},
                  {"removeLast", "rm", T::Bool, false, "Remove the stage's last stroke instead of adding one."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSculpt: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainSculpt");
            const Frame& fr = f.frame;
            const bool base = a.get<std::string>("stage", "base") == "base";
            const std::string nodeName = base ? "Sculpt" : "SculptFinal";
            TerrainGroup t = terrainGroup(g, f, "terrainSculpt");
            const std::set<std::string> skip = base ? withLandformTypes({"ErodeTerrain", "TerrainStamps", "TerrainSculpt"}) : std::set<std::string>{"TerrainStamps"};
            if (base && !nodeNamed(t.group, nodeName))
            {
                // Before erosion means before the Erode node; with none (a baked terrain), before the trailing layers.
                bool hasErode = std::any_of(t.group.nodes.begin(), t.group.nodes.end(), [](const gn::GNode& n) { return n.type == "ErodeTerrain"; });
                if (!hasErode)
                    ensureLayer(t.group, "TerrainSculpt", nodeName, {"TerrainStamps", "TerrainSculpt"}, "terrainSculpt");
            }
            json list = listOf(ensureLayer(t.group, "TerrainSculpt", nodeName, skip, "terrainSculpt"), "strokes");
            if (a.get<bool>("clear", false))
                list = json::array();
            else if (a.get<bool>("removeLast", false))
            {
                if (!list.empty())
                    list.erase(list.end() - 1);
            }
            else
            {
                if (a.get<bool>("replaceLast", false) && !list.empty())
                    list.erase(list.end() - 1);
                SculptStroke s;
                const std::string op = a.get<std::string>("op", "raise");
                s.op = SculptOp(std::find(sculptOpNames().begin(), sculptOpNames().end(), op) - sculptOpNames().begin());
                std::vector<glm::dvec3> pts;
                if (a.has("points"))
                    pts = worldPoints(a.flags["points"], "terrainSculpt");
                else if (a.has("path"))
                {
                    pts = worldPoints(a.flags["path"], "terrainSculpt");
                    s.path = true;
                }
                else if (a.has("curve"))
                {
                    for (const auto& line : curvePolylines(g, a.get<std::string>("curve", ""), "terrainSculpt"))
                        pts.insert(pts.end(), line.begin(), line.end());
                    s.path = true;
                }
                else
                    throw CommandError("terrainSculpt: give points=, path= or curve=");
                if (pts.empty())
                    throw CommandError("terrainSculpt: the stroke has no points");
                glm::dvec2 centroid(0.0);
                for (const glm::dvec3& p : pts)
                {
                    s.points.push_back(fr.local(p));
                    centroid += s.points.back() / double(pts.size());
                }
                s.radius = std::max(1.0, a.get<double>("radius", 2000.0)) / fr.across();
                const double strength = a.get<double>("strength", 100.0);
                const bool inCm = s.op == SculptOp::Raise || s.op == SculptOp::Lower || s.op == SculptOp::Noise || s.op == SculptOp::Stamp;
                s.strength = inCm ? strength / fr.vertical() : std::clamp(strength, 0.0, 1.0);
                s.spacing = std::max(0.0, a.get<double>("spacing", 0.0)) / fr.across();
                if (a.has("height"))
                    s.height = fr.localHeight(a.get<double>("height", 0.0), centroid);
                s.noiseSize = std::max(1.0, a.get<double>("noiseSize", 1000.0)) / fr.across();
                s.seed = uint32_t(a.get<int64_t>("seed", 0));
                if (a.has("pressure"))
                    for (const json& v : a.flags["pressure"])
                        s.pressure.push_back(v.get<float>());
                s.falloff = a.get<std::string>("falloff", "");
                s.mirror = a.get<std::string>("mirror", "");
                if (s.mirror == "off")
                    s.mirror.clear();
                s.settings = brushSettings(a, [&](double up) { return fr.localHeight(up, centroid); }, fr.vertical());
                list.push_back(s.toJson());
            }
            setList(*nodeNamed(t.group, nodeName), "strokes", list);
            commitTerrainGroup(t, "terrainSculpt");
            json out = describe(findTerrain(g, pathOf(f, names[0]), "terrainSculpt"));
            out["stage"] = base ? "base" : "final";
            out["strokes"] = list.size();
            return out;
        },
        .examples = {"terrainSculpt('Terrain', op='raise', points=[[0, 0]], radius=30000, strength=8000)",
                     "terrainSculpt('Terrain', op='flatten', curve='RoadLine', radius=1200, strength=1, stage='final')",
                     "terrainSculpt('Terrain', op='stamp', stamp='crater', points=[[0, 0]], radius=8000, strength=3000, stage='final')"},
    });

    ctx.registerCommand({
        .name = "terrainCostField",
        .doc = "How expensive the ground is to build a route across: 1 + slope weight per 10 degrees + water weight where rivers "
               "(flow mask past waterFlow) or lakes run + weight x mask for any mask named in weights (cityFootprint, rock, ...). "
               "Returns min, max and mean, the revision and, with resolution, the grid itself (rows along stepX, advancing by "
               "stepZ from origin, on the ground plane); file= writes it as a float EXR.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"weights", "w", T::Any, json::object(), "{slope: 1, water: 20, waterFlow: 0.75, <mask>: weight}."},
                  {"resolution", "r", T::Int, int64_t(0), "Return the grid with this many samples along the longer side; 0 leaves it out.", {}, {}, 0.0, 4097.0},
                  {"file", "f", T::String, "", "Write the full grid to this .exr."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainCostField: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainCostField");
            CostWeights w;
            try
            {
                w = CostWeights::fromJson(a.flags.value("weights", json::object()));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainCostField: ") + e.what());
            }
            const std::vector<float> cost = costField(*f.local, w);
            auto [lo, hi] = std::minmax_element(cost.begin(), cost.end());
            double sum = 0.0;
            for (float c : cost)
                sum += c;
            json out = {{"min", *lo}, {"max", *hi}, {"mean", sum / double(cost.size())}, {"revision", revision(*f.local)}};
            if (const int64_t res = a.get<int64_t>("resolution", 0); res > 0)
                out["grid"] = gridJson(f, cost, int(res));
            if (const std::string file = a.get<std::string>("file", ""); !file.empty())
            {
                const Heightfield& h = *f.local;
                std::vector<float> rgba(cost.size() * 4);
                for (size_t k = 0; k < cost.size(); ++k)
                {
                    rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = cost[k];
                    rgba[k * 4 + 3] = 1.0f;
                }
                image::WriteOptions o;
                o.bits = 32;
                try
                {
                    image::write(fs::path(file), {h.nx, h.nz, std::move(rgba)}, o);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("terrainCostField: ") + e.what());
                }
                out["file"] = file;
            }
            return out;
        },
        .examples = {"terrainCostField('Terrain', weights={'slope': 2, 'water': 50, 'cityFootprint': 5}, resolution=64)['grid']"},
    });

    ctx.registerCommand({
        .name = "terrainRoute",
        .doc = "The cheapest way from start to end over the terrain's cost field (see terrainCostField), never steeper than "
               "maxGrade percent where any way allows: switchbacks appear by themselves on steep ground. Returns the points on "
               "the surface (world space), length, cost, the steepest grade along it, withinGrade (false when nothing kept "
               "within maxGrade and the gentlest way was taken) and the revision; curve=True also makes a poly curve along it, and "
               "road='road' (or path, track, rail, runway) lays a road along it in one call, returned as road.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"start", "s", T::List, nullptr, "[x, z] or [x, y, z]."},
                  {"end", "e", T::List, nullptr, "[x, z] or [x, y, z]."},
                  {"weights", "w", T::Any, json::object(), "As terrainCostField's."},
                  {"maxGrade", "mg", T::Float, 10.0, "Steepest grade allowed, percent; 0 for no limit.", {}, {}, 0.0},
                  {"resolution", "r", T::Int, int64_t(1024), "Search grid samples along the longer side.", {}, {}, 16.0, 4097.0},
                  {"curve", "cv", T::Bool, false, "Also make a curve along the route."},
                  {"name", "n", T::String, "Route", "Name of that curve."},
                  {"road", "rd", T::String, "", "Also lay a road of this kind along it (road, path, track, rail, runway), as terrainAddRoad does.",
                   {"", "road", "path", "track", "rail", "runway"}},
                  {"roadId", "rid", T::String, "", "That road's id."},
                  {"roadSettings", "rs", T::Any, nullptr, "More terrainAddRoad flags for that road: {profile: 'lane', width: 600, build: True}."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainRoute: give one terrain");
            if (!a.has("start") || !a.has("end"))
                throw CommandError("terrainRoute: give start= and end=");
            const Found f = findTerrain(Graph::current(), names[0], "terrainRoute");
            const Frame& fr = f.frame;
            CostWeights w;
            try
            {
                w = CostWeights::fromJson(a.flags.value("weights", json::object()));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainRoute: ") + e.what());
            }
            const double grade = a.get<double>("maxGrade", 10.0) * fr.across() / fr.vertical();
            const Route r = route(*f.local, fr.local(worldPoint(a.flags["start"], "terrainRoute")), fr.local(worldPoint(a.flags["end"], "terrainRoute")), w,
                                  grade, int(a.get<int64_t>("resolution", 1024)));
            if (r.points.empty())
                throw CommandError("terrainRoute: no way from start to end");
            json pts = json::array();
            std::vector<glm::dvec3> world;
            for (const glm::dvec3& p : r.points)
            {
                world.push_back(fr.world(p.x, p.y, p.z));
                pts.push_back(worldJson(world.back()));
            }
            json out = {{"points", pts},
                        {"length", r.length * fr.across()},
                        {"cost", r.cost * fr.across()},
                        {"maxGrade", r.maxGrade * fr.vertical() / fr.across()},
                        {"withinGrade", r.withinGrade},
                        {"revision", revision(*f.local)}};
            if (a.get<bool>("curve", false))
                out["curve"] = ops::execute("curve", {{"points", pts}, {"type", "poly"}, {"name", a.get<std::string>("name", "Route")}});
            if (const std::string kind = a.get<std::string>("road", ""); !kind.empty())
            {
                json call = a.has("roadSettings") && a.flags["roadSettings"].is_object() ? a.flags["roadSettings"] : json::object();
                call["args"] = {names[0]};
                call["path"] = pts;
                call["kind"] = kind;
                call["id"] = a.get<std::string>("roadId", "");
                if (!call.contains("maxGrade") && a.get<double>("maxGrade", 10.0) > 0.0)
                    call["maxGrade"] = a.get<double>("maxGrade", 10.0);
                out["road"] = ops::execute("terrainAddRoad", call);
            }
            return out;
        },
        .examples = {"r = terrainRoute('Terrain', start=[-40000, 0], end=[40000, 5000], maxGrade=8, weights={'water': 100})\nr['length'], r['maxGrade']"},
    });
}

std::vector<FlagDef> lineFlags(bool waterfront)
{
    std::vector<FlagDef> flags = {
        {"path", "pa", T::List, nullptr, "The line [[x, z] or [x, y, z], ...] in world space (x, y on a Z-up scene)."},
        nodeFlag({"object", "obj", T::String, nullptr, "A curve the feature follows live."}, "curve"),
        {"id", "id", T::String, "", "Stable id; the same id again replaces it. Empty makes one."},
        {"layer", "ly", T::String, "Stamps", "Stamp layer; the city uses City."},
        {"width", "w", T::Float, 600.0, "Width across the line, cm.", {}, {}, 1.0},
        {"falloff", "fo", T::Float, 300.0, "Skirt back to the ground, cm.", {}, {}, 0.0},
        {"height", "h", T::Float, nullptr, "Level along the up axis, cm: quay top, canal water level, bridge deck."},
        {"offset", "of", T::Float, 0.0, "Added to the level, cm."},
        {"masks", "mk", T::List, nullptr, "Masks its coverage is written to; each kind has its own default."},
        {"maskValue", "mv", T::Float, 1.0, "Mask value on it.", {}, {}, 0.0, 1.0}};
    if (waterfront)
    {
        flags.push_back({"kind", "k", T::String, "quay", "quay (vertical bank, dredged on the water side), levee (raised bank), embankment (one-sided bank) or seawall.",
                         {"quay", "levee", "embankment", "seawall"}});
        flags.push_back({"side", "sd", T::String, "left", "Which side of travel the land is on.", {"left", "right"}});
        flags.push_back({"depth", "dp", T::Float, 200.0, "Quay: dredged depth on the water side, cm.", {}, {}, 0.0});
        flags.push_back({"rise", "rs", T::Float, 200.0, "Levee, embankment, seawall: height above the ground, cm."});
    }
    else
    {
        flags.push_back({"kind", "k", T::String, "street", "street (graded, bridges and tunnels left as spans, culverts found), canal or moat "
                                                           "(carved water), bridge (registered, ground cut to the clearance), culvert or tunnel (registered).",
                         {"street", "canal", "moat", "bridge", "culvert", "tunnel"}});
        flags.push_back({"depth", "dp", T::Float, 200.0, "Canal and moat: depth below the water, cm.", {}, {}, 0.0});
        flags.push_back({"maxGrade", "mg", T::Float, 8.0, "Street: steepest grade, percent.", {}, {}, 0.0});
        flags.push_back({"clearance", "cl", T::Float, 500.0, "Bridge: deck height over the ground below; street: how far above the ground it bridges.", {}, {}, 0.0});
    }
    return flags;
}

/** Per-field min, max, mean and percentiles over the samples set in cover. */
json fieldStats(const Analysis& an, const std::vector<uint8_t>& cover, const std::vector<std::string>& fields, double scale)
{
    json out = json::object();
    for (const std::string& name : fields)
    {
        const std::vector<float>& v = an.field(name);
        if (v.empty())
            continue;
        std::vector<float> vals;
        for (size_t i = 0; i < v.size(); ++i)
            if (cover[i])
                vals.push_back(v[i]);
        if (vals.empty())
            continue;
        std::sort(vals.begin(), vals.end());
        auto pct = [&](double p) { return double(vals[std::min(vals.size() - 1, size_t(p * double(vals.size() - 1) + 0.5))]); };
        const double mean = std::accumulate(vals.begin(), vals.end(), 0.0) / double(vals.size());
        const double s = name == "height" ? scale : 1.0;
        out[name] = {{"min", vals.front() * s}, {"max", vals.back() * s}, {"mean", mean * s}, {"p10", pct(0.1) * s}, {"p50", pct(0.5) * s}, {"p90", pct(0.9) * s}};
    }
    return out;
}

/** Heightfields seen by terrain reads, by terrain and revision, so terrainDirtyRegions can tell what changed. */
struct Seen
{
    std::string revision;
    std::shared_ptr<const Heightfield> h;
};

std::map<std::string, std::deque<Seen>>& seenRevisions()
{
    static std::map<std::string, std::deque<Seen>> s;
    return s;
}

void remember(const Found& f, const std::string& rev)
{
    auto& list = seenRevisions()[pathOf(f, "")];
    if (!list.empty() && list.front().revision == rev)
        return;
    list.push_front({rev, f.local});
    while (list.size() > 8)
        list.pop_back();
}

/** Stamps in every layer of a terrain, with their last results. */
struct StampEntry
{
    Stamp stamp;
    StampResult result;
    std::string layer;
};

std::vector<StampEntry> allStamps(Graph& g, const Found& f, const std::string& cmd)
{
    std::vector<StampEntry> out;
    TerrainGroup t = terrainGroup(g, f, cmd);
    for (const gn::GNode& n : t.group.nodes)
    {
        if (n.type != "TerrainStamps")
            continue;
        const auto results = stampResults(f.modifier->id(), n.name);
        for (const json& j : listOf(n, "stamps"))
        {
            StampEntry e;
            e.stamp = Stamp::fromJson(j);
            e.layer = n.name;
            if (auto it = results.find(e.stamp.id); it != results.end())
                e.result = it->second;
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::shared_ptr<const Analysis> analysisOf(const Found& f, const Args& a) { return analyse(f.local, analysisParams(a)); }

Found oneTerrain(const Args& a, const std::string& cmd)
{
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    Found f = findTerrain(Graph::current(), names[0], cmd);
    remember(f, revision(*f.local));
    return f;
}

void registerContractCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainBatch",
        .doc = "Run many stamp and road edits as one: calls is a list of {command, flags} for terrainStampPad, terrainStampRegion, "
               "terrainStampQuay, terrainAddFeature, terrainAddRoad and terrainAddRoundabout, each with its flags as that command "
               "takes them (args included). The terrain's group is written once at the end and evaluated once on the next read, "
               "so hundreds of edits cost about one. Each call returns its id and layer only; read the results afterwards with "
               "terrainStamps or terrainRoads. Returns ran, results (null for a call that failed) and errors ({index, command, "
               "error}); the calls that worked still apply.",
        .flags = {{"calls", "c", T::List, json::array(), "[{command, flags}, ...]."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            static const std::set<std::string> allowed = {"terrainStampPad", "terrainStampRegion", "terrainStampQuay",
                                                          "terrainAddFeature", "terrainAddRoad", "terrainAddRoundabout"};
            const json calls = a.flags.value("calls", json::array());
            if (!calls.is_array())
                throw CommandError("terrainBatch: calls is a list of {command, flags}");
            for (size_t k = 0; k < calls.size(); ++k)
                if (const json& c = calls[k]; !c.is_object() || !allowed.contains(c.value("command", std::string())))
                    throw CommandError("terrainBatch: call " + std::to_string(k) + " is not one of terrainStampPad, terrainStampRegion, "
                                       "terrainStampQuay, terrainAddFeature, terrainAddRoad or terrainAddRoundabout");
            json results = json::array(), errors = json::array();
            int ran = 0;
            {
                edit::EditBatch batch;
                for (size_t k = 0; k < calls.size(); ++k)
                {
                    const json& c = calls[k];
                    const std::string command = c.value("command", std::string());
                    try
                    {
                        results.push_back(ops::execute(command, c.value("flags", json::object())));
                        ++ran;
                    }
                    catch (const std::exception& e)
                    {
                        results.push_back(nullptr);
                        errors.push_back({{"index", k}, {"command", command}, {"error", e.what()}});
                    }
                }
                batch.commit("terrainBatch");
            }
            return {{"ran", ran}, {"results", results}, {"errors", errors}};
        },
        .examples = {"terrainBatch(calls=[{'command': 'terrainStampPad', 'flags': {'args': ['Terrain'], 'id': 'a', 'footprint': "
                     "[[0, 0], [1000, 0], [1000, 1000], [0, 1000]]}}])['errors']"},
    });

    ctx.registerCommand({
        .name = "terrainStampQuay",
        .doc = "Build a waterfront along a line: quay (a vertical bank, the land side levelled, the water side dredged by depth), "
               "levee (a raised bank rise cm above the ground), embankment (raised on the land side only) or seawall (land side "
               "raised to rise above the highest ground along it, sheer to the water). side names the land side of travel. Lives "
               "in a stamp layer like terrainStampPad, follows a curve with object=, and returns cut and fill, the centreline "
               "and the revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = lineFlags(true),
        .run = [](const Args& a) -> json { return runStamp(a, false, "terrainStampQuay", a.get<std::string>("kind", "quay")); },
        .examples = {"terrainStampQuay('Terrain', id='harbourQuay', path=[[0, 0], [8000, 0]], side='left', depth=400)"},
    });

    ctx.registerCommand({
        .name = "terrainAddFeature",
        .doc = "Add a line feature to a terrain. street: graded along its length to maxGrade, levelled across width with a skirt; "
               "where it would sit far above the ground it becomes a bridge span and far below a tunnel span, left ungraded, and "
               "small streams it crosses are returned as culverts. canal and moat: carved depth below their water level (height=, "
               "else the lowest ground along them), added to terrainWaterInfo. bridge: a deck from end to end (or at height=), "
               "the ground cut to clearance below it. culvert and tunnel: registered (masks culvert, tunnel) for the water graph "
               "and exporters. Returns the centreline, maxGrade, spans, culverts, cut and fill, and the revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = lineFlags(false),
        .run = [](const Args& a) -> json { return runStamp(a, false, "terrainAddFeature", a.get<std::string>("kind", "street")); },
        .examples = {"terrainAddFeature('Terrain', kind='street', id='mainStreet', path=[[-20000, 0], [0, 3000], [20000, 0]], width=800, maxGrade=6, layer='City')",
                     "terrainAddFeature('Terrain', kind='moat', object='CastleRing', width=1200, depth=300)"},
    });

    ctx.registerCommand({
        .name = "terrainField",
        .doc = "An analysis field over the whole terrain: its min, max and mean, and with resolution= the grid itself (as "
               "terrainCostField returns it); file= writes it as a float EXR. Fields: height, slope (deg), aspect (deg from north, "
               "-Z), curvature, roughness, flow, flowDir, basinId, streamOrder, wet, hand (height above the drainage, cm), "
               "floodRisk (0 to 1), distanceToWater (cm, negative in water), nearestWaterId (river id, lakes 100000 + id), "
               "waterDepth, rockHardness, soilDepth, excavationCost, slopeClass (0 flat <5%, 1 gentle, 2 terraced, 3 steep, 4 "
               "unbuildable >35%), buildability (0 to 1), prominence, viewshedScore, solar, windExposure, or any mask.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({{"field", "f", T::String, "buildability", "Field name."},
                                    {"resolution", "r", T::Int, int64_t(0), "Return the grid with this many samples along the longer side.", {}, {}, 0.0, 4097.0},
                                    {"file", "fi", T::String, "", "Write the full grid to this .exr."}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainField");
            const auto an = analysisOf(f, a);
            const std::string name = a.get<std::string>("field", "buildability");
            const std::vector<float>& v = an->field(name);
            if (v.empty())
                throw CommandError("terrainField: no field or mask named " + name);
            auto [lo, hi] = std::minmax_element(v.begin(), v.end());
            double sum = 0.0;
            for (float x : v)
                sum += x;
            json out = {{"field", name}, {"min", *lo}, {"max", *hi}, {"mean", sum / double(v.size())}, {"revision", revision(*f.local)}};
            if (const int64_t res = a.get<int64_t>("resolution", 0); res > 0)
                out["grid"] = gridJson(f, v, int(res));
            if (const std::string file = a.get<std::string>("file", ""); !file.empty())
            {
                std::vector<float> rgba(v.size() * 4);
                for (size_t k = 0; k < v.size(); ++k)
                {
                    rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = v[k];
                    rgba[k * 4 + 3] = 1.0f;
                }
                image::WriteOptions o;
                o.bits = 32;
                image::write(fs::path(file), {f.local->nx, f.local->nz, std::move(rgba)}, o);
                out["file"] = file;
            }
            return out;
        },
        .examples = {"terrainField('Terrain', field='floodRisk', resolution=64)['grid']['values']"},
    });

    ctx.registerCommand({
        .name = "terrainProfile",
        .doc = "A profile along a path every step cm: distance, height, the terrain's slope (deg), grade along the path and "
               "cross-slope across it (percent), any fields= read there, and the water it crosses (rivers with their id, width "
               "and water level; lakes and sea).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({{"path", "pa", T::List, nullptr, "Polyline [[x, z] or [x, y, z], ...]."},
                                    {"step", "st", T::Float, 0.0, "Spacing, cm; 0 is the cell size.", {}, {}, 0.0},
                                    {"fields", "fl", T::List, json::array(), "Fields to read along it."}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainProfile");
            if (!a.has("path"))
                throw CommandError("terrainProfile: give path=");
            const auto an = analysisOf(f, a);
            const Heightfield& h = *f.local;
            std::vector<glm::dvec2> pts;
            for (const glm::dvec3& p : worldPoints(a.flags["path"], "terrainProfile"))
                pts.push_back(groundOf(p));
            if (pts.size() < 2)
                throw CommandError("terrainProfile: path takes at least two points");
            std::vector<std::string> fields;
            for (const json& j : a.flags.value("fields", json::array()))
                fields.push_back(j.get<std::string>());
            const double step = a.get<double>("step", 0.0) > 0.0 ? a.get<double>("step", 0.0) : h.cell * f.frame.across();
            json samples = json::array(), crossings = json::array();
            double travelled = 0.0, prevH = 0.0;
            int prevWater = -2;
            for (size_t k = 1; k < pts.size(); ++k)
            {
                const glm::dvec2 a0 = pts[k - 1], b0 = pts[k];
                const double len = glm::length(b0 - a0);
                const glm::dvec2 dir = len > 1e-9 ? (b0 - a0) / len : glm::dvec2(1.0, 0.0);
                const int n = std::max(1, int(std::ceil(len / step)));
                for (int s = (k == 1 ? 0 : 1); s <= n; ++s)
                {
                    const glm::dvec2 g = a0 + (b0 - a0) * (double(s) / double(n));
                    const double d = travelled + len * double(s) / double(n);
                    json smp = sampleAt(f, fromGround(g), fields, an.get());
                    const double hgt = smp["height"].get<double>();
                    smp["distance"] = d;
                    smp["grade"] = samples.empty() ? 0.0 : (hgt - prevH) / std::max(1e-9, d - samples.back()["distance"].get<double>()) * 100.0;
                    const glm::dvec2 side(-dir.y, dir.x);
                    const double hl = sampleAt(f, fromGround(g + side * step), {})["height"].get<double>();
                    const double hr = sampleAt(f, fromGround(g - side * step), {})["height"].get<double>();
                    smp["crossSlope"] = (hl - hr) / (2.0 * step) * 100.0;
                    const glm::dvec2 q = f.frame.local(fromGround(g));
                    const int x = std::clamp(int(std::lround((q.x - h.origin.x) / h.cell)), 0, h.nx - 1);
                    const int z = std::clamp(int(std::lround((q.y - h.origin.y) / h.cell)), 0, h.nz - 1);
                    const size_t i = h.index(x, z);
                    const int water = an->reachOf()[i] >= 0 ? an->reachOf()[i] : an->lakeOf()[i] >= 0 ? 100000 + an->lakeOf()[i] : -1;
                    if (water >= 0 && water != prevWater)
                    {
                        json c = {{"distance", d}, {"position", smp["position"]}};
                        if (water < 100000)
                        {
                            const double q2 = an->discharge(uint32_t(i));
                            c["river"] = water;
                            c["width"] = 300.0 * std::sqrt(q2) * f.frame.across();
                            c["waterLevel"] = hgt + 30.0 * std::pow(q2, 0.4) * f.frame.vertical();
                        }
                        else
                        {
                            const Lake& lk = an->lakes()[size_t(water - 100000)];
                            c[lk.sea ? "sea" : "lake"] = lk.id;
                        }
                        crossings.push_back(c);
                    }
                    prevWater = water;
                    prevH = hgt;
                    samples.push_back(std::move(smp));
                }
                travelled += len;
            }
            return {{"samples", samples}, {"water", crossings}, {"length", travelled}, {"revision", revision(h)}};
        },
        .examples = {"p = terrainProfile('Terrain', path=[[-20000, 0], [20000, 0]], step=500)\nmax(abs(s['grade']) for s in p['samples']), p['water']"},
    });

    ctx.registerCommand({
        .name = "terrainRegionStats",
        .doc = "Statistics of fields over a footprint polygon: min, max, mean and the 10th, 50th and 90th percentiles of each "
               "field (height by default, also slope, floodRisk, buildability, any mask), the area in m2 and the revision. A "
               "building's foundation depth is height max - min plus a margin.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({{"polygon", "pg", T::List, nullptr, "Footprint [[x, z] or [x, y, z], ...]."},
                                    {"fields", "fl", T::List, json::array({"height", "slope"}), "Fields to summarise."}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainRegionStats");
            if (!a.has("polygon"))
                throw CommandError("terrainRegionStats: give polygon=");
            std::vector<glm::dvec2> ring;
            for (const glm::dvec3& p : worldPoints(a.flags["polygon"], "terrainRegionStats"))
                ring.push_back(f.frame.local(p));
            const std::vector<uint8_t> cover = footprintMask(*f.local, footprintFromPolygon(ring));
            std::vector<std::string> fields;
            for (const json& j : a.flags.value("fields", json::array({"height", "slope"})))
                fields.push_back(j.get<std::string>());
            const auto an = analysisOf(f, a);
            const size_t n = size_t(std::count(cover.begin(), cover.end(), uint8_t(1)));
            json stats = fieldStats(*an, cover, fields, f.frame.vertical());
            if (stats.contains("height"))
                for (const char* k : {"min", "max", "mean", "p10", "p50", "p90"})
                    stats["height"][k] = stats["height"][k].get<double>() + upOf(f.frame.world(0.0, 0.0, 0.0));
            return {{"fields", stats},
                    {"samples", n},
                    {"area", double(n) * f.local->cell * f.local->cell * f.frame.across() * f.frame.across() * 1e-4},
                    {"revision", revision(*f.local)}};
        },
        .examples = {"s = terrainRegionStats('Terrain', polygon=[[0, 0], [2000, 0], [2000, 1500], [0, 1500]])\ns['fields']['height']['max'] - s['fields']['height']['min']"},
    });

    ctx.registerCommand({
        .name = "terrainWaterInfo",
        .doc = "All the water as one graph. rivers: reaches downstream-ordered with points (position, width, depth, bedHeight, "
               "waterLevel, flowSpeed, discharge, bankHeightL/R, bankTypeL/R: low, steep, cliff or gravelBar, floodplainWidthL/R), "
               "streamOrder, navigable, downstream, tributaries and mouth. lakes and sea: waterLevel, shoreline, depth, outlet, "
               "inlets, shore segments (beach, cliff, marsh, rocky) and harbourSites. Plus springs, wetlands, fords, bridgeSites "
               "(scored), confluences, rapids and waterfalls, and the canals, moats, culverts and bridges added as features. "
               "World space, cm; discharge m3/s.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainWaterInfo");
            json g = waterGraph(*analysisOf(f, a), worldMapOf(f));
            json canals = json::array(), culverts = json::array(), bridges = json::array();
            for (const StampEntry& e : allStamps(Graph::current(), f, "terrainWaterInfo"))
            {
                if (!e.stamp.isLine())
                    continue;
                const json j = stampJson(f, e.stamp, e.result, e.layer);
                if (e.stamp.kind == "canal" || e.stamp.kind == "moat")
                    canals.push_back({{"id", e.stamp.id},
                                      {"kind", e.stamp.kind},
                                      {"points", j["centreline"]},
                                      {"waterLevel", j.value("waterLevel", 0.0)},
                                      {"width", e.stamp.width * f.frame.across()},
                                      {"depth", e.stamp.depth * f.frame.vertical()}});
                else if (e.stamp.kind == "culvert")
                    culverts.push_back({{"id", e.stamp.id}, {"points", j["centreline"]}});
                else if (e.stamp.kind == "bridge")
                    bridges.push_back({{"id", e.stamp.id}, {"deck", j["centreline"]}, {"span", j["length"]}});
                for (const json& c : j.value("culverts", json::array()))
                    culverts.push_back({{"street", e.stamp.id}, {"position", c}});
                for (const json& sp : j.value("spans", json::array()))
                    if (sp["kind"] == "bridge")
                        bridges.push_back({{"street", e.stamp.id}, {"deck", sp["points"]}, {"span", sp["length"]}});
            }
            g["canals"] = canals;
            g["culverts"] = culverts;
            g["bridges"] = bridges;
            g["revision"] = revision(*f.local);
            return g;
        },
        .examples = {"w = terrainWaterInfo('Terrain', seaLevel=0)\n[r['id'] for r in w['rivers'] if r['navigable']], w['bridgeSites'][:3]"},
    });

    ctx.registerCommand({
        .name = "terrainBuildable",
        .doc = "Polygons of contiguous buildable land: no steeper than maxSlope degrees, buildable (not water, cliff or unstable "
               "talus) and, with avoidFlood, under floodTolerance flood risk. Each has its outline, holes, area (m2), centroid, "
               "mean slope, height and buildability; largest first.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({{"minArea", "ma", T::Float, 2000.0, "Smallest region, m2.", {}, {}, 0.0},
                                    {"maxSlope", "ms", T::Float, 15.0, "Steepest ground, degrees.", {}, {}, 0.0, 90.0},
                                    {"avoidFlood", "af", T::Bool, true, "Leave out ground that floods."},
                                    {"floodTolerance", "ft", T::Float, 0.3, "Flood risk counted as flooding, 0 to 1.", {}, {}, 0.0, 1.0}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainBuildable");
            return {{"regions", buildableRegions(*analysisOf(f, a), worldMapOf(f), a.get<double>("minArea", 2000.0), a.get<double>("maxSlope", 15.0),
                                                 a.get<bool>("avoidFlood", true), a.get<double>("floodTolerance", 0.3))},
                    {"revision", revision(*f.local)}};
        },
        .examples = {"r = terrainBuildable('Terrain', minArea=5000, maxSlope=10)['regions']\nr[0]['area'], r[0]['centroid']"},
    });

    ctx.registerCommand({
        .name = "terrainSites",
        .doc = "Scored places for a kind of site, best first: townCentre (flat, buildable, near water and a river crossing, out of "
               "flood), harbour (sheltered, deep inshore, flat land behind, a river mouth), bridge and ford (from the rivers), mill "
               "(waterfalls and rapids), castle (prominent and defensible), temple (prominent and seen from afar), farmland "
               "(gentle, wet, deep soil) or quarry (exposed hard rock). Each has its position and score.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({{"kind", "k", T::String, "townCentre", "Kind of site.", siteKinds()},
                                    {"count", "n", T::Int, int64_t(5), "How many.", {}, {}, 1.0, 1000.0},
                                    {"spacing", "sp", T::Float, 20000.0, "Least distance between them, cm.", {}, {}, 0.0}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainSites");
            try
            {
                return {{"sites", sites(*analysisOf(f, a), worldMapOf(f), a.get<std::string>("kind", "townCentre"), int(a.get<int64_t>("count", 5)),
                                        a.get<double>("spacing", 20000.0) / f.frame.across())},
                        {"revision", revision(*f.local)}};
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("terrainSites: ") + e.what());
            }
        },
        .examples = {"terrainSites('Terrain', kind='castle', count=3)['sites'][0]['position']"},
    });

    ctx.registerCommand({
        .name = "terrainContours",
        .doc = "Contour lines every interval cm of height, from base, optionally only inside region (a polygon). Returns levels "
               "with their lines as world-space point lists; closed lines end where they start.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"interval", "i", T::Float, 1000.0, "Height between contours, cm.", {}, {}, 1.0},
                  {"base", "b", T::Float, 0.0, "A height every contour is a whole number of intervals from."},
                  {"region", "rg", T::List, nullptr, "Polygon to keep them inside."},
                  {"curves", "cv", T::Bool, false, "Also make poly curves of them."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainContours");
            const Heightfield& h = *f.local;
            const double interval = std::max(1.0, a.get<double>("interval", 1000.0)) / f.frame.vertical();
            const double base = f.frame.localHeight(a.get<double>("base", 0.0), h.origin);
            std::vector<uint8_t> inside;
            if (a.has("region"))
            {
                std::vector<glm::dvec2> ring;
                for (const glm::dvec3& p : worldPoints(a.flags["region"], "terrainContours"))
                    ring.push_back(f.frame.local(p));
                inside = footprintMask(h, footprintFromPolygon(ring));
            }
            std::vector<float> grid = h.heights;
            const glm::dvec2 r = h.range();
            json levels = json::array();
            int made = 0;
            for (double lv = base + std::ceil((r.x - base) / interval) * interval; lv <= r.y && made < 2000; lv += interval, ++made)
            {
                json lines = json::array();
                for (const auto& line : contourLines(grid, h.nx, h.nz, float(lv)))
                {
                    json pts = json::array();
                    for (const glm::dvec2& g : line)
                    {
                        const glm::dvec2 p = h.origin + g * h.cell;
                        const bool keep = inside.empty() || inside[h.index(std::clamp(int(std::lround(g.x)), 0, h.nx - 1), std::clamp(int(std::lround(g.y)), 0, h.nz - 1))];
                        if (!keep)
                        {
                            if (pts.size() >= 2)
                                lines.push_back(pts);
                            pts = json::array();
                            continue;
                        }
                        pts.push_back(worldJson(f.frame.world(p.x, lv, p.y)));
                    }
                    if (pts.size() >= 2)
                        lines.push_back(pts);
                }
                if (lines.empty())
                    continue;
                if (a.get<bool>("curves", false))
                    for (const json& l : lines)
                        ops::execute("curve", {{"points", l}, {"type", "poly"}, {"name", "Contour"}});
                levels.push_back({{"height", upOf(f.frame.world(h.origin.x, lv, h.origin.y))}, {"lines", lines}});
            }
            return {{"levels", levels}, {"revision", revision(h)}};
        },
        .examples = {"c = terrainContours('Terrain', interval=500)\nlen(c['levels'])"},
    });

    ctx.registerCommand({
        .name = "terrainContourStreets",
        .doc = "A hillside street layout inside region: streets along contours spacing cm apart, and cross streets every "
               "crossSpacing cm joining them; a cross street steeper than maxGrade percent is returned as stairs, or as a "
               "switchback when allowSwitchbacks. Returns streets (kind contour, cross, stairs or switchback, with points and "
               "grade) for terrainAddFeature.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"region", "rg", T::List, nullptr, "Polygon of the hillside."},
                  {"spacing", "sp", T::Float, 3000.0, "Between contour streets across the slope, cm.", {}, {}, 100.0},
                  {"crossSpacing", "cs", T::Float, 8000.0, "Between cross streets along the contours, cm.", {}, {}, 100.0},
                  {"maxGrade", "mg", T::Float, 10.0, "Steepest street, percent.", {}, {}, 0.0},
                  {"allowSwitchbacks", "sw", T::Bool, true, "Zigzag steep cross streets instead of stairs."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Found f = oneTerrain(a, "terrainContourStreets");
            if (!a.has("region"))
                throw CommandError("terrainContourStreets: give region=");
            const Heightfield& h = *f.local;
            std::vector<glm::dvec2> ring;
            for (const glm::dvec3& p : worldPoints(a.flags["region"], "terrainContourStreets"))
                ring.push_back(f.frame.local(p));
            const std::vector<uint8_t> inside = footprintMask(h, footprintFromPolygon(ring));
            const auto an = analyse(f.local);
            const std::vector<float>& slope = an->field("slope");
            double slopeSum = 0.0, hlo = 1e30, hhi = -1e30;
            size_t n = 0;
            for (size_t i = 0; i < h.size(); ++i)
                if (inside[i])
                {
                    slopeSum += std::tan(slope[i] / 57.29577951308232);
                    hlo = std::min(hlo, double(h.heights[i]));
                    hhi = std::max(hhi, double(h.heights[i]));
                    ++n;
                }
            if (n == 0)
                throw CommandError("terrainContourStreets: the region misses the terrain");
            const double spacing = a.get<double>("spacing", 3000.0) / f.frame.across();
            const double interval = std::max(1.0, spacing * slopeSum / double(n));
            const double maxGrade = a.get<double>("maxGrade", 10.0) * 0.01;
            json streets = json::array();
            std::vector<std::vector<glm::dvec2>> rows;
            for (double lv = hlo + interval * 0.5; lv < hhi; lv += interval)
                for (const auto& line : contourLines(h.heights, h.nx, h.nz, float(lv)))
                {
                    std::vector<glm::dvec2> run;
                    auto flush = [&] {
                        if (run.size() >= 2)
                        {
                            json pts = json::array();
                            for (const glm::dvec2& p : run)
                                pts.push_back(worldJson(f.frame.world(p.x, lv, p.y)));
                            streets.push_back({{"kind", "contour"}, {"points", pts}, {"grade", 0.0}});
                            rows.push_back(run);
                        }
                        run.clear();
                    };
                    for (const glm::dvec2& g : line)
                    {
                        const int x = std::clamp(int(std::lround(g.x)), 0, h.nx - 1), z = std::clamp(int(std::lround(g.y)), 0, h.nz - 1);
                        if (inside[h.index(x, z)])
                            run.push_back(h.origin + g * h.cell);
                        else
                            flush();
                    }
                    flush();
                }
            // Cross streets: from points along each contour street straight downhill to the next row.
            const double crossSpacing = a.get<double>("crossSpacing", 8000.0) / f.frame.across();
            for (const auto& row : rows)
            {
                double carry = 0.0;
                for (size_t k = 1; k < row.size(); ++k)
                {
                    carry += glm::length(row[k] - row[k - 1]);
                    if (carry < crossSpacing)
                        continue;
                    carry = 0.0;
                    const glm::dvec2 p = row[k];
                    const glm::dvec3 nrm = h.normalAt(p.x, p.y);
                    glm::dvec2 down(nrm.x, nrm.z);
                    if (glm::length(down) < 1e-9)
                        continue;
                    down = glm::normalize(down);
                    const glm::dvec2 q = p + down * spacing;
                    if (!h.contains(q.x, q.y) || !inside[h.index(std::clamp(int(std::lround((q.x - h.origin.x) / h.cell)), 0, h.nx - 1),
                                                                 std::clamp(int(std::lround((q.y - h.origin.y) / h.cell)), 0, h.nz - 1))])
                        continue;
                    const double rise = std::abs(h.heightAt(p.x, p.y) - h.heightAt(q.x, q.y));
                    const double grade = rise / spacing;
                    std::vector<glm::dvec2> pts{p, q};
                    std::string kind = "cross";
                    if (grade > maxGrade && maxGrade > 0.0)
                    {
                        if (a.get<bool>("allowSwitchbacks", true))
                        {
                            // Zigzag across the fall line until each leg holds the grade.
                            const int legs = int(std::ceil(grade / maxGrade));
                            const glm::dvec2 across(-down.y, down.x);
                            const double run = rise / maxGrade / double(legs);
                            pts.clear();
                            for (int l = 0; l <= legs; ++l)
                                pts.push_back(p + down * (spacing * double(l) / double(legs)) + across * ((l % 2) ? 1.0 : 0.0) * std::sqrt(std::max(0.0, run * run - (spacing / legs) * (spacing / legs))));
                            kind = "switchback";
                        }
                        else
                            kind = "stairs";
                    }
                    json jp = json::array();
                    for (const glm::dvec2& c : pts)
                        jp.push_back(worldJson(f.frame.world(c.x, h.heightAt(c.x, c.y), c.y)));
                    streets.push_back({{"kind", kind}, {"points", jp}, {"grade", grade * 100.0 * f.frame.vertical() / f.frame.across()}});
                }
            }
            return {{"streets", streets}, {"revision", revision(h)}};
        },
        .examples = {"s = terrainContourStreets('Terrain', region=[[0, 0], [40000, 0], [40000, 30000], [0, 30000]], maxGrade=8)['streets']"},
    });

    ctx.registerCommand({
        .name = "terrainRaycast",
        .doc = "Where a ray from origin along direction first meets the terrain: hit, position and distance (cm). isSolid=points "
               "instead says for each point whether it is underground.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"origin", "o", T::Vec3, json::array({0.0, 100000.0, 0.0}), "Start, world space."},
                  {"direction", "d", T::Vec3, json::array({0.0, -1.0, 0.0}), "Direction, world space."},
                  {"isSolid", "is", T::List, nullptr, "Points [[x, y, z], ...] to test instead."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const Found f = oneTerrain(a, "terrainRaycast");
            const Frame& fr = f.frame;
            if (a.has("isSolid"))
            {
                json out = json::array();
                for (const glm::dvec3& p : worldPoints(a.flags["isSolid"], "terrainRaycast"))
                {
                    const glm::dvec3 q(fr.inv * glm::dvec4(p, 1.0));
                    out.push_back(f.local->contains(q.x, q.z) && q.y < f.local->heightAt(q.x, q.z));
                }
                return out;
            }
            const json o = a.flags["origin"], d = a.flags["direction"];
            const glm::dvec3 ow(o[0].get<double>(), o[1].get<double>(), o[2].get<double>()), dw(d[0].get<double>(), d[1].get<double>(), d[2].get<double>());
            const glm::dvec3 ol(fr.inv * glm::dvec4(ow, 1.0)), dl(fr.inv * glm::dvec4(dw, 0.0));
            glm::dvec3 hit;
            if (!raycast(*f.local, ol, dl, &hit))
                return {{"hit", false}};
            const glm::dvec3 hw = fr.world(hit.x, hit.y, hit.z);
            return {{"hit", true}, {"position", worldJson(hw)}, {"distance", glm::length(hw - ow)}};
        },
        .examples = {"terrainRaycast('Terrain', origin=[0, 100000, 0], direction=[0, -1, 0])['position']"},
    });

    ctx.registerCommand({
        .name = "terrainCheck",
        .doc = "What is wrong with a terrain and what sits on it: pads moving more than cutFillLimit m3, streets steeper than "
               "maxGrade, pads, regions and buildings in flood (risk over floodTolerance) with no levee, embankment or seawall "
               "near, buildings over water, buildings floating (the ground under their footprint lower than their base by more "
               "than floatTolerance cm anywhere), stamps and streets across a river with no bridge or culvert, and canals that "
               "reach no water, volume splits whose seams crack (seamTolerance cm), whose holes no mesh fills, or with places no "
               "mesh covers, roads (terrainAddRoad) steeper or more banked than their design or over water with no bridge or culvert, "
               "rivers rising downstream, lakes below their rim with no outlet or above it with no dam, cliff pieces floating above the "
               "ground (floatingCliff), cracks where a cliff meets the terrain (cliffCrack) and cliff tiles over triangleBudget in a "
               "World Partition cell (tileOverBudget). Returns ok and the issues, each with its type, what it is about, where and a message.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({nodeFlag({"buildings", "b", T::List, json::array(), "Meshes standing on the terrain to check."}, "mesh"),
                                    {"cutFillLimit", "cf", T::Float, 500.0, "Largest cut plus fill per pad, m3.", {}, {}, 0.0},
                                    {"maxGrade", "mg", T::Float, 12.0, "Steepest street, percent.", {}, {}, 0.0},
                                    {"floodTolerance", "ft", T::Float, 0.5, "Flood risk counted as flooding.", {}, {}, 0.0, 1.0},
                                    {"floatTolerance", "flt", T::Float, 10.0, "Gap under a building counted as floating, cm.", {}, {}, 0.0},
                                    {"seamTolerance", "stl", T::Float, 1.0, "Gap between a volume piece and the ground at a seam counted as a crack, cm.", {}, {}, 0.0},
                                    {"cliffTolerance", "ctl", T::Float, 5.0, "Gap under a cliff's foot (floating) or over its top or seam (crack), cm.", {}, {}, 0.0},
                                    {"triangleBudget", "tb", T::Float, 400000.0, "Most cliff triangles in one World Partition cell.", {}, {}, 1.0},
                                    {"cellSize", "cs", T::Float, 12800.0, "World Partition cell size for the triangle budget, cm.", {}, {}, 100.0}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const Found f = oneTerrain(a, "terrainCheck");
            const Heightfield& h = *f.local;
            const auto an = analysisOf(f, a);
            const double tol = a.get<double>("floodTolerance", 0.5);
            json issues = json::array();
            auto issue = [&](const char* type, const std::string& about, const json& at, const std::string& msg) {
                issues.push_back({{"type", type}, {"about", about}, {"position", at}, {"message", msg}});
            };
            const std::vector<StampEntry> stamps = allStamps(g, f, "terrainCheck");
            std::vector<std::pair<glm::dvec2, double>> protection;
            for (const StampEntry& e : stamps)
                if (e.stamp.kind == "levee" || e.stamp.kind == "embankment" || e.stamp.kind == "seawall")
                    for (const glm::dvec3& p : e.result.centreline)
                        protection.push_back({{p.x, p.z}, e.stamp.width * 4.0 + 5000.0 / f.frame.across()});
            auto protectedAt = [&](glm::dvec2 p) {
                return std::any_of(protection.begin(), protection.end(), [&](const auto& q) { return glm::length(q.first - p) < q.second; });
            };
            auto floodAt = [&](glm::dvec2 p) { return an->sample("floodRisk", p.x, p.y); };
            auto riverAt = [&](glm::dvec2 p) {
                const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
                const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
                const int32_t r = an->reachOf()[h.index(x, z)];
                return r >= 0 && an->reaches()[size_t(r)].order >= 2 ? r : -1;
            };
            const double limit = a.get<double>("cutFillLimit", 500.0);
            const double maxGrade = a.get<double>("maxGrade", 12.0);
            for (const StampEntry& e : stamps)
            {
                const json j = stampJson(f, e.stamp, e.result, e.layer);
                const json at = j["center"];
                const std::string about = e.layer + "/" + e.stamp.id;
                if ((e.stamp.kind == "pad" || e.stamp.kind == "region") && j["cutM3"].get<double>() + j["fillM3"].get<double>() > limit)
                    issue("cutFill", about, at, std::format("moves {:.0f} m3, over {:.0f}", j["cutM3"].get<double>() + j["fillM3"].get<double>(), limit));
                if (e.stamp.kind == "street" && j.value("maxGrade", 0.0) > maxGrade)
                    issue("streetGrade", about, at, std::format("grade {:.1f}% over {:.1f}%", j["maxGrade"].get<double>(), maxGrade));
                if ((e.stamp.kind == "pad" || e.stamp.kind == "region") && e.result.samples > 0 && floodAt(e.result.centroid) > tol && !protectedAt(e.result.centroid))
                    issue("flood", about, at, std::format("flood risk {:.2f} with no levee near", floodAt(e.result.centroid)));
                // Blocking water: pads over rivers, streets crossing them outside a bridge span or culvert.
                if (e.stamp.kind == "pad" || e.stamp.kind == "region")
                {
                    if (!e.stamp.polygon.empty())
                    {
                        const std::vector<uint8_t> cover = footprintMask(h, footprintFromPolygon(e.stamp.polygon));
                        for (size_t i = 0; i < cover.size(); ++i)
                            if (cover[i] && an->reachOf()[i] >= 0 && an->reaches()[size_t(an->reachOf()[i])].order >= 2)
                            {
                                issue("blocksFlow", about, at, "stands in river " + std::to_string(an->reachOf()[i]));
                                break;
                            }
                    }
                }
                else if (e.stamp.kind == "street")
                {
                    for (size_t k = 0; k < e.result.centreline.size(); ++k)
                    {
                        const glm::dvec2 p(e.result.centreline[k].x, e.result.centreline[k].z);
                        if (riverAt(p) < 0)
                            continue;
                        const bool spanned = std::any_of(e.result.spans.begin(), e.result.spans.end(), [&](const auto& s) { return s.kind == "bridge" && k >= s.from && k <= s.to; });
                        const bool culverted = std::any_of(e.result.culverts.begin(), e.result.culverts.end(), [&](glm::dvec2 c) { return glm::length(c - p) < 3.0 * h.cell; });
                        const bool bridged = std::any_of(stamps.begin(), stamps.end(), [&](const StampEntry& b) {
                            return b.stamp.kind == "bridge" && std::any_of(b.result.centreline.begin(), b.result.centreline.end(),
                                                                            [&](const glm::dvec3& q) { return glm::length(glm::dvec2(q.x, q.z) - p) < 3.0 * h.cell; });
                        });
                        if (!spanned && !culverted && !bridged)
                        {
                            issue("blocksFlow", about, worldJson(f.frame.world(p.x, h.heightAt(p.x, p.y), p.y)), "crosses river " + std::to_string(riverAt(p)) + " with no bridge or culvert");
                            break;
                        }
                    }
                }
                else if ((e.stamp.kind == "canal" || e.stamp.kind == "moat") && e.result.centreline.size() >= 2 && e.stamp.kind == "canal")
                {
                    auto wet = [&](const glm::dvec3& p) {
                        const glm::dvec2 q(p.x, p.z);
                        for (int dz = -2; dz <= 2; ++dz)
                            for (int dx = -2; dx <= 2; ++dx)
                            {
                                const glm::dvec2 s2 = q + glm::dvec2(dx, dz) * h.cell;
                                const int x = std::clamp(int(std::lround((s2.x - h.origin.x) / h.cell)), 0, h.nx - 1);
                                const int z = std::clamp(int(std::lround((s2.y - h.origin.y) / h.cell)), 0, h.nz - 1);
                                if (an->reachOf()[h.index(x, z)] >= 0 || an->lakeOf()[h.index(x, z)] >= 0)
                                    return true;
                            }
                        return false;
                    };
                    if (!wet(e.result.centreline.front()) && !wet(e.result.centreline.back()))
                        issue("canalDisconnected", about, at, "neither end reaches a river, lake or the sea");
                }
            }
            for (const json& b : a.flags.value("buildings", json::array()))
            {
                const std::string name = b.get<std::string>();
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                {
                    issue("missing", name, nullptr, "no mesh named " + name);
                    continue;
                }
                const auto mesh = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
                if (!mesh || mesh->empty())
                    continue;
                const glm::dmat4 m = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                // The base: the lowest points of the mesh, in the terrain's space.
                std::vector<glm::dvec3> local;
                double base = 1e30;
                for (geo::Index p = 0; p < mesh->pointCapacity(); ++p)
                    if (mesh->pointValid(p))
                    {
                        const glm::dvec3 q(f.frame.inv * (m * glm::dvec4(mesh->position(p), 1.0)));
                        local.push_back(q);
                        base = std::min(base, q.y);
                    }
                double worstGap = 0.0;
                glm::dvec2 c(0.0), worstAt(0.0);
                for (const glm::dvec3& q : local)
                {
                    c += glm::dvec2(q.x, q.z) / double(local.size());
                    if (q.y > base + 1.0 / f.frame.vertical())
                        continue;
                    const double gap = base - h.heightAt(q.x, q.z);
                    if (gap > worstGap)
                    {
                        worstGap = gap;
                        worstAt = {q.x, q.z};
                    }
                }
                const json at = worldJson(f.frame.world(c.x, base, c.y));
                if (worstGap * f.frame.vertical() > a.get<double>("floatTolerance", 10.0))
                    issue("floating", name, worldJson(f.frame.world(worstAt.x, base, worstAt.y)), std::format("base {:.0f} cm above the ground", worstGap * f.frame.vertical()));
                if (an->sample("waterDepth", c.x, c.y) > 0.0)
                    issue("overWater", name, at, "stands over water");
                else if (floodAt(c) > tol && !protectedAt(c))
                    issue("flood", name, at, std::format("flood risk {:.2f} with no levee near", floodAt(c)));
            }
            for (const json& i : volumeIssues(f.modifier ? f.modifier->id() : 0, f.frame.m, a.get<double>("seamTolerance", 1.0)))
                issues.push_back(i);
            for (const json& i : featureIssues(pathOf(f, a.objects()[0]), *an, f.frame.m, 0.05))
                issues.push_back(i);
            for (const json& i : cliffs::cliffIssues(pathOf(f, a.objects()[0]), a.get<double>("cliffTolerance", 5.0), a.get<double>("triangleBudget", 400000.0),
                                                     a.get<double>("cellSize", 12800.0)))
                issues.push_back(i);
            return {{"ok", issues.empty()}, {"issues", issues}, {"revision", revision(h)}};
        },
        .examples = {"c = terrainCheck('Terrain', buildings=['House1', 'House2'])\nc['ok'], c['issues']"},
    });

    ctx.registerCommand({
        .name = "terrainDirtyRegions",
        .doc = "What changed on a terrain since a revision another terrain command returned: rectangles on the ground plane "
               "(min and max) where heights or masks differ, so a city re-plans only the blocks they touch. known is false when "
               "that revision is too old to compare (treat everything as changed).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"since", "s", T::String, "", "The revision to compare with."}, {"tile", "t", T::Int, int64_t(64), "Tile size, samples.", {}, {}, 4.0, 1024.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainDirtyRegions: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainDirtyRegions");
            const std::string now = revision(*f.local);
            const std::string since = a.get<std::string>("since", "");
            std::shared_ptr<const Heightfield> old;
            for (const Seen& s : seenRevisions()[pathOf(f, "")])
                if (s.revision == since)
                    old = s.h;
            remember(f, now);
            if (since == now)
                return {{"known", true}, {"revision", now}, {"regions", json::array()}};
            const Heightfield& h = *f.local;
            if (!old || old->nx != h.nx || old->nz != h.nz)
                return {{"known", false}, {"revision", now}, {"regions", json::array()}};
            const int tile = int(a.get<int64_t>("tile", 64));
            json regions = json::array();
            std::vector<const std::vector<float>*> oldMasks;
            for (const auto& [name, v] : h.masks)
            {
                const std::vector<float>* o = old->findMask(name);
                oldMasks.push_back(o && o->size() == v.size() ? o : nullptr);
            }
            for (int tz = 0; tz < h.nz; tz += tile)
                for (int tx = 0; tx < h.nx; tx += tile)
                {
                    // The changed samples' own bounds in the tile, so a small edit on a coarse terrain stays small.
                    int x0 = h.nx, z0 = h.nz, x1 = -1, z1 = -1;
                    for (int z = tz; z < std::min(h.nz, tz + tile); ++z)
                        for (int x = tx; x < std::min(h.nx, tx + tile); ++x)
                        {
                            const size_t i = h.index(x, z);
                            bool dirty = h.heights[i] != old->heights[i];
                            size_t m = 0;
                            for (auto it = h.masks.begin(); it != h.masks.end() && !dirty; ++it, ++m)
                                dirty = !oldMasks[m] || (*oldMasks[m])[i] != it->second[i];
                            if (dirty)
                                x0 = std::min(x0, x), z0 = std::min(z0, z), x1 = std::max(x1, x), z1 = std::max(z1, z);
                        }
                    if (x1 < 0)
                        continue;
                    const glm::dvec2 lo = h.samplePos(std::max(0, x0 - 1), std::max(0, z0 - 1)),
                                     hi = h.samplePos(std::min(h.nx - 1, x1 + 1), std::min(h.nz - 1, z1 + 1));
                    const glm::dvec3 a0 = f.frame.world(lo.x, 0.0, lo.y), a1 = f.frame.world(hi.x, 0.0, hi.y);
                    regions.push_back({{"min", vec2(glm::min(groundOf(a0), groundOf(a1)))}, {"max", vec2(glm::max(groundOf(a0), groundOf(a1)))}});
                }
            return {{"known", true}, {"revision", now}, {"regions", regions}};
        },
        .examples = {"r0 = terrainInfo('Terrain')['revision']\n# ... edits ...\nterrainDirtyRegions('Terrain', since=r0)['regions']"},
    });
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    const NoiseParams np;
    ctx.registerCommand({
        .name = "terrainCreate",
        .doc = "Make a terrain: a mesh whose geometry-nodes modifier builds a heightfield, starting as fractal noise (flat=True for "
               "level ground).\n\n"
               "The size, resolution and noise settings become keyable inputs on the modifier, so change the terrain later with "
               "setAttr on '<modifier>.<flag>' and read it back with terrainInfo or terrainSample. Open the group to add Terrain "
               "nodes. Lengths are cm. On a Z-up scene the mesh is turned so the terrain lies on the XY plane with heights up Z, "
               "and every terrain command works in those axes. Returns the mesh, modifier, group and terrainInfo's stats.",
        .flags = {{"size", "sz", T::List, json::array({100000.0, 100000.0}), "[width, depth] on the ground plane, cm."},
                  {"resolution", "r", T::Int, int64_t(513), "Samples along the longer side.", {}, {}, 2.0, 16385.0},
                  {"center", "c", T::Vec3, json::array({0.0, 0.0, 0.0}), "Centre; its up coordinate is the base height."},
                  {"flat", "f", T::Bool, false, "Level ground, no noise."},
                  {"amplitude", "a", T::Float, np.amplitude, "Height of the tallest features, cm.", {}, {}, 0.0},
                  {"featureSize", "fs", T::Float, np.featureSize, "Size of the largest features, cm.", {}, {}, 0.0},
                  {"octaves", "o", T::Int, int64_t(np.octaves), "Layers of detail.", {}, {}, 1.0, 16.0},
                  {"roughness", "ro", T::Float, np.roughness, "0 to 1: how strong each smaller layer is.", {}, {}, 0.0, 1.0},
                  {"ridged", "ri", T::Float, np.ridged, "0 rolling hills, 1 sharp ridges.", {}, {}, 0.0, 1.0},
                  {"warp", "w", T::Float, np.warp, "Domain warp, a fraction of featureSize.", {}, {}, 0.0},
                  {"seed", "s", T::Int, int64_t(0), "Random seed."},
                  {"erosion", "e", T::Int, int64_t(0), "Erosion steps: rivers carve valleys; 0 is off, 50 to 150 looks natural.", {}, {}, 0.0, 10000.0},
                  {"erosionStrength", "es", T::Float, ErosionParams().strength, "How fast rivers cut.", {}, {}, 0.0},
                  {"uplift", "u", T::Float, 0.0, "Rise per erosion step, cm."},
                  {"talusAngle", "ta", T::Float, ErosionParams().talusAngle, "Slopes steeper than this shed scree, degrees.", {}, {}, 1.0, 89.0},
                  {"age", "ag", T::Float, 0.0, "Erode preset age, 0 to 1: macro form, debris, scree, rills and deposition; 0 is off.", {}, {}, 0.0, 2.0},
                  {"erosionPreset", "ep", T::String, "natural", "Erode preset for age.", erodePresetNames()},
                  {"name", "n", T::String, "Terrain", "Name of the mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const json size = a.flags["size"];
            if (!size.is_array() || size.size() != 2)
                throw CommandError("terrainCreate: size is [width, depth]");
            // On a Z-up scene the heightfield's X runs along world Y and its Z along world X.
            json values = {{"sizeX", zUp() ? size[1] : size[0]},
                           {"sizeZ", zUp() ? size[0] : size[1]},
                           {"resolution", a.get<int64_t>("resolution", 513)},
                           {"amplitude", a.flags["amplitude"]},
                           {"featureSize", a.flags["featureSize"]},
                           {"octaves", a.flags["octaves"]},
                           {"roughness", a.flags["roughness"]},
                           {"ridged", a.flags["ridged"]},
                           {"warp", a.flags["warp"]},
                           {"seed", a.flags["seed"]},
                           {"erosion", a.flags["erosion"]},
                           {"erosionStrength", a.flags["erosionStrength"]},
                           {"uplift", a.flags["uplift"]},
                           {"talusAngle", a.flags["talusAngle"]},
                           {"age", a.get<double>("age", 0.0)},
                           {"erosionPreset", a.get<std::string>("erosionPreset", "natural")}};
            const bool flat = a.get<bool>("flat", false);
            if (flat)
                values["amplitude"] = 0.0;

            const std::string host = createHost(a.get<std::string>("name", "Terrain"));
            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Terrain");

            json inputs = json::array();
            json links = json::array({"Grid.heightfield -> Noise.heightfield", "Noise.heightfield -> Erode.heightfield",
                                      "Erode.heightfield -> Preset.heightfield", "Preset.heightfield -> GroupOutput.geometry"});
            for (const Exposed& e : kInputs)
            {
                json in = {{"name", e.name}, {"type", e.type}, {"default", values[e.name]}, {"panel", e.panel}, {"description", e.description}, {"min", e.min}};
                if (e.softMax > e.min)
                    in["softMax"] = e.softMax;
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> " + e.node + "." + e.socket);
            }
            json spec = {{"name", group},
                         {"description", "A terrain: a heightfield grid shaped by fractal noise. Add Terrain nodes between Noise and the output."},
                         {"inputs", inputs},
                         {"nodes", json::array({{{"name", "Grid"}, {"type", "HeightfieldGrid"}, {"values", {{"center", localCenter(a.flags["center"])}}}},
                                                {{"name", "Noise"}, {"type", "TerrainNoise"}},
                                                {{"name", "Erode"}, {"type", "ErodeTerrain"}},
                                                {{"name", "Preset"}, {"type", "TerrainErodePreset"}}})},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            json mod = ops::execute("gnApply", {{"args", {host, group}}});

            json out = describe(findTerrain(g, host, "terrainCreate"));
            out["group"] = group;
            return out;
        },
        .examples = {"t = terrainCreate(size=[200000, 200000], resolution=1025, amplitude=40000, ridged=0.8)",
                     "terrainCreate(flat=True, size=[50000, 50000], name='Plain')"},
    });

    ctx.registerCommand({
        .name = "terrainInfo",
        .doc = "What a terrain is: resolution, cell size, the area it covers in world X and Z (min, max, center, size), its height "
               "range and each mask's min, max and mean. Lengths are cm.",
        .positionalDoc = "terrain mesh or its modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainInfo: give one terrain");
            return describe(findTerrain(Graph::current(), names[0], "terrainInfo"));
        },
        .examples = {"terrainInfo('Terrain')['heightRange']"},
    });

    ctx.registerCommand({
        .name = "terrainSample",
        .doc = "Heights under points, or a profile along a path.\n\n"
               "points= reads each point; path= walks the polyline every step cm and adds the distance along it and the grade "
               "(rise over run, percent) from the previous sample. Each sample has its ground coordinates (x and z; x and y on a "
               "Z-up scene), height along the up axis, position, normal, slope (degrees), inside, and the masks named by masks=. "
               "World space, cm, for terrains in any placement.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = withAnalysisFlags({{"points", "p", T::List, nullptr, "[[x, z] or [x, y, z], ...]; [x, y] on a Z-up scene."},
                  {"path", "pa", T::List, nullptr, "Polyline of points like points= to profile."},
                  {"step", "st", T::Float, 0.0, "Spacing along path, cm; 0 uses the terrain's cell size.", {}, {}, 0.0},
                  {"masks", "m", T::List, json::array(), "Mask names to read."},
                  {"fields", "fl", T::List, json::array(), "Analysis fields to read (hand, floodRisk, buildability, slopeClass, solar, ... see terrainField)."},
                  }),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSample: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainSample");
            std::vector<std::string> masks;
            for (const json& m : a.flags.value("masks", json::array()))
                masks.push_back(m.get<std::string>());
            for (const json& m : a.flags.value("fields", json::array()))
                masks.push_back(m.get<std::string>());
            std::shared_ptr<const Analysis> an;
            for (const std::string& m : masks)
                if (!f.local->findMask(m) && std::ranges::find(Analysis::fieldNames(), m) != Analysis::fieldNames().end())
                    an = analyse(f.local, analysisParams(a));
            json out = json::array();
            if (a.has("points"))
                for (const json& p : a.flags["points"])
                    out.push_back(sampleAt(f, worldPoint(p, "terrainSample"), masks, an.get()));
            else if (a.has("path"))
            {
                std::vector<glm::dvec2> pts;
                for (const json& p : a.flags["path"])
                    pts.push_back(groundOf(worldPoint(p, "terrainSample")));
                if (pts.size() < 2)
                    throw CommandError("terrainSample: path takes at least two points");
                const double step = a.get<double>("step", 0.0) > 0.0 ? a.get<double>("step", 0.0) : f.local->cell * f.frame.across();
                std::vector<double> at(pts.size(), 0.0);
                for (size_t k = 1; k < pts.size(); ++k)
                    at[k] = at[k - 1] + glm::length(pts[k] - pts[k - 1]);
                const double total = at.back();
                const size_t count = size_t(std::ceil(total / step - 1e-9)) + 1;
                if (count > 1000000)
                    throw CommandError("terrainSample: over a million samples; use a larger step");
                double prevHeight = 0.0, prevAlong = 0.0;
                size_t seg = 1;
                for (size_t i = 0; i < count; ++i)
                {
                    const double d = std::min(total, double(i) * step);
                    while (seg + 1 < pts.size() && at[seg] < d)
                        ++seg;
                    const double len = at[seg] - at[seg - 1];
                    const glm::dvec2 q = len > 1e-12 ? pts[seg - 1] + (pts[seg] - pts[seg - 1]) * ((d - at[seg - 1]) / len) : pts[seg];
                    json smp = sampleAt(f, fromGround(q), masks, an.get());
                    const double h = smp["height"].get<double>();
                    smp["distance"] = d;
                    smp["grade"] = i == 0 || d - prevAlong < 1e-9 ? 0.0 : (h - prevHeight) / (d - prevAlong) * 100.0;
                    prevHeight = h;
                    prevAlong = d;
                    out.push_back(std::move(smp));
                }
            }
            else
                throw CommandError("terrainSample: give points= or path=");
            return out;
        },
        .examples = {"terrainSample('Terrain', points=[[0, 0], [5000, -2000]])",
                     "profile = terrainSample('Terrain', path=[[-40000, 0], [40000, 0]], step=500)\nmax(abs(s['grade']) for s in profile)"},
    });

    ctx.registerCommand({
        .name = "terrainToMesh",
        .doc = "A plain mesh copy of the terrain as it is now, in world space: quads facing up, every step-th sample, uv set 0 over "
               "the whole terrain, masks as point attributes. The terrain is left alone. Returns the new mesh, its point and face counts.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"step", "st", T::Int, int64_t(1), "Keep every step-th sample.", {}, {}, 1.0, 1024.0}, {"name", "n", T::String, "", "Name; defaults to <terrain>_mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainToMesh: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainToMesh");
            geo::HeightfieldMeshOptions o;
            o.step = int(std::clamp<int64_t>(a.get<int64_t>("step", 1), 1, 1024));
            geo::Mesh m = geo::heightfieldToMesh(*f.local, o);
            gn::transformMesh(m, f.frame.m);
            const size_t points = m.numPoints(), faces = m.numFaces();
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = (f.mesh ? f.mesh->name() : std::string("Terrain")) + "_mesh";
            json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
            const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            Node* mesh = g.find(path);
            g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            return {{"mesh", path}, {"points", int64_t(points)}, {"faces", int64_t(faces)}};
        },
        .examples = {"terrainToMesh('Terrain', step=4)"},
    });

    ctx.registerCommand({
        .name = "terrainBake",
        .doc = "Collapse a terrain's node graph into a file: the heightfield, masks and placement are written to a .lilahf next to "
               "the scene (<scene>_terrain/, else the workspace's terrain/ folder), and the terrain's group is replaced by a Read "
               "Heightfield node loading it, its path stored relative to the workspace (else the scene). Stamps and sculpt strokes "
               "are baked in with the rest. Edits made afterwards start from the baked result. Returns the file, the stored path "
               "and terrainInfo's stats.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"file", "f", T::String, "", "Where to write; defaults as above."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainBake: give one terrain");
            Graph& g = Graph::current();
            const Found f = findTerrain(g, names[0], "terrainBake");
            Node* groupNode = gn::groupOfModifier(g, *f.modifier);
            if (!groupNode)
                throw CommandError("terrainBake: " + f.modifier->path() + " has no group");
            const std::string group = gn::readGroup(*groupNode).name;
            const std::string modifier = f.modifier->path();
            fs::path file = a.get<std::string>("file", "");
            if (file.empty())
                file = bakeDir() / (identifier(f.mesh ? f.mesh->name() : group) + ".lilahf");
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            try
            {
                writeHeightfield(file, *f.local);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainBake: ") + e.what());
            }
            // Stored relative to the workspace (else the scene), so the project moves with its bakes.
            const std::string stored = paths::storedPath(file);
            json spec = {{"name", group},
                         {"description", "A baked terrain, read from a file. Add Terrain nodes between Read and the output."},
                         {"inputs", json::array({{{"name", "file"}, {"type", "string"}, {"default", stored}, {"description", "The baked heightfield."}}})},
                         {"nodes", json::array({{{"name", "Read"}, {"type", "ReadHeightfield"}}})},
                         {"links", json::array({"GroupInput.file -> Read.file", "Read.heightfield -> GroupOutput.geometry"})}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", true}});
            ops::execute("setAttr", {{"args", {modifier + ".file", stored}}});
            json out = describe(findTerrain(g, names[0], "terrainBake"));
            out["file"] = file.generic_string();
            out["stored"] = stored;
            return out;
        },
        .examples = {"terrainBake('Terrain')['file']"},
    });
    ctx.registerCommand({
        .name = "terrainExport",
        .doc = "Write a terrain to files for an engine or another tool.\n\n"
               "target='unreal' resamples to a landscape size Unreal accepts and writes <name>.r16 (16-bit heights laid out for "
               "Landscape import), <name>.png, one 8-bit <name>_<layer>.raw weightmap per paint layer and <name>.json with the "
               "section size, sections per component, component counts, actor location, rotation and scale (Unreal axes, cm) to "
               "import it with, and the layers (mask, layer name, file); the holes mask goes to <name>_visibility.raw instead. "
               "target='files' writes <name>.exr (heights in cm), "
               "<name>.png and <name>.r16 (16-bit over the height range), a PNG per mask and <name>.json; a rotated terrain (or "
               "any on a Z-up scene) is written in its own space with its world matrix; tileSize= tiles it (_x<N>_y<M>, one height range "
               "shared by all) and splats=True packs the masks into RGBA PNGs. target='unity' writes edge-matched 2^n+1 RAW tiles, "
               "splatmaps and LilaTerrainImporter.cs; target='godot' writes Terrain3D regions (EXR heights, layer weights) and a "
               "GDScript importer, or glTF tiles with mode='mesh'; target='mesh' writes LOD'd, seam-safe mesh tiles with collision "
               "as FBX, glTF or USD. Volume pieces (see terrainSplit) are written as <name>_<piece>.fbx with their LODs, in world "
               "space about each piece's centre, listed under volume. Returns the settings and files.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = engineExportFlags({{"target", "t", T::String, "unreal", "unreal or files.", {"unreal", "files"}},
                  {"folder", "f", T::String, "", "Output folder; defaults to the bake folder's export/."},
                  {"name", "n", T::String, "", "File name stem; defaults to the terrain's name."},
                  {"pad", "p", T::Bool, false, "unreal: keep the cell size and grow the landscape past the terrain, instead of resampling."},
                  {"minScaleZ", "sz", T::Float, 1.0, "unreal: smallest Z scale (100 is Unreal's default).", {}, {}, 0.01},
                  {"layers", "ly", T::Any, nullptr, "unreal: masks to send as paint layers, a list, or {mask: layer name}; default every mask."}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainExport: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainExport");
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = identifier(f.mesh ? f.mesh->name() : std::string("Terrain"));
            fs::path folder = a.get<std::string>("folder", "");
            if (folder.empty())
                folder = bakeDir() / "export";
            json out;
            try
            {
                if (json engine = exportEngineTarget(a, *f.local, f.frame.m, folder, name); !engine.is_null())
                    out = std::move(engine);
                else if (a.get<std::string>("target", "unreal") == "files")
                {
                    // Files carry the grid in world space when it lines up with it, else in its own space with the placement.
                    gn::Geometry geom;
                    geom.heightfield = f.local;
                    const gn::Geometry w = gn::transformed(geom, f.frame.m);
                    if (w.heightfield && !zUp())
                    {
                        out = exportFiles(*w.heightfield, folder, name);
                        if (json splats = surfaceSplats(*w.heightfield, f.modifier, folder, name); !splats.is_null())
                            out["files"]["splats"] = splats;
                    }
                    else
                    {
                        out = exportFiles(*f.local, folder, name);
                        if (json splats = surfaceSplats(*f.local, f.modifier, folder, name); !splats.is_null())
                            out["files"]["splats"] = splats;
                        json m = json::array();
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 4; ++r)
                                m.push_back(f.frame.m[c][r]);
                        out["matrix"] = m;
                    }
                }
                else
                    out = exportUnreal(*f.local, f.frame.m, folder, name, unrealOptions(a));
                if (json v = exportVolume(f.modifier ? f.modifier->id() : 0, f.frame.m, folder, name); !v.is_null())
                    out["volume"] = v;
                if (json water = exportTerrainWater(names[0], folder, name); !water.is_null())
                    out["water"] = water;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainExport: ") + e.what());
            }
            const json roads = ops::execute("terrainRoadExport", {{"args", {names[0]}}, {"folder", folder.string()}, {"name", name}, {"ifAny", true}});
            if (roads.contains("file"))
                (out.contains("files") && out["files"].is_object() ? out["files"]["roads"] : out["roadsFile"]) = roads["file"];
            return out;
        },
        .examples = {"s = terrainExport('Terrain', target='unreal')\ns['resolution'], s['scale'], s['files']['height']"},
    });

    ctx.registerCommand({
        .name = "terrainSendToUnreal",
        .doc = "Send a terrain to the connected Unreal editor as a Landscape, or update the one it sent before.\n\n"
               "Writes the terrain as terrainExport(target='unreal') does, into the link's cache, then has LilaLink build a landscape "
               "of that size and layout at the matching place and scale. Lila's heights go to an edit layer named Lila, so sculpting "
               "and splines on other layers in Unreal survive later sends; a terrain whose size or layout changed is rebuilt. "
               "The landscape takes the terrain's rotation, and Z-up scenes map straight across.\n\n"
               "Masks go as paint layers (weightmaps on the Lila edit layer), each with a layer info asset next to the landscape's "
               "material. Without material=, LilaLink generates one on MF_LilaLandscapeLayer (a function to drop into your own "
               "landscape materials, one call per layer) sampling each layer; with material=, that material is used and needs the "
               "same layer names. Material layers (terrainMaterialRules) go as weight-blended paint layers named after the layer "
               "(Grass, Rock) and, without material=, get a generated material built on MF_LilaTerrainLayer (one call per layer, "
               "with the layer's colour, tile size and roughness); rvt=True adds a runtime virtual texture and its volume so "
               "meshes can blend into the ground. gridSize sets the World Partition grid in a partitioned level. Landscapes over "
               "8193 x 8193 vertices (or regions='on') are written region by region. kind='meshTerrain' sends it as UE 5.8's "
               "experimental Mesh Terrain instead (LilaLinkMeshTerrain plugin). Roads laid with terrainAddRoad go along as "
               "landscape splines (terrainSendRoadsToUnreal) unless roads=False.\n\n"
               "A terrain with a volume split (caves, overhangs, arches) sends its pieces as static meshes placed where they "
               "belong, and its holes onto the landscape's visibility layer. Returns the landscape, its layers, the volume "
               "pieces, the export settings and terrainMaterial (what was made for the material layers).",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = engineSendFlags({{"material", "m", T::String, "", "Landscape material asset path, such as /Game/Terrain/M_Landscape; empty uses the generated one."},
                  {"makeMaterial", "mm", T::Bool, true, "Without material=, generate a landscape material for the layers."},
                  {"layers", "ly", T::Any, nullptr, "Masks to send as paint layers, a list, or {mask: layer name}; default every mask."},
                  {"gridSize", "g", T::Int, int64_t(0), "World Partition grid size in components; 0 leaves it.", {}, {}, 0.0},
                  {"pad", "p", T::Bool, false, "Keep the cell size and grow the landscape past the terrain, instead of resampling."},
                  {"water", "wa", T::Bool, true, "Send the terrain's rivers, lakes and sea as spline and locator actors too (terrainSendWaterToUnreal)."},
                  {"roads", "rd", T::Bool, true, "Also send the terrain's roads as landscape splines."},
                  {"volumeMaterial", "vm", T::String, "", "Material for the volume pieces; empty generates one on MF_LilaTerrainVolume."},
                  {"rvt", "rvt", T::Bool, false, "Material layers: also write a runtime virtual texture, so meshes can blend into the ground."},
                  {"cliffs", "cl", T::Bool, false, "Also send the terrain's cliffs as World Partition tiles (terrainSendCliffs)."},
                  {"cliffFlags", "clf", T::Any, nullptr, "terrainSendCliffs flags for cliffs=True, such as {'cellSize': 12800, 'hlodLayer': ...}."},
                  {"timeout", "to", T::Float, 300.0, "Seconds to wait for Unreal.", {}, {}, 0.0}}),
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainSendToUnreal: give one terrain");
            const Found f = findTerrain(Graph::current(), names[0], "terrainSendToUnreal");
            if (json sent = sendEngineVariant(a, *f.local, f.frame.m, f.mesh ? f.mesh->path() : names[0],
                                              identifier(f.mesh ? f.mesh->name() : std::string("Terrain")), link::cacheDir(),
                                              [&](json& p) { return surfaceLandscapeParams(p, f.modifier, false); });
                !sent.is_null())
                return sent;
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendToUnreal: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            if (!client || !features.is_array() || std::find(features.begin(), features.end(), json("landscape")) == features.end())
                throw CommandError("terrainSendToUnreal: the LilaLink plugin in Unreal predates landscapes; rebuild it from Lila's ue/LilaLink");

            const std::string name = identifier(f.mesh ? f.mesh->name() : std::string("Terrain"));
            json settings, params, surface;
            try
            {
                settings = exportUnreal(*f.local, f.frame.m, link::cacheDir() / "terrain", name, unrealOptions(a));
                LandscapeSend send;
                send.id = f.mesh ? f.mesh->path() : names[0];
                send.label = name;
                send.material = a.get<std::string>("material", "");
                send.makeMaterial = a.get<bool>("makeMaterial", true);
                send.gridSize = int(a.get<int64_t>("gridSize", 0));
                params = landscapeParams(settings, send);
                surface = surfaceLandscapeParams(params, f.modifier, a.get<bool>("rvt", false));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendToUnreal: ") + e.what());
            }
            const int timeoutMs = int(std::clamp(a.get<double>("timeout", 300.0), 1.0, 3600.0) * 1000.0);
            link::RpcResult r = client->call("setLandscape", params, timeoutMs);
            if (!r.ok)
                throw CommandError("terrainSendToUnreal: " + r.error);
            json out = r.result;
            out["settings"] = settings;
            // Before the volume, which puts the holes' visibility mask into the landscape's material.
            if (!surface.is_null())
                out["terrainMaterial"] = sendSurfaceMaterial(client, surface, timeoutMs);
            try
            {
                if (json v = sendVolumeToUnreal(f.modifier ? f.modifier->id() : 0, f.frame.m, settings, f.mesh ? f.mesh->path() : names[0], name,
                                                a.get<std::string>("volumeMaterial", ""));
                    !v.is_null())
                    out["volume"] = v;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendToUnreal: volume: ") + e.what());
            }
            if (a.get<bool>("water", true))
            {
                try
                {
                    if (json water = sendTerrainWater(names[0], a.get<double>("timeout", 300.0)); !water.is_null())
                        out["water"] = water;
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("terrainSendToUnreal: the landscape went, its water didn't: ") + e.what());
                }
            }
            if (a.get<bool>("roads", true))
                try
                {
                    out["roads"] = ops::execute("terrainSendRoadsToUnreal", {{"args", {names[0]}}, {"landscape", name}, {"ifAny", true}});
                }
                catch (const std::exception& e)
                {
                    log::warn("terrainSendToUnreal: the landscape went, its roads did not: {}", e.what());
                    out["roadsError"] = e.what();
                }
            // Inset cliffs open holes without a volume: the visibility layer still has to reach the landscape.
            const std::string visibility = settings.contains("files") ? settings["files"].value("visibility", std::string()) : std::string();
            if (!out.contains("volume") && f.local->findMask("holes") && !visibility.empty() &&
                std::find(features.begin(), features.end(), json("landscapeVolume")) != features.end())
            {
                link::RpcResult hv = client->call("setLandscapeVolume", {{"id", f.mesh ? f.mesh->path() : names[0]}, {"label", name}, {"meshes", json::array()},
                                                                           {"resolution", settings.value("resolution", json::array())}, {"visibility", visibility}},
                                                  timeoutMs);
                if (hv.ok)
                    out["holes"] = hv.result;
                else
                    log::warn("terrainSendToUnreal: the landscape went without its cliff holes: {}", hv.error);
            }
            if (a.get<bool>("cliffs", false))
                try
                {
                    json flags = a.flags.value("cliffFlags", json::object());
                    if (json c = cliffs::sendCliffsWith(names[0], flags); !c.is_null())
                        out["cliffs"] = c;
                }
                catch (const std::exception& e)
                {
                    log::warn("terrainSendToUnreal: the landscape went, its cliffs did not: {}", e.what());
                    out["cliffsError"] = e.what();
                }
            return out;
        },
        .examples = {"terrainSendToUnreal('Terrain', material='/Game/Terrain/M_Landscape')", "terrainSendToUnreal('Terrain', cliffs=True, cliffFlags={'cellSize': 12800})"},
    });
    ctx.registerCommand({
        .name = "terrainErode",
        .doc = "Erode a terrain made by terrainCreate: rivers carve drainage-consistent valleys, slopes over the talus angle shed "
               "scree, and the masks flow, wear, deposit and lakes appear for materials and scatter. Sets the terrain's erosion "
               "inputs (iterations is age: 50 to 150 looks natural) and returns terrainInfo's stats.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"iterations", "i", T::Int, nullptr, "Erosion steps; 0 turns it off. Default 80; with preset= or age= only the preset changes.", {}, {}, 0.0, 10000.0},
                  {"strength", "s", T::Float, nullptr, "How fast rivers cut.", {}, {}, 0.0},
                  {"uplift", "u", T::Float, nullptr, "Rise per step, cm."},
                  {"talusAngle", "ta", T::Float, nullptr, "Slopes steeper than this shed scree, degrees.", {}, {}, 1.0, 89.0},
                  {"device", "d", T::String, nullptr, "cpu (exact, the same bytes everywhere), gpu, or auto (gpu when there is one).", {"cpu", "gpu", "auto"}},
                  {"preset", "p", T::String, nullptr, "Erode preset (natural, alpine, desert, hills): macro form, amplify, debris and scree, rills, deposition.", erodePresetNames()},
                  {"age", "ag", T::Float, nullptr, "Preset age, 0 to 1: 0 turns the preset off, 1 is very old.", {}, {}, 0.0, 2.0},
                  {"seed", "sd", T::Int, nullptr, "Preset seed."},
                  {"hardnessMask", "hm", T::String, nullptr, "Preset: mask of hard rock (1), eroding slower with steeper scree."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainErode: give one terrain");
            Graph& g = Graph::current();
            if (a.has("preset") || a.has("age"))
                return erodePresetCommand(names[0], a);
            const Found f = findTerrain(g, names[0], "terrainErode");
            if (f.modifier->type().attrIndex("erosion") < 0)
                throw CommandError("terrainErode: " + f.modifier->path() + " has no erosion inputs; add an Erode Terrain node to its group");
            if (a.has("device"))
            {
                const std::string device = a.get<std::string>("device", "cpu");
                if (device == "gpu" && !gpuErosionAvailable())
                    throw CommandError("terrainErode: no GPU here (it needs Lila's renderer running); use device='cpu' or 'auto'");
                TerrainGroup t = terrainGroup(g, f, "terrainErode");
                bool set = false;
                for (gn::GNode& n : t.group.nodes)
                    if (n.type == "ErodeTerrain")
                    {
                        n.values["device"] = device;
                        set = true;
                    }
                if (set)
                    commitTerrainGroup(t, "terrainErode");
            }
            const std::string mod = findTerrain(g, pathOf(f, names[0]), "terrainErode").modifier->path();
            ops::execute("setAttr", {{"args", {mod + ".erosion", a.get<int64_t>("iterations", 80)}}});
            if (a.has("strength"))
                ops::execute("setAttr", {{"args", {mod + ".erosionStrength", a.get<double>("strength", 0.0)}}});
            if (a.has("uplift"))
                ops::execute("setAttr", {{"args", {mod + ".uplift", a.get<double>("uplift", 0.0)}}});
            if (a.has("talusAngle"))
                ops::execute("setAttr", {{"args", {mod + ".talusAngle", a.get<double>("talusAngle", 0.0)}}});
            return describe(findTerrain(g, names[0], "terrainErode"));
        },
        .examples = {"terrainErode('Terrain', iterations=120)['masks']['flow']", "terrainErode('Terrain', preset='alpine', age=0.6)['preset']['stepsMs']"},
    });

    ctx.registerCommand({
        .name = "terrainImport",
        .doc = "Make a terrain from a file. Elevation data (GeoTIFF DEM tiles, SRTM .hgt, .asc, .xyz, Terrarium .png with "
               "mode='elevation') is read at its real size, heights in cm from sea level, north toward -Z. A painted image is "
               "read by mode: guide (default) keeps its big shapes and erodes in natural detail, height is literal, uplift grows "
               "ranges where it is bright, mask puts it in a mask on flat ground; it spans size along X.\n\n"
               "The file and erosion settings become inputs on the modifier, like terrainCreate's, so terrainErode and setAttr "
               "work on it. Returns the mesh, modifier, group and terrainInfo's stats.",
        .positionalDoc = "file; relative paths are under the workspace",
        .flags = {{"mode", "m", T::String, "", "elevation, guide, height, uplift, mask or sketch (colour strokes); empty picks elevation for DEM files, else guide."},
                  {"size", "sz", T::Float, 100000.0, "Images: width along X, cm.", {}, {}, 0.0},
                  {"low", "lo", T::Float, 0.0, "Images: height of black, cm."},
                  {"high", "hi", T::Float, 50000.0, "Images: height of white, cm."},
                  {"center", "c", T::Vec3, json::array({0.0, 0.0, 0.0}), "Centre."},
                  {"cellSize", "cs", T::Float, 0.0, "Elevation: grid spacing, cm; 0 keeps the data's own.", {}, {}, 0.0},
                  {"exaggeration", "ex", T::Float, 1.0, "Elevation: multiplies heights.", {}, {}, 0.0},
                  {"fromLowest", "fl", T::Bool, false, "Elevation: lowest point at 0 instead of sea level."},
                  {"format", "f", T::String, "auto", "Elevation: file type (auto, geotiff, hgt, asc, xyz, terrarium, terrainrgb)."},
                  {"erosion", "e", T::Float, 0.5, "Guide and uplift: 0 to 1, how far erosion may move the painting.", {}, {}, 0.0, 1.0},
                  {"name", "n", T::String, "Terrain", "Name of the mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::vector<std::string> pos = a.objects();
            if (pos.size() != 1)
                throw CommandError("terrainImport: give one file");
            fs::path file = pos[0];
            if (file.is_relative())
                if (const fs::path& root = Workspace::get().root(); !root.empty())
                    file = root / file;
            if (!fs::exists(file))
                throw CommandError("terrainImport: no file " + file.string());
            std::string mode = a.get<std::string>("mode", "");
            if (mode.empty())
                mode = looksLikeElevation(file) ? "elevation" : "guide";
            const bool elevation = mode == "elevation";
            ImageMode imageMode = ImageMode::Guide;
            if (!elevation && !parseImageMode(mode, &imageMode))
                throw CommandError("terrainImport: unknown mode '" + mode + "' (elevation, guide, height, uplift, mask, sketch)");

            Graph& g = Graph::current();
            const std::string host = createHost(a.get<std::string>("name", "Terrain"));
            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Terrain");

            json source;
            if (elevation)
                source = {{"name", "Source"},
                          {"type", "ReadElevationData"},
                          {"props", {{"format", a.get<std::string>("format", "auto")}}},
                          {"values",
                           {{"center", localCenter(a.flags["center"])},
                            {"cellSize", a.flags["cellSize"]},
                            {"exaggeration", a.flags["exaggeration"]},
                            {"fromLowest", a.flags["fromLowest"]}}}};
            else
                source = {{"name", "Source"},
                          {"type", "ImportTerrainImage"},
                          {"props", {{"mode", mode}}},
                          {"values",
                           {{"center", localCenter(a.flags["center"])},
                            {"width", a.flags["size"]},
                            {"low", a.flags["low"]},
                            {"high", a.flags["high"]},
                            {"erosion", a.flags["erosion"]}}}};
            json inputs = json::array({{{"name", "file"}, {"type", "string"}, {"default", paths::storedPath(file)}, {"panel", "Source"},
                                        {"description", elevation ? "Elevation file." : "Painted height image."}}});
            json links = json::array({"GroupInput.file -> Source.file", "Source.heightfield -> Erode.heightfield", "Erode.heightfield -> GroupOutput.geometry"});
            const json defaults = {{"erosion", 0}, {"erosionStrength", ErosionParams().strength}, {"uplift", 0.0}, {"talusAngle", ErosionParams().talusAngle}};
            for (const Exposed& e : kInputs)
            {
                if (std::string_view(e.node) != "Erode")
                    continue;
                json in = {{"name", e.name}, {"type", e.type}, {"default", defaults[e.name]}, {"panel", e.panel}, {"description", e.description}, {"min", e.min}};
                if (e.softMax > e.min)
                    in["softMax"] = e.softMax;
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> " + e.node + "." + e.socket);
            }
            json spec = {{"name", group},
                         {"description", elevation ? "A terrain from elevation data." : "A terrain from a painted image."},
                         {"inputs", inputs},
                         {"nodes", json::array({source, {{"name", "Erode"}, {"type", "ErodeTerrain"}}})},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            ops::execute("gnApply", {{"args", {host, group}}});

            json out = describe(findTerrain(g, host, "terrainImport"));
            out["group"] = group;
            out["mode"] = mode;
            return out;
        },
        .examples = {"terrainImport('dem/N46E007.hgt', cellSize=3000)", "terrainImport('paint/island.png', size=400000, high=60000)",
                     "terrainImport('paint/ranges.psd', mode='uplift', erosion=0.8)"},
    });

    registerStampCommands(ctx);
    registerContractCommands(ctx);
    registerLandformCommands(ctx);
}

TerrainRef findTerrainRef(const std::string& node, const std::string& cmd)
{
    const Found f = findTerrain(Graph::current(), node, cmd);
    return {f.local, f.frame.m, identifier(f.mesh ? f.mesh->name() : std::string("Terrain")), f.mesh ? f.mesh->path() : std::string(),
            f.modifier ? f.modifier->path() : std::string()};
}
} // namespace terrain
