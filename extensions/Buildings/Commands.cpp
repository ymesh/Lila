// Buildings commands: create live building nodes from a settlement, curves, envelopes or a mesh, edit and restyle them
// (edits kept by building id, so they survive regeneration), styles, info for agents and W4, LOD and proxies, standalone
// buildings, age, the trim sheet and materials, and picking a building and bay under a ray.

#include "BuildingsScene.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Names.h>
#include <lila/Node.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Slots.h>
#include <lila/Transform.h>
#include <lila/Workspace.h>

#include <lila/Deformer.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <set>

using namespace lila;
using lila::Args;
using lila::CommandError;
using T = lila::ArgType;
namespace fs = std::filesystem;

namespace buildings
{
namespace
{
scene::Graph& G() { return scene::Graph::current(); }

std::string pathOf(const json& r)
{
    if (r.is_string())
        return r.get<std::string>();
    if (r.is_array() && !r.empty() && r[0].is_string())
        return r[0].get<std::string>();
    if (r.is_object() && r.contains("name"))
        return r["name"].get<std::string>();
    return r.dump();
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string safeName(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "B" + out;
    return out;
}

std::string shortName(const std::string& path)
{
    const size_t k = path.find_last_of('|');
    return k == std::string::npos ? path : path.substr(k + 1);
}

void ensureGroup()
{
    const json existing = ops::execute("gnGroups", json::object());
    for (const json& e : existing)
        if ((e.is_string() && e.get<std::string>() == kGroupName) || (e.is_object() && e.value("name", std::string()) == kGroupName))
            return;
    json inputs = json::array({
        {{"name", "spec"}, {"type", "string"}, {"default", "[]"}, {"description", "Buildings as JSON (buildingCreate)."}},
        {{"name", "edits"}, {"type", "string"}, {"default", "{}"}, {"description", "Edits by building id (buildingEdit)."}},
        {{"name", "lod"}, {"type", "string"}, {"default", "near"}, {"description", "near, hlod, far or auto (by camera distance)."}},
        {{"name", "cell"}, {"type", "string"}, {"default", ""}, {"description", "Cell (x_z) or building id."}},
        {{"name", "modules"}, {"type", "bool"}, {"default", true}, {"description", "Draw modules."}},
        {{"name", "realize"}, {"type", "bool"}, {"default", false}, {"description", "Modules flattened into the cell mesh."}},
    });
    json links = json::array();
    for (const char* in : {"spec", "edits", "lod", "cell", "modules", "realize"})
        links.push_back(std::string("GroupInput.") + in + " -> Build." + in);
    links.push_back("Build.geometry -> GroupOutput.geometry");
    ops::execute("gnBuildGroup", {{"args", {json{{"name", kGroupName},
                                                 {"description", "Procedural buildings: shells and modules from specs, with edits kept by id."},
                                                 {"inputs", inputs},
                                                 {"nodes", json::array({{{"name", "Build"}, {"type", kNodeType}}})},
                                                 {"links", links}}}},
                                  {"replace", false}});
}

/** The Buildings modifier on a mesh, or empty. */
std::string buildingsModifier(const std::string& mesh)
{
    scene::Graph& g = G();
    scene::Node* n = g.find(mesh);
    if (!n)
        return {};
    if (lila::gn::isGnModifier(*n))
        if (scene::Node* grp = lila::gn::groupOfModifier(g, *n); grp && grp->name() == kGroupName)
            return n->path();
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (scene::Node* mod = g.node(m); mod && lila::gn::isGnModifier(*mod))
            if (scene::Node* grp = lila::gn::groupOfModifier(g, *mod); grp && grp->name() == kGroupName)
                return mod->path();
    return {};
}

std::string getString(const std::string& plug)
{
    try
    {
        const json v = ops::execute("getAttr", {{"args", {plug}}});
        return v.is_string() ? v.get<std::string>() : std::string();
    }
    catch (const std::exception&)
    {
        return {};
    }
}

void setString(const std::string& plug, const std::string& value) { ops::execute("setAttr", {{"args", {plug, value}}}); }

void markNode(const std::string& node, const char* attr, const std::string& value)
{
    scene::Node* n = G().find(node);
    if (!n)
        return;
    if (n->type().attrIndex(attr) < 0)
        ops::execute("addAttr", {{"args", {node}}, {"longName", attr}, {"dataType", "string"}});
    setString(node + "." + attr, value);
}

/** Every mesh carrying a Buildings modifier: mesh path -> modifier path. */
std::vector<std::pair<std::string, std::string>> buildingMeshes()
{
    std::vector<std::pair<std::string, std::string>> out;
    scene::Graph& g = G();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !lila::gn::isGnModifier(*n))
            continue;
        scene::Node* grp = lila::gn::groupOfModifier(g, *n);
        if (!grp || grp->name() != kGroupName)
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, n->id());
        if (scene::Node* m = mesh != scene::kNoNode ? g.node(mesh) : nullptr)
            out.push_back({m->path(), n->path()});
    }
    return out;
}

struct Located
{
    std::string mesh, modifier;
    std::shared_ptr<const Published> pub;
    int index = -1;
};

/** Where a building is: its mesh, modifier and what it published. Evaluates first so the tables are current. */
std::optional<Located> locate(const std::string& id)
{
    G().evaluate();
    for (auto& [mesh, mod] : buildingMeshes())
    {
        scene::Node* m = G().find(mod);
        if (!m)
            continue;
        std::shared_ptr<const Published> p = published(m->id());
        if (!p)
            continue;
        auto it = p->byId.find(id);
        if (it != p->byId.end())
            return Located{mesh, mod, p, it->second};
    }
    return std::nullopt;
}

std::shared_ptr<const Published> publishedFor(const std::string& modifier)
{
    scene::Node* m = G().find(modifier);
    return m ? published(m->id()) : nullptr;
}

json parseObject(const std::string& text)
{
    if (text.empty())
        return json::object();
    try
    {
        json j = json::parse(text);
        return j.is_object() ? j : json::object();
    }
    catch (const std::exception&)
    {
        return json::object();
    }
}

json parseArray(const std::string& text)
{
    if (text.empty())
        return json::array();
    try
    {
        json j = json::parse(text);
        return j.is_array() ? j : json::array();
    }
    catch (const std::exception&)
    {
        return json::array();
    }
}

std::string cellId(int64_t x, int64_t z) { return std::to_string(x) + "_" + std::to_string(z); }
std::string cellNodeName(const std::string& base, int64_t x, int64_t z)
{
    auto n = [](int64_t v) { return v < 0 ? "n" + std::to_string(-v) : std::to_string(v); };
    return safeName(base + "_" + n(x) + "_" + n(z));
}

/** Corner heights from the ground mesh, by rays straight down. */
struct GroundSampler
{
    std::shared_ptr<const geo::Mesh> mesh;
    geo::MeshBvh bvh;
    bool ok = false;
    explicit GroundSampler(const std::string& node)
    {
        scene::Graph& g = G();
        scene::Node* n = node.empty() ? nullptr : g.find(node);
        if (!n)
            return;
        std::shared_ptr<const geo::Mesh> local = lila::slots::combinedMesh(g, n->id());
        if (!local || local->numFaces() == 0)
            return;
        geo::Mesh world = *local;
        if (n->type().isA("transform"))
        {
            const glm::dmat4 m = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            for (geo::Index p = 0; p < world.pointCapacity(); ++p)
                if (world.pointValid(p))
                    world.setPosition(p, glm::dvec3(m * glm::dvec4(world.position(p), 1.0)));
        }
        mesh = std::make_shared<geo::Mesh>(std::move(world));
        bvh.build(*mesh);
        ok = true;
    }
    std::optional<double> at(Vec2 p) const
    {
        if (!ok)
            return std::nullopt;
        geo::BvhHit hit;
        if (bvh.raycast({p.x, 1e7, p.y}, {0.0, -1.0, 0.0}, hit))
            return 1e7 - hit.t;
        return std::nullopt;
    }
};

void sampleCorners(Spec& s, const GroundSampler& gs)
{
    if (!gs.ok)
        return;
    std::vector<double> hs;
    for (const Vec2& p : s.footprint)
    {
        const std::optional<double> h = gs.at(p);
        if (!h)
            return;
        hs.push_back(*h);
    }
    s.cornerGround = hs;
    s.ground = std::min(s.ground, *std::min_element(hs.begin(), hs.end()));
    s.highGround = std::max(s.highGround, *std::max_element(hs.begin(), hs.end()));
}

/** The settlement's modifier (the one with edits and ground) and its ground mesh. */
std::pair<std::string, std::string> settlementParts(const std::string& settlement)
{
    scene::Graph& g = G();
    scene::Node* n = g.find(settlement);
    if (!n)
        return {};
    auto groundOf = [](scene::Node* mod) -> std::string {
        if (mod->type().attrIndex("ground") >= 0 && mod->type().attrIndex("edits") >= 0)
            return getString(mod->path() + ".ground");
        return "\x01";
    };
    if (lila::gn::isGnModifier(*n))
    {
        const std::string gr = groundOf(n);
        if (gr != "\x01")
            return {n->path(), gr};
    }
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (scene::Node* mod = g.node(m))
        {
            const std::string gr = groundOf(mod);
            if (gr != "\x01")
                return {mod->path(), gr};
        }
    return {};
}

/** A style id for a spec: the forced one, else Settlement's guess and the program mapped onto the library. */
void assignStyle(Spec& s, const std::string& forced)
{
    if (!forced.empty())
    {
        s.styleFrom = "given";
        s.style = forced;
        return;
    }
    const std::string guess = s.style;
    s.style = styleFor(guess, s.program, s.use);
    s.styleFrom = guess.empty() ? "program" : "settlement:" + guess;
}

Ring curveRing(const json& spline)
{
    Ring r;
    for (const json& p : spline.value("points", json::array()))
        if (p.is_array() && p.size() >= 3)
            r.push_back({p[0].get<double>(), p[2].get<double>()});
    return r;
}

/** Specs from closed curves: one building per closed spline; OSM building curves carry heights and roofs per spline. */
std::vector<Spec> specsFromCurve(const std::string& curve, const Args& a, const char* cmd)
{
    std::vector<Spec> out;
    const json data = ops::execute("curveData", {{"args", {curve}}, {"worldSpace", true}});
    if (!data.is_array() || data.empty())
        throw CommandError(std::string(cmd) + ": " + curve + " has no splines");
    const json feats = parseArray(getString(curve + ".osmFeatures"));
    const int floors = int(a.get<int64_t>("floors", 2));
    const double base = a.get<double>("ground", 0.0);
    for (size_t k = 0; k < data.size(); ++k)
    {
        if (!data[k].value("closed", false))
            continue;
        json feat = k < feats.size() && feats[k].is_object() ? feats[k] : json::object();
        if (feat.value("ring", std::string("outer")) == "inner")
            continue;
        Spec s;
        s.source = "curve";
        Ring ring = cleanRing(curveRing(data[k]));
        if (ring.size() < 3)
            continue;
        // Holes: inner rings of this outer one.
        for (size_t h = 0; h < data.size() && h < feats.size(); ++h)
            if (feats[h].is_object() && feats[h].value("ring", std::string()) == "inner" && feats[h].value("outer", -1) == int(k))
                s.holes.push_back(curveRing(data[h]));
        double y = 0.0;
        for (const json& p : data[k]["points"])
            y += p[1].get<double>();
        y /= double(std::max<size_t>(1, data[k]["points"].size()));
        s.footprint = ring;
        s.sides.assign(ring.size(), 0);
        s.ground = a.has("ground") ? base : y;
        s.highGround = s.ground;
        s.floors = feat.contains("levels") ? std::max(1, int(feat["levels"].get<double>())) : floors;
        if (feat.contains("height"))
            s.top = s.ground + feat["height"].get<double>();
        s.roof = feat.value("roofShape", a.get<std::string>("roof", "flat"));
        s.colour = feat.value("colour", std::string());
        s.roofColour = feat.value("roofColour", std::string());
        s.program = feat.value("program", std::string());
        s.osmId = feat.value("id", std::string());
        const std::string idText = s.osmId.empty() ? curve + "/" + std::to_string(k) + "/" + std::to_string(int(centroid(ring).x)) + "_" +
                                                         std::to_string(int(centroid(ring).y))
                                                   : s.osmId;
        s.key = fnv(idText);
        s.id = "building:" + hex64(s.key);
        // The front: the longest edge.
        double best = 0.0;
        for (size_t e = 0; e < ring.size(); ++e)
        {
            const double l = glm::length(ring[(e + 1) % ring.size()] - ring[e]);
            if (l > best)
            {
                best = l;
                s.front = outward(ring[e], ring[(e + 1) % ring.size()]);
                s.ridge = glm::normalize(ring[(e + 1) % ring.size()] - ring[e]);
                s.entrances = {0.5 * (ring[e] + ring[(e + 1) % ring.size()])};
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

/** A spec from a mesh's footprint: its downward faces' outline, else the convex hull of its points seen from above. */
Spec specFromMesh(const std::string& mesh, const Args& a, const char* cmd)
{
    scene::Graph& g = G();
    scene::Node* n = g.find(mesh);
    if (!n)
        throw CommandError(std::string(cmd) + ": no mesh " + mesh);
    std::shared_ptr<const geo::Mesh> m = lila::slots::combinedMesh(g, n->id());
    if (!m || m->numFaces() == 0)
        throw CommandError(std::string(cmd) + ": " + mesh + " has no faces");
    glm::dmat4 world(1.0);
    if (n->type().isA("transform"))
        world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::vector<Vec2> pts;
    double lo = 1e300, hi = -1e300;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
        {
            const glm::dvec3 w = glm::dvec3(world * glm::dvec4(m->position(p), 1.0));
            pts.push_back({w.x, w.z});
            lo = std::min(lo, w.y);
            hi = std::max(hi, w.y);
        }
    // Convex hull (monotone chain): a footprint that never cuts into the mesh.
    std::sort(pts.begin(), pts.end(), [](Vec2 a2, Vec2 b2) { return a2.x < b2.x || (a2.x == b2.x && a2.y < b2.y); });
    std::vector<Vec2> hull;
    auto cross = [](Vec2 o, Vec2 a2, Vec2 b2) { return (a2.x - o.x) * (b2.y - o.y) - (a2.y - o.y) * (b2.x - o.x); };
    for (int pass = 0; pass < 2; ++pass)
    {
        const size_t start = hull.size();
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const Vec2 p = pass == 0 ? pts[k] : pts[pts.size() - 1 - k];
            while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0.0)
                hull.pop_back();
            hull.push_back(p);
        }
        hull.pop_back();
    }
    Spec s;
    s.source = "mesh";
    s.footprint = cleanRing(hull);
    if (s.footprint.size() < 3)
        throw CommandError(std::string(cmd) + ": " + mesh + " is flat seen from above");
    s.sides.assign(s.footprint.size(), 0);
    s.ground = lo;
    s.highGround = lo;
    s.top = a.has("height") ? lo + a.get<double>("height") : hi;
    s.floors = int(a.get<int64_t>("floors", std::max<int64_t>(1, int64_t(std::lround((hi - lo) / 320.0)))));
    s.roof = a.get<std::string>("roof", "flat");
    s.key = fnv("mesh:" + n->path() + "/" + std::to_string(int(lo)));
    s.id = "building:" + hex64(s.key);
    double best = 0.0;
    for (size_t e = 0; e < s.footprint.size(); ++e)
    {
        const double l = glm::length(s.footprint[(e + 1) % s.footprint.size()] - s.footprint[e]);
        if (l > best)
        {
            best = l;
            s.front = outward(s.footprint[e], s.footprint[(e + 1) % s.footprint.size()]);
            s.ridge = glm::normalize(s.footprint[(e + 1) % s.footprint.size()] - s.footprint[e]);
            s.entrances = {0.5 * (s.footprint[e] + s.footprint[(e + 1) % s.footprint.size()])};
        }
    }
    return s;
}

/** Creates the Lila materials for every style used (render, trim, roof per palette entry; base) and the shared module ones. */
int ensureMaterials(const std::set<std::string>& styleIds)
{
    scene::Graph& g = G();
    int made = 0;
    auto make = [&](const std::string& name, glm::vec3 c, double rough, bool instanceColor = true, bool secondTone = false) {
        if (scene::materials::find(g, name))
            return;
        json call = {{"args", {name}}, {"baseColor", {c.r, c.g, c.b}}, {"roughness", rough}, {"instanceColor", instanceColor}};
        if (secondTone)
            call["instanceColor2"] = true;
        if (name == matName(MatGlassClear))
        {
            call["blendMode"] = "translucent";
            call["opacity"] = 0.25;
        }
        ops::execute("createMaterial", call);
        ++made;
    };
    for (const std::string& id : styleIds)
        if (std::shared_ptr<const Style> s = findStyle(id) ? findStyle(id) : nearestStyle(id))
            for (auto& [name, col] : styleMaterials(*s))
                make(name, col, 0.85);
    static const std::pair<int, glm::vec3> kShared[] = {
        {MatRooftop, {0.32f, 0.31f, 0.3f}}, {MatGlass, {0.05f, 0.07f, 0.09f}}, {MatFrame, {0.85f, 0.85f, 0.82f}}, {MatDoor, {0.18f, 0.09f, 0.04f}},
        {MatMetal, {0.25f, 0.26f, 0.27f}},  {MatSign, {0.75f, 0.72f, 0.65f}},  {MatSignFace, {0.9f, 0.9f, 0.88f}}, {MatAwning, {0.6f, 0.1f, 0.08f}},
        {MatConcrete, {0.45f, 0.44f, 0.42f}}, {MatStone, {0.4f, 0.37f, 0.33f}}, {MatWater, {0.03f, 0.25f, 0.35f}}, {MatWood, {0.25f, 0.14f, 0.07f}},
        {MatDark, {0.03f, 0.03f, 0.03f}},     {MatGlassClear, {0.55f, 0.65f, 0.7f}}};
    for (auto& [m, c] : kShared)
        make(matName(m), c, m == MatGlass || m == MatWater || m == MatGlassClear ? 0.1 : m == MatMetal ? 0.5 : 0.8, m == MatFrame, m == MatFrame);
    // Module trim and frames take each module's second tone; with none they keep their own colour.
    make(matName(MatTrim), glm::vec3(0.85f, 0.83f, 0.78f), 0.85, true, true);

    return made;
}

std::set<std::string> stylesIn(const std::vector<Spec>& specs, const json& edits)
{
    std::set<std::string> out;
    for (const Spec& s : specs)
    {
        out.insert(s.style);
        const json e = edits.value(s.id, json::object());
        if (e.contains("style") && e["style"].is_string())
            out.insert(e["style"].get<std::string>());
    }
    return out;
}

/** The trim sheet file shell uvs are laid for: the workspace's copy if there is one, else the shipped file. */
fs::path& shippedTrimSheet()
{
    static fs::path p;
    return p;
}

fs::path trimSheetFile()
{
    const fs::path ws = lila::Workspace::get().isOpen() ? lila::Workspace::get().root() / "TrimSheets" / "LilaBuilding.lilatrim" : fs::path();
    std::error_code ec;
    if (!ws.empty() && fs::exists(ws, ec))
        return ws;
    return shippedTrimSheet();
}

/** Writes one cell (or standalone) node: made when missing, its spec, edits and cell set. Returns the mesh path. */
std::string writeNode(const std::string& group, const std::string& name, const std::string& cell, const json& specs, const json& edits,
                      const std::string& settlement, const std::string& kind)
{
    scene::Graph& g = G();
    std::string mesh;
    const std::string conformed = g.namingPolicy().conform(*scene::types::find("mesh"), name);
    if (scene::Node* existing = g.find(group + "|" + name) ? g.find(group + "|" + name) : g.find(group + "|" + conformed))
        mesh = existing->path();
    else
    {
        mesh = pathOf(ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}}));
        mesh = pathOf(ops::execute("parent", {{"args", {mesh, group}}}));
        if (!g.find(mesh))
            mesh = group + "|" + name;
    }
    std::string mod = buildingsModifier(mesh);
    if (mod.empty())
    {
        mod = pathOf(ops::execute("gnApply", {{"args", {mesh, kGroupName}}}));
        // A group can say what level of detail its new cells start at (a world builds far towns straight to HLOD).
        if (const std::string lod = getString(group + ".buildingsLod"); lod == "near" || lod == "hlod" || lod == "far")
            setString(mod + ".lod", lod);
    }
    setString(mod + ".spec", specs.dump());
    setString(mod + ".edits", edits.dump());
    setString(mod + ".cell", cell);
    markNode(mesh, kMeshMark, kind);
    if (!settlement.empty())
        markNode(mesh, kMeshSettlement, settlement);
    const fs::path trim = trimSheetFile();
    if (!trim.empty())
        markNode(mesh, "trimSheet", trim.generic_string());
    return mesh;
}

struct Existing
{
    json edits = json::object();
    std::map<std::string, json> specs; // by id, for pinned buildings
    std::set<std::string> standalone;
    std::vector<std::string> meshes;
};

Existing existingUnder(const std::string& group)
{
    Existing ex;
    scene::Graph& g = G();
    scene::Node* grp = g.find(group);
    if (!grp)
        return ex;
    for (auto& [mesh, mod] : buildingMeshes())
    {
        scene::Node* m = g.find(mesh);
        if (!m || g.parent(m->id()) != grp->id())
            continue;
        ex.meshes.push_back(mesh);
        const json e = parseObject(getString(mod + ".edits"));
        for (auto& [k, v] : e.items())
            ex.edits[k] = v;
        for (const json& s : parseArray(getString(mod + ".spec")))
            ex.specs[s.value("id", std::string())] = s;
        if (getString(mesh + "." + kMeshMark) == "standalone")
            for (const json& s : parseArray(getString(mod + ".spec")))
                ex.standalone.insert(s.value("id", std::string()));
    }
    const json orphans = parseObject(getString(grp->path() + ".buildingsOrphanEdits"));
    for (auto& [k, v] : orphans.items())
        if (!ex.edits.contains(k))
            ex.edits[k] = v;
    return ex;
}

/** Lays specs into cell nodes (and standalone ones) under a group, keeping edits by id and pinned buildings as they were. */
json layout(std::vector<Spec> specs, const std::string& groupName, const std::string& settlement, double cellSize, bool standaloneAll,
            const char* cmd)
{
    LILA_ZONE();
    const auto t0 = std::chrono::steady_clock::now();
    scene::Graph& g = G();
    ensureGroup();
    std::string group;
    // The workspace's naming rules may have renamed the group this call asked for before (PuertoCentroBuildings).
    if (scene::Node* n = g.find(groupName) ? g.find(groupName) : g.find(g.namingPolicy().conform(*scene::types::find("transform"), safeName(groupName))))
        group = n->path();
    else
        group = pathOf(ops::execute("group", {{"empty", true}, {"name", safeName(groupName)}}));
    Existing ex = existingUnder(group);
    measureClearance(specs);

    // Pinned buildings keep the spec they were pinned with.
    int pinnedKept = 0;
    for (Spec& s : specs)
        if (lila::slots::isPinned("owner:" + s.id))
            if (auto it = ex.specs.find(s.id); it != ex.specs.end())
            {
                s = Spec::fromJson(it->second);
                ++pinnedKept;
            }

    std::map<std::pair<int64_t, int64_t>, json> cells;
    std::map<std::string, json> alone;
    json edited = json::object();
    std::set<std::string> ids;
    for (const Spec& s : specs)
    {
        if (!ids.insert(s.id).second)
            continue;
        const json j = s.toJson();
        if (ex.edits.contains(s.id))
            edited[s.id] = ex.edits[s.id];
        if (standaloneAll || ex.standalone.count(s.id))
        {
            alone[s.id] = json::array({j});
            continue;
        }
        const Vec2 c = centroid(s.footprint);
        const std::pair<int64_t, int64_t> key = cellSize > 0.0 ? std::pair{int64_t(std::floor(c.x / cellSize)), int64_t(std::floor(c.y / cellSize))}
                                                               : std::pair<int64_t, int64_t>{0, 0};
        cells[key].push_back(j);
    }
    const std::string base = safeName(shortName(group));
    std::set<std::string> written;
    json meshes = json::array();
    auto editsFor = [&](const json& list) {
        json e = json::object();
        for (const json& s : list)
            if (edited.contains(s["id"].get<std::string>()))
                e[s["id"].get<std::string>()] = edited[s["id"].get<std::string>()];
        return e;
    };
    for (auto& [key, list] : cells)
    {
        const std::string name = cellSize > 0.0 ? cellNodeName(base, key.first, key.second) : safeName(base + "_All");
        const std::string mesh = writeNode(group, name, cellSize > 0.0 ? cellId(key.first, key.second) : "all", list, editsFor(list), settlement, "cell");
        written.insert(mesh);
        meshes.push_back(mesh);
    }
    for (auto& [id, list] : alone)
    {
        const std::string name = safeName(base + "_" + id.substr(id.find(':') + 1));
        const std::string mesh = writeNode(group, name, id, list, editsFor(list), settlement, "standalone");
        written.insert(mesh);
        meshes.push_back(mesh);
    }
    for (const std::string& m : ex.meshes)
        if (!written.count(m) && g.find(m))
            ops::execute("delete", {{"args", {m}}});
    // Edits of buildings that are gone wait on the group in case they come back.
    json orphans = json::object();
    for (auto& [k, v] : ex.edits.items())
        if (!ids.count(k))
            orphans[k] = v;
    markNode(group, "buildingsOrphanEdits", orphans.dump());
    if (!settlement.empty())
        markNode(group, kMeshSettlement, settlement);

    const int materials = ensureMaterials(stylesIn(specs, edited));
    const auto tEval = std::chrono::steady_clock::now();
    g.evaluate();
    const double evalMs = msSince(tEval);
    size_t slotCount = 0, faces = 0;
    int generated = 0;
    json styles = json::object();
    json warnings = json::array();
    for (const json& m : meshes)
        if (std::shared_ptr<const Published> p = publishedFor(buildingsModifier(m.get<std::string>())))
        {
            slotCount += p->slots;
            faces += p->faces;
            generated += p->generated;
            for (const auto& r : p->results)
                styles[r->style] = styles.value(r->style, 0) + 1;
            for (const std::string& w : p->warnings)
                if (warnings.size() < 40)
                    warnings.push_back(w);
        }
    json out = {{"group", g.find(group) ? g.find(group)->path() : group},
                {"meshes", meshes},
                {"buildings", ids.size()},
                {"cells", cells.size()},
                {"standalone", alone.size()},
                {"styles", styles},
                {"slots", slotCount},
                {"faces", faces},
                {"generated", generated},
                {"materialsMade", materials},
                {"pinnedKept", pinnedKept},
                {"orphanEdits", orphans.size()},
                {"evaluateMs", evalMs},
                {"ms", msSince(t0)}};
    if (!warnings.empty())
        out["warnings"] = warnings;
    // Interiors follow their buildings; their hand-edited rooms stay.
    std::vector<std::string> enterable;
    for (const std::string& id : ids)
        if (edited.contains(id) && edited[id].value("enterable", false))
            enterable.push_back(id);
    if (!enterable.empty())
        out["interiors"] = refreshInteriors(enterable);
    (void)cmd;
    return out;
}

/** Building ids named by positional args (ids or building meshes), flags or the selection. */
std::vector<std::string> targetIds(const Args& a, const char* cmd, bool allowEmpty = false)
{
    std::vector<std::string> ids;
    auto addMesh = [&](const std::string& mesh) {
        const std::string mod = buildingsModifier(mesh);
        if (mod.empty())
            return false;
        for (const json& s : parseArray(getString(mod + ".spec")))
            ids.push_back(s.value("id", std::string()));
        return true;
    };
    std::vector<std::string> named;
    for (const json& p : a.positional)
        if (p.is_string())
            named.push_back(p.get<std::string>());
        else if (p.is_array())
            for (const json& q : p)
                if (q.is_string())
                    named.push_back(q.get<std::string>());
    if (a.has("building"))
    {
        const json& b = a.flags["building"];
        for (const json& q : b.is_array() ? b : json::array({b}))
            if (q.is_string())
                named.push_back(q.get<std::string>());
    }
    for (const std::string& n : named)
    {
        if (n.rfind("building:", 0) == 0)
            ids.push_back(n);
        else if (!addMesh(n))
            throw CommandError(std::string(cmd) + ": " + n + " is neither a building id nor a building mesh");
    }
    if (a.has("polygon"))
    {
        Ring poly;
        for (const json& p : a.flags["polygon"])
            poly.push_back({p[0].get<double>(), p[p.size() - 1].get<double>()});
        if (poly.size() < 3)
            throw CommandError(std::string(cmd) + ": polygon needs three or more points");
        for (auto& [mesh, mod] : buildingMeshes())
            for (const json& s : parseArray(getString(mod + ".spec")))
            {
                const Spec sp = Spec::fromJson(s);
                if (inside(poly, centroid(sp.footprint)))
                    ids.push_back(sp.id);
            }
    }
    if (a.get<bool>("all", false))
        for (auto& [mesh, mod] : buildingMeshes())
            addMesh(mesh);
    if (ids.empty() && named.empty() && !a.has("polygon"))
        for (scene::NodeId id : G().selection())
            if (scene::Node* n = G().node(id))
                addMesh(n->path());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty() && !allowEmpty)
        throw CommandError(std::string(cmd) + ": name buildings (ids like building:1a2b, building meshes, polygon= or all=True)");
    return ids;
}

/** Applies a change to the edits of each building, one setAttr per modifier. Returns {modifier: [ids]}. */
std::map<std::string, std::vector<std::string>> editBuildings(const std::vector<std::string>& ids, const std::function<void(const std::string&, json&)>& change,
                                                               const char* cmd)
{
    std::map<std::string, std::vector<std::string>> byMod;
    std::map<std::string, std::string> modOf;
    // What the nodes published says where each building is; the specs are read only for buildings it doesn't know yet.
    scene::Graph& g = G();
    for (const auto& p : allPublished())
        if (scene::Node* mod = g.node(scene::NodeId(p->modifier)))
            for (const auto& [id, index] : p->byId)
                modOf[id] = mod->path();
    bool scanned = false;
    for (const std::string& id : ids)
    {
        auto it = modOf.find(id);
        if (it == modOf.end() && !scanned)
        {
            scanned = true;
            for (auto& [mesh, mod] : buildingMeshes())
                for (const json& s : parseArray(getString(mod + ".spec")))
                    modOf[s.value("id", std::string())] = mod;
            it = modOf.find(id);
        }
        if (it == modOf.end())
            throw CommandError(std::string(cmd) + ": no building " + id);
        byMod[it->second].push_back(id);
    }
    for (auto& [mod, list] : byMod)
    {
        json edits = parseObject(getString(mod + ".edits"));
        for (const std::string& id : list)
        {
            json e = edits.value(id, json::object());
            change(id, e);
            if (e.empty())
                edits.erase(id);
            else
                edits[id] = e;
        }
        setString(mod + ".edits", edits.dump());
    }
    return byMod;
}

const std::vector<std::string> kBayKinds = {"window", "door", "shopfront", "shopdoor", "wall", "arcade", "garage"};
const std::vector<std::string> kToggles = {"balcony", "awning", "ac", "grille", "shutter"};

json bayJson(const Bay& b)
{
    return {{"path", b.path},
            {"kind", b.kind},
            {"balcony", b.balcony},
            {"floor", b.floor},
            {"corners", {{b.corners[0].x, b.corners[0].y, b.corners[0].z}, {b.corners[1].x, b.corners[1].y, b.corners[1].z},
                         {b.corners[2].x, b.corners[2].y, b.corners[2].z}, {b.corners[3].x, b.corners[3].y, b.corners[3].z}}},
            {"normal", {b.normal.x, b.normal.y, b.normal.z}}};
}

json ringToJson(const Ring& r)
{
    json out = json::array();
    for (const Vec2& p : r)
        out.push_back({p.x, p.y});
    return out;
}

json infoJson(const Spec& s, const Result& r, const json& edit, bool bays, bool modules)
{
    json types = json::object();
    for (const Module& m : r.modules)
        types[m.type] = types.value(m.type, 0) + 1;
    json facades = json::array();
    for (const Facade& f : r.facades)
        facades.push_back({{"path", f.path}, {"side", f.side == 0 ? "street" : f.side == 1 ? "party" : "open"}, {"courtyard", f.courtyard},
                           {"bays", f.bays}, {"a", {f.a.x, f.a.y}}, {"b", {f.b.x, f.b.y}}, {"bottom", f.bottom}, {"top", f.top}});
    json plates = json::array();
    for (const FloorPlate& p : r.plates)
    {
        json holes = json::array();
        for (const Ring& h : p.holes)
            holes.push_back(ringToJson(h));
        plates.push_back({{"tier", p.tier}, {"floor", p.floor}, {"y", p.y}, {"height", p.height}, {"outline", ringToJson(p.outline)}, {"holes", holes}});
    }
    json openings = json::object();
    for (const Opening& o : r.openings)
    {
        const std::string facade = o.bay.substr(0, o.bay.find("/l"));
        openings[facade].push_back({{"bay", o.bay}, {"kind", o.kind}, {"slot", o.slot}, {"origin", {o.origin.x, o.origin.y, o.origin.z}},
                                    {"normal", {o.normal.x, o.normal.y, o.normal.z}}, {"width", o.width}, {"height", o.height}});
    }
    json out = {{"id", s.id},
                {"name", s.name},
                {"program", s.program},
                {"style", r.style},
                {"styleFrom", s.styleFrom},
                {"floors", r.floors},
                {"wallTop", r.wallTop},
                {"palette", r.palette.toJson()},
                {"enterable", r.enterable},
                {"edits", edit},
                {"modules", r.modules.size()},
                {"moduleTypes", types},
                {"facades", facades},
                {"plates", plates},
                {"openings", openings},
                {"core", {{"at", {r.core.x, r.core.y}}, {"size", r.coreSize}}},
                {"bounds", {{r.lo.x, r.lo.y, r.lo.z}, {r.hi.x, r.hi.y, r.hi.z}}},
                {"pinned", lila::slots::isPinned("owner:" + s.id)},
                {"warnings", r.warnings}};
    if (bays)
    {
        json b = json::array();
        for (const Bay& bay : r.bays)
            b.push_back(bayJson(bay));
        out["bays"] = b;
    }
    if (modules)
    {
        json m = json::array();
        for (const Module& mod : r.modules)
            m.push_back({{"slot", lila::slots::makeId(s.id, mod.path)},
                         {"type", mod.type},
                         {"kind", mod.kind},
                         {"origin", {mod.origin.x, mod.origin.y, mod.origin.z}},
                         {"size", {mod.size.x, mod.size.y, mod.size.z}},
                         {"shell", mod.shell},
                         {"data", mod.data}});
        out["moduleList"] = m;
    }
    return out;
}

json pick(Vec3 origin, Vec3 dir)
{
    G().evaluate();
    const std::optional<BayHit> h = pickBay(origin, dir);
    if (!h)
        return json();
    return {{"building", h->building}, {"bay", h->bay}, {"kind", h->kind}, {"balcony", h->balcony}, {"mesh", h->mesh},
            {"point", {h->point.x, h->point.y, h->point.z}}, {"facade", h->bay.substr(0, h->bay.find("/l"))}};
}

json styleSummary(const Style& s)
{
    return {{"id", s.id},          {"label", s.label},           {"description", s.description}, {"source", s.source},
            {"tags", s.tags},      {"floors", {s.minFloors, s.maxFloors}}, {"roof", s.roof},     {"palette", s.toJson()["palette"]}, {"replaces", s.replaces}};
}

void touchAll()
{
    scene::Graph& g = G();
    for (auto& [mesh, mod] : buildingMeshes())
        if (scene::Node* n = g.find(mod))
            gn::rerunModifier(g, n->id());
}

json deepMerge(json base, const json& over)
{
    if (!base.is_object() || !over.is_object())
        return over;
    for (auto& [k, v] : over.items())
        base[k] = base.contains(k) ? deepMerge(base[k], v) : v;
    return base;
}
} // namespace

std::optional<BayHit> pickBay(Vec3 origin, Vec3 dir)
{
    double best = 1e300;
    std::optional<BayHit> hit;
    scene::Graph& g = G();
    for (const auto& p : allPublished())
    {
        scene::Node* mod = g.node(scene::NodeId(p->modifier));
        if (!mod)
            continue;
        const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id());
        scene::Node* mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
        if (!mesh)
            continue;
        for (size_t i = 0; i < p->results.size(); ++i)
        {
            const Result& r = *p->results[i];
            const Vec3 inv = 1.0 / glm::max(glm::abs(dir), Vec3(1e-12)) * glm::sign(dir + Vec3(1e-30));
            double t0 = 0.0, t1 = 1e300;
            for (int k = 0; k < 3; ++k)
            {
                double a = (r.lo[k] - 50.0 - origin[k]) * inv[k], b = (r.hi[k] + 50.0 - origin[k]) * inv[k];
                if (a > b)
                    std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
            }
            if (t0 > t1 || t0 > best)
                continue;
            for (const Bay& b : r.bays)
            {
                const double denom = glm::dot(dir, b.normal);
                if (denom > -1e-9)
                    continue; // only bays facing the ray
                const double t = glm::dot(b.corners[0] - origin, b.normal) / denom;
                if (t <= 0.0 || t >= best)
                    continue;
                const Vec3 q = origin + dir * t;
                const Vec3 ex = b.corners[1] - b.corners[0], ey = b.corners[3] - b.corners[0];
                const double u = glm::dot(q - b.corners[0], ex) / std::max(glm::dot(ex, ex), 1e-9);
                const double v = glm::dot(q - b.corners[0], ey) / std::max(glm::dot(ey, ey), 1e-9);
                if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
                    continue;
                best = t;
                BayHit h;
                h.building = r.id;
                h.name = p->specs[i].name;
                h.bay = b.path;
                h.kind = b.kind;
                h.balcony = b.balcony;
                h.mesh = mesh->path();
                h.point = q;
                for (int c = 0; c < 4; ++c)
                    h.corners[c] = b.corners[c];
                for (const Module& m : r.modules)
                    if (m.path.rfind(b.path + "/", 0) == 0)
                        h.parts.push_back(m.path.substr(b.path.size() + 1));
                hit = h;
            }
        }
    }
    return hit;
}

void setShippedTrimSheet(const fs::path& p) { shippedTrimSheet() = p; }

std::optional<BuildingLoc> locateBuilding(const std::string& id)
{
    auto l = locate(id);
    if (!l)
        return std::nullopt;
    return BuildingLoc{l->mesh, l->modifier, l->pub, l->index};
}

json buildingEditsOf(const std::string& id)
{
    for (const auto& p : allPublished())
        if (p->byId.count(id))
            if (scene::Node* mod = G().node(scene::NodeId(p->modifier)))
                return parseObject(getString(mod->path() + ".edits")).value(id, json::object());
    for (auto& [mesh, mod] : buildingMeshes())
    {
        const json e = parseObject(getString(mod + ".edits"));
        if (e.contains(id))
            return e[id];
    }
    return json::object();
}

void changeBuildingEdits(const std::vector<std::string>& ids, const std::function<void(const std::string&, json&)>& change, const char* cmd)
{
    editBuildings(ids, change, cmd);
}

std::vector<std::string> buildingTargets(const Args& a, const char* cmd, bool allowEmpty) { return targetIds(a, cmd, allowEmpty); }
void markNodeAttr(const std::string& node, const char* attr, const std::string& value) { markNode(node, attr, value); }
std::string nodeAttrString(const std::string& plug) { return getString(plug); }
std::string safeNodeName(const std::string& s) { return safeName(s); }
int ensureBuildingMaterials(const std::set<std::string>& styleIds) { return ensureMaterials(styleIds); }

constexpr const char* kSourceHashAttr = "buildingsSourceHash";
constexpr const char* kSourceCallAttr = "buildingsSourceCall";

/** A settlement's envelopes as a hash, so a group can tell when the settlement it was built from has changed. */
std::string envelopesHash(const json& envs) { return hex64(fnv(envs.value("envelopes", json::array()).dump())); }

struct SourcedGroup
{
    std::string group, settlement, hash;
    json call;
};

/** Groups built from a settlement, with the call that built them. */
std::vector<SourcedGroup> sourcedGroups()
{
    std::vector<SourcedGroup> out;
    scene::Graph& g = G();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || n->type().attrIndex(kSourceCallAttr) < 0)
            continue;
        SourcedGroup s;
        s.group = n->path();
        s.call = parseObject(getString(s.group + "." + kSourceCallAttr));
        s.hash = getString(s.group + "." + kSourceHashAttr);
        s.settlement = s.call.value("settlement", std::string());
        if (!s.settlement.empty())
            out.push_back(std::move(s));
    }
    return out;
}

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "buildingCreate",
        .doc = "Make live procedural buildings: from a settlement's envelopes (settlement=, lots= to pick some), envelopes given as "
               "JSON (envelopes=), closed footprint curves (curve=, one building per closed spline; OSM building curves carry heights "
               "and roofs) or a mesh's footprint (mesh=). Buildings go into one live mesh per Settlement cell (cell=, cm; 0 for one "
               "mesh) under a group, or one mesh each with standalone=True. Every module is a slot with a stable id "
               "(building:<lot>/t<tier>/f<facade>/l<floor>/b<bay>/<kind>); edits and slot overrides are kept by id, so running this "
               "again regenerates without losing them, and pinned buildings stay as they were. Styles come from the Settlement style "
               "guess and program unless style= is given. Returns group, meshes, buildings, cells, styles, slots, timings.",
        .flags = {{"settlement", "st", T::String, nullptr, "Settlement mesh or its modifier: build its envelopes.", {}, {}, {}, {}, "mesh"},
                  {"lots", "lt", T::List, nullptr, "Only these lots (lot ids as Settlement's lotId)."},
                  {"envelopes", "ev", T::List, nullptr, "Envelopes in Settlement's published schema (settlementEnvelopes)."},
                  {"curve", "cv", T::String, nullptr, "Closed footprint curve(s).", {}, {}, {}, {}, "curve"},
                  {"mesh", "ms", T::String, nullptr, "A mesh whose footprint becomes a building.", {}, {}, {}, {}, "mesh"},
                  {"style", "sy", T::String, nullptr, "One style for every building (buildingStyles lists them)."},
                  {"floors", "fl", T::Int, nullptr, "Curve and mesh buildings: floors.", {}, {}, 1.0, 120.0},
                  {"height", "h", T::Float, nullptr, "Mesh buildings: height, cm.", {}, {}, 100.0},
                  {"roof", "rf", T::String, nullptr, "Curve and mesh buildings: flat, hip, gable, dome.", {"flat", "hip", "gable", "dome", "sheet"}},
                  {"ground", "gr", T::Any, nullptr, "Ground mesh sampled under each corner (default the settlement's), or a height for curves."},
                  {"cell", "ce", T::Float, 6400.0, "Cell size, cm: a live mesh per cell (6400 puts four in each of Settlement's World Partition cells); 0 for one mesh.", {}, {}, 0.0},
                  {"standalone", "sa", T::Bool, false, "One mesh per building."},
                  {"name", "n", T::String, nullptr, "Group name (default <settlement>_Buildings, else Buildings)."},
                  {"culture", "cu", T::String, nullptr, "Naming culture for shops nobody named (default the settlement's, else mexican)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "buildingCreate";
            std::vector<Spec> specs;
            std::string settlement, groundNode, settlementMod, sourceHash;
            const std::string forced = a.get<std::string>("style", "");
            if (!forced.empty() && !findStyle(forced))
                throw CommandError(std::string(cmd) + ": no style '" + forced + "' (buildingStyles lists them)");
            if (a.has("ground") && a.flags["ground"].is_string())
                groundNode = a.get<std::string>("ground");
            if (a.has("settlement"))
            {
                if (!ops::find("settlementEnvelopes"))
                    throw CommandError(std::string(cmd) + ": settlement= needs the Settlement extension");
                const std::string sArg = a.get<std::string>("settlement");
                scene::Node* sn = G().find(sArg);
                if (!sn)
                    throw CommandError(std::string(cmd) + ": no settlement " + sArg);
                const auto [smod, sground] = settlementParts(sArg);
                settlementMod = smod;
                if (groundNode.empty())
                    groundNode = sground;
                const scene::NodeId meshId = lila::gn::isGnModifier(*sn) ? scene::modifiers::meshOf(G(), sn->id()) : sn->id();
                settlement = G().node(meshId) ? G().node(meshId)->name() : shortName(sArg);
                const json envs = ops::execute("settlementEnvelopes", {{"args", {sArg}}, {"limit", 0}});
                sourceHash = envelopesHash(envs);
                std::set<uint64_t> lots;
                if (a.has("lots"))
                    for (const json& l : a.flags["lots"])
                        lots.insert(l.is_string() ? std::strtoull(l.get<std::string>().c_str(), nullptr, 10) : l.is_number_unsigned() ? l.get<uint64_t>() : uint64_t(std::llround(l.get<double>())));
                for (const json& e : envs.value("envelopes", json::array()))
                {
                    if (!lots.empty() && !lots.count(e.value("lotId", uint64_t(0))))
                        continue;
                    specs.push_back(Spec::fromEnvelope(e, settlement));
                }
            }
            if (a.has("envelopes"))
                for (const json& e : a.flags["envelopes"])
                    specs.push_back(Spec::fromEnvelope(e, a.get<std::string>("name", "")));
            if (a.has("curve"))
                for (Spec& s : specsFromCurve(a.get<std::string>("curve"), a, cmd))
                    specs.push_back(std::move(s));
            if (a.has("mesh"))
                specs.push_back(specFromMesh(a.get<std::string>("mesh"), a, cmd));
            if (specs.empty() && !a.has("settlement"))
                throw CommandError(std::string(cmd) + ": give settlement=, envelopes=, curve= or mesh=");
            if (!groundNode.empty())
            {
                LILA_ZONE_N("buildings.groundSamples");
                const GroundSampler gs(groundNode);
                for (Spec& s : specs)
                    sampleCorners(s, gs);
            }
            for (Spec& s : specs)
                assignStyle(s, forced);
            // Shops nobody named get a fictional name for their sign, in the settlement's naming culture.
            std::string culture = a.get<std::string>("culture", "");
            std::string cultureFrom = culture.empty() ? "" : "given";
            if (culture.empty() && !settlementMod.empty())
            {
                culture = parseObject(getString(settlementMod + ".edits")).value("naming", json::object()).value("culture", std::string());
                if (!culture.empty())
                    cultureFrom = "settlement";
            }
            if (culture.empty() || !lila::names::hasCulture(culture))
            {
                culture = "mexican";
                cultureFrom = "default";
            }
            int named = 0;
            {
                lila::names::NameBook book(culture, std::make_shared<lila::names::RealNames>(lila::names::sceneRealNames(G())));
                std::vector<Spec*> unnamed;
                for (Spec& s : specs)
                    if (isBusiness(s) && s.businesses.empty() && s.name.empty())
                        unnamed.push_back(&s);
                // Handed out in key order, so a building's name doesn't depend on the order its cells were read.
                std::sort(unnamed.begin(), unnamed.end(), [](const Spec* x, const Spec* y) { return x->key < y->key; });
                for (Spec* s : unnamed)
                {
                    s->name = book.place(lila::names::Kind::Business, s->program.empty() ? "shop" : s->program, s->key);
                    ++named;
                }
            }
            const std::string group = a.get<std::string>("name", settlement.empty() ? std::string("Buildings") : settlement + "_Buildings");
            json out = layout(std::move(specs), group, settlement, a.get<double>("cell", 6400.0), a.get<bool>("standalone", false), cmd);
            if (!sourceHash.empty())
            {
                // What buildingRefresh needs to tell a changed settlement and to build it again the same way.
                json call = {{"settlement", a.get<std::string>("settlement")}, {"cell", a.get<double>("cell", 6400.0)}, {"name", group}};
                for (const char* k : {"lots", "style", "culture", "standalone", "ground"})
                    if (a.has(k))
                        call[k] = a.flags[k];
                markNode(out["group"].get<std::string>(), kSourceHashAttr, sourceHash);
                markNode(out["group"].get<std::string>(), kSourceCallAttr, call.dump());
            }
            if (named > 0)
                out["names"] = {{"generated", named}, {"culture", culture}, {"cultureFrom", cultureFrom}};
            if (!groundNode.empty())
                out["ground"] = groundNode;
            else if (a.has("settlement"))
                out["note"] = "the settlement has no ground mesh: doors stand at each envelope's highest ground, with steps where needed";
            return out;
        },
        .examples = {"buildingCreate(settlement='Town')", "buildingCreate(settlement='Town', style='colonial', cell=6400)",
                     "buildingCreate(curve='Footprints', floors=3, style='residential')", "buildingCreate(mesh='Block_3', style='warehouse')"},
    });

    ctx.registerCommand({
        .name = "buildingRefresh",
        .doc = "Build again the buildings of settlements that changed since their buildings were made (streets, lots, profile, "
               "districts): each group built from a settlement remembers the settlement's envelopes and the call that made it, and is "
               "rebuilt the same way. Edits (bays, floors, styles), slot overrides and pins carry over by building id, as with "
               "buildingCreate. all=True rebuilds every one; dryRun=True only lists the stale groups. Returns refreshed and stale "
               "groups with their settlements, and each rebuild's counts.",
        .positionalDoc = "settlements or building groups (default every one)",
        .flags = {{"all", "a", T::Bool, false, "Rebuild every group, changed or not."},
                  {"dryRun", "dr", T::Bool, false, "List the stale groups without rebuilding."}},
        .undoable = true,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "buildingRefresh";
            std::set<std::string> only;
            for (const json& p : a.positional)
                if (p.is_string())
                {
                    scene::Node* n = G().find(p.get<std::string>());
                    if (!n)
                        throw CommandError(std::string(cmd) + ": no node " + p.get<std::string>());
                    only.insert(n->path());
                    only.insert(n->name());
                }
            json stale = json::array(), refreshed = json::array(), missing = json::array();
            for (const SourcedGroup& s : sourcedGroups())
            {
                scene::Node* sn = G().find(s.settlement);
                if (!only.empty() && !only.count(s.group) && !(sn && (only.count(sn->path()) || only.count(sn->name()))))
                    continue;
                if (!sn)
                {
                    missing.push_back({{"group", s.group}, {"settlement", s.settlement}});
                    continue;
                }
                const std::string now = envelopesHash(ops::execute("settlementEnvelopes", {{"args", {s.settlement}}, {"limit", 0}}));
                const bool changed = now != s.hash;
                if (changed)
                    stale.push_back({{"group", s.group}, {"settlement", s.settlement}});
                if (a.get<bool>("dryRun", false) || (!changed && !a.get<bool>("all", false)))
                    continue;
                const json r = ops::execute("buildingCreate", s.call);
                refreshed.push_back({{"group", s.group}, {"settlement", s.settlement}, {"buildings", r.value("buildings", 0)},
                                     {"generated", r.value("generated", 0)}, {"orphanEdits", r.value("orphanEdits", 0)}, {"changed", changed}});
            }
            json out = {{"stale", stale}, {"refreshed", refreshed}};
            if (!missing.empty())
                out["missingSettlements"] = missing;
            return out;
        },
        .examples = {"buildingRefresh()", "buildingRefresh(dryRun=True)['stale']", "buildingRefresh('Town', all=True)"},
    });

    ctx.registerCommand({
        .name = "buildingEdit",
        .doc = "Edit buildings: floors, bay width, floor height, roof, seed, palette, enterable, bays painted (bays={'t0/f2/l0/b3': "
               "'door'}: window, door, shopfront, shopdoor, wall, arcade, garage), toggles per bay (toggles={'t0/f2/l1/b3': "
               "{'balcony': True}}: balcony, awning, ac, grille, shutter) and bay counts per facade (facadeBays={'t0/f2': 5}). Edits "
               "are kept by building id and survive regeneration; clear=True drops them. Returns the buildings edited and the "
               "evaluation time.",
        .positionalDoc = "building ids or building meshes (default the selection)",
        .flags = {{"building", "b", T::Any, nullptr, "Building id(s)."},
                  {"style", "sy", T::String, nullptr, "Style id."},
                  {"floors", "fl", T::Int, nullptr, "Floors.", {}, {}, 1.0, 120.0},
                  {"bayWidth", "bw", T::Float, nullptr, "Target bay width, cm.", {}, {}, 80.0, 3000.0},
                  {"floorHeight", "fh", T::Float, nullptr, "Upper floor height, cm.", {}, {}, 200.0, 1200.0},
                  {"roof", "rf", T::String, nullptr, "flat, hip, gable, dome, sheet, mansard.", {"flat", "hip", "gable", "dome", "sheet", "mansard"}},
                  {"seed", "s", T::Int, nullptr, "Another seed gives other palette and module choices."},
                  {"enterable", "en", T::Bool, nullptr, "Mark for interiors (W4)."},
                  {"palette", "pl", T::Any, nullptr, "{'render': '#e4572e', 'trim': '#f4f1de', 'roof': '#8b3a2b', 'dirt': 0.3}."},
                  {"bays", "by", T::Any, nullptr, "{bay path: kind}."},
                  {"toggles", "tg", T::Any, nullptr, "{bay path: {balcony|awning|ac|grille|shutter: bool}}."},
                  {"facadeBays", "fb", T::Any, nullptr, "{facade path: bay count}."},
                  {"clearBays", "cb", T::List, nullptr, "Bay paths whose paint and toggles go."},
                  {"clear", "cl", T::Bool, false, "Drop every edit of these buildings first."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "buildingEdit";
            const std::vector<std::string> ids = targetIds(a, cmd);
            if (a.has("style") && !findStyle(a.get<std::string>("style")))
                throw CommandError(std::string(cmd) + ": no style '" + a.get<std::string>("style") + "'");
            if (a.has("bays"))
            {
                if (!a.flags["bays"].is_object())
                    throw CommandError(std::string(cmd) + ": bays is {bay path: kind}");
                for (auto& [k, v] : a.flags["bays"].items())
                    if (!v.is_string() || std::find(kBayKinds.begin(), kBayKinds.end(), v.get<std::string>()) == kBayKinds.end())
                        throw CommandError(std::string(cmd) + ": bay kind for " + k + " is one of window, door, shopfront, shopdoor, wall, arcade, garage");
            }
            if (a.has("toggles"))
            {
                if (!a.flags["toggles"].is_object())
                    throw CommandError(std::string(cmd) + ": toggles is {bay path: {name: bool}}");
                for (auto& [k, v] : a.flags["toggles"].items())
                    for (auto& [t, on] : v.items())
                        if (std::find(kToggles.begin(), kToggles.end(), t) == kToggles.end() || !on.is_boolean())
                            throw CommandError(std::string(cmd) + ": toggle " + t + " is one of balcony, awning, ac, grille, shutter, as a bool");
            }
            editBuildings(ids, [&](const std::string&, json& e) {
                if (a.get<bool>("clear", false))
                    e = json::object();
                for (const char* k : {"style", "floors", "bayWidth", "floorHeight", "roof", "seed", "enterable"})
                    if (a.has(k))
                        e[k] = a.flags[k];
                if (a.has("palette"))
                    e["palette"] = deepMerge(e.value("palette", json::object()), a.flags["palette"]);
                if (a.has("bays"))
                    for (auto& [k, v] : a.flags["bays"].items())
                        e["bays"][k] = v;
                if (a.has("toggles"))
                    for (auto& [k, v] : a.flags["toggles"].items())
                        for (auto& [t, on] : v.items())
                            e["toggles"][k][t] = on;
                if (a.has("facadeBays"))
                    for (auto& [k, v] : a.flags["facadeBays"].items())
                        e["facadeBays"][k] = v;
                if (a.has("clearBays"))
                    for (const json& b : a.flags["clearBays"])
                    {
                        if (e.contains("bays"))
                            e["bays"].erase(b.get<std::string>());
                        if (e.contains("toggles"))
                            e["toggles"].erase(b.get<std::string>());
                    }
                for (const char* k : {"bays", "toggles", "facadeBays"})
                    if (e.contains(k) && e[k].empty())
                        e.erase(k);
            }, cmd);
            const auto t0 = std::chrono::steady_clock::now();
            G().evaluate();
            const double ms = msSince(t0);
            json out = {{"buildings", ids}, {"evaluateMs", ms}};
            if (const json in = refreshInteriors(ids); !in.empty())
                out["interiors"] = in;
            if (ids.size() == 1)
                if (auto loc = locate(ids[0]))
                {
                    out["generated"] = loc->pub->generated;
                    out["nodeMs"] = loc->pub->ms;
                    out["mesh"] = loc->mesh;
                }
            return out;
        },
        .examples = {"buildingEdit('building:1a2b', floors=3)", "buildingEdit('building:1a2b', bays={'t0/f0/l0/b2': 'door'})",
                     "buildingEdit('building:1a2b', toggles={'t0/f0/l1/b1': {'balcony': True}})"},
    });

    ctx.registerCommand({
        .name = "buildingStyle",
        .doc = "Give buildings a style: by id, building mesh, district polygon (polygon=, buildings whose footprint centre is inside) "
               "or all=True. clear=True goes back to the style mapped from Settlement's guess. Kept as an edit by building id. Returns "
               "the count.",
        .positionalDoc = "building ids or building meshes (default the selection)",
        .flags = {{"style", "sy", T::String, nullptr, "Style id (buildingStyles lists them)."},
                  {"building", "b", T::Any, nullptr, "Building id(s)."},
                  {"polygon", "pg", T::List, nullptr, "District [[x, z], ...]."},
                  {"all", "a", T::Bool, false, "Every building."},
                  {"clear", "cl", T::Bool, false, "Drop the style edit."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "buildingStyle";
            const bool clear = a.get<bool>("clear", false);
            const std::string style = a.get<std::string>("style", "");
            if (!clear && style.empty())
                throw CommandError(std::string(cmd) + ": give style= (or clear=True)");
            if (!clear && !findStyle(style))
                throw CommandError(std::string(cmd) + ": no style '" + style + "'");
            const std::vector<std::string> ids = targetIds(a, cmd);
            editBuildings(ids, [&](const std::string&, json& e) {
                if (clear)
                    e.erase("style");
                else
                    e["style"] = style;
            }, cmd);
            if (!clear)
                ensureMaterials({style});
            const auto t0 = std::chrono::steady_clock::now();
            G().evaluate();
            json out = {{"count", ids.size()}, {"style", clear ? json() : json(style)}, {"evaluateMs", msSince(t0)}};
            if (const json in = refreshInteriors(ids); !in.empty())
                out["interiors"] = in;
            return out;
        },
        .examples = {"buildingStyle('Town_Buildings_0_0', style='seafront')", "buildingStyle(polygon=[[0, 0], [8000, 0], [8000, 8000], [0, 8000]], style='colonial')"},
    });

    ctx.registerCommand({
        .name = "buildingStyles",
        .doc = "The building styles: shipped (colonial, seafront, resort tower, modern commercial, residential, mansion, civic, "
               "police station, church, mausoleum, warehouse, packing plant, port shed, hangar, adobe, rural shack, terminal, "
               "lighthouse), the user's and the workspace's (<workspace>/BuildingStyles, where regional styles belong). style= gives one style's full JSON (the format to copy for your own); reload=True "
               "reads the folders again.",
        .flags = {{"style", "sy", T::String, nullptr, "One style's full JSON."}, {"reload", "rl", T::Bool, false, "Read the style folders again."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("reload", false))
            {
                reloadStyles();
                touchAll();
            }
            if (a.has("style"))
            {
                std::shared_ptr<const Style> s = findStyle(a.get<std::string>("style"));
                if (!s)
                    throw CommandError("buildingStyles: no style '" + a.get<std::string>("style") + "'");
                json j = s->toJson();
                j["source"] = s->source;
                return j;
            }
            json out = json::array();
            for (const auto& s : allStyles())
                out.push_back(styleSummary(*s));
            return {{"styles", out}, {"userFolder", userStyleDir().generic_string()}, {"workspaceFolder", workspaceStyleDir().generic_string()}};
        },
        .examples = {"[s['id'] for s in buildingStyles()['styles']]", "buildingStyles(style='colonial')"},
    });

    ctx.registerCommand({
        .name = "buildingStyleSave",
        .doc = "Save a style for everyone in this workspace (scope='workspace') or for you (scope='user'): a copy of base= with spec= "
               "merged over it (any part of the style JSON). Buildings using it regenerate. Returns the file.",
        .flags = {{"id", "i", T::String, nullptr, "Style id."},
                  {"base", "bs", T::String, "residential", "Style to start from."},
                  {"spec", "sp", T::Any, nullptr, "Style JSON to merge over the base."},
                  {"label", "lb", T::String, nullptr, "Menu label."},
                  {"description", "ds", T::String, nullptr, "One line."},
                  {"scope", "sc", T::String, "user", "user or workspace.", {"user", "workspace"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string id = a.get<std::string>("id", "");
            if (id.empty())
                throw CommandError("buildingStyleSave: give id=");
            std::shared_ptr<const Style> base = findStyle(a.get<std::string>("base", "residential"));
            json j = base ? base->toJson() : json::object();
            if (a.has("spec"))
                j = deepMerge(j, a.flags["spec"]);
            j["id"] = id;
            j["label"] = a.get<std::string>("label", j.value("label", id) == (base ? base->label : std::string()) ? id : j.value("label", id));
            if (a.has("description"))
                j["description"] = a.get<std::string>("description");
            std::string err;
            const Style s = Style::fromJson(j, &err);
            if (!err.empty())
                throw CommandError("buildingStyleSave: " + err);
            fs::path f;
            try
            {
                f = saveStyle(s, a.get<std::string>("scope", "user"));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("buildingStyleSave: ") + e.what());
            }
            touchAll();
            return {{"style", id}, {"file", f.generic_string()}};
        },
        .examples = {"buildingStyleSave(id='pinkCentro', base='colonial', spec={'palette': {'render': ['#e8a0b4', '#f2c6d3']}})"},
    });

    ctx.registerCommand({
        .name = "buildingStyleExport",
        .doc = "Write a style's JSON to a file to share it.",
        .flags = {{"style", "sy", T::String, nullptr, "Style id."}, {"file", "f", T::String, nullptr, "Where to write (.json)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::shared_ptr<const Style> s = findStyle(a.get<std::string>("style", ""));
            if (!s)
                throw CommandError("buildingStyleExport: no style '" + a.get<std::string>("style", "") + "'");
            const std::string file = a.get<std::string>("file", "");
            if (file.empty())
                throw CommandError("buildingStyleExport: give file=");
            std::ofstream out(file);
            if (!out)
                throw CommandError("buildingStyleExport: can't write " + file);
            out << s->toJson().dump(2);
            return {{"file", file}};
        },
    });

    ctx.registerCommand({
        .name = "buildingStyleImport",
        .doc = "Add a shared style file to your styles (scope='user') or the workspace's. Returns its id.",
        .flags = {{"file", "f", T::String, nullptr, "A style .json."}, {"scope", "sc", T::String, "user", "user or workspace.", {"user", "workspace"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string file = a.get<std::string>("file", "");
            std::ifstream in(file);
            if (!in)
                throw CommandError("buildingStyleImport: can't read " + file);
            json j;
            try
            {
                j = json::parse(in);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("buildingStyleImport: not JSON: ") + e.what());
            }
            std::string err;
            const Style s = Style::fromJson(j, &err);
            if (!err.empty())
                throw CommandError("buildingStyleImport: " + err);
            const fs::path f = saveStyle(s, a.get<std::string>("scope", "user"));
            touchAll();
            return {{"style", s.id}, {"file", f.generic_string()}};
        },
    });

    ctx.registerCommand({
        .name = "buildingInfo",
        .doc = "A building as the generator made it: style and where it came from, floors, palette, edits, facades (street, party, "
               "open, courtyard), module counts by slot type, floor plates, stair core and openings per facade (what interiors build "
               "on), enterable, pinned. bays=True lists every bay with its corners (what bay paint takes); modules=True every module "
               "with its slot id. Given a building mesh, lists its buildings.",
        .positionalDoc = "building id or building mesh (default the selection)",
        .flags = {{"bays", "by", T::Bool, false, "List bays."}, {"modules", "md", T::Bool, false, "List modules."}, {"limit", "l", T::Int, int64_t(200), "Mesh listing: at most this many.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "buildingInfo";
            std::vector<std::string> named = a.objects();
            if (named.empty())
                throw CommandError(std::string(cmd) + ": name a building id or a building mesh");
            const std::string what = named[0];
            if (what.rfind("building:", 0) == 0)
            {
                auto loc = locate(what);
                if (!loc)
                    throw CommandError(std::string(cmd) + ": no building " + what);
                const json edits = parseObject(getString(loc->modifier + ".edits"));
                json out = infoJson(loc->pub->specs[size_t(loc->index)], *loc->pub->results[size_t(loc->index)], edits.value(what, json::object()),
                                    a.get<bool>("bays", false), a.get<bool>("modules", false));
                out["mesh"] = loc->mesh;
                out["modifier"] = loc->modifier;
                return out;
            }
            const std::string mod = buildingsModifier(what);
            if (mod.empty())
                throw CommandError(std::string(cmd) + ": " + what + " is not a building mesh");
            G().evaluate();
            std::shared_ptr<const Published> p = publishedFor(mod);
            json list = json::array();
            const int64_t limit = a.get<int64_t>("limit", 200);
            if (p)
                for (size_t i = 0; i < p->results.size() && (limit == 0 || int64_t(list.size()) < limit); ++i)
                    list.push_back({{"id", p->specs[i].id}, {"name", p->specs[i].name}, {"style", p->results[i]->style}, {"floors", p->results[i]->floors},
                                    {"program", p->specs[i].program}, {"modules", p->results[i]->modules.size()}});
            json out = {{"mesh", what}, {"modifier", mod}, {"buildings", list}, {"count", p ? p->results.size() : 0}};
            if (p)
            {
                out["cell"] = p->cell;
                out["lod"] = p->lod;
                out["slots"] = p->slots;
                out["faces"] = p->faces;
                out["lastEvaluateMs"] = p->ms;
                out["generated"] = p->generated;
                out["warnings"] = p->warnings;
            }
            return out;
        },
        .examples = {"buildingInfo('building:1a2b', bays=True)", "buildingInfo('Town_Buildings_0_0')"},
    });

    ctx.registerCommand({
        .name = "buildingLod",
        .doc = "What building meshes show: near (shell and modules), hlod (a simplified proxy: tiers and roofs, no openings), far "
               "(flat-roof massing) or auto (near, hlod or far by the mesh's distance from the viewport camera; the distances are "
               "the prefs buildings/lodNear and buildings/lodFar, default 400 m and 2 km). Per mesh, so per cell. Slots stay "
               "published whatever is shown.",
        .positionalDoc = "building meshes (default every one)",
        .flags = {{"lod", "l", T::String, "near", "near, hlod, far or auto.", {"near", "hlod", "far", "auto"}}},
        .run = [](const Args& a) -> json {
            const std::string lod = a.get<std::string>("lod", "near");
            int n = 0;
            std::vector<std::string> meshes = a.positional.empty() ? std::vector<std::string>{} : a.objects();
            if (meshes.empty())
                for (auto& [mesh, mod] : buildingMeshes())
                    meshes.push_back(mesh);
            for (const std::string& m : meshes)
                if (const std::string mod = buildingsModifier(m); !mod.empty())
                {
                    setString(mod + ".lod", lod);
                    ++n;
                }
            return {{"meshes", n}, {"lod", lod}};
        },
    });

    ctx.registerCommand({
        .name = "buildingProxies",
        .doc = "Make the HLOD and far proxies of building meshes as meshes of their own (<mesh>_HLOD, <mesh>_Far, hidden), for "
               "export: Unreal gets them as each cell's HLOD and far levels. Made again on each call.",
        .positionalDoc = "building meshes (default every one)",
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            scene::Graph& g = G();
            g.evaluate();
            std::vector<std::string> meshes = a.positional.empty() ? std::vector<std::string>{} : a.objects();
            if (meshes.empty())
                for (auto& [mesh, mod] : buildingMeshes())
                    meshes.push_back(mesh);
            json made = json::array();
            for (const std::string& m : meshes)
            {
                std::shared_ptr<const Published> p = publishedFor(buildingsModifier(m));
                scene::Node* host = g.find(m);
                if (!p || !host)
                    continue;
                for (const char* lod : {"hlod", "far"})
                {
                    Soup s;
                    std::vector<std::string> names;
                    std::map<std::string, int> idx;
                    for (const auto& r : p->results)
                    {
                        const Soup& src = std::string(lod) == "hlod" ? r->hlod : r->far;
                        for (size_t f = 0, off = 0; f < src.sizes.size(); off += src.sizes[f], ++f)
                        {
                            const std::string name = materialFor(*r, src.mat[f]);
                            auto it = idx.find(name);
                            if (it == idx.end())
                                it = idx.emplace(name, int(names.size())).first, names.push_back(name);
                            s.poly(src.pts.data() + off, src.uvs.data() + off, src.sizes[f], it->second, TrimNone, -1, src.part[f], -1, src.colour[f]);
                        }
                    }
                    const std::string name = host->name() + (std::string(lod) == "hlod" ? "_HLOD" : "_Far");
                    const scene::NodeId parent = g.parent(host->id());
                    // The proxy made last time is refilled, so it keeps the link id its Unreal placement is keyed by.
                    scene::Node* n = parent != scene::kNoNode && g.node(parent) ? g.find(g.node(parent)->path() + "|" + name) : g.find(name);
                    if (n && (!n->type().isA("mesh") || g.parent(n->id()) != parent))
                    {
                        ops::execute("delete", {{"args", {n->path()}}});
                        n = nullptr;
                    }
                    if (!n)
                        n = g.createNode(*scene::types::find("mesh"), name, parent, true);
                    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(toMesh(s, names))));
                    markNode(n->path(), "buildingsProxy", lod);
                    markNode(n->path(), "buildingsCell", p->cell);
                    ops::execute("setAttr", {{"args", {n->path() + ".visibility", false}}});
                    made.push_back({{"mesh", n->path()}, {"lod", lod}, {"faces", s.faces()}});
                }
            }
            return {{"proxies", made}};
        },
    });

    ctx.registerCommand({
        .name = "buildingStandalone",
        .doc = "Move buildings out of their cell mesh into a mesh each (kept standalone when buildingCreate runs again), to weather, "
               "export or hand-edit one on its own. Returns the meshes.",
        .positionalDoc = "building ids (default the selection's buildings)",
        .flags = {{"building", "b", T::Any, nullptr, "Building id(s)."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "buildingStandalone";
            const std::vector<std::string> ids = targetIds(a, cmd);
            json made = json::array();
            for (const std::string& id : ids)
            {
                auto loc = locate(id);
                if (!loc)
                    throw CommandError(std::string(cmd) + ": no building " + id);
                const json spec = parseArray(getString(loc->modifier + ".spec"));
                json edits = parseObject(getString(loc->modifier + ".edits"));
                json keep = json::array(), mine = json::array();
                for (const json& s : spec)
                    (s.value("id", std::string()) == id ? mine : keep).push_back(s);
                if (keep.empty())
                {
                    markNode(loc->mesh, kMeshMark, "standalone");
                    made.push_back(loc->mesh);
                    continue;
                }
                json myEdits = json::object();
                if (edits.contains(id))
                {
                    myEdits[id] = edits[id];
                    edits.erase(id);
                }
                setString(loc->modifier + ".spec", keep.dump());
                setString(loc->modifier + ".edits", edits.dump());
                scene::Node* host = G().find(loc->mesh);
                const scene::NodeId parent = G().parent(host->id());
                const std::string group = parent != scene::kNoNode ? G().node(parent)->path() : std::string();
                const std::string name = safeName(shortName(group.empty() ? std::string("Buildings") : group) + "_" + id.substr(id.find(':') + 1));
                made.push_back(writeNode(group, name, id, mine, myEdits, getString(loc->mesh + "." + kMeshSettlement), "standalone"));
            }
            G().evaluate();
            return {{"meshes", made}};
        },
    });

    ctx.registerCommand({
        .name = "buildingAge",
        .doc = "Age buildings with a Weathering preset (weatherPresets lists them: grunge, worn edges, ivy, debris...): per building "
               "(each made standalone first, so the dressing follows only it) or per district (meshes= of whole cells, or polygon=). "
               "dirt= sets the palette's dirt too (pristine resort 0, run-down centro 0.7). Needs the Weathering extension for the "
               "preset; dirt alone works without it.",
        .positionalDoc = "building ids (default the selection's buildings)",
        .flags = {{"building", "b", T::Any, nullptr, "Building id(s)."},
                  {"polygon", "pg", T::List, nullptr, "District [[x, z], ...]."},
                  {"meshes", "ms", T::List, nullptr, "Whole building meshes (cells) to age as they are."},
                  {"preset", "p", T::String, nullptr, "Weathering preset name."},
                  {"dirt", "dt", T::Float, nullptr, "Palette dirt, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"seed", "s", T::Int, int64_t(1), "Weathering seed."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "buildingAge";
            json out = json::object();
            std::vector<std::string> targets;
            if (a.has("meshes"))
                for (const json& m : a.flags["meshes"])
                    targets.push_back(m.get<std::string>());
            std::vector<std::string> ids;
            if (!a.has("meshes") || a.has("polygon") || a.has("building") || !a.positional.empty())
                ids = targetIds(a, cmd, a.has("meshes"));
            if (a.has("dirt"))
            {
                const double d = a.get<double>("dirt");
                std::vector<std::string> all = ids;
                for (const std::string& m : targets)
                    for (const json& s : parseArray(getString(buildingsModifier(m) + ".spec")))
                        all.push_back(s.value("id", std::string()));
                if (!all.empty())
                    editBuildings(all, [&](const std::string&, json& e) { e["palette"]["dirt"] = d; }, cmd);
                out["dirt"] = d;
            }
            if (a.has("preset"))
            {
                if (!ops::find("weather"))
                    throw CommandError(std::string(cmd) + ": preset= needs the Weathering extension");
                if (!ids.empty())
                {
                    const json made = ops::execute("buildingStandalone", {{"building", ids}});
                    for (const json& m : made["meshes"])
                        targets.push_back(m.get<std::string>());
                }
                json results = json::array();
                for (const std::string& t : targets)
                    results.push_back(ops::execute("weather", {{"args", {t}}, {"preset", a.get<std::string>("preset")}, {"seed", a.get<int64_t>("seed", 1)},
                                                               {"name", safeName(shortName(t) + "_Age")}}));
                out["weather"] = results;
            }
            out["buildings"] = ids.size();
            out["meshes"] = targets;
            return out;
        },
        .examples = {"buildingAge(polygon=[[0, 0], [8000, 0], [8000, 8000], [0, 8000]], dirt=0.7)", "buildingAge('building:1a2b', preset='Grunge')"},
    });

    ctx.registerCommand({
        .name = "buildingTrimSheet",
        .doc = "The LilaBuilding trim sheet shells are laid on: copied into the workspace's TrimSheets folder (the user's trim-sheets "
               "folder when no workspace is open) and baked with Trim Sheets "
               "(needs that extension), then building meshes point at the copy. The strips: Render, Plinth, Cornice, Band, Coping, "
               "Pilaster, RoofTile, Sheet. Returns the file and the bake result.",
        .flags = {{"bake", "b", T::Bool, true, "Bake the sheet's maps."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const fs::path src = shippedTrimSheet();
            std::error_code ec;
            if (src.empty() || !fs::exists(src, ec))
                throw CommandError("buildingTrimSheet: the shipped LilaBuilding.lilatrim is missing from the Buildings resources");
            // Baking writes maps beside the file, so it goes to the workspace, else the user's folder, never the shipped resources.
            const bool inWorkspace = lila::Workspace::get().isOpen();
            const fs::path dst = (inWorkspace ? lila::Workspace::get().root() / "TrimSheets" : lila::PrefStore::get().userDir() / "trim-sheets") /
                                 "LilaBuilding.lilatrim";
            fs::create_directories(dst.parent_path(), ec);
            if (!fs::exists(dst, ec))
                fs::copy_file(src, dst, ec);
            if (ec)
                throw CommandError("buildingTrimSheet: could not copy the sheet to " + dst.generic_string() + " (" + ec.message() + ")");
            json out = {{"file", dst.generic_string()}, {"fileFrom", inWorkspace ? "workspace" : "user folder (no workspace open)"}};
            if (a.get<bool>("bake", true))
            {
                if (!ops::find("trimSheetCreate"))
                    out["note"] = "Trim Sheets is off: the sheet is laid out but not baked";
                else
                    out["bake"] = ops::execute("trimSheetCreate", {{"file", dst.generic_string()}, {"name", "LilaBuilding"}, {"bake", true}});
            }
            for (auto& [mesh, mod] : buildingMeshes())
                markNode(mesh, "trimSheet", dst.generic_string());
            return out;
        },
    });

    ctx.registerCommand({
        .name = "buildingPick",
        .doc = "The building and bay a ray hits (origin=, direction=) or nearest a point (point=, cast straight down from above): "
               "building id, bay path, facade path, bay kind, mesh and the hit point. Empty when nothing is hit.",
        .flags = {{"origin", "o", T::Vec3, nullptr, "Ray origin."}, {"direction", "d", T::Vec3, nullptr, "Ray direction."}, {"point", "p", T::Vec3, nullptr, "A point near a facade."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto vec = [&](const char* f) {
                const json& v = a.flags[f];
                return Vec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
            };
            if (a.has("origin") && a.has("direction"))
            {
                const Vec3 d = vec("direction");
                if (glm::length(d) < 1e-9)
                    throw CommandError("buildingPick: direction is zero");
                return pick(vec("origin"), glm::normalize(d));
            }
            if (a.has("point"))
            {
                // Rays from the point toward each side, nearest bay wins.
                const Vec3 p = vec("point");
                json best;
                double bestD = 1e300;
                for (Vec3 d : {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)})
                {
                    json h = pick(p - d * 2000.0, d);
                    if (h.is_object() && h.contains("point"))
                    {
                        const Vec3 q(h["point"][0].get<double>(), h["point"][1].get<double>(), h["point"][2].get<double>());
                        if (glm::length(q - p) < bestD)
                        {
                            bestD = glm::length(q - p);
                            best = h;
                        }
                    }
                }
                return best.is_null() ? json::object() : best;
            }
            throw CommandError("buildingPick: give origin= and direction=, or point=");
        },
    });

    ctx.registerCommand({
        .name = "buildingMaterials",
        .doc = "Make the Lila materials building meshes use: one per palette colour of each style used (Bld_<style>_render<k>, "
               "_trim<k>, _roof<k>, _base) and the module ones (bld_glass, bld_frame, ...). buildingCreate does this itself. Returns "
               "how many were made.",
        .flags = {{"styles", "sy", T::List, nullptr, "Style ids (default every style in the library)."}},
        .run = [](const Args& a) -> json {
            std::set<std::string> ids;
            if (a.has("styles"))
                for (const json& s : a.flags["styles"])
                    ids.insert(s.get<std::string>());
            else
                for (const auto& s : allStyles())
                    ids.insert(s->id);
            return {{"made", ensureMaterials(ids)}};
        },
    });
}
} // namespace buildings
