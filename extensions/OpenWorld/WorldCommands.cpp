// worldBuild and worldSpec: a whole map from one JSON through the parts' own commands (remix, terrain spec, settlements,
// Buildings, site kits, slots, zones), timed and measured per step, and read back from the scene as one spec.

#include "OpenWorld.h"
#include "Remix.h"
#include "RemixSpec.h"
#include "WorldScene.h"
#include "Zones.h"

#include <clipper2/clipper.h>

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mcp.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Slots.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <set>

using namespace lila;

namespace openworld::world
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;
using T = ArgType;

// Scene helpers ---------------------------------------------------------------------------------------------------------

std::string worldNodeName(const std::string& name) { return name + "_World"; }

namespace
{
json parsed(const json& v, json fallback)
{
    if (!v.is_string() || v.get<std::string>().empty())
        return fallback;
    json j = json::parse(v.get<std::string>(), nullptr, false);
    return j.is_discarded() ? fallback : j;
}

json attrOf(const std::string& node, const char* attr, json fallback = json::object())
{
    Node* n = Graph::current().find(node);
    if (!n || n->type().attrIndex(attr) < 0)
        return fallback;
    return parsed(ops::execute("getAttr", {{"args", {node + "." + attr}}}), std::move(fallback));
}

void setStringAttr(const std::string& node, const char* attr, const std::string& value, const char* doc)
{
    Node* n = Graph::current().find(node);
    if (!n)
        return;
    if (n->type().attrIndex(attr) < 0)
        ops::execute("addAttr", {{"args", {node}}, {"longName", attr}, {"dataType", "string"}, {"doc", doc}});
    ops::execute("setAttr", {{"args", {node + "." + attr, value}}});
}

WorldRef readWorld(const Node& n)
{
    WorldRef w;
    w.node = n.path();
    w.spec = attrOf(w.node, kSpecAttr);
    w.built = attrOf(w.node, kBuiltAttr);
    return w;
}

bool isGroupOf(Graph& g, const Node& mod, std::string_view groupName)
{
    if (!gn::isGnModifier(mod))
        return false;
    Node* grp = gn::groupOfModifier(g, mod);
    return grp && grp->name() == groupName;
}

/** The node a build asked to call wanted: that name, else the name the workspace's naming rules turn it into. */
Node* findAsked(const std::string& wanted, const char* type = "transform")
{
    Graph& g = Graph::current();
    if (Node* n = g.find(wanted))
        return n;
    const scene::NodeType* t = scene::types::find(type);
    return t ? g.find(g.namingPolicy().conform(*t, wanted)) : nullptr;
}
} // namespace

std::vector<WorldRef> allWorlds()
{
    std::vector<WorldRef> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().attrIndex(kSpecAttr) >= 0)
            out.push_back(readWorld(*n));
    return out;
}

std::optional<WorldRef> findWorld(const std::string& name)
{
    if (!name.empty())
    {
        for (const std::string& candidate : {name, worldNodeName(name)})
            if (Node* n = findAsked(candidate); n && n->type().attrIndex(kSpecAttr) >= 0)
                return readWorld(*n);
        // Renamed by hand or by the naming rules of the workspace it was built in: the spec it holds names it.
        for (WorldRef& w : allWorlds())
            if (w.spec.is_object() && w.spec.value("name", std::string()) == name)
                return w;
        return std::nullopt;
    }
    std::vector<WorldRef> all = allWorlds();
    if (all.size() == 1)
        return all[0];
    return std::nullopt;
}

std::vector<std::string> terrainMeshes()
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !gn::isGnModifier(*n) || !gn::modifierHeightfield(id))
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, id);
        if (mesh != scene::kNoNode && g.node(mesh))
            out.push_back(g.node(mesh)->path());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string worldTerrain(const WorldRef* w)
{
    Graph& g = Graph::current();
    if (w)
        if (const std::string t = w->built.value("terrain", std::string()); !t.empty() && g.find(t))
            return g.find(t)->path();
    g.evaluate();
    const std::vector<std::string> all = terrainMeshes();
    return all.size() == 1 ? all[0] : std::string();
}

std::shared_ptr<const geo::Heightfield> terrainHeights(const std::string& mesh)
{
    Graph& g = Graph::current();
    Node* n = g.find(mesh);
    if (!n)
        return nullptr;
    g.evaluate();
    std::shared_ptr<const geo::Heightfield> found;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            if (auto h = gn::modifierHeightfield(m))
                found = h;
    return found;
}

std::vector<std::string> settlementMeshes()
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !gn::isGnModifier(*n) || n->type().attrIndex("walkable") < 0 || n->type().attrIndex("ground") < 0 || n->type().attrIndex("edits") < 0)
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, id);
        if (mesh != scene::kNoNode && g.node(mesh))
            out.push_back(g.node(mesh)->path());
    }
    return out;
}

std::vector<std::string> zoneSetNodes()
{
    std::vector<std::string> out;
    Graph& g = Graph::current();
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("zoneSet"))
            out.push_back(n->path());
    return out;
}

std::vector<std::pair<std::string, json>> buildingEdits()
{
    Graph& g = Graph::current();
    std::map<std::string, json> byGroup;
    for (scene::NodeId id : g.nodes())
    {
        Node* mod = g.node(id);
        if (!mod || !isGroupOf(g, *mod, "LilaBuildings"))
            continue;
        const scene::NodeId mesh = scene::modifiers::meshOf(g, id);
        if (mesh == scene::kNoNode)
            continue;
        const scene::NodeId parent = g.parent(mesh);
        if (parent == scene::kNoNode || !g.node(parent))
            continue;
        json& edits = byGroup[g.node(parent)->path()];
        if (edits.is_null())
            edits = json::object();
        const json own = attrOf(mod->path(), "edits");
        for (auto& [k, v] : own.items())
            edits[k] = v;
    }
    for (auto& [grp, edits] : byGroup)
    {
        const json orphans = attrOf(grp, "buildingsOrphanEdits");
        for (auto& [k, v] : orphans.items())
            if (!edits.contains(k))
                edits[k] = v;
    }
    return {byGroup.begin(), byGroup.end()};
}

fs::path outputFolder(const std::string& kind, const std::string& name, json* notes)
{
    const fs::path& root = Workspace::get().root();
    if (!root.empty())
        return root / "openworld" / kind / name;
    const fs::path tmp = fs::temp_directory_path() / ("lila_" + kind) / name;
    if (notes)
        notes->push_back("no workspace open; files went to " + paths::toUtf8(tmp));
    return tmp;
}

namespace
{
// Build -----------------------------------------------------------------------------------------------------------------

/** One step of a build: what ran, how long, the memory after it and its result (big lists counted) or error. */
class Steps
{
public:
    /** scope shows the running step on the progress card, which keeps Script Output painting between steps. */
    Steps(bool stopOnError, progress::Scope* scope = nullptr) : mStop(stopOnError), mScope(scope) {}

    bool stopped() const { return mStopped; }
    bool failed() const { return mFailed; }
    const json& list() const { return mSteps; }
    std::string firstError() const { return mFirstError; }

    /** Runs fn as step name unless an earlier failure stopped the build; false on failure. */
    bool run(const std::string& name, const std::function<json()>& fn, json* result = nullptr)
    {
        if (mStopped)
            return false;
        LILA_ZONE_N("world.step");
        LILA_ZONE_TEXT(name.c_str(), name.size());
        if (mScope)
            mScope->setStatus(std::format("step {}: {}", mSteps.size() + 1, name));
        const auto t0 = std::chrono::steady_clock::now();
        json step = {{"step", name}};
        bool ok = true;
        try
        {
            json r = fn();
            if (result)
                *result = r;
            step["result"] = summarize(r);
        }
        catch (const std::exception& e)
        {
            ok = false;
            step["error"] = e.what();
            if (mFirstError.empty())
                mFirstError = name + ": " + e.what();
            mFailed = true;
            if (mStop)
                mStopped = true;
        }
        step["ok"] = ok;
        step["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        step["memoryMB"] = std::round(memoryMB());
        LILA_PLOT("world.memoryMB", memoryMB());
        if (ok)
            log::info("worldBuild: {} done in {:.1f} s ({:.0f} MB)", name, step["seconds"].get<double>(), step["memoryMB"].get<double>());
        else
            log::warn("worldBuild: {} failed: {}", name, step["error"].get<std::string>());
        mSteps.push_back(std::move(step));
        return ok;
    }

    static json summarize(const json& j, int depth = 0)
    {
        if (j.is_array())
        {
            if (j.size() > 12 || depth > 2)
                return {{"count", j.size()}};
            json out = json::array();
            for (const json& v : j)
                out.push_back(summarize(v, depth + 1));
            return out;
        }
        if (j.is_object())
        {
            if (depth > 3)
                return {{"keys", j.size()}};
            json out = json::object();
            for (const auto& [k, v] : j.items())
                out[k] = summarize(v, depth + 1);
            return out;
        }
        if (j.is_string() && j.get<std::string>().size() > 400)
            return j.get<std::string>().substr(0, 400) + "...";
        return j;
    }

private:
    bool mStop = true, mStopped = false, mFailed = false;
    progress::Scope* mScope = nullptr;
    std::string mFirstError;
    json mSteps = json::array();
};

json run(const char* cmd, json flags) { return ops::execute(cmd, std::move(flags)); }

std::string pathOfResult(const json& made)
{
    if (made.is_string())
        return made.get<std::string>();
    if (made.is_array() && !made.empty() && made[0].is_string())
        return made[0].get<std::string>();
    return {};
}

/** The OSM nodes' world positions on the terrain, through Terrain's own georeference (origin given for a remix). */
std::vector<glm::dvec2> worldPositions(const OsmDoc& d, const std::string& terrain, const json& origin)
{
    std::vector<glm::dvec2> out;
    out.reserve(d.nodes.size());
    const size_t chunk = 20000;
    for (size_t i = 0; i < d.nodes.size(); i += chunk)
    {
        json ll = json::array();
        for (size_t k = i; k < std::min(d.nodes.size(), i + chunk); ++k)
            ll.push_back({d.nodes[k].at.lat, d.nodes[k].at.lon});
        json call = {{"args", {terrain}}, {"latLon", ll}};
        if (!origin.is_null())
            call["origin"] = origin;
        const json r = run("terrainGeoref", call);
        for (const json& p : r.value("points", json::array()))
            out.push_back(p.size() >= 3 ? glm::dvec2(p[0].get<double>(), p[2].get<double>()) : glm::dvec2(p[0].get<double>(), p[1].get<double>()));
    }
    if (out.size() != d.nodes.size())
        throw std::runtime_error("terrainGeoref gave " + std::to_string(out.size()) + " points for " + std::to_string(d.nodes.size()) + " OSM nodes");
    return out;
}

/**
 * Every site road on the terrain (ids under site:) as a closed corridor round its centreline, its paved width plus 3 m
 * each side: the left edge out and the right edge back, so one polygon per road.
 */
json siteRoadCorridors(const std::string& terrain)
{
    json out = json::array();
    const json roads = run("terrainRoads", {{"args", {terrain}}, {"detail", true}});
    const json layers = roads.value("layers", json::object());
    for (const auto& [layer, list] : layers.items())
        for (const json& r : list)
        {
            if (r.value("id", std::string()).rfind("site:", 0) != 0)
                continue;
            const double half = r.value("pavedWidth", r.value("width", 400.0)) * 0.5 + 300.0;
            std::vector<glm::dvec2> line;
            for (const json& p : r.value("centreline", json::array()))
                if (p.size() >= 3)
                    line.push_back({p[0].get<double>(), p[2].get<double>()});
            if (line.size() < 2)
                continue;
            std::vector<glm::dvec2> left, right;
            for (size_t i = 0; i < line.size(); ++i)
            {
                const glm::dvec2 d = glm::normalize(line[std::min(i + 1, line.size() - 1)] - line[i > 0 ? i - 1 : 0]);
                const glm::dvec2 n(-d.y, d.x);
                // Ends pushed out along the road too, so the corridor covers the road's rounded end.
                const glm::dvec2 along = i == 0 ? -d * half : i + 1 == line.size() ? d * half : glm::dvec2(0.0);
                left.push_back(line[i] + n * half + along);
                right.push_back(line[i] - n * half + along);
            }
            json poly = json::array();
            for (const glm::dvec2& p : left)
                poly.push_back({p.x, p.y});
            for (auto it = right.rbegin(); it != right.rend(); ++it)
                poly.push_back({it->x, it->y});
            out.push_back(std::move(poly));
        }
    return out;
}

/** Seeds a Buildings group with edits before it is built, so buildingCreate hands them to the buildings they name. */
void seedEdits(const std::string& groupName, const json& edits)
{
    if (!edits.is_object() || edits.empty())
        return;
    Node* found = findAsked(groupName);
    std::string grp = found ? found->path() : std::string();
    if (grp.empty())
        grp = pathOfResult(run("group", {{"empty", true}, {"name", groupName}}));
    json merged = attrOf(grp, "buildingsOrphanEdits");
    for (auto& [k, v] : edits.items())
        merged[k] = v;
    setStringAttr(grp, "buildingsOrphanEdits", merged.dump(), "Buildings edits waiting for the buildings they name.");
}

/** The level of detail a Buildings group's new cells start at (near, hlod or far). */
void seedLod(const std::string& groupName, const std::string& lod)
{
    if (lod != "near" && lod != "hlod" && lod != "far")
        throw std::runtime_error("buildings lod is near, hlod or far, not " + lod);
    Node* found = findAsked(groupName);
    std::string grp = found ? found->path() : std::string();
    if (grp.empty())
        grp = pathOfResult(run("group", {{"empty", true}, {"name", groupName}}));
    setStringAttr(grp, "buildingsLod", lod, "The level of detail new building cells start at.");
}

struct BuildOptions
{
    bool replace = false;
    bool stopOnError = true;
    std::string folder;
    std::vector<std::string> skip;
};

Clipper2Lib::PathD fillPath(const Polygon& p)
{
    Clipper2Lib::PathD path;
    path.reserve(p.size());
    for (const Vec2& v : p)
        path.push_back({v.x, v.y});
    return path;
}

/**
 * Fills what a must-cover layer leaves of the play area with one zone, so a district layer made from settlements also
 * covers the countryside between them: the play area (the set's, else the terrain's extent) minus every zone of the
 * layer, as polygons with holes, so the fill meets each zone edge exactly (no sliver of gap or overlap). Painted zones
 * count by their traced outline. The zone gets a fictional district name.
 */
json fillLayerGaps(const std::string& set, const std::string& terrain, const std::string& layerId)
{
    using namespace Clipper2Lib;
    zones::ZoneSpec spec = zones::zonesFromJson(run("zoneSpec", {{"set", set}}));
    zones::Layer* layer = nullptr;
    for (zones::Layer& l : spec.layers)
        if (l.id == layerId)
            layer = &l;
    if (!layer)
        throw std::runtime_error("no layer " + layerId + " in " + set);
    const std::string zoneId = "outlying";
    std::erase_if(layer->zones, [&](const zones::Zone& z) { return z.id == zoneId; });
    PathsD play;
    if (!spec.playArea.empty())
        for (const Polygon& p : spec.playArea)
            play.push_back(fillPath(p));
    else if (auto h = terrainHeights(terrain))
    {
        const Vec2 lo = h->origin, hi = h->origin + h->extent();
        play.push_back(fillPath({lo, {hi.x, lo.y}, hi, {lo.x, hi.y}}));
    }
    else
        throw std::runtime_error("no play area and no terrain to fill " + layerId + " over");
    PathsD cover;
    for (const zones::Zone& z : layer->zones)
    {
        PathsD outer, holes;
        for (const Polygon& p : z.polygons)
            outer.push_back(fillPath(p));
        for (const Polygon& p : z.holes)
            holes.push_back(fillPath(p));
        if (z.hasMask())
        {
            std::vector<Polygon> rings, maskHoles;
            zones::maskOutline(z.mask, 128, z.mask.cell * 0.25, rings, maskHoles);
            for (const Polygon& p : rings)
                outer.push_back(fillPath(p));
            for (const Polygon& p : maskHoles)
                holes.push_back(fillPath(p));
        }
        const PathsD covered = holes.empty() ? Union(outer, FillRule::NonZero, 2) : Difference(outer, holes, FillRule::NonZero, 2);
        cover.insert(cover.end(), covered.begin(), covered.end());
    }
    const PathsD rest = Difference(play, Union(cover, FillRule::NonZero, 2), FillRule::NonZero, 2);
    zones::Zone z;
    z.id = zoneId;
    double area = 0.0;
    for (const PathD& path : rest)
    {
        Polygon poly;
        for (const PointD& q : path)
            poly.push_back({q.x, q.y});
        if (poly.size() < 3)
            continue;
        const double a = Area(path);
        area += a;
        // Clipper2 gives outers counter-clockwise (positive area) and holes clockwise.
        (a > 0.0 ? z.polygons : z.holes).push_back(std::move(poly));
    }
    if (z.polygons.empty() || area < 1.0)
        return {{"layer", layerId}, {"areaM2", 0}};
    z.priority = -1;
    try
    {
        const json n = run("fictionalName", {{"culture", spec.culture}, {"kind", "district"}, {"key", set + "/" + layerId + "/" + zoneId}});
        z.name = n.is_string() ? n.get<std::string>() : n.value("name", std::string());
        z.nameSource = "fictionalName";
    }
    catch (const std::exception&)
    {
    }
    if (z.name.empty())
    {
        z.name = "Outlying " + layerId;
        z.nameSource = "given";
    }
    const size_t parts = z.polygons.size(), holes = z.holes.size();
    layer->zones.push_back(std::move(z));
    run("zoneSpec", {{"set", set}, {"spec", zones::toJson(spec)}});
    return {{"layer", layerId}, {"zone", zoneId}, {"polygons", parts}, {"holes", holes}, {"areaM2", area * 1e-4}};
}

/** Interiors for the buildings the spec's edits mark enterable: an edit alone marks them, buildingMakeEnterable makes the rooms. */
void makeEnterable(Steps& st, const std::string& groupName, const json& edits)
{
    json ids = json::array();
    for (const auto& [id, e] : edits.items())
        if (e.is_object() && e.value("enterable", false))
            ids.push_back(id);
    if (!ids.empty())
        st.run("buildings.enterable." + groupName, [&] { return run("buildingMakeEnterable", {{"building", ids}}); });
}

/** Lower-case letters and digits of a name, trailing digits dropped: PuertoCentro_Buildings1 and PuertoCentroBuildings match. */
std::string squashed(std::string_view s)
{
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    while (!out.empty() && out.back() >= '0' && out.back() <= '9')
        out.pop_back();
    return out;
}

/** What follows prefix in a squashed name, past the digits a renamed base may carry (sinaloablock1remixosm: remixosm). */
std::optional<std::string> restAfter(const std::string& q, const std::string& prefix)
{
    if (prefix.empty() || q.compare(0, prefix.size(), prefix) != 0)
        return std::nullopt;
    size_t i = prefix.size();
    while (i < q.size() && q[i] >= '0' && q[i] <= '9')
        ++i;
    return q.substr(i);
}

std::string linkIdOf(const Node& n)
{
    if (!n.type().dag || n.type().attrs.size() <= size_t(scene::Transform::kLinkId))
        return {};
    const std::string* v = std::get_if<std::string>(&n.value(scene::Transform::kLinkId));
    return v ? *v : std::string();
}

/** A link id assignWorldLinkIds gave: w, 8 hex of the world's name, 12 hex of the node's path. */
bool worldLinkId(const std::string& id)
{
    return id.size() == 21 && id[0] == 'w' && std::all_of(id.begin() + 1, id.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

bool hasBuildingsModifier(Graph& g, scene::NodeId mesh)
{
    for (scene::NodeId m : scene::modifiers::stack(g, mesh))
        if (Node* mod = g.node(m); mod && isGroupOf(g, *mod, "LilaBuildings"))
            return true;
    return false;
}

/** A Buildings group: seeded with Buildings attributes, or holding only Buildings cell meshes. */
bool buildingsGroup(Graph& g, const Node& n)
{
    if (!n.type().dag || n.type().isA("mesh"))
        return false;
    for (const char* a : {"buildingsLod", "buildingsOrphanEdits"})
        if (n.type().attrIndex(a) >= 0)
            return true;
    const std::vector<scene::NodeId>& kids = g.children(n.id());
    return !kids.empty() && std::all_of(kids.begin(), kids.end(), [&](scene::NodeId k) { return hasBuildingsModifier(g, k); });
}

/** A group of curves only, as an OSM import makes. */
bool curveGroup(Graph& g, scene::NodeId id)
{
    bool curves = false;
    for (scene::NodeId d : g.descendants(id))
    {
        Node* n = g.node(d);
        if (n && n->type().isA("curve"))
            curves = true;
        else if (n && n->type().dag && n->type().name != "transform")
            return false;
    }
    return curves;
}

/** The meshSource nodes feeding a modifier (a stack's base, which goes with it). */
std::vector<scene::NodeId> meshSourcesInto(const Node& mod)
{
    Graph& g = Graph::current();
    std::vector<scene::NodeId> out;
    const json inputs = ops::execute("listConnections", {{"args", {mod.path()}}, {"source", true}, {"destination", false}});
    for (const json& in : inputs.is_array() ? inputs : json::array())
        if (Node* src = in.is_string() ? g.find(in.get<std::string>()) : nullptr; src && src->type().isA("meshSource"))
            out.push_back(src->id());
    return out;
}

/** Building ids a Buildings cell modifier holds. */
void buildingIdsOf(const Node& mod, std::set<std::string>& out)
{
    const json specs = attrOf(mod.path(), "spec", json::array());
    if (specs.is_array())
        for (const json& s : specs)
            if (s.is_object() && s.contains("id") && s["id"].is_string())
                out.insert(s["id"].get<std::string>());
}

/**
 * Deletes what earlier builds of the world called name made, so a rebuild leaves one copy. Found by each build's record
 * and by the link ids every build gives its nodes (both survive a save and a rename); for builds that recorded less or
 * nothing, by the world's name and node types (its terrain, OSM line groups and water, the specs' settlements, their
 * Buildings groups and zone set) and by the modifiers its meshes left behind. Nodes another world made are never matched.
 * Returns what went and a note saying how it was found.
 */
json removeBuilt(const std::string& name, const WorldRef* w, const json& spec)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    // Every world node built under this name is this world (a renamed one, and the copy a missed replace made beside it).
    std::vector<WorldRef> ours, others;
    for (WorldRef& r : allWorlds())
    {
        const bool same = (w && r.node == w->node) || (r.spec.is_object() && r.spec.value("name", std::string()) == name);
        (same ? ours : others).push_back(std::move(r));
    }
    std::set<scene::NodeId> foreign;
    std::set<std::string> foreignPrefixes;
    for (const WorldRef& r : others)
    {
        if (Node* n = g.find(r.node))
            foreign.insert(n->id());
        if (r.spec.is_object() && r.spec.contains("name") && r.spec["name"].is_string())
            foreignPrefixes.insert(worldLinkPrefix(r.spec["name"].get<std::string>()));
        for (const char* k : {"nodes", "siteMeshes", "buildingGroups", "settlements"})
            for (const json& p : r.built.value(k, json::array()))
                if (Node* n = p.is_string() ? g.find(p.get<std::string>()) : nullptr)
                    foreign.insert(n->id());
    }
    auto isForeign = [&](const Node& n) {
        if (const std::string id = linkIdOf(n); worldLinkId(id) && foreignPrefixes.count(id.substr(0, 9)))
            return true;
        for (scene::NodeId p = n.id(); p != scene::kNoNode; p = g.parent(p))
            if (foreign.count(p))
                return true;
        return false;
    };
    std::set<scene::NodeId> picked;
    int byRecord = 0, byLink = 0;
    json byName = json::array();
    auto pick = [&](Node* n) {
        if (!n || picked.count(n->id()) || isForeign(*n))
            return false;
        picked.insert(n->id());
        return true;
    };

    std::set<std::string> recordedGroups;
    for (const WorldRef& r : ours)
    {
        byRecord += pick(g.find(r.node));
        for (const char* k : {"nodes", "siteMeshes", "buildingGroups", "settlements"})
            for (const json& p : r.built.value(k, json::array()))
                if (p.is_string())
                    byRecord += pick(g.find(p.get<std::string>()));
        for (const char* k : {"terrain", "remix", "remixFeatures", "zones"})
            if (const json p = r.built.value(k, json()); p.is_string())
                byRecord += pick(g.find(p.get<std::string>()));
        for (const json& p : r.built.value("gnGroups", json::array()))
            if (p.is_string())
                recordedGroups.insert(p.get<std::string>());
    }
    const std::string prefix = worldLinkPrefix(name);
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id))
            if (const std::string lid = linkIdOf(*n); worldLinkId(lid) && lid.compare(0, prefix.size(), prefix) == 0)
                byLink += pick(n);

    // By name and type, for what a build recorded under another name or not at all.
    const std::string key = squashed(name);
    std::set<std::string> settlementKeys, zoneKeys;
    bool sites = false;
    std::vector<const json*> specs = {&spec};
    for (const WorldRef& r : ours)
    {
        if (r.spec.is_object())
            specs.push_back(&r.spec);
        for (const json& s : r.built.value("settlementNames", json::array()))
            if (s.is_string())
                settlementKeys.insert(squashed(s.get<std::string>()));
    }
    for (const json* sp : specs)
    {
        for (const json& s : sp->value("settlements", json::array()))
            if (s.is_object() && s.contains("name") && s["name"].is_string())
                settlementKeys.insert(squashed(s["name"].get<std::string>()));
        if (const json si = sp->value("sites", json::array()); si.is_array() && !si.empty())
            sites = true;
        if (const json z = sp->value("zones", json()); z.is_object())
            zoneKeys.insert(squashed(z.value("name", std::string("Zones"))));
    }
    std::set<scene::NodeId> terrainIds, settlementIds;
    for (const std::string& p : terrainMeshes())
        if (Node* n = g.find(p))
            terrainIds.insert(n->id());
    for (const std::string& p : settlementMeshes())
        if (Node* n = g.find(p))
            settlementIds.insert(n->id());
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || picked.count(id) || !((n->type().dag && g.parent(id) == scene::kNoNode) || n->type().isA("zoneSet")))
            continue;
        const std::string q = squashed(n->name());
        bool match = false;
        if (const std::optional<std::string> rest = restAfter(q, key))
        {
            if (rest->empty())
                match = terrainIds.count(id) > 0;
            else if (*rest == "osm" || *rest == "remixosm")
                match = curveGroup(g, id);
            else if (*rest == "water" || *rest == "groundcover")
                match = n->type().isA("mesh");
        }
        for (const std::string& s : settlementKeys)
            match = match || (q == s && settlementIds.count(id)) || (q == s + "buildings" && buildingsGroup(g, *n));
        match = match || (sites && q == "sitebuildings" && buildingsGroup(g, *n)) || (zoneKeys.count(q) && n->type().isA("zoneSet"));
        if (match && pick(n))
            byName.push_back(n->path());
    }

    // A parent and its child both picked: delete the parents only.
    std::vector<std::string> gone;
    for (scene::NodeId id : picked)
        if (Node* n = g.node(id))
            gone.push_back(n->path());
    std::sort(gone.begin(), gone.end());
    std::vector<std::string> roots;
    for (const std::string& p : gone)
        if (std::none_of(roots.begin(), roots.end(), [&](const std::string& r) { return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '|'; }))
            roots.push_back(p);
    // Deleting a mesh leaves its modifier stack behind; a Buildings modifier left alone keeps publishing its slots, which
    // then count twice beside the rebuilt ones.
    std::vector<scene::NodeId> stacks;
    std::set<scene::NodeId> groups;
    std::vector<scene::NodeId> cells;
    auto takeModifier = [&](scene::NodeId m) {
        stacks.push_back(m);
        if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
            if (Node* grp = gn::groupOfModifier(g, *mod))
            {
                groups.insert(grp->id());
                if (grp->name() == "LilaBuildings")
                    cells.push_back(m);
            }
    };
    for (const std::string& r : roots)
        if (Node* top = g.find(r))
        {
            std::vector<scene::NodeId> under = g.descendants(top->id());
            under.push_back(top->id());
            for (scene::NodeId id : under)
                if (Node* n = g.node(id); n && n->type().isA("mesh"))
                {
                    for (scene::NodeId m : scene::modifiers::stack(g, id))
                        takeModifier(m);
                    if (const scene::NodeId src = scene::modifiers::stackSource(g, id); src != scene::kNoNode && g.node(src) && !g.node(src)->type().dag)
                        stacks.push_back(src);
                }
        }
    // Modifiers an earlier build's meshes left without a mesh: the world's own node groups', and Buildings cells holding its buildings.
    auto worldGroup = [&](const Node& grp) {
        if (recordedGroups.count(grp.name()))
            return true;
        const std::string q = squashed(grp.name());
        if (const std::optional<std::string> rest = restAfter(q, key); rest && (*rest == "remix" || *rest == "terrain"))
            return true;
        return std::any_of(settlementKeys.begin(), settlementKeys.end(), [&](const std::string& s) { return q == s + "settlement"; });
    };
    // The removed cells' building ids, read only if a Buildings modifier without a mesh needs them (their specs are big).
    std::optional<std::set<std::string>> buildingIds;
    auto removedBuildings = [&]() -> const std::set<std::string>& {
        if (!buildingIds)
        {
            buildingIds.emplace();
            for (scene::NodeId m : cells)
                if (Node* mod = g.node(m))
                    buildingIdsOf(*mod, *buildingIds);
        }
        return *buildingIds;
    };
    int dangling = 0;
    for (scene::NodeId id : g.nodes())
    {
        Node* mod = g.node(id);
        if (!mod || !gn::isGnModifier(*mod) || scene::modifiers::meshOf(g, id) != scene::kNoNode)
            continue;
        Node* grp = gn::groupOfModifier(g, *mod);
        if (!grp)
            continue;
        bool mine = worldGroup(*grp);
        if (!mine && grp->name() == "LilaBuildings")
        {
            std::set<std::string> ids;
            buildingIdsOf(*mod, ids);
            const std::set<std::string>& removed = removedBuildings();
            mine = std::any_of(ids.begin(), ids.end(), [&](const std::string& b) { return removed.count(b) > 0; });
        }
        if (!mine)
            continue;
        takeModifier(id);
        ++dangling;
        for (scene::NodeId src : meshSourcesInto(*mod))
            stacks.push_back(src);
    }

    if (!roots.empty())
        run("delete", {{"args", roots}});
    for (scene::NodeId id : stacks)
        if (g.node(id))
            g.deleteNode(id);
    int groupsGone = 0;
    for (scene::NodeId id : groups)
        if (Node* grp = g.node(id); grp && worldGroup(*grp) && gn::instancesOf(g, id).empty())
        {
            g.deleteNode(id);
            ++groupsGone;
        }
    // Sites by the records' ids, then by the specs' (a site id is unique in a scene, and the rebuild makes these again).
    std::set<std::string> recordedSites, specSites;
    for (const WorldRef& r : ours)
        for (const json& s : r.built.value("sites", json::array()))
            if (s.is_string())
                recordedSites.insert(s.get<std::string>());
    for (const json* sp : specs)
        for (const json& s : sp->value("sites", json::array()))
            if (s.is_object() && s.contains("id") && s["id"].is_string())
                specSites.insert(s["id"].get<std::string>());
    for (const std::set<std::string>* ids : {&recordedSites, &specSites})
        for (const std::string& id : *ids)
        {
            if (ids == &specSites && recordedSites.count(id))
                continue;
            try
            {
                run("siteRemove", {{"args", {id}}});
                if (ids == &specSites)
                    byName.push_back("site " + id);
            }
            catch (const std::exception&)
            {
            }
        }

    json out = {{"roots", roots}, {"byRecord", byRecord}, {"byLinkId", byLink}, {"byName", byName}, {"danglingModifiers", dangling}, {"groups", groupsGone}};
    if (roots.empty() && dangling == 0 && byName.empty())
        out["note"] = "replace: no earlier build of " + name + " in the scene";
    else
    {
        std::string note = "replace: removed the earlier build of " + name + " (" + std::to_string(roots.size()) + " nodes";
        if (!byName.empty())
        {
            note += "; " + std::to_string(byName.size()) + " not in its record, matched by the world's names and node types:";
            for (const json& p : byName)
                note += " " + p.get<std::string>();
        }
        if (dangling > 0)
            note += "; " + std::to_string(dangling) + " modifiers left without a mesh";
        note += ")";
        out["note"] = note;
    }
    return out;
}

/**
 * Deletes Buildings cell modifiers left without a mesh that hold buildings a live cell holds too: an earlier build's
 * leftovers that would publish those buildings' slots a second time. Returns how many went.
 */
int dropStaleCells()
{
    Graph& g = Graph::current();
    std::vector<scene::NodeId> dangling, attached;
    for (scene::NodeId id : g.nodes())
        if (Node* mod = g.node(id); mod && isGroupOf(g, *mod, "LilaBuildings"))
            (scene::modifiers::meshOf(g, id) == scene::kNoNode ? dangling : attached).push_back(id);
    if (dangling.empty())
        return 0;
    std::set<std::string> live;
    for (scene::NodeId id : attached)
        buildingIdsOf(*g.node(id), live);
    int gone = 0;
    for (scene::NodeId id : dangling)
    {
        Node* mod = g.node(id);
        std::set<std::string> ids;
        buildingIdsOf(*mod, ids);
        if (std::none_of(ids.begin(), ids.end(), [&](const std::string& b) { return live.count(b) > 0; }))
            continue;
        for (scene::NodeId src : meshSourcesInto(*mod))
            if (g.node(src))
                g.deleteNode(src);
        g.deleteNode(id);
        ++gone;
    }
    return gone;
}

/** Records every node the build made: the new DAG roots in nodes, the new node groups in gnGroups. */
void recordMade(const std::set<scene::NodeId>& existing, json& built)
{
    Graph& g = Graph::current();
    std::set<std::string> nodes;
    for (const json& p : built["nodes"])
        if (Node* n = p.is_string() ? g.find(p.get<std::string>()) : nullptr)
            nodes.insert(n->path());
    json groups = json::array();
    for (scene::NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || existing.count(id))
            continue;
        if (n->type().dag)
        {
            const scene::NodeId parent = g.parent(id);
            if (parent == scene::kNoNode || existing.count(parent))
                nodes.insert(n->path());
        }
        else if (n->type().isA("gnGroup"))
            groups.push_back(n->name());
    }
    built["nodes"] = json(std::vector<std::string>(nodes.begin(), nodes.end()));
    built["gnGroups"] = groups;
}

json buildWorld(json spec, const BuildOptions& o)
{
    LILA_ZONE();
    try
    {
        validateSpec(spec);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("worldBuild: ") + e.what());
    }
    Graph& g = Graph::current();
    const std::string name = spec.value("name", std::string("World"));
    const std::string culture = spec.value("culture", std::string("mexican"));
    const std::optional<WorldRef> old = findWorld(name);
    if (old && !o.replace)
        throw CommandError("worldBuild: a world named " + name + " exists (" + old->node + "); replace=True rebuilds it");
    json notes = json::array();
    json replaced;
    if (o.replace)
    {
        replaced = removeBuilt(name, old ? &*old : nullptr, spec);
        if (const std::string note = replaced.value("note", std::string()); !note.empty())
            notes.push_back(note);
    }
    // Everything made from here on is the world's: what is new afterwards is recorded, so a rebuild finds it all.
    std::set<scene::NodeId> existing;
    for (scene::NodeId id : g.nodes())
        existing.insert(id);
    auto skipped = [&](const char* step) { return std::find(o.skip.begin(), o.skip.end(), step) != o.skip.end(); };
    const fs::path folder = o.folder.empty() ? outputFolder("world", name, &notes) : paths::fromUtf8(o.folder);
    std::error_code ec;
    fs::create_directories(folder, ec);

    const auto t0 = std::chrono::steady_clock::now();
    const double mem0 = memoryMB();
    progress::Scope buildScope("Building world " + name);
    Steps st(o.stopOnError, &buildScope);
    json built = {{"nodes", json::array()}, {"settlements", json::array()}, {"sites", json::array()}, {"siteMeshes", json::array()}, {"buildingGroups", json::array()}};
    // Slot rules change first and the roots they would re-send are being rebuilt: the export sends them once they are whole.
    link::SlotSyncPause slotSyncPause;
    auto made = [&](const std::string& path) {
        if (!path.empty())
            built["nodes"].push_back(path);
    };

    // Rules and overrides first, so every generator's slots resolve through them as they are made.
    if (spec.contains("slots") && !skipped("slots"))
        st.run("slots", [&] {
            const json& sl = spec["slots"];
            std::vector<slots::Rule> rules;
            for (const json& r : sl.value("rules", json::array()))
                rules.push_back(slots::Rule::fromJson(r));
            slots::setRules(std::move(rules));
            int overrides = 0;
            const json overrideList = sl.value("overrides", json::object());
            for (const auto& [id, p] : overrideList.items())
            {
                slots::setOverride(id, slots::Payload::fromJson(p), true);
                ++overrides;
            }
            return json{{"rules", slots::state()->rules.size()}, {"overrides", overrides}};
        });

    // Terrain: the remix makes it (and the remixed OSM), the terrain spec's steps run on it; without a remix the spec makes it.
    std::string terrain, remixOsm;
    json origin;
    std::shared_ptr<const RemixResult> remixResult;
    if (spec.contains("remix") && !spec["remix"].is_null())
    {
        json r;
        if (st.run("remix", [&] { return run("remixCreate", {{"name", name}, {"spec", spec["remix"]}}); }, &r))
        {
            terrain = r.value("mesh", std::string());
            made(terrain);
            built["remix"] = terrain;
            built["terrain"] = terrain;
            json applied;
            if (st.run("remix.apply", [&] { return run("remixApply", {{"args", {terrain}}, {"folder", paths::toUtf8(folder / "remix")}}); }, &applied))
            {
                remixOsm = applied["files"].value("osm", std::string());
                origin = applied.value("origin", json());
                built["remixOsm"] = remixOsm;
                built["origin"] = origin;
                if (applied.contains("features") && applied["features"].contains("group"))
                {
                    built["remixFeatures"] = applied["features"]["group"];
                    // Thousands of source lines (every road, building outline and shore) drawn over the finished map bury
                    // it; the terrain, settlements and sites already carry them.
                    run("setAttr", {{"args", {applied["features"]["group"].get<std::string>() + ".visibility", false}}});
                    notes.push_back("the remixed OpenStreetMap lines (" + applied["features"]["group"].get<std::string>() +
                                    ") are hidden; show the group to see them");
                }
                // The remix overlay is an editing aid that projects every remixed line each frame: a built world
                // starts with it off (Remix Overlay turns it back on).
                run("remixView", {{"args", {terrain}}, {"overlay", false}});
            }
        }
        if (!terrain.empty() && spec.contains("terrain") && spec["terrain"].is_object() && !skipped("terrain"))
            st.run("terrain", [&] {
                json t = spec["terrain"];
                t.erase("materials");
                t["name"] = g.find(terrain)->name();
                t["source"] = {{"type", "existing"}, {"mesh", terrain}};
                if (!t.contains("check"))
                    t["check"] = false;
                if (const std::string blended = remixBathymetryFiles(spec["remix"]); !blended.empty() && t.contains("coast") && t["coast"].is_object() &&
                                                                                 t["coast"].contains("bathymetry"))
                {
                    t["coast"].erase("bathymetry");
                    notes.push_back("terrain.coast.bathymetry skipped: the remix already blended " + blended +
                                    " into the seabed, and the remixed map has no single georeference to place a grid by");
                }
                json r = run("terrainSpec", {{"spec", t}, {"stopOnError", o.stopOnError}});
                if (!r.value("ok", false) && r.contains("failed"))
                    throw std::runtime_error(r["failed"].dump());
                // The coast step's warnings name spec items it could not build (a reservoir off the map): the world's notes carry them.
                for (const json& step : r.value("steps", json::array()))
                    if (step.value("step", std::string()) == "coast")
                        for (const json& w : step.value("warnings", json::array()))
                            if (w.is_string())
                                notes.push_back("terrain.coast: " + w.get<std::string>());
                return r;
            });
    }
    else
    {
        json r;
        if (st.run("terrain", [&] {
                json t = spec["terrain"];
                t.erase("materials");
                if (!t.contains("name"))
                    t["name"] = name;
                if (!t.contains("check"))
                    t["check"] = false;
                json res = run("terrainSpec", {{"spec", t}, {"replace", o.replace}, {"stopOnError", o.stopOnError}});
                if (!res.value("ok", false) && res.contains("failed"))
                    throw std::runtime_error(res["failed"].dump());
                return res;
            }, &r))
        {
            terrain = r.value("terrain", std::string());
            made(terrain);
            built["terrain"] = terrain;
        }
    }
    if (terrain.empty() && !st.stopped())
        st.run("terrain", [] () -> json { throw std::runtime_error("no terrain was made"); });

    // Site kits first: their grading settles the ground the settlements are laid on, and their outlines keep the
    // settlements clear. Their buildings come after the settlements, in one Buildings pass.
    json siteIds = json::array();
    json siteOutlines = json::array();
    for (const json& s : spec.value("sites", json::array()))
    {
        if (skipped("sites") || terrain.empty())
            break;
        const std::string id = s.value("id", std::string());
        json made1;
        if (!st.run("site." + (id.empty() ? s["kind"].get<std::string>() : id), [&] {
                // Graded all together after the loop: one terrain change instead of one per site.
                json call = {{"kind", s["kind"]}, {"terrain", terrain}, {"culture", s.value("culture", culture)}, {"ground", false}};
                for (const char* k : {"id", "name", "seed", "at", "heading", "waterLevel", "params"})
                    if (s.contains(k) && !s[k].is_null())
                        call[k] = s[k];
                return run("siteCreate", call);
            }, &made1))
            continue;
        const std::string sid = made1.value("id", id);
        built["sites"].push_back(sid);
        if (made1.contains("mesh") && made1["mesh"].is_string())
            built["siteMeshes"].push_back(made1["mesh"]);
        try
        {
            const json info = run("siteInfo", {{"args", {sid}}});
            if (info.contains("boundary") && info["boundary"].size() >= 3)
                siteOutlines.push_back(info["boundary"]);
        }
        catch (const std::exception&)
        {
        }
        if (s.value("buildings", true))
            siteIds.push_back(sid);
    }
    if (!built["sites"].empty())
    {
        st.run("sites.grade", [&] { return run("siteGround", {{"sites", built["sites"]}}); });
        // A site's access roads and tracks reach past its outline: settlements keep clear of them too.
        try
        {
            for (const json& corridor : siteRoadCorridors(terrain))
                siteOutlines.push_back(corridor);
        }
        catch (const std::exception& e)
        {
            notes.push_back(std::string("site roads not kept clear of settlements: ") + e.what());
        }
    }

    // Settlements: each from OSM (the remix's or a file, clipped to its area) or laid out by Settlement, then its buildings.
    const json editsByGroup = spec.value("buildings", json::object()).value("edits", json::object());
    // A spec read back from a scene keys edits by the group's name there, which the naming rules may have changed.
    auto groupEdits = [&](const std::string& groupName) {
        if (editsByGroup.contains(groupName))
            return editsByGroup[groupName];
        const std::string conformed = g.namingPolicy().conform(*scene::types::find("transform"), groupName);
        return editsByGroup.value(conformed, json::object());
    };
    std::optional<OsmDoc> remixDoc;
    std::vector<glm::dvec2> remixWorld;
    // Site outlines a settlement must keep clear: only those reaching into its box (Settlement makes every exclusion a
    // district of its own, so far ones would turn up as stray districts).
    auto outlinesIn = [&](double x0, double z0, double x1, double z1) {
        json out = json::array();
        for (const json& o : siteOutlines)
        {
            double lx = 1e300, lz = 1e300, hx = -1e300, hz = -1e300;
            for (const json& p : o)
                lx = std::min(lx, p[0].get<double>()), hx = std::max(hx, p[0].get<double>()), lz = std::min(lz, p[1].get<double>()), hz = std::max(hz, p[1].get<double>());
            if (hx >= x0 && lx <= x1 && hz >= z0 && lz <= z1)
                out.push_back(o);
        }
        return out;
    };
    for (const json& s : spec.value("settlements", json::array()))
    {
        if (skipped("settlements") || terrain.empty())
            break;
        const std::string sname = s["name"].get<std::string>();
        built["settlementNames"].push_back(sname);
        json made1;
        const bool ok = st.run("settlement." + sname, [&] {
            if (s.contains("create"))
            {
                json call = s["create"];
                call["name"] = sname;
                if (call.contains("outline") && call["outline"].is_array())
                {
                    double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
                    for (const json& p : call["outline"])
                        x0 = std::min(x0, p[0].get<double>()), x1 = std::max(x1, p[0].get<double>()), z0 = std::min(z0, p[1].get<double>()), z1 = std::max(z1, p[1].get<double>());
                    json ex = call.value("exclusions", json::array());
                    for (const json& outline : outlinesIn(x0, z0, x1, z1))
                        ex.push_back(outline);
                    if (!ex.empty())
                        call["exclusions"] = ex;
                }
                if (!call.contains("ground"))
                    call["ground"] = terrain;
                return run("settlementCreate", call);
            }
            std::string file = s["osm"].get<std::string>() == "remix" ? remixOsm : s["osm"].get<std::string>();
            if (file.empty())
                throw std::runtime_error("osm 'remix' but the remix wrote no OSM");
            if (s.contains("area"))
            {
                const json& a = s["area"];
                const double x0 = a[0].get<double>(), z0 = a[1].get<double>(), x1 = a[2].get<double>(), z1 = a[3].get<double>();
                OsmDoc local;
                std::vector<glm::dvec2> world;
                if (file == remixOsm)
                {
                    if (!remixDoc)
                    {
                        remixDoc = readOsmDoc(paths::fromUtf8(remixOsm));
                        remixWorld = worldPositions(*remixDoc, terrain, origin);
                    }
                    local = *remixDoc;
                    world = remixWorld;
                }
                else
                {
                    local = readOsmDoc(paths::fromUtf8(file));
                    world = worldPositions(local, terrain, origin);
                }
                std::function<bool(glm::dvec2)> keep;
                std::shared_ptr<const RemixResult> present;
                if (s.contains("within") && spec.contains("remix"))
                {
                    present = cachedRemix(specFromJson(spec["remix"]));
                    const std::string mask = s["within"].get<std::string>();
                    if (!present->after.findMask(mask))
                        throw std::runtime_error("within: the remix has no mask " + mask + " (remixPresent_<id> needs that source's extent builtUp)");
                    keep = [present, mask](glm::dvec2 p) { return present->after.maskAt(mask, p.x, p.y) >= 0.5; };
                }
                const OsmDoc clipped = clipOsm(local, world, {x0, z0}, {x1, z1}, keep);
                const fs::path f = folder / "settlements" / (sname + ".osm.json");
                fs::create_directories(f.parent_path());
                std::ofstream(f, std::ios::binary) << writeOsmDocJson(clipped);
                file = paths::toUtf8(f);
            }
            json call = {{"args", {file}}, {"ground", terrain}, {"name", sname}, {"culture", s.value("culture", culture)}};
            if (!origin.is_null())
                call["origin"] = origin;
            if (s.contains("area"))
            {
                const json near = outlinesIn(s["area"][0].get<double>(), s["area"][1].get<double>(), s["area"][2].get<double>(), s["area"][3].get<double>());
                if (!near.empty())
                    call["exclusions"] = near;
            }
            for (const char* k : {"profile", "overrides", "fill", "coverage", "margin", "within"})
                if (s.contains(k))
                    call[k] = s[k];
            return run("settlementFromOsm", call);
        }, &made1);
        if (!ok)
            continue;
        const std::string mesh = made1.value("mesh", sname);
        made(mesh);
        built["settlements"].push_back(mesh);
        if (made1.contains("import") && made1["import"].contains("curves") && made1["import"]["curves"].contains("group"))
            made(made1["import"]["curves"]["group"].get<std::string>());
        const json b = s.value("buildings", json(false));
        if (!(b.is_object() || (b.is_boolean() && b.get<bool>())) || skipped("buildings"))
            continue;
        const std::string groupName = g.find(mesh) ? g.find(mesh)->name() + "_Buildings" : sname + "_Buildings";
        st.run("buildings." + sname, [&] {
            seedEdits(groupName, groupEdits(groupName));
            const std::string lod = b.is_object() && b.contains("lod") ? b["lod"].get<std::string>() : spec.value("buildings", json::object()).value("lod", std::string());
            if (!lod.empty())
                seedLod(groupName, lod);
            json call = {{"args", {mesh}}, {"route", "buildings"}};
            if (b.is_object())
            {
                if (b.contains("style"))
                    call["style"] = b["style"];
                if (b.contains("cell"))
                    call["cell"] = b["cell"];
            }
            json r = run("settlementBuildings", call);
            json out = r.value("buildings", r);
            // The buildings stand on the lots: their outlines and the envelopes' footprints would only draw under them.
            if (const std::string mod = made1.value("modifier", json()).is_string() ? made1["modifier"].get<std::string>() : std::string(); !mod.empty())
                for (const char* attr : {"showLots", "showBuildings"})
                    run("setAttr", {{"args", {mod + "." + attr, false}}});
            if (b.is_object() && b.contains("districts") && b["districts"].is_object() && !b["districts"].empty())
            {
                const json names = run("settlementNames", {{"args", {mesh}}});
                json styled = json::object();
                for (const json& d : names.value("districts", json::array()))
                {
                    const std::string dn = d.value("name", std::string());
                    const std::string key = b["districts"].contains(dn) ? dn : std::to_string(d.value("id", -1));
                    if (!b["districts"].contains(key) || !d.contains("outline"))
                        continue;
                    run("buildingStyle", {{"style", b["districts"][key]}, {"polygon", d["outline"]}});
                    styled[dn] = b["districts"][key];
                }
                out["districtStyles"] = styled;
            }
            return out;
        });
        if (Node* grp = findAsked(groupName))
        {
            built["buildingGroups"].push_back(grp->path());
            made(grp->path());
            makeEnterable(st, groupName, groupEdits(groupName));
        }
        if (s.contains("furniture") && !(s["furniture"].is_boolean() && !s["furniture"].get<bool>()))
            st.run("furniture." + sname, [&] {
                json call = s["furniture"].is_object() ? s["furniture"] : json::object();
                call["args"] = {mesh};
                return run("settlementFurniture", call);
            });
    }

    if (!siteIds.empty() && !skipped("buildings") && ops::find("buildingCreate"))
        st.run("buildings.sites", [&] {
            seedEdits("SiteBuildings", groupEdits("SiteBuildings"));
            if (const std::string lod = spec.value("buildings", json::object()).value("lod", std::string()); !lod.empty())
                seedLod("SiteBuildings", lod);
            json call = {{"args", siteIds}};
            json r = run("siteBuildings", call);
            if (r.contains("group") && r["group"].is_string())
            {
                built["buildingGroups"].push_back(r["group"]);
                made(r["group"].get<std::string>());
            }
            return r;
        });
    else if (!siteIds.empty() && !ops::find("buildingCreate"))
        notes.push_back("the Buildings extension is not loaded: site buildings stay greybox massing");
    if (!siteIds.empty() && !skipped("buildings") && ops::find("buildingCreate"))
        makeEnterable(st, "SiteBuildings", groupEdits("SiteBuildings"));

    // Style calls over everything built.
    for (const json& c : spec.value("buildings", json::object()).value("styles", json::array()))
        if (!skipped("buildings"))
            st.run("buildings.style", [&] { return run("buildingStyle", c); });

    // Enterable buildings, picked by program, landmark, style or id once every group is built.
    if (const json en = spec.value("buildings", json::object()).value("enterable", json()); !en.is_null() && !skipped("buildings") && ops::find("buildingMakeEnterable"))
        for (const json& pick : en.is_array() ? en : json::array({en}))
            st.run("buildings.enterable", [&] {
                json call = json::object();
                for (const char* k : {"programs", "landmarks", "styles", "limit", "program", "furnish"})
                    if (pick.contains(k))
                        call[k] = pick[k];
                if (pick.contains("ids"))
                    call["building"] = pick["ids"];
                json r = run("buildingMakeEnterable", call);
                json brief = {{"count", r.value("count", 0)}, {"buildings", json::array()}};
                for (const json& b : r.value("buildings", json::array()))
                    brief["buildings"].push_back({{"building", b.value("building", std::string())}, {"program", b.value("program", std::string())},
                                                  {"rooms", b.value("rooms", 0)}, {"entries", b.value("entries", json::array()).size()}});
                if (r.contains("note"))
                    brief["note"] = r["note"];
                return brief;
            });

    // Material layers last, over the finished ground (site pads, roads, settlements): the spec's own, else defaults fitted
    // to the terrain; without them the terrain and its seabed draw in one flat white.
    if (const json ts = spec.value("terrain", json::object()); !terrain.empty() && !skipped("terrain"))
        st.run("terrain.materials", [&] {
            const bool own = ts.is_object() && ts.contains("materials") && ts["materials"].is_object();
            json call = own ? ts["materials"] : json{{"layers", "default"}};
            call["args"] = {terrain};
            const json sea = ts.is_object() ? ts.value("water", json::object()).value("sea", json::object()) : json::object();
            if (!own && sea.contains("level") && sea["level"].is_number())
                call["seaLevel"] = sea["level"];
            json r = run("terrainMaterialRules", call);
            if (!own)
                notes.push_back("terrain materials: default layers fitted to the terrain (terrain.materials in the spec sets your own)");
            return r;
        });

    // Zones: the set's spec, then the generators (districts, water, terrain masks), then the play area.
    if (spec.contains("zones") && spec["zones"].is_object() && !skipped("zones") && !terrain.empty())
    {
        const json& z = spec["zones"];
        const std::string setName = z.value("name", std::string("Zones"));
        auto setFlags = [&](json call) {
            if (Node* n = findAsked(setName, "zoneSet"))
                call["set"] = n->path();
            else
            {
                call["setName"] = setName;
                call["ground"] = terrain;
            }
            return call;
        };
        json spec1 = z.value("spec", json());
        if (spec1.is_object() && !spec1.contains("culture"))
            spec1["culture"] = culture;
        if (spec1.is_object())
            st.run("zones.spec", [&] { return run("zoneSpec", setFlags({{"spec", spec1}})); });
        else
            st.run("zones.set", [&] { return run("zoneSpec", setFlags({{"spec", {{"culture", culture}, {"layers", json::array()}}}})); });
        const json fd = z.value("fromDistricts", json(false));
        if (fd.is_array() || (fd.is_boolean() && fd.get<bool>()))
            for (const json& sm : built["settlements"])
            {
                const std::string sn = g.find(sm.get<std::string>()) ? g.find(sm.get<std::string>())->name() : sm.get<std::string>();
                if (fd.is_array() && std::find(fd.begin(), fd.end(), json(sn)) == fd.end())
                    continue;
                st.run("zones.districts." + sn, [&] { return run("zoneFromDistricts", setFlags({{"settlement", sm}})); });
            }
        json fw = z.value("fromWater", json(false));
        if (fw.is_boolean())
            fw = fw.get<bool>() ? json::array({json::object()}) : json::array();
        else if (fw.is_object())
            fw = json::array({fw});
        for (const json& c : fw)
            st.run("zones.water", [&] {
                json call = setFlags(c);
                call["terrain"] = terrain;
                return run("zoneFromWater", call);
            });
        for (const json& c : z.value("fromTerrain", json::array()))
            st.run("zones.terrain", [&] {
                json call = setFlags(c);
                call["terrain"] = terrain;
                return run("zoneFromTerrain", call);
            });
        if (z.contains("playArea"))
            st.run("zones.playArea", [&] { return run("zonePlayArea", setFlags({{"polygon", z["playArea"]}})); });
        // Must-cover layers (districts, police) filled out to the play area unless the spec says otherwise.
        const json fg = z.value("fillGaps", json(true));
        if (Node* setNode = findAsked(setName, "zoneSet"); setNode && !(fg.is_boolean() && !fg.get<bool>()))
        {
            const zones::ZoneSpec now = zones::zonesFromJson(run("zoneSpec", {{"set", setNode->path()}}));
            for (const zones::Layer& l : now.layers)
                if (l.mustCover && (fg.is_boolean() || std::find(fg.begin(), fg.end(), json(l.id)) != fg.end()))
                    st.run("zones.fill." + l.id, [&] { return fillLayerGaps(setNode->path(), terrain, l.id); });
        }
        if (Node* n = findAsked(setName, "zoneSet"))
        {
            built["zones"] = n->path();
            made(n->path());
        }
    }

    // Pins last: they freeze what the build made.
    if (spec.contains("slots") && spec["slots"].contains("pins") && !skipped("slots"))
        st.run("slots.pins", [&] {
            int n = 0;
            json missed = json::array();
            for (const json& k : spec["slots"]["pins"])
                if (k.is_string())
                {
                    if (slots::pin(g, k.get<std::string>()))
                        ++n;
                    else
                        missed.push_back(k);
                }
            return json{{"pinned", n}, {"nothingMatched", missed}};
        });

    if (o.replace)
        if (const int stale = dropStaleCells(); stale > 0)
            notes.push_back("replace: " + std::to_string(stale) + " Buildings modifiers an earlier build left without a mesh held this build's buildings; removed");
    recordMade(existing, built);

    // The world node: the spec it was built from and what was made, for worldSpec, worldCheck and a rebuild.
    const std::string node = pathOfResult(run("group", {{"empty", true}, {"name", worldNodeName(name)}}));
    built["folder"] = paths::toUtf8(folder);
    // Link ids from the world's name and each node's path: a rebuild keys its Unreal placements as this build does.
    std::vector<std::string> linkRoots;
    for (const char* k : {"nodes", "siteMeshes"})
        for (const json& p : built[k])
            if (p.is_string())
                linkRoots.push_back(p.get<std::string>());
    built["linkIds"] = assignWorldLinkIds(name, linkRoots);
    setStringAttr(node, kSpecAttr, spec.dump(), "The world spec this world was built from (worldSpec reads it back).");
    setStringAttr(node, kBuiltAttr, built.dump(), "What worldBuild made: terrain, remix, settlements, sites, zones, building groups, files.");
    WorldRef w{node, spec, built};

    json check;
    const json checkFlags = spec.value("check", json(true));
    if (!st.stopped() && !(checkFlags.is_boolean() && !checkFlags.get<bool>()))
        st.run("check", [&] { return checkWorld(&w, checkFlags.is_object() ? checkFlags : json::object()); }, &check);

    json exports = json::object();
    const json ex = spec.value("export", json::object());
    const std::string target = ex.value("target", std::string("none"));
    if (target != "none" && !st.stopped())
    {
        const fs::path out = ex.contains("folder") ? paths::fromUtf8(ex["folder"].get<std::string>()) : folder / "export";
        st.run("export.terrain", [&] {
            json r = run("terrainExport", {{"args", {terrain}}, {"target", target == "unreal" ? "unreal" : "files"}, {"folder", paths::toUtf8(out / "terrain")}});
            exports["terrain"] = r.value("files", json());
            return r;
        });
        if (built.contains("zones"))
            st.run("export.zones", [&] {
                json r = run("zoneExport", {{"set", built["zones"]}, {"folder", paths::toUtf8(out / "zones")}});
                exports["zones"] = r.value("json", json());
                return r;
            });
        const json mapFlags = ex.value("map", json(true));
        if (!(mapFlags.is_boolean() && !mapFlags.get<bool>()))
            st.run("export.map", [&] {
                json f = mapFlags.is_object() ? mapFlags : json::object();
                f["folder"] = paths::toUtf8(out / "map");
                json r = writeWorldMap(&w, f);
                exports["map"] = {{"image", r["image"]}, {"json", r["json"]}};
                return r;
            });
        if (target == "unreal")
            exports["unreal"] = sendWorldToUnreal(name, built, ex.value("unreal", json::object()),
                                                  [&](const std::string& step, const std::function<json()>& fn) { return st.run(step, fn); });
    }

    json out = {{"world", node},
                {"terrain", terrain},
                {"steps", st.list()},
                {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()},
                {"memoryMB", {{"start", std::round(mem0)}, {"end", std::round(memoryMB())}, {"peak", std::round(peakMemoryMB())}}},
                {"built", built},
                {"notes", notes}};
    if (!check.is_null())
        out["check"] = {{"ok", check.value("ok", false)}, {"counts", check.value("counts", json::object())}, {"issues", Steps::summarize(check.value("issues", json::array()))}};
    if (!exports.empty())
        out["exports"] = exports;
    out["ok"] = !st.failed() && (check.is_null() || check.value("ok", false));
    if (st.failed())
        out["failed"] = st.firstError();
    return out;
}

// Read back -------------------------------------------------------------------------------------------------------------

json readBack(const std::optional<WorldRef>& w)
{
    Graph& g = Graph::current();
    json spec = w ? w->spec : json::object();
    json from = json::object();
    json notes = json::array();
    if (!w)
        notes.push_back("no world node in the scene: the spec is read from the parts alone (settlements and export are not known)");
    if (!spec.contains("name"))
        spec["name"] = "World";

    const std::string remix = w ? w->built.value("remix", std::string()) : std::string();
    if (!remix.empty() && g.find(remix))
    {
        spec["remix"] = run("remixSpec", {{"args", {remix}}});
        from["remix"] = "scene";
    }
    const std::string terrain = worldTerrain(w ? &*w : nullptr);
    if (!terrain.empty())
        try
        {
            const json t = run("terrainSpec", {{"args", {terrain}}});
            if (t.value("from", std::string()) == "spec")
            {
                json ts = t["spec"];
                if (!remix.empty())
                    ts.erase("source");
                spec["terrain"] = ts;
                from["terrain"] = "scene";
            }
            else
            {
                from["terrain"] = "stored";
                notes.push_back("the terrain changed after it was built from its spec; terrainSpec('" + terrain + "') gives its node group");
            }
        }
        catch (const std::exception& e)
        {
            notes.push_back(std::string("terrain not read: ") + e.what());
        }

    // Slots: what the scene holds now.
    const slots::StatePtr st = slots::state();
    json rules = json::array(), overrides = json::object(), pins = json::array();
    for (const slots::Rule& r : st->rules)
        rules.push_back(r.toJson());
    for (const auto& [id, ov] : st->overrides)
        overrides[id] = ov.payload.toJson();
    for (const auto& [k, p] : st->pins)
        pins.push_back(k);
    if (!rules.empty() || !overrides.empty() || !pins.empty() || spec.contains("slots"))
    {
        spec["slots"] = {{"rules", rules}, {"overrides", overrides}, {"pins", pins}};
        from["slots"] = "scene";
    }

    // Sites as they stand, with whether the Buildings generator builds them.
    if (ops::find("siteList"))
    {
        json sites = json::array();
        for (const json& s : run("siteList", json::object()))
        {
            json info = run("siteInfo", {{"args", {s["mesh"]}}});
            json e = info.value("spec", json::object());
            e["buildings"] = false;
            if (Node* mesh = g.find(s.value("mesh", std::string())))
                for (scene::NodeId m : scene::modifiers::stack(g, mesh->id()))
                    if (Node* mod = g.node(m); mod && mod->type().attrIndex("massing") >= 0 && mod->type().attrIndex("siteSpec") >= 0)
                    {
                        const json massing = run("getAttr", {{"args", {mod->path() + ".massing"}}});
                        e["buildings"] = massing.is_boolean() && !massing.get<bool>();
                    }
            sites.push_back(std::move(e));
        }
        if (!sites.empty() || spec.contains("sites"))
        {
            spec["sites"] = sites;
            from["sites"] = "scene";
        }
    }

    // Zones: the set's spec; the generators stay as stored (re-running them replaces only what they made).
    const std::string zset = w && w->built.contains("zones") ? w->built["zones"].get<std::string>() : (zoneSetNodes().size() == 1 ? zoneSetNodes()[0] : std::string());
    if (!zset.empty() && g.find(zset))
    {
        json z = spec.value("zones", json::object());
        z["name"] = g.find(zset)->name();
        z["spec"] = run("zoneSpec", {{"set", zset}});
        spec["zones"] = z;
        from["zones"] = "scene";
    }

    // Buildings edits by group.
    json edits = json::object();
    for (const auto& [grp, e] : buildingEdits())
        if (!e.empty() && g.find(grp))
            edits[g.find(grp)->name()] = e;
    if (!edits.empty() || spec.contains("buildings"))
    {
        json b = spec.value("buildings", json::object());
        b["edits"] = edits;
        spec["buildings"] = b;
        from["buildings"] = "scene";
    }
    for (const char* k : {"settlements", "export", "check", "culture"})
        if (spec.contains(k))
            from[k] = "stored";
    return {{"spec", spec}, {"from", from}, {"notes", notes}, {"world", w ? json(w->node) : json()}};
}

std::string worldArg(const Args& a) { return a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>(); }

const char* kGuide = R"(# Open World in Lila

A whole open-world map from one JSON: a World Remix of real places (fictional names everywhere), the terrain with its coast and water, settlements from the remixed OpenStreetMap with procedural buildings, site kits (ports, cemeteries, airstrips, farms, villages, compounds, islands, lighthouses), slot rules that swap every placed part, and zones for gameplay. Lengths cm, Y up, positions [x, z].

## Recipe

1. Read lila://schema/world-spec (and the section schemas it names: lila://schema/remix-spec, lila://schema/terrain-spec, lila://schema/site-kits, lila://schema/zones).
2. Write the spec. Start small: one remix source, one settlement, two sites. `worldSpec(spec=s)` checks it without building.
3. Real data: remix sources with a bbox and file paths get their files in the same call. `worldBuild(file='world.json', estimate=True)['estimate']` lists each missing file with its size; `worldBuild(file='world.json', fetch=True)` downloads them to the paths the spec names (relative to the spec file), then builds. Over 25 MB (or on a metered connection) needs `consent=True`: ask the user first. GEBCO bathymetry needs an OpenTopography key (`apiKey='...', saveKey=True` once). `worldFetch` downloads without building.
4. Build: `r = worldBuild(spec=s)` (or `worldBuild(file='world.json')`, relative paths from the file's folder). Read `r['steps']`: every step's ok, seconds, memoryMB and error. `replace=True` rebuilds a world of the same name.
5. Check: `c = worldCheck(report=True)`. Fix by kind (below) and rebuild, until `c['ok']`. `c['placeholders']` is swap progress: slots still on placeholders by type.
6. Overview map: `worldMap(resolution=2048)` writes a PNG and JSON (districts, zones, settlements, POIs, roads, water) for an in-game map.
7. Read back after hand edits: `worldSpec(file='world.json')` writes the spec the scene now holds (remix, terrain, slots, sites, zones and Buildings edits from the scene; settlements and export as built).

## Sections in build order

- `slots`: rules (`slotRule(rule=)` dicts), overrides `{slot id: payload}`, pins (applied last).
- `remix`: makes the terrain and the remixed OSM; `terrain` steps (water, coast, biomes, groundCover) then run on it.
- `settlements`: `{name, osm: 'remix', area: [x0, z0, x1, z1], profile, fill, buildings: {style, districts: {name: style}}, furniture}` or `{name, create: {settlementCreate flags}}`.
- `sites`: `{kind, id, at: [x, z], heading, params, buildings: true}`; site buildings come from the Buildings generator (group SiteBuildings).
- `buildings`: `edits` `{group: {building id: edit}}` (seeded before each group builds) and `styles` (buildingStyle calls).
- `zones`: `{name, spec, fromDistricts: true, fromWater: true, playArea}`.
- `export`: `{target: files | unreal, map: true, unreal: {folder, interiors, roads, roadMesh, pcgGraph, landscapeMaterial}}`. unreal sends the terrain (landscape, water, routes, roads), each settlement, the buildings (shells, modules, slots, HLOD proxies, interiors), the site kits and the zones into the open level, each as a step saying what it sent, what the level already held unchanged and what it skipped. Every node the world makes has a link id from the world's name and its path, so a rebuild with replace=True sends only what changed and removes the world's groups it no longer makes (step `export.unreal.stale`).

## worldCheck kinds

- realName: output text matching a real name (imported or the brand blocklist). Rename it (siteEdit name=, zoneEdit name='', settlementNames) or let the generator pick again.
- terrain / water / unreachableWater: terrainCheck and waterCheck (channels too shallow or narrow, lanes disconnected, harbours unreachable).
- remix: rivers that no longer drain, dangling roads, coast gaps, folds.
- settlement, building, site, zone (gaps, overlaps, numbered names), slots (orphans, duplicates).
- buildingOverWater / buildingOverRoad: a building's footprint on water or a road surface.
- propFloating / propBuried: a ground slot off the terrain by more than the tolerance.
- buildingsStale: a settlement changed after its buildings were made; `buildingRefresh()` rebuilds them keeping edits.
- interiorNoEntry: an enterable building without an entry (from buildingCheck).
- placeholders (info): slots still drawn as placeholders, by type.

## Settlement changes

Settlement edits do not rebuild buildings live. `buildingRefresh()` (Level > Buildings > Refresh Buildings) rebuilds every group whose settlement changed; edits, overrides and pins carry over by building id. worldCheck lists stale groups.
)";

const char* kPrompt = R"(Build an open-world map in Lila from this description, check it and export it.

Description: {{description}}
Size: {{size}}. Real places to remix: {{places}}. Target engine: {{engine}}.

1. Read lila://guide/open-world, then lila://schema/world-spec and the section schemas it names.
2. Real data: remixSource(fetch=True) downloads DEM, OSM and land cover for a bbox and shows the size first; ask the user before any large download. Without data, build from files the user gives or a noise terrain.
3. Write one world spec: remix (sources placed, the inland city cut and moved toward the coast), terrain steps (sea, coast: bathymetry, masks, mangroves, channels, reservoirs, routes, airstrips; biomes, groundCover), settlements with osm 'remix' and an area each, buildings with styles, sites (port, cruise, lighthouse, cemetery, airstrip, farm, village, compound, island), slot rules, zones (fromDistricts, fromWater).
4. Check the spec with worldSpec(spec=s), then r = worldBuild(spec=s). Read every step in r['steps']; fix and rebuild with replace=True.
5. c = worldCheck(report=True). Fix each kind (lila://guide/open-world lists them) until c['ok']. Report c['placeholders'] as swap progress.
6. worldMap(resolution=2048) for the overview, then export={'target': '{{engine}}'} in the spec (or terrainExport, zoneExport, buildingSend).
7. Report the steps' times and memory, the check, the files and what is still on placeholders.
)";

std::string fillPrompt(std::string text, const json& args)
{
    const std::pair<const char*, const char*> keys[] = {{"description", "a coastal crime-game region"},
                                                        {"size", "about 12 km by 12 km of land plus sea"},
                                                        {"places", "none given (ask, or build from noise)"},
                                                        {"engine", "unreal"}};
    for (const auto& [key, fallback] : keys)
    {
        const char* argName = std::string(key) == "engine" ? "target" : key;
        const std::string value = args.contains(argName) && args[argName].is_string() && !args[argName].get<std::string>().empty() ? args[argName].get<std::string>() : fallback;
        const std::string tag = std::string("{{") + key + "}}";
        for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + value.size()))
            text.replace(at, tag.size(), value);
    }
    return text;
}
} // namespace

void registerWorld(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "worldBuild",
        .doc = "Build a whole open-world map from one world spec (lila://schema/world-spec): slot rules and overrides, the World "
               "Remix and its remixed OSM, the terrain spec's steps (water, coast, biomes, ground cover), settlements from the "
               "remix's OSM clipped to each one's area (or laid out by Settlement), their procedural buildings with style "
               "assignments and edits, site kits with their buildings, zones (spec, districts, water), pins, worldCheck and the "
               "export. Each step is timed and measured (seconds, memoryMB) in a Tracy zone. The world node keeps the spec and "
               "what was made, so worldSpec reads it back and replace=True rebuilds it. Returns ok, every step's result or error, "
               "the check, the exports, total time and memory. fetch=True first downloads the real data the spec names and lacks "
               "(worldFetch: sizes shown, over 25 MB needs consent=True, files written to the spec's own paths); without it a "
               "missing data file stops the build before it starts, naming the file.",
        .flags = {{"spec", "s", T::Any, nullptr, "The world spec, a dict."},
                  {"file", "f", T::String, nullptr, "A world spec JSON file; relative paths in it are from its folder."},
                  {"replace", "r", T::Bool, false, "Delete an earlier build of this world first."},
                  {"stopOnError", "soe", T::Bool, true, "Stop at the first failed step."},
                  {"folder", "fo", T::String, nullptr, "Where files go; default the workspace's openworld/world/<name>."},
                  {"skip", "sk", T::List, nullptr, "Sections to leave out: slots, terrain, settlements, buildings, sites, zones."},
                  {"check", "ch", T::Bool, nullptr, "Run worldCheck at the end (default the spec's check, else true)."},
                  {"fetch", "fe", T::Bool, false, "Download the spec's missing real data to the paths it names first (worldFetch)."},
                  {"estimate", "es", T::Bool, false, "Only list the missing data and its download sizes; build nothing."},
                  {"consent", "cn", T::Bool, false, "With fetch: allow more than 25 MB, or downloads on a metered connection."},
                  {"offline", "off", T::Bool, false, "Take missing data from the download cache only."},
                  {"apiKey", "ak", T::String, "", "With fetch: OpenTopography API key (for GEBCO); default the saved one. Never logged."},
                  {"saveKey", "svk", T::Bool, false, "With fetch: save apiKey for later fetches."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            json spec;
            if (a.has("file"))
                spec = loadSpecFile(a.get<std::string>("file"), "worldBuild");
            else if (a.has("spec"))
                spec = a.flags["spec"].is_string() ? json::parse(a.get<std::string>("spec")) : a.flags["spec"];
            else
                throw CommandError("worldBuild: give spec= or file=");
            if (a.has("check"))
                spec["check"] = a.get<bool>("check");
            FetchFlags f;
            f.estimate = a.get<bool>("estimate", false);
            f.consent = a.get<bool>("consent", false);
            f.offline = a.get<bool>("offline", false);
            f.apiKey = a.get<std::string>("apiKey", "");
            f.saveKey = a.get<bool>("saveKey", false);
            if (f.estimate)
                return fetchWorldData(spec, f, "worldBuild");
            json fetched;
            if (a.get<bool>("fetch", false) || f.offline)
                fetched = fetchWorldData(spec, f, "worldBuild");
            else if (const std::string missing = missingData(spec); !missing.empty())
                throw CommandError("worldBuild: " + missing);
            BuildOptions o;
            o.replace = a.get<bool>("replace", false);
            o.stopOnError = a.get<bool>("stopOnError", true);
            o.folder = a.get<std::string>("folder", "");
            if (a.has("skip"))
                for (const json& k : a.flags["skip"])
                    o.skip.push_back(k.get<std::string>());
            json out = buildWorld(std::move(spec), o);
            if (!fetched.is_null())
                out["fetch"] = std::move(fetched);
            return out;
        },
        .examples = {"r = worldBuild(file='worlds/sinaloa.json')\nr['ok'], [(s['step'], s['seconds'], s['memoryMB']) for s in r['steps']]",
                     "worldBuild(file='worlds/sinaloa_real.json', estimate=True)['estimate']['summary']",
                     "worldBuild(file='worlds/sinaloa_real.json', fetch=True, consent=True, replace=True)",
                     "worldBuild(spec={'name': 'Tiny', 'terrain': {'size': [400000, 400000], 'resolution': 257, 'source': {'type': 'noise'}},\n"
                     "    'sites': [{'kind': 'village', 'id': 'v1', 'at': [0, 0]}]}, replace=True)"},
    });

    ctx.registerCommand({
        .name = "worldSpec",
        .doc = "Read a built world back as one world spec, or check a spec without building it. With a world (or the only one): the "
               "spec it was built from with the remix, terrain, slot rules/overrides/pins, sites, zones and Buildings edits as the "
               "scene holds them now (from says which sections came from the scene and which are as built); file= writes it. "
               "With spec= or specFile=: checks the spec's shape and returns it with relative paths resolved. Returns spec, from "
               "and notes.",
        .positionalDoc = "world node or name (default the only world)",
        .flags = {{"spec", "s", T::Any, nullptr, "A spec to check."},
                  {"specFile", "sf", T::String, nullptr, "A spec file to check."},
                  {"file", "f", T::String, nullptr, "Write the read-back spec to this JSON file."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "worldSpec";
            if (a.has("spec") || a.has("specFile"))
            {
                json spec = a.has("specFile") ? loadSpecFile(a.get<std::string>("specFile"), cmd) : a.flags["spec"];
                try
                {
                    validateSpec(spec);
                }
                catch (const std::exception& e)
                {
                    return {{"ok", false}, {"error", e.what()}};
                }
                return {{"ok", true}, {"spec", spec}};
            }
            const std::string name = worldArg(a);
            std::optional<WorldRef> w = findWorld(name);
            if (!name.empty() && !w)
                throw CommandError(std::string(cmd) + ": no world named " + name);
            json out = readBack(w);
            if (a.has("file"))
            {
                const fs::path p = paths::resolvePath(a.get<std::string>("file"));
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
                std::ofstream f(p, std::ios::binary);
                if (!f)
                    throw CommandError(std::string(cmd) + ": can't write " + paths::toUtf8(p));
                f << out["spec"].dump(2);
                out["file"] = paths::toUtf8(p);
            }
            return out;
        },
        .examples = {"s = worldSpec()['spec']\ns['sites'][0]['params']['tiers'] = 6\nworldBuild(spec=s, replace=True)",
                     "worldSpec(file='worlds/now.json')", "worldSpec(spec={'name': 'Test', 'terrain': {}})['ok']"},
    });

    registerCheckCommands(ctx);
    registerFetchCommands(ctx);

    mcp::Server& server = mcp::Server::get();
    server.addResource(ctx, {.uri = "lila://schema/world-spec",
                             .name = "World spec schema",
                             .description = "JSON schema of the world spec worldBuild builds and worldSpec reads back: remix, terrain, settlements, "
                                            "buildings, slots, sites, zones, check, export.",
                             .mimeType = "application/json",
                             .read = [] { return worldSpecSchema().dump(2); }});
    server.addResource(ctx, {.uri = "lila://guide/open-world",
                             .name = "Open World guide",
                             .description = "Building a whole open-world map over MCP: the world spec recipe, build order, worldCheck kinds and fixes, the overview map.",
                             .read = [] { return std::string(kGuide); }});
    server.addPrompt(ctx, {.name = "make_open_world",
                           .description = "Build an open-world map from a description: remix real places, settlements, buildings, sites, zones; check and export.",
                           .arguments = json::array({
                               {{"name", "description"}, {"description", "The map, e.g. \"coastal city with a port, an inland capital, foothill villages and a farm valley\""}, {"required", true}},
                               {{"name", "size"}, {"description", "Land extent, e.g. \"12 km by 12 km\""}, {"required", false}},
                               {{"name", "places"}, {"description", "Real places to remix, e.g. \"Mazatlan and Culiacan\""}, {"required", false}},
                               {{"name", "target"}, {"description", "Engine to export for: unreal or files"}, {"required", false}},
                           }),
                           .get = [](const json& args) { return fillPrompt(kPrompt, args); }});
}
} // namespace openworld::world
