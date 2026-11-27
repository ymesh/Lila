// Site kit commands: create a site from a kind and its settings, edit it, grade the terrain under it, list its envelopes
// for a building generator, and check it. A site is a mesh whose Site Kit node lays it out from the siteSpec input, so
// every edit is a setAttr and undoes; the Terrain calls that grade it are kept by id under the site's owner id.

#include "OpenWorld.h"
#include "SitesNode.h"
#include "WorldScene.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mcp.h>
#include <lila/Mesh.h>
#include <lila/Names.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Slots.h>
#include <lila/Transform.h>
#include <lila/Undo.h>

#include <glm/ext/matrix_relational.hpp>
#include <glm/ext/vector_relational.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

using namespace lila;

namespace openworld
{
namespace
{
using sites::P2;
using sites::Ring;
using AT = ArgType;
using scene::Graph;
using scene::Node;

constexpr const char* kSiteSpecAttr = "siteSpec";
constexpr const char* kGroundAttr = "siteGround";
/** Terrain layers for the site's stamps and its roads (a group can't hold a stamp and a road layer of one name). */
constexpr const char* kLayer = "Sites";
constexpr const char* kRoadLayer = "SiteRoads";

struct SiteRef
{
    std::string mesh, modifier, group;
    uint64_t modifierId = 0;
    sites::Spec spec;
    json ground = json::object();
};

bool isSiteModifier(const Node& n) { return gn::isGnModifier(n) && n.type().attrIndex(kSiteSpecAttr) >= 0; }

json attrText(const std::string& plug)
{
    const json v = ops::execute("getAttr", {{"args", {plug}}});
    const std::string s = v.is_string() ? v.get<std::string>() : std::string("{}");
    return json::parse(s.empty() ? std::string("{}") : s);
}

/** Kit names steer clear of the real names imported into the scene (layouts are built without it). */
void refreshAvoid() { sites::setAvoidNames(names::sceneRealNames(Graph::current()).hashes()); }

SiteRef readSite(Graph& g, Node& mod, const std::string& cmd)
{
    SiteRef r;
    r.modifier = mod.path();
    r.modifierId = mod.id();
    const scene::NodeId mesh = scene::modifiers::meshOf(g, mod.id());
    r.mesh = mesh != scene::kNoNode ? g.node(mesh)->path() : std::string();
    if (Node* grp = gn::groupOfModifier(g, mod))
        r.group = grp->name();
    try
    {
        r.spec = sites::specFromJson(attrText(r.modifier + "." + kSiteSpecAttr));
        r.ground = attrText(r.modifier + "." + kGroundAttr);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": the site's spec on " + r.modifier + " is broken: " + e.what());
    }
    return r;
}

std::vector<Node*> siteModifiers(Graph& g)
{
    std::vector<Node*> out;
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && isSiteModifier(*n) && scene::modifiers::meshOf(g, n->id()) != scene::kNoNode)
            out.push_back(n);
    return out;
}

std::vector<SiteRef> allSites(const std::string& cmd)
{
    Graph& g = Graph::current();
    std::vector<SiteRef> out;
    for (Node* n : siteModifiers(g))
        out.push_back(readSite(g, *n, cmd));
    return out;
}

Node* siteModifierOf(Graph& g, Node* n)
{
    if (!n)
        return nullptr;
    if (isSiteModifier(*n))
        return n;
    for (scene::NodeId id : scene::modifiers::stack(g, n->id()))
        if (Node* m = g.node(id); m && isSiteModifier(*m))
            return m;
    return nullptr;
}

/** The site a name means: a site mesh, its modifier or a site id; empty takes the selection, then the only site. */
SiteRef findSite(const std::string& name, const std::string& cmd)
{
    Graph& g = Graph::current();
    Node* mod = nullptr;
    if (!name.empty())
    {
        mod = siteModifierOf(g, g.find(name));
        if (!mod)
            for (Node* n : siteModifiers(g))
                if (readSite(g, *n, cmd).spec.id == name)
                    mod = n;
        if (!mod)
            throw CommandError(cmd + ": no site named '" + name + "' (a site mesh, its modifier or its id)");
    }
    else
    {
        for (scene::NodeId id : g.selection())
            if ((mod = siteModifierOf(g, g.node(id))))
                break;
        if (!mod)
        {
            const std::vector<Node*> all = siteModifiers(g);
            if (all.size() != 1)
                throw CommandError(cmd + (all.empty() ? ": no site in the scene (make one with siteCreate)" : ": several sites; name one or select it"));
            mod = all[0];
        }
    }
    return readSite(g, *mod, cmd);
}

std::string siteArg(const Args& a) { return a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>(); }

void storeSpec(const SiteRef& r, const sites::Spec& s) { ops::execute("setAttr", {{"args", {r.modifier + "." + kSiteSpecAttr, s.toJson().dump()}}}); }

/**
 * Folds a move or turn of the site mesh's own transform into the spec's origin and heading and puts the transform back
 * to identity, so the layout and its grading follow where the mesh was dragged. Tilt and scale can't be laid on the
 * ground: they are dropped with a warning. False when the transform is already identity.
 */
bool foldTransform(const SiteRef& r, sites::Spec& s, const std::string& cmd)
{
    Graph& g = Graph::current();
    Node* mesh = g.find(r.mesh);
    if (!mesh)
        return false;
    const glm::dmat4 L = scene::Transform::localMatrix(*mesh);
    if (glm::all(glm::equal(L, glm::dmat4(1.0), 1e-9)))
        return false;
    const glm::dvec3 x = glm::dvec3(L[0]), y = glm::dvec3(L[1]);
    if (std::abs(glm::length(x) - 1.0) > 1e-6 || std::abs(glm::length(y) - 1.0) > 1e-6 || std::abs(y.y - 1.0) > 1e-6)
        log::warn("{}: {} keeps only its move and turn about up; tilt and scale are dropped", cmd, r.mesh);
    const glm::dvec4 o = L * glm::dvec4(s.at.x, 0.0, s.at.y, 1.0);
    s.at = {o.x, o.z};
    s.heading = std::remainder(s.heading + glm::degrees(std::atan2(-x.z, x.x)), 360.0);
    scene::Transform::setLocalMatrix(g, mesh->id(), glm::dmat4(1.0));
    return true;
}
void storeGround(const SiteRef& r, const json& j) { ops::execute("setAttr", {{"args", {r.modifier + "." + kGroundAttr, j.dump()}}}); }

/** Evaluates the site's mesh so its node publishes what it laid out. */
sites::Evaluated evaluate(const SiteRef& r, const std::string& cmd)
{
    Graph& g = Graph::current();
    if (Node* m = g.find(r.mesh))
        (void)m->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    sites::Evaluated ev;
    if (!sites::evaluatedSite(r.modifierId, &ev) || ev.spec.toJson() != r.spec.toJson())
    {
        // Not evaluated with this spec (a batch run with no draw): lay it out here, flat.
        ev = {};
        ev.spec = r.spec;
        try
        {
            ev.layout = sites::layoutFor(r.spec);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what());
        }
        ev.envelopeGround.assign(ev.layout->envelopes.size(), {0.0, 0.0});
    }
    return ev;
}

// Terrain

bool isTerrain(const std::string& node)
{
    if (node.empty() || !ops::find("terrainInfo"))
        return false;
    try
    {
        ops::execute("terrainInfo", {{"args", {node}}});
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

/** The terrain a site stands on: given, else a selected terrain, else the only one; with where it came from. */
std::string pickTerrain(const Args& a, std::string* from, const std::string& cmd)
{
    Graph& g = Graph::current();
    if (a.has("terrain"))
    {
        const std::string t = a.get<std::string>("terrain");
        if (t.empty())
        {
            *from = "none (terrain='')";
            return {};
        }
        if (!isTerrain(t))
            throw CommandError(cmd + ": " + t + " is not a terrain");
        *from = "given";
        return g.find(t)->path();
    }
    for (scene::NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && isTerrain(n->path()))
        {
            *from = "selection";
            return n->path();
        }
    std::vector<std::string> found;
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && !siteModifierOf(g, n) && isTerrain(n->path()))
            found.push_back(n->path());
    if (found.size() == 1)
    {
        *from = "the only terrain";
        return found[0];
    }
    *from = found.empty() ? "none in the scene: the site stands at height 0" : "several terrains and none chosen: the site stands at height 0";
    return {};
}

/** The terrain's sea level, when it has one. */
std::optional<double> seaLevelOf(const std::string& terrain)
{
    if (terrain.empty() || !ops::find("terrainWater"))
        return std::nullopt;
    try
    {
        const json w = ops::execute("terrainWater", {{"args", {terrain}}});
        const json water = w.contains("water") && w["water"].is_object() ? w["water"] : w;
        for (const json& l : water.value("locators", json::array()))
            if (l.value("kind", std::string()) == "seaLevel" && l.contains("level"))
                return l["level"].get<double>();
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

json toWorldFlags(const sites::Spec& s, json flags)
{
    auto point = [&](const json& p) { return sites::pt(sites::toWorld(s, {p[0].get<double>(), p[1].get<double>()})); };
    for (const char* key : {"footprint", "path"})
        if (flags.contains(key))
        {
            json out = json::array();
            for (const json& p : flags[key])
                out.push_back(point(p));
            flags[key] = out;
        }
    for (const char* key : {"a", "b", "at"})
        if (flags.contains(key))
            flags[key] = point(flags[key]);
    if (flags.contains("heightFromWater"))
    {
        flags["height"] = s.waterLevel + flags["heightFromWater"].get<double>();
        flags.erase("heightFromWater");
    }
    return flags;
}

bool isStamp(const std::string& c) { return c == "terrainStampPad" || c == "terrainStampRegion" || c == "terrainStampQuay" || c == "terrainAddFeature"; }

/** Takes everything a site put on a terrain off it again, by the ids its ground record lists. */
json ungrade(const json& record, const std::set<std::string>& keep, std::vector<std::string>* errors)
{
    const std::string terrain = record.value("terrain", std::string());
    std::map<std::string, std::vector<std::string>> stamps, roads; // layer -> ids
    std::vector<std::string> strips;
    for (const json& op : record.value("ops", json::array()))
    {
        const std::string id = op.value("id", std::string()), c = op.value("command", std::string()), layer = op.value("layer", std::string(kLayer));
        if (keep.contains(id))
            continue;
        if (isStamp(c))
            stamps[layer].push_back(id);
        else if (c == "terrainAddRoad")
            roads[layer].push_back(id);
        else if (c == "terrainAddAirstrip")
            strips.push_back(id);
    }
    int removed = 0;
    if (terrain.empty() || !Graph::current().find(terrain))
        return removed;
    auto call = [&](const char* c, json flags) {
        flags["args"] = json::array({terrain});
        try
        {
            ops::execute(c, flags);
        }
        catch (const std::exception& e)
        {
            errors->push_back(std::string(c) + ": " + e.what());
        }
    };
    for (const auto& [layer, ids] : stamps)
    {
        call("terrainStampRemove", {{"ids", ids}, {"layer", layer}});
        removed += int(ids.size());
    }
    for (const auto& [layer, ids] : roads)
    {
        call("terrainRoadRemove", {{"ids", ids}, {"layer", layer}});
        removed += int(ids.size());
    }
    if (!strips.empty())
    {
        call("terrainCoastRemove", {{"ids", strips}});
        std::vector<std::string> pads;
        for (const std::string& id : strips)
            pads.push_back("airstrip_" + id);
        call("terrainStampRemove", {{"ids", pads}, {"layer", "Stamps"}});
        removed += int(strips.size());
    }
    return removed;
}

std::string layoutGroundHash(const sites::Spec& s, const sites::Layout& L)
{
    json all = json::array();
    for (const sites::GroundOp& op : L.ground)
        all.push_back({op.id, op.command, toWorldFlags(s, op.flags)});
    return slots::hex(slots::idHash(all.dump()));
}

/** Grades the terrain under a site: every ground op sent (stamps and roads in one batch), the ones it no longer has removed. */
/** What grading a site sends to its terrain: batched calls, calls made one by one (the airstrip), the record kept. */
struct GradePlan
{
    std::shared_ptr<const sites::Layout> layout;
    json calls = json::array(), record = json::array();
    std::vector<std::pair<std::string, json>> direct;
    std::set<std::string> made;
    std::vector<std::string> errors;
};

GradePlan planGrade(const SiteRef& r, const std::string& terrain, const std::string& cmd)
{
    GradePlan p;
    try
    {
        p.layout = sites::layoutFor(r.spec);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
    for (const char* need : {"terrainBatch", "terrainStampPad", "terrainAddRoad"})
        if (!ops::find(need))
            throw CommandError(cmd + ": the Terrain command " + std::string(need) + " is not available; load the Terrain extension");
    const std::string owner = sites::ownerOf(r.spec.id);
    for (const sites::GroundOp& op : p.layout->ground)
    {
        json flags = toWorldFlags(r.spec, op.flags);
        const std::string id = owner + "/" + op.id;
        flags["id"] = id;
        std::string layer;
        if (isStamp(op.command) || op.command == "terrainAddRoad")
        {
            if (!flags.contains("layer") || flags["layer"] == kLayer)
                flags["layer"] = op.command == "terrainAddRoad" ? kRoadLayer : kLayer;
            layer = flags["layer"].get<std::string>();
        }
        if (op.command != "terrainAddAirstrip" && op.command != "terrainAddChannel" && !ops::find(op.command))
        {
            p.errors.push_back(cmd + ": the Terrain command " + op.command + " is not available, so " + id + " was not made");
            continue;
        }
        flags["args"] = json::array({terrain});
        p.record.push_back({{"id", id}, {"command", op.command}, {"layer", layer}, {"op", op.id}});
        p.made.insert(id);
        if (op.command == "terrainAddAirstrip")
            p.direct.push_back({op.command, flags});
        else
            p.calls.push_back({{"command", op.command}, {"flags", flags}});
    }
    return p;
}

/** Sends the calls made one by one (after the batch), counting what ran. */
int sendDirect(const std::vector<std::pair<std::string, json>>& direct, const std::string& cmd, std::vector<std::string>& errors)
{
    int sent = 0;
    for (const auto& [c, flags] : direct)
    {
        if (!ops::find(c))
        {
            errors.push_back(cmd + ": the Terrain command " + c + " is not available (Terrain without coast and water), so " + flags["id"].get<std::string>() +
                             " was not graded");
            continue;
        }
        try
        {
            ops::execute(c, flags);
            ++sent;
        }
        catch (const std::exception& e)
        {
            errors.push_back(c + " " + flags["id"].get<std::string>() + ": " + e.what());
        }
    }
    return sent;
}

/**
 * Culverts where the sites' pads stand across streams: terrainCheck names each pad over a stream with the stream's way
 * across it, and a registered culvert is laid along that way, recorded with the site's other stamps so a regrade or
 * siteRemove takes it away. Returns how many were laid.
 */
int layCulverts(const std::vector<SiteRef>& sites, std::vector<json*>& records, const std::string& terrain, std::vector<std::string>& errors)
{
    LILA_ZONE();
    if (terrain.empty() || !ops::find("terrainCheck") || !ops::find("terrainAddFeature"))
        return 0;
    json check;
    try
    {
        check = ops::execute("terrainCheck", {{"args", {terrain}}});
    }
    catch (const std::exception& e)
    {
        errors.push_back(std::string("culverts not laid: terrainCheck failed: ") + e.what());
        return 0;
    }
    const std::string prefix = std::string(kLayer) + "/";
    json calls = json::array();
    std::map<std::string, int> perStamp;
    for (const json& i : check.value("issues", json::array()))
    {
        const std::string about = i.value("about", std::string());
        if (i.value("type", std::string()) != "blocksFlow" || !about.starts_with(prefix) || !i.contains("crossing"))
            continue;
        const std::string stamp = about.substr(prefix.size());
        for (size_t k = 0; k < sites.size(); ++k)
            if (stamp.starts_with(sites::ownerOf(sites[k].spec.id) + "/"))
            {
                json path = json::array();
                for (const json& p : i["crossing"])
                    path.push_back({p[0], p[2]});
                const std::string id = stamp + "/culvert" + std::to_string(perStamp[stamp]++);
                calls.push_back({{"command", "terrainAddFeature"},
                                 {"flags", {{"args", {terrain}}, {"kind", "culvert"}, {"id", id}, {"path", path}, {"width", 300.0}, {"layer", kLayer}}}});
                records[k]->push_back({{"id", id}, {"command", "terrainAddFeature"}, {"layer", kLayer}, {"op", "culvert"}});
                break;
            }
    }
    if (calls.empty())
        return 0;
    const json done = ops::execute("terrainBatch", {{"calls", calls}});
    for (const json& e : done.value("errors", json::array()))
        errors.push_back("culvert: " + e.value("error", std::string()));
    return done.value("ran", 0);
}

json grade(const SiteRef& r, const std::string& terrain, const std::string& cmd)
{
    LILA_ZONE();
    std::vector<std::string> errors;
    json out = {{"terrain", terrain}};
    const std::string prevTerrain = r.ground.value("terrain", std::string());
    if (terrain.empty())
    {
        out["removed"] = ungrade(r.ground, {}, &errors);
        out["sent"] = 0;
        out["errors"] = errors;
        storeGround(r, {{"terrain", ""}, {"ops", json::array()}});
        return out;
    }
    GradePlan plan = planGrade(r, terrain, cmd);
    errors = plan.errors;
    const std::shared_ptr<const sites::Layout> L = plan.layout;
    json& calls = plan.calls;
    json& record = plan.record;
    const std::set<std::string>& made = plan.made;
    json prev = r.ground;
    if (prevTerrain != terrain)
        out["removed"] = ungrade(prev, {}, &errors);
    else
        out["removed"] = ungrade(prev, made, &errors);
    int sent = 0;
    if (!calls.empty())
    {
        const json done = ops::execute("terrainBatch", {{"calls", calls}});
        sent += done.value("ran", 0);
        for (const json& e : done.value("errors", json::array()))
        {
            const size_t k = e.value("index", size_t(0));
            errors.push_back(e.value("command", std::string()) + " " + (k < calls.size() ? calls[k]["flags"].value("id", std::string()) : std::string()) + ": " +
                             e.value("error", std::string()));
        }
    }
    sent += sendDirect(plan.direct, cmd, errors);
    {
        std::vector<json*> records = {&record};
        out["culverts"] = layCulverts({r}, records, terrain, errors);
    }
    storeGround(r, {{"terrain", terrain}, {"ops", record}, {"hash", layoutGroundHash(r.spec, *L)}});
    out["sent"] = sent;
    out["ops"] = record.size();
    out["errors"] = errors;
    for (const std::string& e : errors)
        log::warn("{}", e);
    return out;
}

/**
 * Grades several sites with one Terrain batch: the terrain changes once and evaluates once, instead of once per site
 * (a 2k terrain with coast and water takes seconds to evaluate). Each site keeps its own record as grade() writes it.
 */
json gradeMany(const std::vector<SiteRef>& sitesIn, const std::string& cmd)
{
    LILA_ZONE();
    json calls = json::array(), out = json::array();
    std::vector<std::pair<std::string, json>> direct;
    std::vector<std::string> errors;
    std::vector<size_t> owner;
    std::vector<GradePlan> plans;
    std::vector<std::string> terrains;
    for (size_t i = 0; i < sitesIn.size(); ++i)
    {
        const SiteRef& r = sitesIn[i];
        const std::string terrain = r.ground.value("terrain", std::string());
        terrains.push_back(terrain);
        plans.push_back(terrain.empty() ? GradePlan{} : planGrade(r, terrain, cmd));
        if (terrain.empty())
            continue;
        json prev = r.ground;
        ungrade(prev, plans.back().made, &errors);
        for (const json& c : plans.back().calls)
        {
            calls.push_back(c);
            owner.push_back(i);
        }
        for (auto& d : plans.back().direct)
            direct.push_back(d);
        for (const std::string& e : plans.back().errors)
            errors.push_back(e);
    }
    int sent = 0;
    if (!calls.empty())
    {
        const json done = ops::execute("terrainBatch", {{"calls", calls}});
        sent += done.value("ran", 0);
        for (const json& e : done.value("errors", json::array()))
        {
            const size_t k = e.value("index", size_t(0));
            errors.push_back(e.value("command", std::string()) + " " + (k < calls.size() ? calls[k]["flags"].value("id", std::string()) : std::string()) + ": " +
                             e.value("error", std::string()));
        }
    }
    sent += sendDirect(direct, cmd, errors);
    int culverts = 0;
    for (const std::string& t : std::set<std::string>(terrains.begin(), terrains.end()))
    {
        std::vector<SiteRef> on;
        std::vector<json*> records;
        for (size_t i = 0; i < sitesIn.size(); ++i)
            if (terrains[i] == t && plans[i].layout)
            {
                on.push_back(sitesIn[i]);
                records.push_back(&plans[i].record);
            }
        culverts += layCulverts(on, records, t, errors);
    }
    for (size_t i = 0; i < sitesIn.size(); ++i)
        if (!terrains[i].empty() && plans[i].layout)
        {
            storeGround(sitesIn[i], {{"terrain", terrains[i]}, {"ops", plans[i].record}, {"hash", layoutGroundHash(sitesIn[i].spec, *plans[i].layout)}});
            out.push_back({{"site", sitesIn[i].spec.id}, {"ops", plans[i].record.size()}});
        }
    for (const std::string& e : errors)
        log::warn("{}", e);
    return {{"sites", out}, {"sent", sent}, {"culverts", culverts}, {"errors", errors}};
}

// Info

json info(const SiteRef& r, const sites::Evaluated& ev, bool slotsToo)
{
    const sites::Layout& L = *ev.layout;
    json envs = json::array();
    for (size_t i = 0; i < L.envelopes.size(); ++i)
        envs.push_back(sites::envelopeJson(ev, i));
    json boundary = json::array();
    for (P2 q : L.boundary)
        boundary.push_back(sites::pt(sites::toWorld(r.spec, q)));
    json out = {{"mesh", r.mesh},
                {"modifier", r.modifier},
                {"boundary", boundary},
                {"id", r.spec.id},
                {"kind", r.spec.kind},
                {"name", r.spec.name},
                {"names", L.names},
                {"liveries", L.liveries},
                {"spec", r.spec.toJson()},
                {"owner", sites::ownerOf(r.spec.id)},
                {"slots", L.slots.size()},
                {"slotTypes", L.stats.value("slotTypes", json::object())},
                {"envelopes", envs},
                {"styles", L.stats.value("styles", json::object())},
                {"stats", L.stats},
                {"notes", L.notes},
                {"terrain", r.ground.value("terrain", std::string())},
                {"hasGround", ev.hasGround},
                {"groundStale", !r.ground.value("terrain", std::string()).empty() && r.ground.value("hash", std::string()) != layoutGroundHash(r.spec, L)}};
    if (slotsToo)
    {
        json list = json::array();
        for (const sites::SlotOut& s : L.slots)
            list.push_back({{"id", slots::makeId(sites::ownerOf(r.spec.id), s.path)}, {"type", s.type}, {"at", sites::pt(sites::toWorld(r.spec, s.at))}});
        out["slotList"] = list;
    }
    return out;
}

// Flags

std::vector<FlagDef> paramFlags()
{
    std::vector<FlagDef> out;
    for (const sites::Param& p : sites::params())
    {
        FlagDef f;
        f.name = p.name;
        f.type = p.type == sites::Param::Type::Int     ? AT::Int
                 : p.type == sites::Param::Type::Float ? AT::Float
                 : p.type == sites::Param::Type::Bool  ? AT::Bool
                                                       : AT::String;
        f.defaultValue = nullptr;
        f.shownDefault = p.def;
        std::string kinds;
        for (const sites::Kind& k : sites::kinds())
            if (std::ranges::find(k.params, p.name) != k.params.end())
                kinds += (kinds.empty() ? "" : ", ") + k.id;
        f.doc = p.doc + " (" + kinds + ")";
        f.choices = p.choices;
        if (!std::isnan(p.min))
            f.min = p.min;
        if (!std::isnan(p.max))
            f.max = p.max;
        out.push_back(std::move(f));
    }
    return out;
}

/** The kit settings a call gives, checked against the kind. */
json givenParams(const Args& a, const sites::Kind& k, const std::string& cmd)
{
    json out = json::object();
    for (const sites::Param& p : sites::params())
        if (a.has(p.name))
            try
            {
                out[p.name] = sites::checkParam(k, p.name, a.flags[p.name]);
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
    if (a.has("params"))
    {
        if (!a.flags["params"].is_object())
            throw CommandError(cmd + ": params is an object of settings");
        for (const auto& [key, v] : a.flags["params"].items())
            try
            {
                out[key] = sites::checkParam(k, key, v);
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
    }
    return out;
}

P2 atOf(const json& v, const std::string& cmd)
{
    if (!v.is_array() || v.size() < 2 || !v[0].is_number() || !v[v.size() == 3 ? 2 : 1].is_number())
        throw CommandError(cmd + ": at is [x, z] in cm");
    return {v[0].get<double>(), v[v.size() == 3 ? 2 : 1].get<double>()};
}

/** A fictional name for the site, or the given one if it is not a real name. */
std::string nameFor(const sites::Spec& s, const std::string& given, const std::string& cmd)
{
    const names::RealNames real = names::sceneRealNames(Graph::current());
    auto leaks = [&](const std::string& n) {
        const names::Text t{"name", n};
        return names::findRealNames(std::span<const names::Text>(&t, 1), real, true);
    };
    if (!given.empty())
    {
        if (const auto l = leaks(given); !l.empty())
            throw CommandError(cmd + ": '" + given + "' matches the real name '" + l[0].real + "' (" + l[0].source + "); give a fictional one or leave name out");
        return given;
    }
    for (uint64_t salt = 0; salt < 64; ++salt)
    {
        const std::string n = sites::siteName(s.kind, s.culture, s.id, salt);
        if (leaks(n).empty())
            return n;
    }
    throw CommandError(cmd + ": every name tried matched a real name in the scene; give one");
}

std::string newSiteId(const std::string& kind)
{
    std::set<std::string> taken;
    for (const SiteRef& r : allSites("siteCreate"))
        taken.insert(r.spec.id);
    for (int n = 1;; ++n)
        if (const std::string id = kind + std::to_string(n); !taken.contains(id))
            return id;
}

/** Puts the site mesh's pivots on the site's origin, so the gizmo sits there and turns it in place. */
void placePivot(const SiteRef& r, const sites::Evaluated& ev)
{
    Graph& g = Graph::current();
    Node* mesh = g.find(r.mesh);
    if (!mesh)
        return;
    const glm::dvec3 at(r.spec.at.x, ev.originHeight, r.spec.at.y);
    for (int attr : {int(scene::Transform::kRotatePivot), int(scene::Transform::kScalePivot)})
        if (glm::any(glm::notEqual(std::get<glm::dvec3>(g.value({mesh->id(), attr, -1})), at, 1e-6)))
            g.setValue({mesh->id(), attr, -1}, at);
}

json created(const SiteRef& r, const json& groundOut, const std::string& cmd)
{
    const sites::Evaluated ev = evaluate(r, cmd);
    placePivot(r, ev);
    json out = info(r, ev, false);
    out["envelopes"] = ev.layout->envelopes.size();
    out["ground"] = groundOut;
    return out;
}

// Check

struct Issue
{
    std::string kind, site, part, what;
    P2 at{0.0};
};

double p90(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[size_t(std::min<double>(double(v.size() - 1), std::floor(0.9 * double(v.size() - 1) + 0.5)))];
}

/** Points to sample a ring at: its corners, its centre and a grid inside it, at most about 40. */
std::vector<P2> samplesOf(const Ring& r)
{
    // Corners pulled in a little, so a part ending on a quay line samples its own side of it.
    const P2 c = sites::centroidOf(r);
    std::vector<P2> out;
    for (P2 q : r)
        out.push_back(q + (c - q) * 0.04);
    out.push_back(c);
    P2 lo(1e300), hi(-1e300);
    for (P2 q : r)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    for (int i = 1; i < 6; ++i)
        for (int j = 1; j < 6; ++j)
        {
            const P2 q = lo + (hi - lo) * P2(i / 6.0, j / 6.0);
            if (sites::insideRing(r, q))
                out.push_back(q);
        }
    return out;
}

double distanceToRing(const Ring& r, P2 p)
{
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
    {
        const P2 a = r[j], ab = r[i] - a;
        const double t = glm::dot(ab, ab) > 0.0 ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(p - (a + ab * t)));
    }
    return best;
}

/** How far apart two rings' outlines are (0 when they overlap). */
double ringGap(const Ring& a, const Ring& b)
{
    if (a.empty() || b.empty() || sites::ringsOverlap(a, b))
        return 0.0;
    double best = std::numeric_limits<double>::infinity();
    for (P2 q : a)
        best = std::min(best, distanceToRing(b, q));
    for (P2 q : b)
        best = std::min(best, distanceToRing(a, q));
    return best;
}

bool bulky(const std::string& type)
{
    for (const char* t : {"container", "crane", "truck", "greenhouse", "shadehouse", "grave", "pool", "helipad", "drum", "tree"})
        if (type == t || type.starts_with(std::string(t) + "."))
            return true;
    return false;
}

json check(const std::vector<SiteRef>& list, const std::vector<SiteRef>& everyone, const std::string& cmd)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    std::vector<Issue> issues;
    json notes = json::array(), perSite = json::object();
    const names::RealNames real = names::sceneRealNames(g);
    // Outlines of every site in the scene, for overlaps between sites.
    std::vector<std::pair<std::string, Ring>> outlines;
    for (const SiteRef& o : everyone)
    {
        try
        {
            const auto L = sites::layoutFor(o.spec);
            Ring w;
            for (P2 q : L->boundary)
                w.push_back(sites::toWorld(o.spec, q));
            outlines.push_back({o.spec.id, w});
        }
        catch (const std::exception&)
        {
        }
    }
    for (const SiteRef& r : list)
    {
        const size_t before = issues.size();
        const sites::Evaluated ev = evaluate(r, cmd);
        const sites::Layout& L = *ev.layout;
        const sites::Spec& s = r.spec;
        auto W = [&](P2 q) { return sites::toWorld(s, q); };
        auto add = [&](std::string kind, std::string part, std::string what, P2 at) { issues.push_back({std::move(kind), s.id, std::move(part), std::move(what), W(at)}); };

        // Names: every output name against the scene's imported real names and the brand blocklist.
        std::vector<names::Text> texts = {{s.id + ".name", s.name}};
        for (const auto& [role, n] : L.names.items())
            if (n.is_string())
                texts.push_back({s.id + "." + role, n.get<std::string>()});
        for (const sites::EnvelopeOut& e : L.envelopes)
            if (!e.name.empty())
                texts.push_back({s.id + "/" + e.path, e.name});
        for (const names::Leak& l : names::findRealNames(texts, real, true))
            issues.push_back({"realName", s.id, l.where, "'" + l.text + "' matches the real name '" + l.real + "' (" + l.source + ")", s.at});

        // Overlaps: buildings on buildings, bulky parts inside buildings, this site over another.
        for (size_t i = 0; i < L.envelopes.size(); ++i)
            for (size_t j = i + 1; j < L.envelopes.size(); ++j)
                if (sites::ringsOverlap(L.envelopes[i].footprint, L.envelopes[j].footprint))
                    add("overlap", L.envelopes[i].path, L.envelopes[i].path + " and " + L.envelopes[j].path + " overlap", sites::centroidOf(L.envelopes[i].footprint));
        for (const sites::SlotOut& sl : L.slots)
        {
            if (!bulky(sl.type))
                continue;
            for (const sites::EnvelopeOut& e : L.envelopes)
                if (sites::insideRing(e.footprint, sl.at))
                {
                    add("overlap", sl.path, sl.type + " " + sl.path + " stands inside " + e.path, sl.at);
                    break;
                }
        }
        Ring mine;
        for (P2 q : L.boundary)
            mine.push_back(W(q));
        for (const auto& [id, ring] : outlines)
            if (id != s.id && sites::ringsOverlap(mine, ring))
                add("overlap", "boundary", "the site overlaps site " + id, L.boundary.empty() ? P2(0.0) : sites::centroidOf(L.boundary));

        // Ground: water under land parts and buildings, too little under water parts, slopes and pad relief.
        const std::string terrain = r.ground.value("terrain", std::string());
        if (terrain.empty() || !g.find(terrain))
        {
            notes.push_back(s.id + ": no terrain under it, so water and slope were not checked");
        }
        else
        {
            if (r.ground.value("hash", std::string()) != layoutGroundHash(s, L))
                add("groundStale", "ground", "the site changed since its ground was graded: run siteGround", {0.0, 0.0});
            struct Probe
            {
                std::string part;
                sites::Part::Needs needs;
                double minDepth, maxSlope;
                std::vector<P2> pts;
                Ring ring;
            };
            std::vector<Probe> probes;
            for (const sites::Part& p : L.parts)
                probes.push_back({p.id, p.needs, p.minDepth, p.maxSlope, samplesOf(p.ring), p.ring});
            for (const sites::EnvelopeOut& e : L.envelopes)
                probes.push_back({e.path, sites::Part::Needs::Land, 0.0, 0.0, samplesOf(e.footprint), e.footprint});
            // The grid draws a quay face between land and water parts as a slope one cell wide: a part's band a cell deep
            // along the edge facing a part needing the other is not judged.
            if (const auto hf = world::terrainHeights(terrain); hf && hf->cell > 0.0)
                for (Probe& p : probes)
                {
                    using N = sites::Part::Needs;
                    if (p.needs == N::Any)
                        continue;
                    for (const sites::Part& o : L.parts)
                        if (o.needs != N::Any && o.needs != p.needs)
                        {
                            const double gap = ringGap(p.ring, o.ring);
                            std::erase_if(p.pts, [&](P2 q) { return distanceToRing(o.ring, q) < gap + hf->cell; });
                        }
                }
            json points = json::array();
            for (const Probe& p : probes)
                for (P2 q : p.pts)
                    points.push_back(sites::pt(W(q)));
            json samples = json::array();
            try
            {
                samples = ops::execute("terrainSample", {{"args", {terrain}}, {"points", points}, {"masks", json::array({"water"})}});
            }
            catch (const std::exception& e)
            {
                notes.push_back(s.id + ": terrainSample failed (" + std::string(e.what()) + "), ground not checked");
            }
            size_t k = 0;
            for (const Probe& p : probes)
            {
                if (samples.size() < k + p.pts.size())
                    break;
                int wet = 0, shallow = 0;
                std::vector<double> slopes;
                P2 wetAt(0.0), shallowAt(0.0);
                for (P2 q : p.pts)
                {
                    const json& sm = samples[k++];
                    const double h = sm.value("height", 0.0);
                    // Below the site's water level, or under the terrain's own water (rivers, lakes, channels).
                    const bool inWater = sm.contains("water") && sm["water"].is_number() && sm["water"].get<double>() > 0.5;
                    const double depth = std::max(s.waterLevel - h, inWater ? 100.0 : 0.0);
                    if (sm.value("inside", true) == false)
                        continue;
                    if (depth > 30.0 && wet++ == 0)
                        wetAt = q;
                    if (depth < p.minDepth && shallow++ == 0)
                        shallowAt = q;
                    slopes.push_back(sm.value("slope", 0.0));
                }
                if (p.needs == sites::Part::Needs::Land && wet > 0)
                    add("water", p.part, p.part + " stands in water at " + std::to_string(wet) + " of " + std::to_string(p.pts.size()) + " points", wetAt);
                if (p.needs == sites::Part::Needs::Water && shallow > 0)
                    add(p.minDepth > 0.0 && wet > 0 ? "shallow" : "water", p.part,
                        p.part + " needs water at least " + std::to_string(int(p.minDepth)) + " cm deep; " + std::to_string(shallow) + " of " +
                            std::to_string(p.pts.size()) + " points are shallower (or land)",
                        shallowAt);
                if (p.maxSlope > 0.0)
                    if (const double sl = p90(slopes); sl > p.maxSlope)
                        add("steepGround", p.part, p.part + " lies on ground of " + std::to_string(int(sl)) + " deg (it takes " + std::to_string(int(p.maxSlope)) + ")",
                            sites::centroidOf(Ring(p.pts.begin(), p.pts.end())));
            }
            // Pads: how much ground they cut and fill across.
            try
            {
                std::map<std::string, std::pair<double, double>> relief;
                const json st = ops::execute("terrainStamps", {{"args", {terrain}}});
                const json layers = st.value("layers", json::object());
                for (const auto& [layer, items] : layers.items())
                    for (const json& it : items)
                    {
                        const json res = it.contains("result") ? it["result"] : it;
                        const std::string id = it.value("id", res.value("id", std::string()));
                        // How deep the pad cuts and fills on average over its area.
                        if (res.contains("cut") && res.contains("fill") && res.value("area", 0.0) > 0.0)
                            relief[id] = {(res["cut"].get<double>() + res["fill"].get<double>()) / res["area"].get<double>(), 0.0};
                    }
                for (const sites::Part& p : L.parts)
                {
                    if (p.maxRelief <= 0.0 || p.groundOp.empty())
                        continue;
                    std::string id = sites::ownerOf(s.id) + "/" + p.groundOp;
                    auto it = relief.find(id);
                    if (it == relief.end())
                        it = relief.find("airstrip_" + id);
                    if (it == relief.end())
                        continue;
                    if (const double d = it->second.first; d > p.maxRelief)
                        add("steepGround", p.id,
                            "the pad under " + p.id + " cuts and fills " + std::to_string(int(d)) + " cm deep on average to level the slope (limit " +
                                std::to_string(int(p.maxRelief)) + " cm)",
                            sites::centroidOf(p.ring));
                }
            }
            catch (const std::exception& e)
            {
                notes.push_back(s.id + ": terrainStamps failed (" + std::string(e.what()) + ")");
            }
            // Airstrips: the coast check's corridor and runway issues for this site's strip.
            if (s.kind == "airstrip" && ops::find("waterCheck"))
                try
                {
                    const std::string strip = sites::ownerOf(s.id) + "/strip";
                    for (const json& i : ops::execute("waterCheck", {{"args", {terrain}}}).value("issues", json::array()))
                        if (i.dump().find(strip) != std::string::npos)
                            issues.push_back({i.value("kind", i.value("type", std::string("airClearance"))), s.id, "strip",
                                              i.value("message", i.value("what", i.dump())), s.at});
                }
                catch (const std::exception&)
                {
                }
        }
        for (const std::string& n : L.notes)
            add("fit", "layout", n, {0.0, 0.0});
        perSite[s.id] = issues.size() - before;
    }
    json list2 = json::array(), counts = json::object();
    for (const char* k : {"realName", "overlap", "water", "shallow", "steepGround", "groundStale", "fit"})
        counts[k] = 0;
    for (const Issue& i : issues)
    {
        list2.push_back({{"kind", i.kind}, {"site", i.site}, {"part", i.part}, {"what", i.what}, {"at", sites::pt(i.at)}});
        counts[i.kind] = counts.value(i.kind, 0) + 1;
    }
    return {{"ok", issues.empty()}, {"issues", list2}, {"counts", counts}, {"sites", perSite}, {"notes", notes}};
}
/** The group the Buildings generator builds every site's envelopes under. */
constexpr const char* kSiteBuildings = "SiteBuildings";

bool massingOn(const SiteRef& r)
{
    const json v = ops::execute("getAttr", {{"args", {r.modifier + ".massing"}}});
    return !v.is_boolean() || v.get<bool>();
}

/**
 * Builds the envelopes of every site whose massing is off with the Buildings generator, under one group, so its
 * styles, slots and edits apply to site buildings like any other; the group goes when no site is left on it. Edits,
 * overrides and pins carry over by building id (each envelope's id hashed to a stable building key).
 */
json rebuildSiteBuildings(const std::string& cmd)
{
    Graph& g = Graph::current();
    json envs = json::array();
    std::string terrain;
    json sitesOn = json::array();
    for (const SiteRef& r : allSites(cmd))
    {
        if (massingOn(r))
            continue;
        sitesOn.push_back(r.spec.id);
        if (terrain.empty())
            terrain = r.ground.value("terrain", std::string());
        const sites::Evaluated ev = evaluate(r, cmd);
        for (size_t i = 0; i < ev.layout->envelopes.size(); ++i)
        {
            json e = sites::envelopeJson(ev, i);
            e["lotId"] = slots::idHash(e["id"].get<std::string>());
            envs.push_back(std::move(e));
        }
    }
    if (envs.empty())
    {
        if (Node* n = g.find(kSiteBuildings))
            ops::execute("delete", {{"args", {n->path()}}});
        return {{"group", nullptr}, {"buildings", 0}, {"sites", sitesOn}};
    }
    if (!ops::find("buildingCreate"))
        throw CommandError(cmd + ": site buildings need the Buildings extension (buildingCreate)");
    json call = {{"envelopes", envs}, {"name", kSiteBuildings}};
    if (!terrain.empty() && g.find(terrain))
        call["ground"] = terrain;
    json made = ops::execute("buildingCreate", call);
    return {{"group", made.value("group", json())},  {"buildings", made.value("buildings", 0)}, {"styles", made.value("styles", json::object())},
            {"slots", made.value("slots", 0)},       {"sites", sitesOn},                        {"ground", terrain.empty() ? json() : json(terrain)},
            {"warnings", made.value("warnings", json::array())}};
}

/** Turns a site's greybox massing off (its buildings come from the Buildings generator) or back on. */
void setMassing(const SiteRef& r, bool on) { ops::execute("setAttr", {{"args", {r.modifier + ".massing", on}}}); }
int& transformListener()
{
    static int id = -1;
    return id;
}

/** Site meshes a top-level move, rotate or xform call left with a transform of their own. */
std::vector<std::string> movedSites(const json& call)
{
    Graph& g = Graph::current();
    std::vector<scene::NodeId> targets;
    if (call.contains("args") && call["args"].is_array() && !call["args"].empty())
    {
        for (const json& n : call["args"])
            if (n.is_string())
                if (Node* node = g.find(n.get<std::string>()))
                    targets.push_back(node->id());
    }
    else
        targets.assign(g.selection().begin(), g.selection().end());
    std::vector<std::string> out;
    for (scene::NodeId id : targets)
        if (Node* mod = siteModifierOf(g, g.node(id)))
        {
            const scene::NodeId mesh = scene::modifiers::meshOf(g, mod->id());
            if (Node* m = g.node(mesh); m && !glm::all(glm::equal(scene::Transform::localMatrix(*m), glm::dmat4(1.0), 1e-9)))
                out.push_back(m->path());
        }
    return out;
}

/**
 * After the transform gizmo (or move/rotate/xform) moves a site mesh, its spec takes the move in the same undo step:
 * the step is reopened, the call replayed and siteEdit folds the transform in.
 */
void followTransform(const ExecuteEvent& e)
{
    static bool busy = false;
    static uint64_t stepBefore = 0;
    if (busy || ops::previewing() || (e.name != "move" && e.name != "rotate" && e.name != "xform"))
        return;
    UndoStack& undo = UndoStack::get();
    if (!e.done)
    {
        stepBefore = undo.newestId();
        return;
    }
    if (!e.ok || undo.replaying())
        return;
    const std::vector<std::string> moved = movedSites(e.call);
    if (moved.empty())
        return;
    busy = true;
    struct Done
    {
        ~Done() { busy = false; }
    } done;
    try
    {
        if (undo.chunkOpen())
        {
            for (const std::string& m : moved)
                ops::execute("siteEdit", {{"args", {m}}});
            return;
        }
        if (undo.newestId() == stepBefore || !undo.reopenNewest("Move Site"))
            return;
        struct Close
        {
            ~Close() { UndoStack::get().closeChunk(); }
        } close;
        ops::execute(e.name, e.call);
        for (const std::string& m : movedSites(e.call))
            ops::execute("siteEdit", {{"args", {m}}});
    }
    catch (const std::exception& ex)
    {
        log::warn("Move Site: {}", ex.what());
    }
}
} // namespace

void registerSites(ext::Context& ctx)
{
    for (const sites::TypeDef& t : sites::slotTypes())
    {
        // Types another extension registered first (Buildings' wall, gate, pool) are shared rather than registered twice.
        if (slots::types().find(t.id))
            continue;
        slots::SlotType st;
        st.id = t.id;
        st.label = t.label;
        st.tags = t.tags;
        st.footprint = t.footprint;
        st.attach = slots::parseAttach(t.attach).value_or(slots::Attach::Ground);
        st.placeholder = t.placeholder;
        st.doc = t.doc;
        slots::registerType(ctx, std::move(st));
    }
    sites::registerSiteNode(ctx);

    std::vector<std::string> kindIds;
    for (const sites::Kind& k : sites::kinds())
        kindIds.push_back(k.id);
    std::vector<FlagDef> createFlags = {
        {"kind", "k", AT::String, "port", "What to build: " + sites::kindList() + ".", kindIds},
        {"name", "n", AT::String, nullptr, "The site's fictional name; left out, one is generated (never a real name)."},
        {"id", "id", AT::String, nullptr, "Stable id: slot ids and terrain ids start with site:<id>. Default <kind><n>."},
        {"culture", "cu", AT::String, "mexican", "Naming culture for every name in the site.", names::cultures()},
        {"seed", "sd", AT::Int, int64_t(0), "Variation seed."},
        nodeFlag({"terrain", "tr", AT::String, nullptr, "Terrain it stands on and grades; default the selected or only terrain, '' for none."}, "mesh"),
        {"at", "at", AT::List, nullptr, "Where its origin stands, [x, z] cm; default the selected object's position, else the origin."},
        {"heading", "hd", AT::Float, 0.0, "Turn about up, degrees: its x axis (quay, runway, farm road) turned this far from world x."},
        {"ground", "gr", AT::Bool, true, "Grade the terrain: pads, quays, roads, canals, the airstrip."},
        {"waterLevel", "wl", AT::Float, nullptr, "Water level its quays, docks and ships sit at, cm; default the terrain's sea level, else 0."},
        {"params", "pm", AT::Any, nullptr, "Kit settings as one object, as the flags below take them."},
        {"buildings", "bl", AT::Bool, false, "Build its buildings with the Buildings generator (styles, slots, edits) instead of greybox massing."},
    };
    for (FlagDef& f : paramFlags())
        createFlags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "siteCreate",
        .doc = "Make a site kit: a cargo port, cruise terminal, mausoleum cemetery, hidden airstrip, industrial farm, mountain village, walled "
               "compound, private island or lighthouse, laid out on the terrain and graded into it.\n\n"
               "The site is a mesh whose Site Kit node lays it out live from its spec: buildings as Settlement-style envelopes with a style tag "
               "(greybox massing until a building generator builds them; siteEnvelopes lists them), every other part (containers, cranes, crop "
               "rows, graves, walls, docks, markings) a typed slot with a stable id under site:<id>, so slot rules and overrides swap them. "
               "Every name (the site, shipping lines, mausoleum families, businesses, streets) is fictional. The terrain is graded with Terrain "
               "stamps, roads, canals and the coast airstrip, kept by id so siteEdit and siteGround replace them. Each kind takes its own "
               "settings (siteKinds lists them). Returns the mesh, modifier, id, names, slot counts by type, envelope count, styles and what the "
               "grading sent.",
        .flags = createFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const std::string cmd = "siteCreate";
            refreshAvoid();
            Graph& g = Graph::current();
            const std::string kindId = a.get<std::string>("kind", "port");
            const sites::Kind* kind = sites::findKind(kindId);
            if (!kind)
                throw CommandError(cmd + ": kind is one of " + sites::kindList() + ", not " + kindId);
            std::string terrainFrom;
            const std::string terrain = pickTerrain(a, &terrainFrom, cmd);

            sites::Spec s;
            s.kind = kind->id;
            s.id = a.has("id") ? a.get<std::string>("id") : newSiteId(kind->id);
            for (const SiteRef& r : allSites(cmd))
                if (r.spec.id == s.id)
                    throw CommandError(cmd + ": a site with id " + s.id + " exists (" + r.mesh + ")");
            s.culture = a.get<std::string>("culture", "mexican");
            if (!names::hasCulture(s.culture))
                throw CommandError(cmd + ": culture is one of the naming cultures (namingCultures), not " + s.culture);
            s.seed = a.get<int64_t>("seed", 0);
            s.heading = a.get<double>("heading", 0.0);
            std::string atFrom = "given";
            if (a.has("at"))
                s.at = atOf(a.flags["at"], cmd);
            else
            {
                atFrom = "origin";
                for (scene::NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("transform") && n->path() != terrain)
                    {
                        const glm::dmat4 m = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                        s.at = {m[3].x, m[3].z};
                        atFrom = "selection (" + n->path() + ")";
                        break;
                    }
            }
            std::string waterFrom = "given";
            if (a.has("waterLevel"))
                s.waterLevel = a.get<double>("waterLevel");
            else if (const std::optional<double> sea = seaLevelOf(terrain))
            {
                s.waterLevel = *sea;
                waterFrom = "terrain sea level";
            }
            else
            {
                s.waterLevel = 0.0;
                waterFrom = terrain.empty() ? "0 (no terrain)" : "0 (the terrain has no sea)";
            }
            s.params = givenParams(a, *kind, cmd);
            json specJson = s.toJson();
            specJson.erase("name");
            try
            {
                s = sites::specFromJson(specJson);
                s.name = nameFor(s, a.get<std::string>("name", ""), cmd);
                (void)sites::layoutFor(s);
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }

            const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", kind->nodeName}});
            const std::string host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            const std::string hostName = g.find(host)->name();
            std::string group = hostName + "_Site";
            const std::vector<std::string> groups = gn::groupNames(g);
            for (int n = 2; std::ranges::find(groups, group) != groups.end(); ++n)
                group = hostName + "_Site" + std::to_string(n);
            const json gspec = {
                {"name", group},
                {"description", "A site kit (" + kind->label + "): its layout from the siteSpec input on the terrain Ground names."},
                {"inputs", json::array({{{"name", kSiteSpecAttr}, {"type", "string"}, {"default", s.toJson().dump()}, {"panel", "Site"}, {"description", "The site spec, JSON."}},
                                        {{"name", kGroundAttr}, {"type", "string"}, {"default", "{}"}, {"panel", "Site"},
                                         {"description", "What the site put on its terrain (ids by command), for siteGround and siteRemove."}},
                                        {{"name", "massing"}, {"type", "bool"}, {"default", true}, {"panel", "Site"}, {"description", "Draw the buildings as greybox massing."}}})},
                {"nodes", json::array({{{"name", "Ground"}, {"type", "ObjectInfo"}, {"props", {{"object", terrain}, {"transformSpace", "instance"}}}},
                                       {{"name", "Site"}, {"type", "SiteKit"}}})},
                {"links", json::array({std::string("GroupInput.") + kSiteSpecAttr + " -> Site.spec", "GroupInput.massing -> Site.massing", "Ground.geometry -> Site.ground",
                                       "Site.geometry -> GroupOutput.geometry"})}};
            ops::execute("gnBuildGroup", {{"args", {gspec}}, {"replace", false}});
            ops::execute("gnApply", {{"args", {host, group}}});
            SiteRef r = findSite(host, cmd);
            json groundOut = {{"terrain", terrain}, {"sent", 0}};
            if (!terrain.empty() && a.get<bool>("ground", true))
            {
                groundOut = grade(r, terrain, cmd);
                r = findSite(host, cmd);
            }
            else if (!terrain.empty())
            {
                storeGround(r, {{"terrain", terrain}, {"ops", json::array()}});
                r = findSite(host, cmd);
            }
            json out = created(r, groundOut, cmd);
            out["terrainFrom"] = terrainFrom;
            out["atFrom"] = atFrom;
            out["waterLevelFrom"] = waterFrom;
            out["group"] = group;
            if (a.get<bool>("buildings", false))
            {
                setMassing(r, false);
                out["buildings"] = rebuildSiteBuildings(cmd);
            }
            ops::execute("select", {{"args", {r.mesh}}});
            return out;
        },
        .examples = {"p = siteCreate(kind='port', terrain='Terrain', at=[0, -20000], quayLength=80000, tiers=5)\np['names']['site'], p['slotTypes']",
                     "siteCreate(kind='airstrip', at=[150000, 90000], heading=30, runwayLength=110000, aircraft='twin')",
                     "siteCreate(kind='farm', params={'farmWidth': 90000, 'greenhouses': 0.4, 'crops': 'tomato, chile'})"},
    });

    std::vector<FlagDef> editFlags = {
        {"name", "n", AT::String, nullptr, "A new fictional name."},
        {"culture", "cu", AT::String, nullptr, "Naming culture.", names::cultures()},
        {"seed", "sd", AT::Int, nullptr, "Variation seed."},
        nodeFlag({"terrain", "tr", AT::String, nullptr, "Move it onto another terrain ('' for none); its grading moves with it."}, "mesh"),
        {"at", "at", AT::List, nullptr, "New origin [x, z], cm."},
        {"heading", "hd", AT::Float, nullptr, "New turn, degrees."},
        {"waterLevel", "wl", AT::Float, nullptr, "Water level, cm."},
        {"ground", "gr", AT::Bool, true, "Re-grade the terrain after the edit."},
        {"params", "pm", AT::Any, nullptr, "Kit settings as one object."},
    };
    for (FlagDef& f : paramFlags())
        editFlags.push_back(std::move(f));
    ctx.registerCommand({
        .name = "siteEdit",
        .doc = "Change a site: its kit settings (the same flags as siteCreate, only its kind's), name, seed, placement, water level or "
               "terrain. The layout follows live; with ground=True (default) the terrain is graded again and the parts the site no longer "
               "has are taken off it. Slot ids of parts that stay where they were stay the same. Returns the site as siteInfo does with what "
               "the grading sent. A move or turn of the site mesh (the transform gizmo, move, rotate) is folded into at and heading and "
               "the transform reset; that happens on its own after move and rotate (fromTransform says when it did).",
        .positionalDoc = "site mesh, modifier or id (default the selected or only site)",
        .flags = editFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const std::string cmd = "siteEdit";
            refreshAvoid();
            SiteRef r = findSite(siteArg(a), cmd);
            sites::Spec s = r.spec;
            const json before = r.spec.toJson();
            const sites::Kind* kind = sites::findKind(s.kind);
            const json given = givenParams(a, *kind, cmd);
            for (const auto& [key, v] : given.items())
                s.params[key] = v;
            if (a.has("culture"))
            {
                s.culture = a.get<std::string>("culture");
                if (!names::hasCulture(s.culture))
                    throw CommandError(cmd + ": culture is one of the naming cultures (namingCultures), not " + s.culture);
            }
            if (a.has("seed"))
                s.seed = a.get<int64_t>("seed");
            const bool folded = foldTransform(r, s, cmd);
            if (a.has("at"))
                s.at = atOf(a.flags["at"], cmd);
            if (a.has("heading"))
                s.heading = a.get<double>("heading");
            if (a.has("waterLevel"))
                s.waterLevel = a.get<double>("waterLevel");
            if (a.has("name"))
                s.name = nameFor(s, a.get<std::string>("name"), cmd);
            else if (a.has("culture"))
                s.name = nameFor(s, "", cmd);
            try
            {
                s = sites::specFromJson(s.toJson());
                (void)sites::layoutFor(s);
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
            std::string terrain = r.ground.value("terrain", std::string());
            if (a.has("terrain"))
            {
                terrain = a.get<std::string>("terrain");
                if (!terrain.empty() && !isTerrain(terrain))
                    throw CommandError(cmd + ": " + terrain + " is not a terrain");
                if (!terrain.empty())
                    terrain = Graph::current().find(terrain)->path();
                ops::execute("gnSetValue", {{"args", {r.group, "Ground.object", terrain}}});
            }
            if (s.toJson() != r.spec.toJson())
                storeSpec(r, s);
            r = findSite(r.mesh, cmd);
            json groundOut = nullptr;
            if (a.get<bool>("ground", true) && (!terrain.empty() || !r.ground.value("terrain", std::string()).empty()))
            {
                groundOut = grade(r, terrain, cmd);
                r = findSite(r.mesh, cmd);
            }
            json out = created(r, groundOut, cmd);
            out["fromTransform"] = folded;
            if (!massingOn(r) && s.toJson() != before)
                out["buildings"] = rebuildSiteBuildings(cmd);
            return out;
        },
        .examples = {"siteEdit('port1', tiers=6, fill=0.9)", "siteEdit('CargoPort', at=[5000, -21000], heading=12)"},
    });

    ctx.registerCommand({
        .name = "siteGround",
        .doc = "Grade the terrain under a site again from its spec (after editing the terrain, or with ground=False edits), or take "
               "everything the site put on the terrain off it with remove=True. Lays a culvert where one of its pads stands across a stream. Returns what was sent and removed and the culverts laid.",
        .positionalDoc = "site mesh, modifier or id",
        .flags = {{"remove", "rm", AT::Bool, false, "Take the site's stamps, roads, canals and airstrip off the terrain."},
                  {"sites", "s", AT::List, nullptr, "Several sites graded in one terrain change (ids or meshes); every site with all=True."},
                  {"all", "a", AT::Bool, false, "Every site in the scene, in one terrain change."}},
        .run = [](const Args& a) -> json {
            const std::string cmd = "siteGround";
            refreshAvoid();
            if ((a.has("sites") || a.get<bool>("all", false)) && !a.get<bool>("remove", false))
            {
                std::vector<SiteRef> list;
                if (a.get<bool>("all", false))
                    list = allSites(cmd);
                else
                    for (const json& n : a.flags["sites"])
                        list.push_back(findSite(n.get<std::string>(), cmd));
                return gradeMany(list, cmd);
            }
            const SiteRef r = findSite(siteArg(a), cmd);
            if (a.get<bool>("remove", false))
                return grade(r, "", cmd);
            const std::string terrain = r.ground.value("terrain", std::string());
            if (terrain.empty())
                throw CommandError(cmd + ": " + r.spec.id + " stands on no terrain; give it one with siteEdit(terrain=...)");
            return grade(r, terrain, cmd);
        },
        .examples = {"siteGround('airstrip1')", "siteGround('port1', remove=True)"},
    });

    ctx.registerCommand({
        .name = "siteRemove",
        .doc = "Delete a site: its mesh and everything it put on the terrain. Returns what was removed.",
        .positionalDoc = "site mesh, modifier or id",
        .run = [](const Args& a) -> json {
            const std::string cmd = "siteRemove";
            const SiteRef r = findSite(siteArg(a), cmd);
            std::vector<std::string> errors;
            const json removed = ungrade(r.ground, {}, &errors);
            Graph& g = Graph::current();
            // The stack's base stays behind when its mesh goes.
            std::string base;
            if (Node* m = g.find(r.mesh))
                if (const scene::NodeId src = scene::modifiers::stackSource(g, m->id()); src != scene::kNoNode && g.node(src) && g.node(src)->type().isA("meshSource"))
                    base = g.node(src)->path();
            ops::execute("delete", {{"args", {r.mesh}}});
            if (g.find(r.modifier))
                ops::execute("delete", {{"args", {r.modifier}}});
            if (!base.empty() && g.find(base))
                ops::execute("delete", {{"args", {base}}});
            try
            {
                ops::execute("gnDeleteGroup", {{"args", {r.group}}});
            }
            catch (const std::exception& e)
            {
                errors.push_back(std::string("the site's node group stays: ") + e.what());
            }
            slots::unpublish(r.modifierId);
            sites::forgetSite(r.modifierId);
            json out = {{"site", r.spec.id}, {"mesh", r.mesh}, {"terrainRemoved", removed}, {"errors", errors}};
            if (Graph::current().find(kSiteBuildings))
                out["buildings"] = rebuildSiteBuildings(cmd);
            return out;
        },
        .examples = {"siteRemove('cemetery1')"},
    });

    ctx.registerCommand({
        .name = "siteInfo",
        .doc = "A site as laid out: kind, spec, every fictional name, container liveries, slot counts by type (slots=True lists every "
               "slot id, type and position), its envelopes in world space with style tags, stats, notes and whether the terrain grading is "
               "stale.",
        .positionalDoc = "site mesh, modifier or id",
        .flags = {{"slots", "sl", AT::Bool, false, "List every slot."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            refreshAvoid();
            const SiteRef r = findSite(siteArg(a), "siteInfo");
            return info(r, evaluate(r, "siteInfo"), a.get<bool>("slots", false));
        },
        .examples = {"i = siteInfo('port1')\ni['liveries'], len(i['envelopes'])"},
    });

    ctx.registerCommand({
        .name = "siteEnvelopes",
        .doc = "Every building the site kits want, as Settlement-style envelopes in world space (footprint, base, top, height, floors, use, "
               "roof, roofHeight, front, entrances, program, name) with a style tag (colonial, seafront, resortTower, modernCommercial, "
               "mansion, civic, warehouse, packingPlant, portShed, adobe, ruralShack, church, mausoleum, hangar, lighthouse, "
               "terminal) and a stable id under site:<id>. This is what a building generator builds from; until one does they draw as "
               "greybox massing. sites= limits it to some sites.",
        .flags = {{"sites", "s", AT::List, nullptr, "Site ids or meshes; default every site."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string cmd = "siteEnvelopes";
            refreshAvoid();
            std::vector<SiteRef> list;
            if (a.has("sites"))
                for (const json& n : a.flags["sites"])
                    list.push_back(findSite(n.get<std::string>(), cmd));
            else
                list = allSites(cmd);
            json envs = json::array(), styles = json::object();
            for (const SiteRef& r : list)
            {
                const sites::Evaluated ev = evaluate(r, cmd);
                for (size_t i = 0; i < ev.layout->envelopes.size(); ++i)
                {
                    json e = sites::envelopeJson(ev, i);
                    styles[e["style"].get<std::string>()] = styles.value(e["style"].get<std::string>(), 0) + 1;
                    envs.push_back(std::move(e));
                }
            }
            return {{"envelopes", envs}, {"count", envs.size()}, {"styles", styles}, {"sites", list.size()}};
        },
        .examples = {"e = siteEnvelopes()\n[x['style'] for x in e['envelopes']][:5]"},
    });

    ctx.registerCommand({
        .name = "siteBuildings",
        .doc = "Build site kits' buildings with the Buildings generator: each envelope (siteEnvelopes) becomes a procedural building "
               "in its kit's style (portShed, warehouse, packingPlant, mausoleum, hangar, mansion, church, civic, colonial, "
               "adobe, ruralShack, terminal, lighthouse), under the group SiteBuildings, and the site's greybox massing turns "
               "off. Building ids are stable per envelope, so edits, slot overrides and pins carry over; siteEdit rebuilds them when "
               "the layout changes. remove=True turns massing back on and drops those buildings. Returns the group, building count, "
               "styles and slots.",
        .positionalDoc = "sites (ids or meshes; default every site)",
        .flags = {{"remove", "rm", AT::Bool, false, "Back to greybox massing."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const std::string cmd = "siteBuildings";
            refreshAvoid();
            std::vector<SiteRef> list;
            for (const json& p : a.positional)
                if (p.is_string())
                    list.push_back(findSite(p.get<std::string>(), cmd));
            if (list.empty())
                list = allSites(cmd);
            if (list.empty())
                throw CommandError(cmd + ": no site in the scene (make one with siteCreate)");
            for (const SiteRef& r : list)
                setMassing(r, a.get<bool>("remove", false));
            return rebuildSiteBuildings(cmd);
        },
        .examples = {"siteBuildings()", "siteBuildings('port1')['styles']", "siteBuildings('cemetery1', remove=True)"},
    });

    ctx.registerCommand({
        .name = "siteList",
        .doc = "The sites in the scene: id, kind, name, mesh, terrain, slot and envelope counts.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const SiteRef& r : allSites("siteList"))
            {
                json e = {{"id", r.spec.id}, {"kind", r.spec.kind}, {"name", r.spec.name}, {"mesh", r.mesh}, {"terrain", r.ground.value("terrain", std::string())}};
                try
                {
                    const auto L = sites::layoutFor(r.spec);
                    e["slots"] = L->slots.size();
                    e["envelopes"] = L->envelopes.size();
                }
                catch (const std::exception& ex)
                {
                    e["error"] = ex.what();
                }
                out.push_back(std::move(e));
            }
            return out;
        },
        .examples = {"[s['id'] for s in siteList()]"},
    });

    ctx.registerCommand({
        .name = "siteKinds",
        .doc = "The site kits: id, label, what each builds, and its settings with type, default, range and doc; plus the slot types the "
               "kits publish and the envelope style tags.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json kinds = json::array();
            for (const sites::Kind& k : sites::kinds())
            {
                json ps = json::array();
                for (const std::string& n : k.params)
                {
                    const sites::Param* p = sites::findParam(n);
                    json e = {{"name", n},
                              {"type", p->type == sites::Param::Type::Int ? "int" : p->type == sites::Param::Type::Float ? "float" : p->type == sites::Param::Type::Bool ? "bool" : "string"},
                              {"default", p->def},
                              {"doc", p->doc}};
                    if (!std::isnan(p->min))
                        e["min"] = p->min;
                    if (!std::isnan(p->max))
                        e["max"] = p->max;
                    if (!p->choices.empty())
                        e["choices"] = p->choices;
                    ps.push_back(std::move(e));
                }
                kinds.push_back({{"id", k.id}, {"label", k.label}, {"group", k.group}, {"doc", k.doc}, {"params", ps}});
            }
            json types = json::array();
            for (const sites::TypeDef& t : sites::slotTypes())
                types.push_back(t.id);
            return {{"kinds", kinds}, {"slotTypes", types}, {"styles", sites::styles()}};
        },
        .examples = {"[k['id'] for k in siteKinds()['kinds']]"},
    });

    ctx.registerCommand({
        .name = "siteCheck",
        .doc = "Check sites (one, or every site): names that match a real name (imports in the scene or the brand blocklist), buildings "
               "overlapping each other, bulky parts inside buildings, sites overlapping sites, land parts and buildings standing in water, "
               "berths, piers and docks without the water depth they need, ground too steep for unpaved fields and plots, pads cutting "
               "too far into a slope, airstrip corridors without clearance, grading older than the site, and parts that did not fit. "
               "Returns ok, issues ({kind, site, part, what, at}), counts by kind and notes.",
        .positionalDoc = "site mesh, modifier or id (default every site)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string cmd = "siteCheck";
            refreshAvoid();
            const std::vector<SiteRef> everyone = allSites(cmd);
            std::vector<SiteRef> list;
            if (const std::string n = siteArg(a); !n.empty())
                list.push_back(findSite(n, cmd));
            else
                list = everyone;
            return check(list, everyone, cmd);
        },
        .examples = {"c = siteCheck()\nc['ok'], c['counts']", "siteCheck('port1')['issues']"},
    });

    mcp::Server::get().addResource(ctx, {.uri = "lila://schema/site-kits",
                                         .name = "Site kits",
                                         .description = "The site kits siteCreate builds, their settings with defaults and ranges, the slot types they publish "
                                                        "and the envelope style tags. Read before calling siteCreate.",
                                         .mimeType = "application/json",
                                         .read = [] { return ops::execute("siteKinds", json::object()).dump(2); }});

    unregisterSites();
    transformListener() = ops::addExecuteListener(followTransform);
    // An opened scene's imported real names reach the layouts before anything draws them.
    for (events::Event e : {events::Event::SceneOpened, events::Event::SceneImported})
        ctx.subscribe(e, [](const events::EventData&) {
            LILA_ZONE_N("sites.sceneOpened");
            const uint64_t before = sites::avoidRevision();
            refreshAvoid();
            if (sites::avoidRevision() == before)
                return;
            Graph& g = Graph::current();
            for (Node* n : siteModifiers(g))
                gn::rerunModifier(g, n->id());
        });
}

void unregisterSites()
{
    if (transformListener() >= 0)
        ops::removeExecuteListener(transformListener());
    transformListener() = -1;
}
} // namespace openworld
