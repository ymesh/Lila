// settlementCreate builds a settlement as a geometry-nodes group anyone can open (a Settlement node fed by a mesh holding
// the outline, fixed streets and attractors, and optionally a ground mesh); settlementInfo measures and checks it;
// settlementProfiles lists the styles.

#include "Settlement.h"
#include "Streetscape.h"
#include "Parcels.h"
#include "SettlementTerrain.h"
#include "GenCommands.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Deformer.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <mutex>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using scene::Node;
using T = ArgType;

/** Exposed group inputs, in the order the attribute editor shows them: name, type, panel, min, soft max, description. */
struct Exposed
{
    const char* name;
    const char* type;
    const char* panel;
    double min;
    double softMax;
    const char* description;
};

constexpr Exposed kInputs[] = {
    {"profile", "string", "Style", 0, 0, "hamlet, streetVillage, medievalTown, usTown, mexicanCity, manhattan, europeanCity, modernCity or scifiMegacity."},
    {"pattern", "string", "Style", 0, 0, "Empty for the profile's; else organic, grid, radial, linear, organicGrid, none, tensor, contour, osm or example."},
    {"overrides", "string", "Style", 0, 0, "JSON object of profile fields to change, such as {\"maxFloors\": 6}."},
    {"growth", "float", "Layout", 0, 1, "How far the settlement has grown, 0 to 1."},
    {"seed", "int", "Layout", 0, 0, "Another seed gives another settlement on the same site."},
    {"size", "float", "Layout", 100, 500000, "Side of the square used when the site has no outline, cm."},
    {"heightScale", "float", "Scale", 0, 4, "Multiplies every building's floors."},
    {"widthScale", "float", "Scale", 0.05, 4, "Multiplies every street width."},
    {"lotScale", "float", "Scale", 0.05, 4, "Multiplies lot frontage, depth and building depth."},
    {"blocks", "bool", "Layout", 0, 0, "Blocks and lots between the streets; off for streets and paths only."},
    {"buildings", "bool", "Layout", 0, 0, "A building envelope on each lot."},
    {"walkSlope", "float", "Surface", 0, 89, "Steepest walkable face, degrees."},
    {"walkStep", "float", "Surface", 0, 200, "Height a walk may step between separate pieces, cm."},
    {"edits", "string", "Edits", 0, 0, "Streets, routes, exclusions and road profiles as JSON."},
    {"showStreets", "bool", "Display", 0, 0, "Show street ribbons."},
    {"showLots", "bool", "Display", 0, 0, "Show lot outlines."},
    {"showBuildings", "bool", "Display", 0, 0, "Show building envelopes."},
    {"showGround", "bool", "Display", 0, 0, "Show the ground of blocks and lots: yards, parks, squares."},
    {"generators", "string", "Style", 0, 0, "Tensor fields, eras, districts, walls, imported streets and levels as JSON."},
};

std::vector<FlagDef> layoutFlags()
{
    const Options o;
    return {
        {"profile", "pf", T::String, "medievalTown", "Style.", profileNames()},
        {"pattern", "pt", T::String, "", "Override the profile's pattern: organic, grid, radial, linear, organicGrid, none, tensor, contour, osm, example."},
        {"overrides", "ov", T::String, "", "JSON object of profile fields to change."},
        {"growth", "gr", T::Float, o.growth, "How far the settlement has grown, 0 to 1.", {}, {}, 0.0, 1.0},
        {"seed", "s", T::Int, int64_t(o.seed), "Random seed."},
        {"size", "sz", T::Float, 40000.0, "Side of the square used without an outline, cm.", {}, {}, 0.0},
        {"heightScale", "hs", T::Float, o.heightScale, "Multiplies building floors.", {}, {}, 0.0},
        {"widthScale", "ws", T::Float, o.widthScale, "Multiplies street widths.", {}, {}, 0.0},
        {"lotScale", "ls", T::Float, o.lotScale, "Multiplies lot sizes.", {}, {}, 0.0},
        {"blocks", "bk", T::Bool, true, "Blocks and lots between the streets; off for streets and paths only."},
        {"buildings", "bu", T::Bool, true, "A building envelope on each lot; off for empty lots."},
        {"walkSlope", "wsl", T::Float, o.walk.maxSlope, "Steepest walkable face of the walkable mesh, degrees.", {}, {}, 0.0, 89.0},
        {"walkStep", "wst", T::Float, o.walk.step, "Height a walk may step between separate pieces of it, cm.", {}, {}, 0.0},
        {"edits", "ed", T::String, "", "Streets, routes, exclusions and road profiles as JSON (the edit commands write it)."},
        {"showStreets", "ss", T::Bool, true, "Show street ribbons."},
        {"showLots", "sl", T::Bool, true, "Show lot outlines."},
        {"showBuildings", "sb", T::Bool, true, "Show building envelopes."},
        {"showGround", "sgd", T::Bool, true, "Show the ground of blocks and lots: yards, parks, squares."},
        {"generators", "gen", T::String, "", "Tensor fields, eras, districts, walls, imported streets and levels as JSON."},
    };
}

Options optionsFromJson(const json& values)
{
    auto num = [&](const char* id) { return values.at(id).get<double>(); };
    auto integer = [&](const char* id) { return int64_t(std::llround(values.at(id).get<double>())); };
    auto text = [&](const char* id) { return values.at(id).is_string() ? values.at(id).get<std::string>() : std::string(); };
    auto flag = [&](const char* id) {
        const json& v = values.at(id);
        return v.is_boolean() ? v.get<bool>() : v.is_number() ? v.get<double>() != 0.0 : true;
    };
    try
    {
        return optionsFrom(num, integer, text, flag);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("settlement: ") + e.what());
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
        out = "Settlement" + out;
    return out;
}

glm::dvec3 point3(const json& p, const char* what)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(std::string("settlementCreate: each point in ") + what + " is [x, z] or [x, y, z]");
    if (p.size() == 2)
        return {p[0].get<double>(), 0.0, p[1].get<double>()};
    return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
}

/** What a site mesh holds besides its outline: lines by kind, and loose points. */
struct SiteLines
{
    json streets, roads, water, points;
    double waterWidth = 2000.0;
};

/**
 * The host mesh: the outline as a loop, fixed streets, roads and rivers as lines (edge attributes road and water, rivers
 * with width), the centre and attractors as loose points.
 */
geo::Mesh siteMesh(const json& outline, const SiteLines& lines)
{
    geo::Mesh m;
    std::vector<int32_t> road, water;
    std::vector<float> width;
    auto addLine = [&](const json& pts, bool closed, const char* what, int kind) {
        std::vector<geo::Index> ids;
        for (const json& p : pts)
            ids.push_back(m.addPoint(point3(p, what)));
        auto edge = [&](geo::Index a, geo::Index b) {
            m.addEdge(a, b);
            road.push_back(kind == 1);
            water.push_back(kind == 2);
            width.push_back(kind == 2 ? float(lines.waterWidth) : 0.0f);
        };
        for (size_t k = 0; k + 1 < ids.size(); ++k)
            edge(ids[k], ids[k + 1]);
        if (closed && ids.size() > 2)
            edge(ids.back(), ids.front());
    };
    if (outline.is_array() && outline.size() >= 3)
        addLine(outline, true, "outline", 0);
    const std::pair<const json*, int> kinds[] = {{&lines.streets, 0}, {&lines.roads, 1}, {&lines.water, 2}};
    for (const auto& [list, kind] : kinds)
        if (list->is_array())
            for (const json& s : *list)
                if (s.is_array() && s.size() >= 2)
                    addLine(s, false, kind == 2 ? "water" : kind == 1 ? "roads" : "streets", kind);
    if (lines.points.is_array())
        for (const json& p : lines.points)
            m.addPoint(point3(p, "attractors"));
    m.edgeAttrs().add("road", geo::AttrType::Int);
    m.edgeAttrs().add("water", geo::AttrType::Int);
    m.edgeAttrs().add("width", geo::AttrType::Float);
    std::span<int32_t> r = m.edgeAttrs().get<int32_t>("road"), w = m.edgeAttrs().get<int32_t>("water");
    std::span<float> wd = m.edgeAttrs().get<float>("width");
    for (size_t e = 0; e < road.size() && e < r.size(); ++e)
    {
        r[e] = road[e];
        w[e] = water[e];
        wd[e] = width[e];
    }
    return m;
}

json xzPoints(std::span<const Vec2> pts)
{
    json a = json::array();
    for (const Vec2& q : pts)
        a.push_back(json::array({q.x, q.y}));
    return a;
}

std::vector<Vec2> pointsFromJson(const json& pts, const char* cmd, const char* what)
{
    std::vector<Vec2> out;
    if (!pts.is_array())
        return out;
    for (const json& p : pts)
    {
        if (!p.is_array() || (p.size() != 2 && p.size() != 3))
            throw CommandError(std::string(cmd) + ": each point in " + what + " is [x, z] or [x, y, z]");
        out.push_back({p[0].get<double>(), p[p.size() == 2 ? 1 : 2].get<double>()});
    }
    return out;
}

std::vector<WaterLine> waterFromJson(const json& lines, double width, const char* cmd)
{
    std::vector<WaterLine> out;
    if (lines.is_array())
        for (const json& l : lines)
        {
            WaterLine w;
            w.pts = pointsFromJson(l, cmd, "water");
            w.width = width;
            if (w.pts.size() >= 2)
                out.push_back(std::move(w));
        }
    return out;
}

/** The Settlement modifier in a mesh's stack (the modifier itself may be named). */
Node* settlementModifier(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    auto isSettlement = [](const Node& m) { return m.type().attrIndex("lotScale") >= 0 && m.type().attrIndex("profile") >= 0; };
    if (isSettlement(*n))
        return n;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && isSettlement(*mod))
            return mod;
    throw CommandError(std::string(cmd) + ": " + n->path() + " has no settlement (make one with settlementCreate)");
}

/** The materials the street and ground meshes name by slot, made where the scene has none of that name. */
void ensureSurfaceMaterials(Graph& g)
{
    for (const SurfaceMaterial& s : surfaceMaterials())
        if (!scene::materials::find(g, s.name))
            ops::execute("createMaterial", {{"args", {s.name}}, {"baseColor", {s.color.r, s.color.g, s.color.b}}, {"roughness", s.roughness}});
}

json modifierValues(const Node& mod)
{
    json values = json::object();
    for (const Exposed& e : kInputs)
    {
        // Settlements made before an input existed leave it out; their node takes its default.
        if (mod.type().attrIndex(e.name) < 0)
            continue;
        values[e.name] = ops::execute("getAttr", {{"args", {mod.path() + "." + e.name}}});
    }
    for (const char* extra : {"ground", "walkable"})
        values[extra] = ops::execute("getAttr", {{"args", {mod.path() + "." + extra}}});
    return values;
}

/** A mesh object's evaluated mesh in world space. */
geo::Mesh worldMesh(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": ground '" + name + "' is not a mesh");
    const scene::PlugValue mv = g.value({n->id(), scene::MeshNode::kOutMesh, -1});
    const scene::PlugValue xv = g.value({n->id(), scene::Transform::kWorldMatrix, -1});
    const scene::MeshPtr* mesh = std::get_if<scene::MeshPtr>(&mv);
    if (!mesh || !*mesh || (*mesh)->numFaces() == 0)
        throw CommandError(std::string(cmd) + ": ground '" + name + "' has no faces");
    geo::Mesh world = **mesh;
    if (const glm::dmat4* xf = std::get_if<glm::dmat4>(&xv))
        gn::transformMesh(world, *xf);
    if (world.hasDeleted())
        world.compact();
    return world;
}

Ground groundOf(Graph& g, const std::string& name, double planeY, const char* cmd)
{
    if (name.empty())
        return Ground::plane(planeY);
    return Ground::fromMesh(worldMesh(g, name, cmd), 200.0);
}

std::string textOf(const json& v) { return v.is_string() ? v.get<std::string>() : std::string(); }

/** The region to plan in: the outline given, else the ground's extent, else a square of size. */
Polygon regionOutline(Graph& g, const Args& a, const std::string& groundName, const char* cmd)
{
    std::vector<Vec2> pts = pointsFromJson(a.flags.value("outline", json()), cmd, "outline");
    if (pts.size() < 3 && !groundName.empty())
    {
        const geo::Bounds b = worldMesh(g, groundName, cmd).bounds();
        const Vec2 lo(b.min.x, b.min.z), hi(b.max.x, b.max.z), pad = (hi - lo) * 0.02;
        pts = {lo + pad, Vec2(hi.x - pad.x, lo.y + pad.y), hi - pad, Vec2(lo.x + pad.x, hi.y - pad.y)};
    }
    if (pts.size() < 3)
    {
        const double h = std::max(100.0, a.get<double>("size", 400000.0)) * 0.5;
        pts = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    }
    Polygon p = makePolygon(std::move(pts));
    if (p.pts.empty())
        throw CommandError(std::string(cmd) + ": the outline has no area");
    return p;
}

/**
 * Layouts already rebuilt, by what went into them: a big settlement takes minutes to lay out and the commands that read
 * it (envelopes, names, checks) ask in turn. The meshes it read are held, so their identity can't be reused.
 */
struct RebuildCache
{
    struct Entry
    {
        uint64_t key = 0;
        std::vector<scene::MeshPtr> inputs;
        std::shared_ptr<const Rebuilt> rebuilt;
    };
    std::mutex lock;
    std::vector<Entry> entries;
};
RebuildCache& rebuildCache()
{
    static RebuildCache c;
    return c;
}

/** The evaluated mesh of a node and its world matrix, for the rebuild cache's key. */
scene::MeshPtr evaluatedMesh(Graph& g, const std::string& name, uint64_t& key)
{
    Node* n = name.empty() ? nullptr : g.find(name);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    const scene::PlugValue mv = g.value({n->id(), scene::MeshNode::kOutMesh, -1});
    const scene::PlugValue xv = g.value({n->id(), scene::Transform::kWorldMatrix, -1});
    const scene::MeshPtr* mesh = std::get_if<scene::MeshPtr>(&mv);
    key = (key ^ uint64_t(reinterpret_cast<uintptr_t>(mesh ? mesh->get() : nullptr))) * 1099511628211ull;
    if (const glm::dmat4* xf = std::get_if<glm::dmat4>(&xv))
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                key = (key ^ std::hash<double>{}((*xf)[c][k])) * 1099511628211ull;
    return mesh ? *mesh : nullptr;
}

Rebuilt rebuildUncached(Graph& g, const std::string& name, const char* cmd, const json* editsOverride);

Rebuilt rebuild(Graph& g, const std::string& name, const char* cmd, const json* editsOverride = nullptr)
{
    Node* mod = settlementModifier(g, name, cmd);
    const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id());
    Node* mesh = meshId != scene::kNoNode ? g.node(meshId) : g.find(name);
    const json values = modifierValues(*mod);
    uint64_t key = std::hash<std::string>{}(values.dump() + "|" + (editsOverride ? editsOverride->dump() : std::string()) + "|" + mod->path());
    std::vector<scene::MeshPtr> inputs;
    inputs.push_back(evaluatedMesh(g, textOf(values.at("ground")), key));
    inputs.push_back(evaluatedMesh(g, textOf(values.at("walkable")), key));
    if (auto base = mesh ? scene::modifiers::cage(g, mesh->id()) : nullptr)
    {
        key = (key ^ uint64_t(reinterpret_cast<uintptr_t>(base.get()))) * 1099511628211ull;
        inputs.push_back(std::const_pointer_cast<const geo::Mesh>(base));
    }
    RebuildCache& c = rebuildCache();
    {
        std::lock_guard l(c.lock);
        for (const RebuildCache::Entry& e : c.entries)
            if (e.key == key)
                return *e.rebuilt;
    }
    auto made = std::make_shared<const Rebuilt>(rebuildUncached(g, name, cmd, editsOverride));
    std::lock_guard l(c.lock);
    if (c.entries.size() >= 4)
        c.entries.erase(c.entries.begin());
    c.entries.push_back({key, std::move(inputs), made});
    return *made;
}

Rebuilt rebuildUncached(Graph& g, const std::string& name, const char* cmd, const json* editsOverride)
{
    Node* mod = settlementModifier(g, name, cmd);
    const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id());
    Node* mesh = meshId != scene::kNoNode ? g.node(meshId) : g.find(name);
    Rebuilt r;
    r.values = modifierValues(*mod);
    r.options = optionsFromJson(r.values);
    double planeY = 0.0;
    Site site;
    if (auto base = mesh ? scene::modifiers::cage(g, mesh->id()) : nullptr)
    {
        gn::Geometry geom;
        geom.mesh = base;
        site = siteFrom(geom, &planeY);
    }
    site.size = std::max(100.0, r.values.at("size").get<double>());
    r.ground = groundOf(g, textOf(r.values.at("ground")), planeY, cmd);
    if (const std::string walkable = textOf(r.values.at("walkable")); !walkable.empty())
        r.walk = WalkSurface::fromMesh(worldMesh(g, walkable, cmd), r.options.walk);
    try
    {
        const json edits = editsOverride ? *editsOverride : parseEdits(textOf(r.values.at("edits")));
        applyEdits(site, edits);
        r.layout = buildPlanned(site, r.ground, r.options, &r.walk);
        addFurniturePlacements(r.layout, r.ground, edits);
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    r.mesh = mesh ? mesh->path() : std::string();
    r.modifier = mod->path();
    r.site = std::move(site);
    return r;
}

json parseEditsOr(const std::string& text, const char* cmd)
{
    try
    {
        return parseEdits(text);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** The next free edit id, advanced. */
int nextId(json& edits)
{
    int id = edits.value("next", 1);
    for (const char* list : {"streets", "routes", "exclusions"})
        for (const json& item : edits.value(list, json::array()))
            id = std::max(id, item.value("id", 0) + 1);
    edits["next"] = id + 1;
    return id;
}

int addExclusion(json& edits, std::span<const Vec2> pts, double margin, const std::string& source, const std::string& name)
{
    const int id = nextId(edits);
    json item = {{"id", id}, {"polygon", xzPoints(pts)}, {"margin", margin}};
    if (!source.empty())
        item["source"] = source;
    if (!name.empty())
        item["name"] = name;
    edits["exclusions"].push_back(item);
    return id;
}

/**
 * What an object keeps clear, seen from above: a mesh's footprint (grown by margin), a closed curve's inside (grown), an
 * open curve's corridor (margin each side, at least 1 m).
 */
std::vector<std::vector<Vec2>> objectFootprints(Graph& g, const std::string& name, double margin, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no object named '" + name + "'");
    std::vector<Polygon> polys;
    if (n->type().isA("mesh"))
        polys = meshFootprint(worldMesh(g, name, cmd), margin);
    else if (n->type().isA("curve"))
    {
        g.evaluate();
        std::vector<std::vector<Vec2>> open;
        for (const curve::Spline& s : curve::splinesOf(n->value(curve::CurveShape::kWorldSpace)))
        {
            if (!curve::isValid(s) || s.size() < 2)
                continue;
            const curve::Polyline line = curve::tessellate(s);
            std::vector<Vec2> pts;
            for (const glm::dvec3& p : line.points)
                pts.push_back({p.x, p.z});
            if (line.closed && pts.size() >= 3)
            {
                if (Polygon p = grow(makePolygon(pts), margin); !p.pts.empty())
                    polys.push_back(std::move(p));
            }
            else
                open.push_back(std::move(pts));
        }
        if (!open.empty())
            for (Polygon& p : lineFootprint(open, std::max(margin, 100.0)))
                polys.push_back(std::move(p));
    }
    else
        throw CommandError(std::string(cmd) + ": '" + name + "' is neither a mesh nor a curve");
    if (polys.empty())
        throw CommandError(std::string(cmd) + ": '" + name + "' covers no ground");
    std::vector<std::vector<Vec2>> out;
    for (Polygon& p : polys)
        out.push_back(std::move(p.pts));
    return out;
}

/** A settlement's edits, to change and write back with setEdits. */
struct EditTarget
{
    Node* modifier = nullptr;
    json edits;
};

EditTarget editTarget(Graph& g, const Args& a, const char* cmd)
{
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one settlement");
    EditTarget t;
    t.modifier = settlementModifier(g, names[0], cmd);
    t.edits = parseEditsOr(textOf(ops::execute("getAttr", {{"args", {t.modifier->path() + ".edits"}}})), cmd);
    return t;
}

void setEdits(const EditTarget& t)
{
    ops::execute("setAttr", {{"args", {t.modifier->path() + ".edits", t.edits.dump()}}});
}

/** The result an edit command returns: the edit's outcome in the rebuilt layout, the checks, and the counts. */
json editResult(Graph& g, const EditTarget& t, const std::vector<int>& ids, const char* cmd)
{
    Rebuilt r = rebuild(g, t.modifier->path(), cmd);
    json out = {{"ids", ids}, {"modifier", r.modifier}, {"mesh", r.mesh}};
    json results = json::array();
    for (const json& p : pathResults(r.layout))
        if (std::ranges::find(ids, p.value("id", -1)) != ids.end())
            results.push_back(p);
    out["paths"] = results;
    out["checks"] = checks(r.layout, r.ground, r.options);
    out["stats"] = stats(r.layout);
    return out;
}

/** A street or route edit from the flags (points or stops already read), class, profile and mode checked. */
json pathEdit(const Args& a, int id, const char* pointsKey, const json& points, const char* defaultClass, const char* cmd)
{
    json item = {{"id", id}, {pointsKey, points}};
    const std::string cls = a.get<std::string>("streetClass", defaultClass);
    if (StreetClass c; !parseClass(cls, &c) || c >= StreetClass::Boundary)
        throw CommandError(std::string(cmd) + ": unknown class '" + cls + "' (arterial, collector, local, alley, path)");
    item["class"] = cls;
    for (const char* key : {"profile", "name", "mode"})
        if (a.has(key))
            item[key] = a.get<std::string>(key);
    if (item.contains("mode"))
        if (PathMode m; !parsePathMode(item["mode"].get<std::string>(), &m))
            throw CommandError(std::string(cmd) + ": unknown mode '" + item["mode"].get<std::string>() + "' (auto, ground, surface)");
    if (a.has("width"))
        item["width"] = a.get<double>("width");
    return item;
}

json pointList(const json& pts, const char* cmd, const char* what)
{
    if (!pts.is_array())
        throw CommandError(std::string(cmd) + ": " + what + " is a list of points");
    for (const json& p : pts)
        if (!p.is_array() || (p.size() != 2 && p.size() != 3))
            throw CommandError(std::string(cmd) + ": each point in " + what + " is [x, z] or [x, y, z]");
    return pts;
}

bool hasStreet(const Layout& l, uint64_t id)
{
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)) && l.graph.edges[e].street == id)
            return true;
    return false;
}

/** The streets a layout makes, by stable id, with what they are and where they run. */
json streetsJson(const Rebuilt& r, const std::function<bool(const StreetLine&)>& keep)
{
    std::map<uint64_t, json> byId;
    for (const StreetLine& s : streetLines(r.layout, r.ground))
    {
        if (s.surface || !keep(s))
            continue;
        json& j = byId[s.street];
        if (j.is_null())
            j = {{"id", s.street},
                 {"class", className(s.cls)},
                 {"profile", r.layout.roads[size_t(s.profile)].name},
                 {"width", s.width},
                 {"path", s.path},
                 {"name", streetName(r.layout, s.street)},
                 {"lengthM", 0.0},
                 {"points", json::array()}};
        double len = 0.0;
        for (size_t k = 0; k + 1 < s.pts.size(); ++k)
            len += glm::length(s.pts[k + 1] - s.pts[k]);
        j["lengthM"] = j["lengthM"].get<double>() + len / 100.0;
        for (const glm::dvec3& p : s.pts)
            j["points"].push_back(json::array({p.x, p.y, p.z}));
    }
    json out = json::array();
    for (auto& [id, j] : byId)
        out.push_back(std::move(j));
    return out;
}

json& overrideFor(json& edits, uint64_t id)
{
    for (json& o : edits["streetOverrides"])
        if (o.value("street", uint64_t(0)) == id)
            return o;
    edits["streetOverrides"].push_back({{"street", id}});
    return edits["streetOverrides"].back();
}

std::vector<FlagDef> pathFlags(const char* defaultClass)
{
    return {{"streetClass", "c", T::String, defaultClass, "Street class.", {"arterial", "collector", "local", "alley", "path"}},
            {"profile", "pf", T::String, nullptr, "Road profile (cross-section): settlementRoadProfiles lists them. Default: the class's."},
            {"width", "w", T::Float, nullptr, "Width in cm, over the profile's.", {}, {}, 0.0},
            {"mode", "md", T::String, nullptr, "auto (default): stops with a height on the walkable mesh go there, the rest on the "
                                               "ground; ground; or surface.", {"auto", "ground", "surface"}},
            {"name", "n", T::String, nullptr, "A name to find it by."}};
}
/** The siting rules from profile=, kind= and rules= (in that order). */
SitingRules sitingRulesFrom(const Args& a, const char* cmd)
{
    Profile p;
    if (const std::string name = a.get<std::string>("profile", "medievalTown"); !builtinProfile(name, &p))
        throw CommandError(std::string(cmd) + ": unknown profile '" + name + "'");
    SitingRules r = p.siting;
    try
    {
        if (const std::string kind = a.get<std::string>("kind", ""); !kind.empty())
            applyJson(r, json{{"kind", kind}});
        if (a.has("rules") && !a.flags["rules"].is_null())
            applyJson(r, a.flags["rules"]);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    return r;
}
} // namespace

Rebuilt rebuildSettlement(scene::Graph& g, const std::string& name, const char* cmd, const json* edits) { return rebuild(g, name, cmd, edits); }

/** The site's outline from settlementCreate's flags, for generators that need it before the settlement exists. */
static std::vector<Vec2> generatorRegion(const Args& a)
{
    std::vector<Vec2> pts = pointsFromJson(a.flags.value("outline", json()), "settlementCreate", "outline");
    if (pts.size() >= 3)
        return pts;
    const double h = std::max(100.0, a.get<double>("size", 40000.0)) * 0.5;
    Vec2 c(0.0);
    if (const json at = a.flags.value("attractors", json()); at.is_array() && !at.empty())
        c = pointsFromJson(json::array({at[0]}), "settlementCreate", "attractors").front();
    return {c + Vec2(-h, -h), c + Vec2(h, -h), c + Vec2(h, h), c + Vec2(-h, h)};
}

void registerCommands(ext::Context& ctx)
{
    // Layouts are keyed by the meshes they read and hold them (gigabytes of terrain): a replaced scene's can never match again.
    for (const lila::events::Event e : {lila::events::Event::SceneNew, lila::events::Event::SceneOpened})
        lila::events::subscribe(
            e,
            [](const lila::events::EventData&) {
                RebuildCache& c = rebuildCache();
                std::lock_guard l(c.lock);
                c.entries.clear();
            },
            "Settlement");
    std::vector<FlagDef> flags = {
        {"outline", "o", T::List, nullptr, "Closed outline, [[x, z], ...] or [[x, y, z], ...] in cm. Omit for a square of size."},
        {"streets", "st", T::List, nullptr, "Streets to keep as they are: a list of polylines."},
        {"roads", "rd", T::List, nullptr, "Main roads arriving from outside, kept as arterials: a list of polylines."},
        {"water", "wa", T::List, nullptr, "Rivers and canals as centrelines: a list of polylines. Only arterials bridge them."},
        {"waterWidth", "ww", T::Float, 2000.0, "Width of the water lines, cm.", {}, {}, 0.0},
        {"attractors", "at", T::List, nullptr, "Points: the first is the centre, the rest are places main roads run to (bridge, harbour, gate)."},
        nodeFlag({"ground", "gd", T::String, "", "Mesh the settlement stands on (world space). Empty for flat ground."}, "mesh"),
        nodeFlag({"walkable", "wk", T::String, "", "Mesh routes may walk on: a structure's ramps, terraces and floors (world space)."}, "mesh"),
        {"exclusions", "ex", T::List, nullptr, "Regions kept clear: a list of closed polygons [[x, z], ...]."},
        {"exclude", "xo", T::List, nullptr, "Objects kept clear: meshes by their footprint seen from above, curves (closed: their "
                                            "inside; open: a corridor along them)."},
        {"exclusionMargin", "xm", T::Float, 0.0, "Clearance around exclusions, cm.", {}, {}, 0.0},
        {"name", "n", T::String, "Settlement", "Name of the mesh made to hold the site."},
        {"terrainWater", "tw", T::Bool, true, "On a Terrain ground with no water of its own: the terrain's rivers crossing the outline become the settlement's water."},
        {"buildingRoute", "br", T::String, "outline", "How buildings are made once laid out (settlementBuildings route=); buildings makes procedural ones.",
         {"outline", "massing", "library", "masonry", "unreal", "buildings"}},
        {"buildingStyle", "bs", T::String, nullptr, "buildings route: one Buildings style for every building; empty picks per envelope."},
    };
    for (FlagDef& f : layoutFlags())
        flags.push_back(std::move(f));
    for (FlagDef& f : generatorFlags())
        flags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "settlementCreate",
        .doc = "Lay out a settlement: arterial roads routed over the ground from the centre to gates and attractors, streets "
               "grown, gridded or ringed by the profile, blocks between them, lots along the streets and a building envelope "
               "on each lot (footprint, floors, height, roof: the space a building may fill; buildings are made elsewhere).\n\n"
               "Give outline= (and optionally streets=, attractors=), or a mesh whose loose-edge loop is the outline, open "
               "loose-edge lines fixed streets and loose points the centre and attractors. The settlement is a geometry-nodes "
               "modifier around a Settlement node; every layout flag becomes a keyable input on it, so change it later with "
               "setAttr on '<modifier>.<flag>' and check it with settlementInfo. Returns the mesh, modifier, group, stats, "
               "metrics and checks. Lengths are cm, Y up.",
        .positionalDoc = "mesh holding the site (optional)",
        .flags = flags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            json values = json::object();
            for (const Exposed& e : kInputs)
                values[e.name] = a.flags[e.name];
            const std::string groundName = a.get<std::string>("ground", ""), walkName = a.get<std::string>("walkable", "");
            resolveGeneratorFlags(g, a, values, generatorRegion(a), groundName, "settlementCreate");
            const Options o = optionsFromJson(values);
            for (const std::string* n : {&groundName, &walkName})
                if (!n->empty())
                    if (Node* node = g.find(*n); !node || !node->type().isA("mesh"))
                        throw CommandError("settlementCreate: '" + *n + "' is not a mesh");
            {
                json edits = parseEditsOr(textOf(values["edits"]), "settlementCreate");
                const double margin = std::max(0.0, a.get<double>("exclusionMargin", 0.0));
                for (const json& poly : a.flags.value("exclusions", json::array()))
                    addExclusion(edits, pointsFromJson(poly, "settlementCreate", "exclusions"), margin, "", "");
                for (const json& obj : a.flags.value("exclude", json::array()))
                    for (const std::vector<Vec2>& pts : objectFootprints(g, textOf(obj), margin, "settlementCreate"))
                        addExclusion(edits, pts, 0.0, textOf(obj), "");
                values["edits"] = edits.empty() ? std::string() : edits.dump();
            }

            std::string host;
            if (!a.positional.empty())
            {
                Node* mesh = g.find(a.positional[0].get<std::string>());
                if (!mesh || !mesh->type().isA("mesh"))
                    throw CommandError("settlementCreate: " + a.positional[0].dump() + " is not a mesh");
                host = mesh->path();
            }
            else
            {
                json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()},
                                                        {"name", a.get<std::string>("name", "Settlement")}});
                host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                Node* mesh = g.find(host);
                SiteLines lines{a.flags.value("streets", json()), a.flags.value("roads", json()), a.flags.value("water", json()),
                                a.flags.value("attractors", json()), a.get<double>("waterWidth", 2000.0)};
                g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1},
                           scene::MeshPtr(std::make_shared<geo::Mesh>(siteMesh(a.flags.value("outline", json()), lines))));
            }

            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Settlement");
            json inputs = json::array();
            json nodes = json::array({{{"name", "Settle"}, {"type", "Settlement"}}});
            json links = json::array({"GroupInput.geometry -> Settle.site", "Settle.geometry -> GroupOutput.geometry"});
            for (const Exposed& e : kInputs)
            {
                json in = {{"name", e.name}, {"type", e.type}, {"default", values[e.name]}, {"panel", e.panel}, {"description", e.description}};
                if (std::string_view(e.type) == "float" || std::string_view(e.type) == "int")
                {
                    in["min"] = e.min;
                    if (e.softMax > e.min)
                        in["softMax"] = e.softMax;
                }
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> Settle." + e.name);
            }
            // The ground's and walkable mesh's names ride along as inputs of their own, so commands rebuild on the same ones.
            inputs.push_back({{"name", "ground"}, {"type", "string"}, {"default", groundName}, {"panel", "Layout"},
                              {"description", "Mesh the settlement stands on (settlementSurfaces sets it)."}});
            inputs.push_back({{"name", "walkable"}, {"type", "string"}, {"default", walkName}, {"panel", "Surface"},
                              {"description", "Mesh routes may walk on (settlementSurfaces sets it)."}});
            nodes.push_back({{"name", "Ground"}, {"type", "ObjectInfo"}, {"props", {{"object", groundName}, {"transformSpace", "world"}}}});
            nodes.push_back({{"name", "Walkable"}, {"type", "ObjectInfo"}, {"props", {{"object", walkName}, {"transformSpace", "world"}}}});
            links.push_back("Ground.geometry -> Settle.ground");
            links.push_back("Walkable.geometry -> Settle.walkable");
            json spec = {{"name", group},
                         {"description", "Lays out streets, lots and building envelopes inside the mesh's outline."},
                         {"inputs", inputs},
                         {"nodes", nodes},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            json mod = ops::execute("gnApply", {{"args", {host, group}}});
            // A group type registered before under this name keeps its first defaults, so every input is set here.
            const std::string modName = mod.is_string() ? mod.get<std::string>() : mod.dump();
            for (const Exposed& e : kInputs)
                ops::execute("setAttr", {{"args", {modName + "." + e.name, values[e.name]}}});
            ops::execute("setAttr", {{"args", {modName + ".ground", groundName}}});
            ops::execute("setAttr", {{"args", {modName + ".walkable", walkName}}});

            const json onTerrain = followTerrain(g, modName, groundName, a.get<bool>("terrainWater", true), "settlementCreate");
            ensureSurfaceMaterials(g);
            Rebuilt r = rebuild(g, host, "settlementCreate");
            json out = stats(r.layout);
            if (!onTerrain.is_null())
                out["terrain"] = onTerrain;
            out["metrics"] = metrics(r.layout);
            out["checks"] = checks(r.layout, r.ground, r.options);
            out["mesh"] = host;
            out["modifier"] = mod;
            out["group"] = group;
            if (const std::string route = a.get<std::string>("buildingRoute", "outline"); route != "outline")
            {
                json call = {{"args", {host}}, {"route", route}};
                if (a.has("buildingStyle") && !a.get<std::string>("buildingStyle").empty())
                    call["style"] = a.get<std::string>("buildingStyle");
                out["buildingRoute"] = ops::execute("settlementBuildings", call);
            }
            return out;
        },
        .examples = {"settlementCreate(profile='medievalTown', size=60000)",
                     "settlementCreate(size=200000, exclude=['Temple'], exclusionMargin=2000, pattern='none', blocks=False)",
                     "town = settlementCreate(outline=[[-40000, -30000], [45000, -25000], [38000, 35000], [-35000, 30000]],\n"
                     "                        attractors=[[0, 0], [40000, 0]], ground='Terrain', profile='europeanCity')\n"
                     "setAttr(town['modifier'] + '.overrides', '{\"maxFloors\": 9}')\n"
                     "settlementInfo(town['mesh'])['checks']",
                     "settlementCreate(profile='medievalTown', size=200000, eras='medievalToModern')",
                     "settlementCreate(profile='modernCity', size=150000, tensor={'fields': [{'kind': 'grid', 'angle': 20}, "
                     "{'kind': 'boundary', 'follow': 'water', 'weight': 2}]}, water=[[[-80000, 0], [80000, 20000]]])"},
    });

    ctx.registerCommand({
        .name = "settlementInfo",
        .doc = "Measures and checks a settlement.\n\n"
               "Returns stats (street metres by class, blocks, lots by use, buildings, floors), metrics comparable with real "
               "cities (street length and density, orientation entropy and order after Boeing 2019, intersections, dead-end "
               "ratio, degree histogram, block areas), checks (components: separate street networks, crossings: streets that "
               "cross without a junction, steepStreets, sharpJunctions, landlockedLots, badEnvelopes, ok), the current "
               "settings and the resolved profile.",
        .positionalDoc = "mesh or its settlement modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("settlementInfo: give one mesh");
            Rebuilt r = rebuild(g, names[0], "settlementInfo");
            json out = stats(r.layout);
            out["metrics"] = metrics(r.layout);
            out["checks"] = checks(r.layout, r.ground, r.options);
            out["settings"] = r.values;
            out["resolvedProfile"] = toJson(r.layout.profile);
            out["mesh"] = r.mesh;
            out["modifier"] = r.modifier;
            out["plan"] = r.layout.plan;
            return out;
        },
        .examples = {"settlementInfo('Settlement')['metrics']['orientationOrder']"},
    });

    ctx.registerCommand({
        .name = "settlementProfiles",
        .doc = "The built-in styles with every field, as {name: profile}. Any field can be changed per settlement through "
               "the overrides input (a JSON object of the fields to change).",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::object();
            for (const std::string& n : profileNames())
            {
                Profile p;
                builtinProfile(n, &p);
                out[n] = toJson(p);
            }
            return out;
        },
        .examples = {"settlementProfiles()['manhattan']['maxFloors']"},
    });

    ctx.registerCommand({
        .name = "settlementSuggest",
        .doc = "Where settlements would go: the best sites inside an outline, best first, no two closer than spacing. Each "
               "site has its position, score and reasons: reasons.terms scores every siting term (flat, water, trade, defense, "
               "resource, climate, flood) with its value, weight, contribution and why (\"ford on river 2, 340 m away\"), from "
               "the kind's preset (town, market, port, mining, fort, farming, mill) and any rules= weights. On a Terrain "
               "ground the terms come from the terrain's analysis (terrainSample fields, terrainWaterInfo crossings and "
               "harbours, terrainSites resources, terrainBuildable land, terrainRegionStats over the best sites); on a mesh or "
               "flat ground from its slope, height and the water= lines given. ground says which was used. Changes nothing.",
        .flags = {{"outline", "o", T::List, nullptr, "Region outline, [[x, z], ...] in cm. Omit for the ground's extent, else a square of size."},
                  {"size", "sz", T::Float, 400000.0, "Side of the square used without an outline or ground, cm.", {}, {}, 0.0},
                  nodeFlag({"ground", "gd", T::String, "", "Ground mesh or Terrain (world space). Empty for flat ground."}, "mesh"),
                  {"water", "wa", T::List, nullptr, "Rivers as centrelines: a list of polylines (a Terrain brings its own)."},
                  {"waterWidth", "ww", T::Float, 2000.0, "Width of the rivers, cm.", {}, {}, 0.0},
                  {"count", "c", T::Int, int64_t(5), "How many sites.", {}, {}, 1.0},
                  {"spacing", "sp", T::Float, 0.0, "Least distance between sites, cm; 0 picks one from the area and count.", {}, {}, 0.0},
                  {"kind", "k", T::String, "", "Kind of place, setting the term weights; empty for the profile's (town).", [] {
                       std::vector<std::string> k{""};
                       for (const std::string& n : sitingKinds())
                           k.push_back(n);
                       return k;
                   }()},
                  {"profile", "pf", T::String, "medievalTown", "Profile whose siting rules start the weights.", profileNames()},
                  {"rules", "ru", T::Any, nullptr, "Term weights over the kind's: {water: 0.6, defense: 0.3, ...}."},
                  {"riverArea", "ra", T::Float, nullptr, "Terrain: drainage area, m2, past which water is a river."},
                  {"seaLevel", "sea", T::Float, nullptr, "Terrain: sea level along the up axis, cm."},
                  {"seed", "s", T::Int, int64_t(1), "Breaks ties between equal sites."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const char* cmd = "settlementSuggest";
            const std::string groundName = a.get<std::string>("ground", "");
            const Polygon outline = regionOutline(g, a, groundName, cmd);
            const Ground ground = groundOf(g, groundName, 0.0, cmd);
            const std::vector<WaterLine> water = waterFromJson(a.flags.value("water", json()), a.get<double>("waterWidth", 2000.0), cmd);
            SiteOptions o;
            o.count = int(std::max<int64_t>(1, a.get<int64_t>("count", 5)));
            o.spacing = std::max(0.0, a.get<double>("spacing", 0.0));
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            o.rules = sitingRulesFrom(a, cmd);
            const bool terrain = !groundName.empty() && isTerrainGround(groundName);
            json analysis = json::object();
            for (const char* k : {"riverArea", "seaLevel"})
                if (a.has(k))
                    analysis[k] = a.flags[k];
            const std::vector<SiteScore> sites = terrain ? suggestOnTerrain(groundName, outline, ground, o, analysis) : suggestSites(outline, ground, water, o);
            const char* used = terrain ? "terrain" : groundName.empty() ? "plane" : "mesh";
            json out = json::array();
            for (const SiteScore& s : sites)
                out.push_back({{"position", json::array({s.p.x, ground.height(s.p), s.p.y})},
                               {"score", s.score},
                               {"reasons", s.reasons},
                               {"kind", o.rules.kind},
                               {"ground", used}});
            return out;
        },
        .examples = {"settlementSuggest(ground='Terrain', kind='market', count=3)",
                     "settlementSuggest(ground='Terrain', kind='fort', rules={'defense': 0.9})[0]['reasons']['terms']['defense']",
                     "settlementSuggest(ground='Hills', water=[[[-200000, -150000], [0, 0], [180000, 160000]]], count=3)"},
    });

    ctx.registerCommand({
        .name = "settlementRegion",
        .doc = "Settles a whole map: cities on the best sites (scored as settlementSuggest scores them, for kind=), towns "
               "between them and villages filling in, spaced as central places; roads routed between neighbours over the ground, "
               "bridging rivers where they must; then each settlement laid out with settlementCreate, its main roads continuing "
               "the regional ones. Makes a Region mesh holding the roads and rivers as lines, plus one settlement mesh per place. "
               "On a Terrain ground the sites come from its analysis, the roads from terrainRoute and, with no water= given, the "
               "rivers from terrainWaterInfo; each settlement keeps its site record for settlementExplain id='site'. Returns "
               "each settlement (name, tier, profile, centre, radius, site score and reasons, lots, buildings, checks), the road "
               "count, the bridges, which ground was used and roadNotes (pairs the terrain router could not join).",
        .flags = {{"outline", "o", T::List, nullptr, "Region outline, [[x, z], ...] in cm. Omit for the ground's extent, else a square of size."},
                  {"size", "sz", T::Float, 400000.0, "Side of the square used without an outline or ground, cm.", {}, {}, 0.0},
                  nodeFlag({"ground", "gd", T::String, "", "Ground mesh (world space). Empty for flat ground."}, "mesh"),
                  {"water", "wa", T::List, nullptr, "Rivers as centrelines: a list of polylines."},
                  {"waterWidth", "ww", T::Float, 2000.0, "Width of the rivers, cm.", {}, {}, 0.0},
                  {"cities", "ci", T::Int, int64_t(1), "How many cities.", {}, {}, 0.0},
                  {"towns", "to", T::Int, int64_t(3), "How many towns.", {}, {}, 0.0},
                  {"villages", "vi", T::Int, int64_t(8), "How many villages.", {}, {}, 0.0},
                  {"cityProfile", "cp", T::String, "europeanCity", "Profile for cities.", profileNames()},
                  {"townProfile", "tp", T::String, "medievalTown", "Profile for towns.", profileNames()},
                  {"villageProfile", "vp", T::String, "hamlet", "Profile for villages.", profileNames()},
                  {"cityRadius", "cr", T::Float, 120000.0, "Largest city radius, cm.", {}, {}, 0.0},
                  {"townRadius", "tr", T::Float, 45000.0, "Largest town radius, cm.", {}, {}, 0.0},
                  {"villageRadius", "vr", T::Float, 15000.0, "Largest village radius, cm.", {}, {}, 0.0},
                  {"seed", "s", T::Int, int64_t(1), "Random seed."},
                  {"name", "n", T::String, "Region", "Name of the mesh holding the roads and rivers."},
                  {"kind", "k", T::String, "", "Kind of place the sites are scored for (settlementSuggest); empty for town.", [] {
                       std::vector<std::string> k{""};
                       for (const std::string& n : sitingKinds())
                           k.push_back(n);
                       return k;
                   }()},
                  {"rules", "ru", T::Any, nullptr, "Term weights over the kind's: {water: 0.6, defense: 0.3, ...}."},
                  {"riverArea", "ra", T::Float, nullptr, "Terrain: drainage area, m2, past which water is a river."},
                  {"seaLevel", "sea", T::Float, nullptr, "Terrain: sea level along the up axis, cm."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const char* cmd = "settlementRegion";
            const std::string groundName = a.get<std::string>("ground", "");
            const Polygon outline = regionOutline(g, a, groundName, cmd);
            const Ground ground = groundOf(g, groundName, 0.0, cmd);
            const double waterWidth = a.get<double>("waterWidth", 2000.0);
            std::vector<WaterLine> water = waterFromJson(a.flags.value("water", json()), waterWidth, cmd);
            const bool terrain = !groundName.empty() && isTerrainGround(groundName);
            json analysis = json::object();
            for (const char* k : {"riverArea", "seaLevel"})
                if (a.has(k))
                    analysis[k] = a.flags[k];
            if (terrain && water.empty())
                water = terrainRivers(groundName, analysis);
            RegionOptions o;
            o.cities = int(std::max<int64_t>(0, a.get<int64_t>("cities", 1)));
            o.towns = int(std::max<int64_t>(0, a.get<int64_t>("towns", 3)));
            o.villages = int(std::max<int64_t>(0, a.get<int64_t>("villages", 8)));
            o.cityProfile = a.get<std::string>("cityProfile", "europeanCity");
            o.townProfile = a.get<std::string>("townProfile", "medievalTown");
            o.villageProfile = a.get<std::string>("villageProfile", "hamlet");
            o.cityRadius = a.get<double>("cityRadius", o.cityRadius);
            o.townRadius = a.get<double>("townRadius", o.townRadius);
            o.villageRadius = a.get<double>("villageRadius", o.villageRadius);
            o.seed = uint32_t(a.get<int64_t>("seed", 1));
            for (const std::string* prof : {&o.cityProfile, &o.townProfile, &o.villageProfile})
                if (Profile p; !builtinProfile(*prof, &p))
                    throw CommandError(std::string(cmd) + ": unknown profile '" + *prof + "'");
            SiteOptions so;
            so.count = std::max(1, o.cities + o.towns + o.villages) * 12;
            so.spacing = std::sqrt(area(outline) / std::max(1, o.cities + o.towns + o.villages)) * 0.3;
            so.seed = o.seed;
            so.rules = sitingRulesFrom(a, cmd);
            const std::vector<SiteScore> candidates = terrain ? suggestOnTerrain(groundName, outline, ground, so, analysis) : suggestSites(outline, ground, water, so);
            RegionPlan plan = planRegion(outline, ground, water, o, &candidates);
            json roadNotes = json::array();
            if (terrain)
            {
                // Roads between settlements follow the terrain's router; a pair it cannot join keeps Settlement's own road.
                for (std::vector<Vec2>& r : plan.roads)
                {
                    std::string note;
                    std::vector<Vec2> way = terrainRoad(groundName, r.front(), r.back(), 0.1, &note);
                    if (way.size() >= 2)
                        r = std::move(way);
                    else
                        roadNotes.push_back("kept Settlement's own road: " + note);
                }
                plan.bridges = roadBridges(plan.roads, water);
                for (RegionSettlement& rs : plan.settlements)
                    rs.site.roads = plan.roads;
            }

            // The lines lie on the ground, a point every 10 m so they don't cut through the hills between their corners.
            auto onGround = [&](std::span<const Vec2> pts) {
                json out = json::array();
                for (size_t k = 0; k < pts.size(); ++k)
                {
                    const int steps = k + 1 < pts.size() ? std::max(1, int(std::ceil(glm::length(pts[k + 1] - pts[k]) / 1000.0))) : 1;
                    for (int t = 0; t < steps; ++t)
                    {
                        const Vec2 q = k + 1 < pts.size() ? glm::mix(pts[k], pts[k + 1], double(t) / steps) : pts[k];
                        out.push_back(json::array({q.x, ground.surfaceHeight(q) + 30.0, q.y}));
                    }
                }
                return out;
            };
            json roadLines = json::array(), waterLines = json::array();
            for (const std::vector<Vec2>& r : plan.roads)
                roadLines.push_back(onGround(r));
            for (const WaterLine& w : water)
                waterLines.push_back(onGround(w.pts));
            json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", a.get<std::string>("name", "Region")}});
            const std::string regionMesh = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            SiteLines lines{json::array(), roadLines, waterLines, json::array(), waterWidth};
            g.setValue({g.find(regionMesh)->id(), scene::MeshNode::kInMesh, -1},
                       scene::MeshPtr(std::make_shared<geo::Mesh>(siteMesh(json(), lines))));

            json settlements = json::array();
            int counts[3] = {};
            for (size_t i = 0; i < plan.settlements.size(); ++i)
            {
                const RegionSettlement& s = plan.settlements[i];
                const Vec2 c = s.score.p;
                // Only the roads and rivers that reach the settlement are stored with it.
                auto reaches = [&](std::span<const Vec2> pts) {
                    for (const Vec2& q : pts)
                        if (glm::length(q - c) < s.radius * 1.5)
                            return true;
                    for (size_t k = 0; k + 1 < pts.size(); ++k)
                    {
                        const Vec2 d = pts[k + 1] - pts[k];
                        const double t = std::clamp(glm::dot(c - pts[k], d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
                        if (glm::length(pts[k] + d * t - c) < s.radius * 1.2)
                            return true;
                    }
                    return false;
                };
                json roads = json::array(), rivers = json::array();
                for (const std::vector<Vec2>& r : s.site.roads)
                    if (reaches(r))
                        roads.push_back(xzPoints(r));
                for (const WaterLine& w : s.site.water)
                    if (reaches(w.pts))
                        rivers.push_back(xzPoints(w.pts));
                const char* tier = tierName(s.tier);
                const std::string name = std::string(1, char(std::toupper(static_cast<unsigned char>(tier[0])))) + (tier + 1) +
                                         std::to_string(++counts[int(s.tier)]);
                json r = ops::execute("settlementCreate", {{"outline", xzPoints(s.site.outline)},
                                                           {"attractors", json::array({json::array({c.x, c.y})})},
                                                           {"roads", roads},
                                                           {"water", rivers},
                                                           {"waterWidth", waterWidth},
                                                           {"ground", groundName},
                                                           {"profile", s.profile},
                                                           {"seed", int64_t(o.seed) * 1000 + int64_t(i)},
                                                           {"name", name}});
                if (const std::string modName = textOf(r.value("modifier", json())); !modName.empty())
                {
                    json edits = parseEditsOr(textOf(ops::execute("getAttr", {{"args", {modName + ".edits"}}})), cmd);
                    edits["siting"] = {{"kind", so.rules.kind},
                                       {"score", s.score.score},
                                       {"reasons", s.score.reasons},
                                       {"position", json::array({c.x, ground.height(c), c.y})},
                                       {"tier", tier},
                                       {"ground", terrain ? "terrain" : groundName.empty() ? "plane" : "mesh"}};
                    ops::execute("setAttr", {{"args", {modName + ".edits", edits.dump()}}});
                }
                settlements.push_back({{"name", r.value("mesh", name)},
                                       {"tier", tier},
                                       {"profile", s.profile},
                                       {"center", json::array({c.x, ground.height(c), c.y})},
                                       {"radiusM", s.radius / 100.0},
                                       {"score", s.score.score},
                                       {"reasons", s.score.reasons},
                                       {"modifier", r.value("modifier", json())},
                                       {"lots", r.value("lots", json())},
                                       {"buildings", r.value("buildings", json())},
                                       {"bridges", r.value("bridges", json())},
                                       {"checks", r.value("checks", json())}});
            }
            json bridges = json::array();
            for (const Vec2& b : plan.bridges)
                bridges.push_back(json::array({b.x, ground.height(b), b.y}));
            return {{"region", regionMesh},
                    {"settlements", settlements},
                    {"roads", int64_t(plan.roads.size())},
                    {"bridges", bridges},
                    {"ground", terrain ? "terrain" : groundName.empty() ? "plane" : "mesh"},
                    {"roadNotes", roadNotes}};
        },
        .examples = {"settlementRegion(ground='Terrain', water=[[[-200000, -150000], [0, 0], [180000, 160000]]], cities=1, towns=2, villages=6)"},
    });

    std::vector<FlagDef> streetFlags = {{"points", "p", T::List, nullptr, "The street's points, [[x, z], ...] or [[x, y, z], ...] in cm."}};
    for (FlagDef& f : pathFlags("collector"))
        streetFlags.push_back(std::move(f));
    ctx.registerCommand({
        .name = "settlementAddStreet",
        .doc = "Adds a street the way it is drawn: its points are kept, and generated streets fill in around it. Points with "
               "heights that all lie on the walkable mesh (ramps, terraces, floors) make a path on it; otherwise it lies on the "
               "ground, cut where it enters an exclusion or leaves the outline. Returns the edit id, how it came out (paths), "
               "the checks and the counts. Undo removes it; it saves with the scene.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = streetFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementAddStreet";
            Graph& g = Graph::current();
            EditTarget t = editTarget(g, a, cmd);
            const json pts = pointList(a.flags.value("points", json()), cmd, "points");
            if (pts.size() < 2)
                throw CommandError(std::string(cmd) + ": points= needs two or more points");
            const int id = nextId(t.edits);
            t.edits["streets"].push_back(pathEdit(a, id, "points", pts, "collector", cmd));
            setEdits(t);
            return editResult(g, t, {id}, cmd);
        },
        .examples = {"settlementAddStreet('Settlement', points=[[-20000, 0], [0, 5000], [20000, 0]], profile='avenue')"},
    });

    std::vector<FlagDef> routeFlags = {
        {"start", "st", T::List, nullptr, "Where the route starts, [x, z] on the ground or [x, y, z] (on the walkable mesh when it lies there)."},
        {"end", "en", T::List, nullptr, "Where it ends."},
        {"via", "v", T::List, nullptr, "Stops on the way, in order: a list of points."}};
    for (FlagDef& f : pathFlags("local"))
        routeFlags.push_back(std::move(f));
    ctx.registerCommand({
        .name = "settlementRoute",
        .doc = "Routes a street or path between points: each leg the cheapest way over the ground (around exclusions, along "
               "the slope, across rivers only where it must) or across the walkable mesh, from floor to ramp to terrace; a leg "
               "from one to the other leaves the mesh where it meets the ground nearest the way on. The route is re-solved "
               "whenever the ground, the walkable mesh or the exclusions change. Returns the edit id, its ground and surface "
               "lengths (or why it failed), the checks and the counts. Undo removes it; it saves with the scene.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = routeFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementRoute";
            Graph& g = Graph::current();
            EditTarget t = editTarget(g, a, cmd);
            json stops = json::array();
            stops.push_back(a.flags.value("start", json()));
            for (const json& v : a.flags.value("via", json::array()))
                stops.push_back(v);
            stops.push_back(a.flags.value("end", json()));
            pointList(stops, cmd, "start, via and end");
            const int id = nextId(t.edits);
            t.edits["routes"].push_back(pathEdit(a, id, "stops", stops, "local", cmd));
            setEdits(t);
            return editResult(g, t, {id}, cmd);
        },
        .examples = {"settlementRoute('Settlement', start=[-90000, -90000], end=[12000, 4500, 3000], streetClass='path', profile='footpath')",
                     "settlementRoute('Settlement', start=[-90000, 0], via=[[0, 60000]], end=[90000, 0], profile='serviceRoad')"},
    });

    std::vector<FlagDef> editFlags = {
        {"id", "i", T::Int, nullptr, "The street or route to change: an edit id, or a generated street's stable id."},
        {"points", "p", T::List, nullptr, "New points (a street) or stops (a route)."}};
    for (FlagDef& f : pathFlags("local"))
    {
        f.defaultValue = nullptr;
        editFlags.push_back(std::move(f));
    }
    ctx.registerCommand({
        .name = "settlementEditStreet",
        .doc = "Changes a street or route added by settlementAddStreet or settlementRoute: its points (or stops), class, road "
               "profile, width, mode or name; or a generated street by its stable id (settlementStreets lists them): its class, "
               "profile, width or name (the name holds along the whole street through its junctions), kept through re-runs. Only the flags given change. Returns the outcome as settlementRoute does.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = editFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementEditStreet";
            Graph& g = Graph::current();
            EditTarget t = editTarget(g, a, cmd);
            if (!a.has("id"))
                throw CommandError(std::string(cmd) + ": id= required");
            const int64_t rawId = a.get<int64_t>("id", -1);
            if (rawId > 0 && uint64_t(rawId) >= kStreetIdBit)
            {
                // A generated street: the change rides on its stable id and is applied each time the layout makes it.
                const uint64_t street = uint64_t(rawId);
                if (!hasStreet(rebuild(g, t.modifier->path(), cmd).layout, street))
                    throw CommandError(std::string(cmd) + ": no street with id " + std::to_string(street) + " (settlementStreets lists them)");
                if (a.has("points"))
                    throw CommandError(std::string(cmd) + ": a generated street runs where the layout puts it; delete it and add a street instead");
                json& o = overrideFor(t.edits, street);
                if (a.has("streetClass"))
                {
                    const std::string cls = a.get<std::string>("streetClass");
                    if (StreetClass c; !parseClass(cls, &c) || c >= StreetClass::Boundary)
                        throw CommandError(std::string(cmd) + ": unknown class '" + cls + "'");
                    o["class"] = cls;
                }
                if (a.has("profile"))
                    o["profile"] = a.get<std::string>("profile");
                if (a.has("width"))
                    o["width"] = a.get<double>("width");
                if (a.has("name"))
                    o["name"] = a.get<std::string>("name");
                setEdits(t);
                const Rebuilt r = rebuild(g, t.modifier->path(), cmd);
                json out = {{"ids", json::array({street})}, {"modifier", r.modifier}, {"mesh", r.mesh}};
                out["streets"] = streetsJson(r, [&](const StreetLine& s) { return s.street == street; });
                out["checks"] = checks(r.layout, r.ground, r.options);
                out["stats"] = stats(r.layout);
                return out;
            }
            const int id = int(rawId);
            json* item = nullptr;
            const char* pointsKey = "points";
            for (const char* list : {"streets", "routes"})
                for (json& s : t.edits[list])
                    if (s.value("id", -1) == id)
                    {
                        item = &s;
                        pointsKey = std::string_view(list) == "routes" ? "stops" : "points";
                    }
            if (!item)
                throw CommandError(std::string(cmd) + ": no street or route with id " + std::to_string(id));
            if (a.has("points"))
            {
                const json pts = pointList(a.flags["points"], cmd, "points");
                if (pts.size() < 2)
                    throw CommandError(std::string(cmd) + ": points= needs two or more points");
                (*item)[pointsKey] = pts;
            }
            if (a.has("streetClass"))
            {
                const std::string cls = a.get<std::string>("streetClass");
                if (StreetClass c; !parseClass(cls, &c) || c >= StreetClass::Boundary)
                    throw CommandError(std::string(cmd) + ": unknown class '" + cls + "'");
                (*item)["class"] = cls;
            }
            for (const char* key : {"profile", "name", "mode"})
                if (a.has(key))
                    (*item)[key] = a.get<std::string>(key);
            if (item->contains("mode"))
                if (PathMode m; !parsePathMode((*item)["mode"].get<std::string>(), &m))
                    throw CommandError(std::string(cmd) + ": unknown mode '" + (*item)["mode"].get<std::string>() + "'");
            if (a.has("width"))
                (*item)["width"] = a.get<double>("width");
            setEdits(t);
            return editResult(g, t, {id}, cmd);
        },
        .examples = {"settlementEditStreet('Settlement', id=3, profile='highway', streetClass='arterial')"},
    });

    ctx.registerCommand({
        .name = "settlementAddExclusion",
        .doc = "Keeps a region clear of everything the settlement lays out (streets, blocks, lots, buildings, routes, growth): "
               "a polygon, or objects (a mesh by its footprint seen from above, a closed curve by its inside, an open curve as a "
               "corridor along it), with a clearance margin. Objects are measured now; run it again after moving them. Returns "
               "the edit ids, the checks and the counts. Undo removes them; they save with the scene.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"polygon", "pg", T::List, nullptr, "A closed polygon, [[x, z], ...] in cm."},
                  nodeFlag({"objects", "ob", T::List, nullptr, "Meshes or curves to keep clear, by name."}, "mesh|curve"),
                  {"margin", "m", T::Float, 0.0, "Clearance around it, cm.", {}, {}, 0.0},
                  {"name", "n", T::String, nullptr, "A name to find it by."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementAddExclusion";
            Graph& g = Graph::current();
            EditTarget t = editTarget(g, a, cmd);
            const double margin = std::max(0.0, a.get<double>("margin", 0.0));
            const std::string name = a.get<std::string>("name", "");
            std::vector<int> ids;
            if (a.has("polygon"))
            {
                const std::vector<Vec2> pts = pointsFromJson(a.flags["polygon"], cmd, "polygon");
                if (pts.size() < 3)
                    throw CommandError(std::string(cmd) + ": polygon= needs three or more points");
                ids.push_back(addExclusion(t.edits, pts, margin, "", name));
            }
            for (const json& obj : a.flags.value("objects", json::array()))
                for (const std::vector<Vec2>& pts : objectFootprints(g, textOf(obj), margin, cmd))
                    ids.push_back(addExclusion(t.edits, pts, 0.0, textOf(obj), name));
            if (ids.empty())
                throw CommandError(std::string(cmd) + ": give polygon= or objects=");
            setEdits(t);
            return editResult(g, t, ids, cmd);
        },
        .examples = {"settlementAddExclusion('Settlement', objects=['Monolith'], margin=1500)"},
    });

    ctx.registerCommand({
        .name = "settlementDelete",
        .doc = "Removes streets, routes and exclusions added by the edit commands, by id, and generated streets by their "
               "stable id (settlementStreets lists them; the removal holds through re-runs while the layout still makes that "
               "street). Returns the ids removed, the checks and the counts.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"ids", "i", T::List, nullptr, "Edit ids to remove."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementDelete";
            Graph& g = Graph::current();
            EditTarget t = editTarget(g, a, cmd);
            std::vector<int> gone, wanted;
            json goneStreets = json::array();
            Layout current;
            bool built = false;
            for (const json& v : a.flags.value("ids", json::array()))
            {
                if (!v.is_number_integer())
                    continue;
                if (v.get<int64_t>() > 0 && uint64_t(v.get<int64_t>()) >= kStreetIdBit)
                {
                    // A generated street: removed by its stable id.
                    const uint64_t street = v.get<uint64_t>();
                    if (!built)
                    {
                        current = rebuild(g, t.modifier->path(), cmd).layout;
                        built = true;
                    }
                    if (!hasStreet(current, street))
                        throw CommandError(std::string(cmd) + ": no street with id " + std::to_string(street));
                    overrideFor(t.edits, street) = {{"street", street}, {"remove", true}};
                    goneStreets.push_back(street);
                }
                else
                    wanted.push_back(v.get<int>());
            }
            for (const char* list : {"streets", "routes", "exclusions"})
            {
                json kept = json::array();
                for (const json& item : t.edits.value(list, json::array()))
                {
                    const int id = item.value("id", -1);
                    if (std::ranges::find(wanted, id) != wanted.end())
                        gone.push_back(id);
                    else
                        kept.push_back(item);
                }
                if (t.edits.contains(list))
                    t.edits[list] = kept;
            }
            if (gone.size() != wanted.size())
                throw CommandError(std::string(cmd) + ": some ids are not edits of this settlement");
            setEdits(t);
            json out = editResult(g, t, {}, cmd);
            out["ids"] = gone;
            out["streets"] = goneStreets;
            return out;
        },
        .examples = {"settlementDelete('Settlement', ids=[4, 5])"},
    });

    ctx.registerCommand({
        .name = "settlementStreets",
        .doc = "The streets a settlement lays, each with its stable id (the same street keeps it through unrelated edits and "
               "re-runs), class, road profile, width, length, the edit it came from (path, -1 when generated) and its points "
               "[x, y, z]. near= and radius= keep those passing close to a point; streetClass= one class. Pass an id to "
               "settlementEditStreet or settlementDelete to change or remove a generated street. Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"near", "nr", T::List, nullptr, "[x, z]: only streets passing within radius of it."},
                  {"radius", "r", T::Float, 2000.0, "cm.", {}, {}, 0.0},
                  {"streetClass", "c", T::String, nullptr, "Only this class.", {"arterial", "collector", "local", "alley", "path"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementStreets";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuild(Graph::current(), names[0], cmd);
            const bool useNear = a.has("near");
            const std::vector<Vec2> near = useNear ? pointsFromJson(json::array({a.flags["near"]}), cmd, "near") : std::vector<Vec2>{};
            const double radius = a.get<double>("radius", 2000.0);
            StreetClass only = StreetClass::Local;
            const bool useClass = a.has("streetClass") && parseClass(a.get<std::string>("streetClass"), &only);
            return streetsJson(r, [&](const StreetLine& s) {
                if (useClass && s.cls != only)
                    return false;
                if (!useNear)
                    return true;
                for (size_t k = 0; k + 1 < s.pts.size(); ++k)
                {
                    const Vec2 p(s.pts[k].x, s.pts[k].z), q(s.pts[k + 1].x, s.pts[k + 1].z), d = q - p;
                    const double t = std::clamp(glm::dot(near[0] - p, d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
                    if (glm::length(p + d * t - near[0]) <= radius)
                        return true;
                }
                return false;
            });
        },
        .examples = {"main = settlementStreets('Settlement', near=[1200, -800], streetClass='arterial')[0]['id']\n"
                     "settlementEditStreet('Settlement', id=main, profile='highway')"},
    });

    ctx.registerCommand({
        .name = "settlementSurfaces",
        .doc = "Sets what a settlement stands and walks on: ground (the mesh its streets and lots sit on; empty for flat "
               "ground) and walkable (the mesh routes may walk on: a structure's ramps, terraces and floors). Both are live: "
               "editing the meshes re-runs the layout.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {nodeFlag({"ground", "gd", T::String, nullptr, "Ground mesh, or empty for flat ground."}, "mesh"),
                  nodeFlag({"walkable", "wk", T::String, nullptr, "Walkable mesh, or empty for none."}, "mesh")},
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementSurfaces";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            Node* mod = settlementModifier(g, names[0], cmd);
            const Node* groupNode = gn::groupOfModifier(g, *mod);
            const std::string group = groupNode ? groupNode->name() : std::string();
            for (const auto& [flag, node] : {std::pair{"ground", "Ground"}, std::pair{"walkable", "Walkable"}})
            {
                if (!a.has(flag))
                    continue;
                const std::string mesh = a.get<std::string>(flag);
                if (!mesh.empty())
                    if (Node* n = g.find(mesh); !n || !n->type().isA("mesh"))
                        throw CommandError(std::string(cmd) + ": '" + mesh + "' is not a mesh");
                ops::execute("setAttr", {{"args", {mod->path() + "." + flag, mesh}}});
                if (!group.empty())
                    ops::execute("gnSetValue", {{"args", {group, std::string(node) + ".object", mesh}}});
            }
            Rebuilt r = rebuild(g, mod->path(), cmd);
            json out = stats(r.layout);
            out["checks"] = checks(r.layout, r.ground, r.options);
            out["walkableNodes"] = int64_t(r.walk.nodeCount());
            return out;
        },
        .examples = {"settlementSurfaces('Settlement', ground='Terrain', walkable='Temple')"},
    });

    ctx.registerCommand({
        .name = "settlementMetrics",
        .doc = "Street measures comparable with real cities: street length and density, orientation entropy and order "
               "(Boeing 2019), intersections, dead-end ratio, degree histogram, block areas. Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("settlementMetrics: give one settlement");
            return metrics(rebuild(Graph::current(), names[0], "settlementMetrics").layout);
        },
        .examples = {"settlementMetrics('Settlement')['deadEndRatio']"},
    });

    ctx.registerCommand({
        .name = "settlementRoadProfiles",
        .doc = "The built-in road profiles (cross-sections streets and routes may name) with every field, as {name: profile}, "
               "and which one each class takes by default. Custom ones go in a settlement's edits under roadProfiles, as "
               "{name: {base: 'street', sidewalk: 300}}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::object(), defaults = json::object();
            for (const std::string& n : roadProfileNames())
            {
                RoadProfile r;
                builtinRoadProfile(n, &r);
                out[n] = toJson(r);
            }
            for (StreetClass c : {StreetClass::Arterial, StreetClass::Collector, StreetClass::Local, StreetClass::Alley, StreetClass::Path})
                defaults[className(c)] = defaultRoadProfile(c);
            return {{"profiles", out}, {"classDefaults", defaults}};
        },
        .examples = {"settlementRoadProfiles()['profiles']['avenue']['width']"},
    });

    registerUnrealCommands(ctx);
    registerExportCommands(ctx);
    registerStreetscapeCommands(ctx);
    registerLotCommands(ctx);
    registerTerrainCommands(ctx);
    registerGeneratorCommands(ctx);
}
} // namespace settlement
