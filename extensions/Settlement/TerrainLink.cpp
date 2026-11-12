// Settlement on a Terrain, through the terrain contract's commands called by name: siting from the terrain's analysis,
// main roads from its router, grading as road features, pads and stamps (settlementGrade), re-planning only where the
// terrain changed (settlementReplan), and the agent's checks and explanations (settlementCheck, settlementExplain).

#include "Settlement.h"
#include "SettlementTerrain.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <queue>
#include <set>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using scene::Node;
using LinkT = ArgType;

uint64_t linkHash(std::string_view s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string linkHex(uint64_t v) { return std::format("{:016x}", v); }

json linkXz(Vec2 p) { return json::array({p.x, p.y}); }

json xzLine(std::span<const Vec2> pts)
{
    json a = json::array();
    for (const Vec2& q : pts)
        a.push_back(linkXz(q));
    return a;
}

Vec2 groundPoint(const json& p)
{
    if (!p.is_array() || p.size() < 2)
        return Vec2(0.0);
    return p.size() >= 3 ? Vec2(p[0].get<double>(), p[2].get<double>()) : Vec2(p[0].get<double>(), p[1].get<double>());
}

json merged(json base, const json& more)
{
    for (auto& [k, v] : more.items())
        base[k] = v;
    return base;
}

/** A terrain contract command on the terrain; a missing command is a CommandError naming it. */
json terrainCall(const char* name, const std::string& terrain, json flags, const std::string& cmd)
{
    if (!ops::find(name))
        throw CommandError(cmd + ": the Terrain command " + name + " is not available; load the Terrain extension");
    flags["args"] = json::array({terrain});
    return ops::execute(name, flags);
}

std::string linkTextOf(const json& v) { return v.is_string() ? v.get<std::string>() : std::string(); }

/** The current revision, remembered by the terrain so terrainDirtyRegions can compare with it later. */
std::string revisionOf(const std::string& terrain, const std::string& cmd)
{
    return terrainCall("terrainDirtyRegions", terrain, {{"since", ""}}, cmd).value("revision", std::string());
}

/** The Settlement modifier of a mesh (or the modifier itself). */
Node* modifierOf(Graph& g, const std::string& settlement, const std::string& cmd)
{
    Node* n = g.find(settlement);
    if (!n)
        throw CommandError(cmd + ": no node named '" + settlement + "'");
    auto isSettlement = [](const Node& m) { return m.type().attrIndex("lotScale") >= 0 && m.type().attrIndex("profile") >= 0; };
    if (isSettlement(*n))
        return n;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && isSettlement(*mod))
            return mod;
    throw CommandError(cmd + ": " + n->path() + " has no settlement (make one with settlementCreate)");
}

json editsOf(const Node& mod, const std::string& cmd)
{
    const std::string text = linkTextOf(ops::execute("getAttr", {{"args", {mod.path() + ".edits"}}}));
    try
    {
        return parseEdits(text);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void setEditsOf(const Node& mod, const json& edits) { ops::execute("setAttr", {{"args", {mod.path() + ".edits", edits.dump()}}}); }

std::string terrainOf(const Rebuilt& r, const Args& a, const std::string& cmd)
{
    std::string terrain = a.get<std::string>("terrain", "");
    if (terrain.empty())
        terrain = linkTextOf(r.values.value("ground", json()));
    if (terrain.empty())
        throw CommandError(cmd + ": give terrain= (the settlement stands on flat ground)");
    if (!isTerrainGround(terrain))
        throw CommandError(cmd + ": '" + terrain + "' is not a Terrain (terrainInfo does not know it); on mesh and flat grounds the "
                                                   "settlement conforms to the ground itself");
    return terrain;
}

// ---------------------------------------------------------------------------------------------------------------------
// Routes

struct TerrainWay
{
    std::vector<Vec2> pts;
    double grade = 0.0; // rise over run
    bool within = true;
    std::string note;
};

TerrainWay routeTerrain(const std::string& terrain, Vec2 a, Vec2 b, double maxGrade, const std::string& cmd)
{
    if (!ops::find("terrainRoute"))
        throw CommandError(cmd + ": the Terrain command terrainRoute is not available; load the Terrain extension");
    TerrainWay w;
    try
    {
        const json r = terrainCall("terrainRoute", terrain,
                                   {{"start", linkXz(a)}, {"end", linkXz(b)}, {"maxGrade", maxGrade * 100.0}, {"weights", {{"water", 25.0}}}, {"resolution", 512}}, cmd);
        for (const json& p : r.value("points", json::array()))
            w.pts.push_back(groundPoint(p));
        if (w.pts.size() >= 2)
        {
            w.pts.front() = a;
            w.pts.back() = b;
        }
        w.grade = r.value("maxGrade", 0.0) / 100.0;
        w.within = r.value("withinGrade", true);
    }
    catch (const std::exception& e)
    {
        w.pts.clear();
        w.note = e.what();
    }
    return w;
}

/** The book's entries split by whether any of their segments reach into a dirty rectangle. */
void splitTouching(const RouteBook& book, std::span<const DirtyRect> dirty, std::vector<RouteBook::Entry>* kept)
{
    for (const RouteBook::Entry& e : book.entries)
    {
        bool hit = false;
        for (const DirtyRect& r : dirty)
            for (size_t k = 0; k + 1 < e.pts.size() && !hit; ++k)
            {
                Polygon seg;
                seg.pts = {e.pts[k], e.pts[k + 1]};
                hit = touches(seg, r);
            }
        if (!hit)
            kept->push_back(e);
    }
}

/**
 * Routes the settlement's main roads with terrainRoute until the layout asks for none it lacks, keeping them in edits.
 * all re-routes every road; otherwise only those touching dirty. A terrain route that leaves the outline or crosses an
 * exclusion is replaced by Settlement's own (noted).
 */
json routeMainRoads(Graph& g, const std::string& name, const std::string& terrain, json& edits, std::span<const DirtyRect> dirty, bool all,
                    const std::string& cmd)
{
    LILA_ZONE();

    RouteBook book;
    if (!all && edits.contains("terrainRoutes"))
    {
        auto old = routeBookFromJson(edits["terrainRoutes"]);
        std::vector<RouteBook::Entry> kept;
        splitTouching(*old, dirty, &kept);
        book.entries = std::move(kept);
    }
    json notes = json::array();
    int passes = 0;
    for (; passes < 4; ++passes)
    {
        edits["terrainRoutes"] = toJson(book);
        const Rebuilt r = rebuildSettlement(g, name, cmd.c_str(), &edits);
        const std::vector<RouteBook::Entry> missing = r.layout.routeBook ? r.layout.routeBook->missing() : std::vector<RouteBook::Entry>{};
        if (missing.empty())
            break;
        for (const RouteBook::Entry& m : missing)
        {
            RouteBook::Entry e = m;
            TerrainWay w = routeTerrain(terrain, m.a, m.b, m.maxGrade, cmd);
            std::string why = w.note.empty() ? std::string() : "terrainRoute found no way (" + w.note + ")";
            for (size_t k = 0; k < w.pts.size() && why.empty(); ++k)
                if (!contains(r.layout.outline.pts, w.pts[k]) && boundaryDistance(r.layout.outline.pts, w.pts[k]) > 50.0)
                    why = "the terrain's way left the settlement's outline";
            for (size_t k = 0; k + 1 < w.pts.size() && why.empty(); ++k)
                if (crossesExclusion(r.layout, w.pts[k], w.pts[k + 1]))
                    why = "the terrain's way crossed an exclusion";
            if (why.empty() && w.pts.size() >= 2)
            {
                e.pts = std::move(w.pts);
                e.source = "terrain";
                e.gradeFound = w.grade;
                e.withinGrade = w.within;
            }
            else
            {
                Layout own = r.layout;
                own.routeBook.reset();
                Profile p = r.layout.profile;
                p.maxGrade = m.maxGrade;
                e.pts = route(own, r.ground, p, m.a, m.b);
                e.source = "own";
                e.note = why + ", so Settlement's own router took it";
                double steepest = 0.0;
                for (size_t k = 0; k + 1 < e.pts.size(); ++k)
                    steepest = std::max(steepest, r.ground.gradeAlong(e.pts[k], e.pts[k + 1]));
                e.gradeFound = steepest;
                e.withinGrade = steepest <= m.maxGrade + 1e-9;
                notes.push_back(std::format("route from [{:.0f}, {:.0f}] to [{:.0f}, {:.0f}]: {}", m.a.x, m.a.y, m.b.x, m.b.y, e.note));
            }
            book.entries.push_back(std::move(e));
        }
    }
    edits["terrainRoutes"] = toJson(book);
    int onTerrain = 0, own = 0, steep = 0;
    for (const RouteBook::Entry& e : book.entries)
    {
        (e.source == "terrain" ? onTerrain : own)++;
        steep += !e.withinGrade;
    }
    return {{"routes", book.entries.size()}, {"terrain", onTerrain}, {"own", own}, {"overGrade", steep}, {"passes", passes + 1}, {"notes", notes}};
}

// ---------------------------------------------------------------------------------------------------------------------
// Re-planning

/** Brings the pinned plan up to the terrain: re-plans what changed (or all of it) and pins the result in edits. */
json replan(Graph& g, const std::string& name, const std::string& terrain, json& edits, bool routes, bool full, const std::string& cmd)
{
    LILA_ZONE();

    const Rebuilt before = rebuildSettlement(g, name, cmd.c_str(), &edits);
    std::string mode = "dirty", why;
    std::vector<DirtyRect> rects;
    if (full)
        mode = "full", why = "full=True";
    else if (!edits.contains("plan") || edits["plan"].is_null())
        mode = "full", why = "no pinned plan yet";
    else if (before.layout.plan == "stale")
        mode = "full", why = "the settlement's inputs changed since the plan was pinned";
    else if (edits["plan"].value("terrain", std::string()) != terrain)
        mode = "full", why = "the plan was made on another terrain";
    else
    {
        const json d = terrainCall("terrainDirtyRegions", terrain, {{"since", edits["plan"].value("revision", std::string())}}, cmd);
        if (!d.value("known", false))
            mode = "full", why = "the plan's terrain revision is too old to compare";
        else
        {
            for (const json& reg : d.value("regions", json::array()))
                rects.push_back({groundPoint(reg.at("min")), groundPoint(reg.at("max"))});
            if (rects.empty())
                mode = "unchanged", why = "the terrain has not changed since the plan";
        }
    }

    json report = {{"mode", mode}, {"why", why}};
    if (mode == "full")
    {
        edits.erase("plan");
        if (routes)
            report["routes"] = routeMainRoads(g, name, terrain, edits, {}, true, cmd);
        const Rebuilt r = rebuildSettlement(g, name, cmd.c_str(), &edits);
        const std::string rev = revisionOf(terrain, cmd);
        edits["plan"] = planToJson(r.layout, planKey(r.site, r.options), rev, terrain);
        report["blocks"] = r.layout.blocks.size();
        report["revision"] = rev;
    }
    else if (mode == "dirty")
    {
        json fresh = edits;
        fresh.erase("plan");
        if (routes)
        {
            report["routes"] = routeMainRoads(g, name, terrain, fresh, rects, false, cmd);
            edits["terrainRoutes"] = fresh["terrainRoutes"];
        }
        const Rebuilt now = rebuildSettlement(g, name, cmd.c_str(), &fresh);
        json counts;
        const Layout m = mergeDirty(before.layout, now.layout, rects, &counts);
        const std::string rev = revisionOf(terrain, cmd);
        edits["plan"] = planToJson(m, planKey(now.site, now.options), rev, terrain);
        report.update(counts);
        json regions = json::array();
        for (const DirtyRect& r : rects)
            regions.push_back({{"min", linkXz(r.lo)}, {"max", linkXz(r.hi)}});
        report["regions"] = regions;
        report["revision"] = rev;
    }
    else
    {
        report["revision"] = edits["plan"].value("revision", std::string());
        report["blocks"] = before.layout.blocks.size();
    }
    return report;
}

// ---------------------------------------------------------------------------------------------------------------------
// Grading

const char* terrainProfileFor(StreetClass c)
{
    switch (c)
    {
    case StreetClass::Alley: return "lane";
    case StreetClass::Path: return "path";
    default: return "urban";
    }
}

double maxBankFor(StreetClass c)
{
    switch (c)
    {
    case StreetClass::Arterial: return 6.0;
    case StreetClass::Collector: return 4.0;
    case StreetClass::Local: return 2.0;
    default: return 0.0;
    }
}

double lineDistance(std::span<const glm::dvec3> pts, Vec2 q)
{
    double best = std::numeric_limits<double>::max();
    for (size_t k = 0; k + 1 < pts.size(); ++k)
    {
        const Vec2 a(pts[k].x, pts[k].z), b(pts[k + 1].x, pts[k + 1].z), d = b - a;
        const double t = std::clamp(glm::dot(q - a, d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
        best = std::min(best, glm::length(a + d * t - q));
    }
    return best;
}

std::string nodeKey(Vec2 p) { return std::format("{},{}", std::llround(p.x), std::llround(p.y)); }

double lineLength(const StreetLine& line)
{
    double length = 0.0;
    for (size_t k = 0; k + 1 < line.pts.size(); ++k)
        length += glm::length(Vec2(line.pts[k + 1].x - line.pts[k].x, line.pts[k + 1].z - line.pts[k].z));
    return length;
}

/** The point at arc length d along the line from its start (or from its end), on the plan. */
Vec2 alongLine(const StreetLine& line, double d, bool fromEnd)
{
    const size_t n = line.pts.size();
    auto at = [&](size_t i) { const glm::dvec3& q = line.pts[fromEnd ? n - 1 - i : i]; return Vec2(q.x, q.z); };
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const double seg = glm::length(at(i + 1) - at(i));
        if (d <= seg)
            return at(i) + (at(i + 1) - at(i)) * (seg > 0.0 ? d / seg : 0.0);
        d -= seg;
    }
    return at(n - 1);
}

/**
 * How far each junction is held level along the streets meeting there: past the widest one's kerb with room to spare,
 * so a street joining another meets its surface at the junction's height whatever the other's grade or bank.
 */
std::map<int, double> junctionPlateaus(const std::vector<StreetLine>& lines, const Layout& l)
{
    std::map<int, double> out;
    for (const StreetLine& line : lines)
        if (!line.surface && line.startNode >= 0 && line.endNode >= 0)
            for (int n : {line.startNode, line.endNode})
                if (l.graph.degree(n) >= 3)
                    out[n] = std::max(out[n], 0.5 * line.width + 400.0);
    return out;
}

/** The plateau each end of a line keeps level, or none at either end when the line is too short for both. */
std::pair<double, double> linePlateaus(const StreetLine& line, const std::map<int, double>& plateaus)
{
    auto of = [&](int n) { auto it = plateaus.find(n); return it != plateaus.end() ? it->second : 0.0; };
    const double a = of(line.startNode), b = of(line.endNode);
    if (lineLength(line) < a + b + 300.0)
        return {0.0, 0.0};
    return {a, b};
}

/**
 * Heights for every street line's ends, so that no line has to climb faster than its class's grade between its two
 * ends. Each node starts at its height from the last grade (kept, so re-grading sends the same keys) or the ground,
 * then the result is the mean of the highest grade-limited heights at or below those and the lowest at or above them:
 * both meet every line's limit, so their mean does too, and a network that already meets them is left as it is.
 */
std::map<int, double> junctionHeights(const std::vector<StreetLine>& lines, const Layout& l, const Ground& ground, const json& kept,
                                      const std::map<int, double>& plateaus)
{
    std::map<int, double> base;
    std::map<int, std::vector<std::pair<int, double>>> links;
    for (const StreetLine& line : lines)
    {
        if (line.surface || line.startNode < 0 || line.endNode < 0 || line.pts.size() < 2)
            continue;
        // Only the part between the level junction ends climbs, and a short street between two junctions stays level.
        const auto [pa, pb] = linePlateaus(line, plateaus);
        const bool shortBetween = pa == 0.0 && plateaus.contains(line.startNode) && plateaus.contains(line.endNode);
        const double climb = shortBetween ? 0.0 : std::max(0.0, lineLength(line) - pa - pb);
        // A margin under the limit: the road solver measures along its own stations and rounds its vertical curves.
        const double rise = 0.7 * classMaxGrade(l.profile, line.cls) * climb;
        for (int n : {line.startNode, line.endNode})
            if (!base.contains(n))
            {
                const Vec2 p = l.graph.nodes[size_t(n)].p;
                const json& h = kept.contains(nodeKey(p)) ? kept[nodeKey(p)] : json();
                base[n] = h.is_number() ? h.get<double>() : ground.surfaceHeight(p);
            }
        if (line.startNode != line.endNode)
        {
            links[line.startNode].push_back({line.endNode, rise});
            links[line.endNode].push_back({line.startNode, rise});
        }
    }
    auto envelope = [&](double sign) {
        // sign 1: the highest heights no line limit is broken by, at or below the base; -1: the lowest at or above it.
        std::map<int, double> h;
        std::priority_queue<std::pair<double, int>, std::vector<std::pair<double, int>>, std::greater<>> open;
        for (const auto& [n, v] : base)
        {
            h[n] = sign * v;
            open.push({sign * v, n});
        }
        while (!open.empty())
        {
            const auto [v, n] = open.top();
            open.pop();
            if (v > h[n])
                continue;
            for (const auto& [m, rise] : links[n])
                if (v + rise < h[m])
                {
                    h[m] = v + rise;
                    open.push({h[m], m});
                }
        }
        return h;
    };
    const std::map<int, double> below = envelope(1.0), above = envelope(-1.0);
    std::map<int, double> out;
    for (const auto& [n, v] : below)
        out[n] = 0.5 * (v - above.at(n));
    return out;
}

/** One terrain call the grade makes: kept by id and the hash of what it sends, so an unchanged part is not sent again. */
struct Send
{
    std::string id, command;
    json flags;
    std::string hash;
};

json grade(Graph& g, const std::string& name, const Args& a, const std::string& cmd)
{
    LILA_ZONE();

    Node* mod = modifierOf(g, name, cmd);
    json edits = editsOf(*mod, cmd);
    Rebuilt r = rebuildSettlement(g, name, cmd.c_str(), &edits);
    const std::string terrain = terrainOf(r, a, cmd);
    for (const char* need : {"terrainAddRoad", "terrainStampPad", "terrainStampRegion", "terrainStamps", "terrainRoads", "terrainDirtyRegions"})
        if (!ops::find(need))
            throw CommandError(cmd + ": the Terrain command " + std::string(need) + " is not available; load the Terrain extension");

    json out = json::object();
    if (a.get<bool>("replan", true) && edits.contains("plan") && !edits["plan"].is_null())
    {
        out["replan"] = replan(g, name, terrain, edits, true, false, cmd);
        r = rebuildSettlement(g, name, cmd.c_str(), &edits);
    }

    const std::string key = r.mesh.empty() ? r.modifier : r.mesh;
    const std::string roadLayer = a.get<std::string>("roadLayer", "CityRoads"), layer = "City";
    const std::string padMode = a.get<std::string>("padMode", "flatten");
    const json graded = edits.value("terrain", json::object()).value("graded", json::object());
    json errors = json::array();
    json keys = edits.value("terrain", json::object()).value("keys", json::object());
    std::vector<Send> roads, rings, stamps, rivers;
    std::map<std::string, std::vector<std::string>> ringRoads;

    if (a.get<bool>("streets", true))
    {
        std::vector<std::pair<Vec2, bool>> spans; // midpoints of bridge (true) and tunnel (false) edges
        for (size_t e = 0; e < r.layout.graph.edges.size(); ++e)
        {
            if (!r.layout.graph.isStreet(int(e)))
                continue;
            const StreetEdge& x = r.layout.graph.edges[e];
            const Vec2 mid = (r.layout.graph.nodes[size_t(x.a)].p + r.layout.graph.nodes[size_t(x.b)].p) * 0.5;
            if (x.tunnel)
                spans.push_back({mid, false});
            else if (x.bridge || (edgeIsBridge(r.layout, int(e)) && edgeInWater(r.layout, int(e))))
                spans.push_back({mid, true});
        }
        std::map<std::string, int> taken;
        std::map<uint64_t, std::string> idOfStreet;
        const std::vector<StreetLine> lines = streetLines(r.layout, r.ground);
        const std::map<int, double> plateaus = junctionPlateaus(lines, r.layout);
        const std::map<int, double> heights =
            junctionHeights(lines, r.layout, r.ground, edits.value("terrain", json::object()).value("keys", json::object()), plateaus);
        keys = json::object();
        for (const auto& [n, h] : heights)
            keys[nodeKey(r.layout.graph.nodes[size_t(n)].p)] = h;
        for (const StreetLine& line : lines)
        {
            if (line.surface || line.pts.size() < 2)
                continue;
            std::string id = key + "/street/" + std::to_string(line.street);
            if (const int n = taken[id]++; n > 0)
                id += "/" + std::to_string(n);
            idOfStreet.emplace(line.street, id);
            bool bridge = false, tunnel = false;
            for (const auto& [mid, isBridge] : spans)
                if (lineDistance(line.pts, mid) < 1.0)
                    (isBridge ? bridge : tunnel) = true;
            const RoadProfile& rp = r.layout.roads[size_t(line.profile)];
            json path = json::array();
            for (const glm::dvec3& q : line.pts)
                path.push_back(json::array({q.x, q.z}));
            json c = {{"id", id},
                      {"layer", roadLayer},
                      {"path", path},
                      {"kind", line.cls == StreetClass::Path ? "path" : "road"},
                      {"profile", terrainProfileFor(line.cls)},
                      {"smooth", false},
                      {"maxGrade", classMaxGrade(r.layout.profile, line.cls) * 100.0},
                      {"designSpeed", std::max(5.0, rp.speed)},
                      {"maxBank", maxBankFor(line.cls)},
                      {"bridgeHeight", bridge ? 150.0 : 0.0},
                      {"tunnelDepth", tunnel ? 300.0 : 0.0},
                      {"masks", json::array({"paved", "cityFootprint"})}};
            json ends = json::array();
            const auto [pa, pb] = linePlateaus(line, plateaus);
            for (int n : {line.startNode, line.endNode})
                if (auto it = heights.find(n); it != heights.end())
                {
                    const Vec2 p = r.layout.graph.nodes[size_t(n)].p;
                    ends.push_back(json::array({p.x, it->second, p.y}));
                    if (const double d = n == line.startNode ? pa : pb; d > 0.0)
                    {
                        const Vec2 q = alongLine(line, d, n != line.startNode);
                        ends.push_back(json::array({q.x, it->second, q.y}));
                    }
                }
            if (!ends.empty())
                c["keys"] = ends;
            if (rp.lanes > 0 && line.cls != StreetClass::Path)
            {
                const double sides = 2.0 * (rp.sidewalk + rp.verge + rp.parking);
                c["lanes"] = rp.lanes;
                c["laneWidth"] = std::max(100.0, (line.width - sides - rp.median) / rp.lanes);
                c["sidewalk"] = rp.sidewalk;
                c["verge"] = rp.verge;
                c["kerbHeight"] = rp.sidewalk > 0.0 ? 15.0 : 0.0;
            }
            else
                c["width"] = std::max(100.0, line.width);
            roads.push_back({id, "terrainAddRoad", c, linkHex(linkHash(c.dump()))});
        }

        if (const std::string mode = a.get<std::string>("roundabouts", "arterial"); mode != "none")
        {
            const RoadNetwork net = roadNetwork(r.layout, r.ground);
            for (const Junction& j : net.junctions)
            {
                int arterials = 0;
                std::vector<uint64_t> streets;
                for (const RoadEnd& e : j.ends)
                {
                    const StreetLine& l = net.lines[size_t(e.line)];
                    arterials += l.cls == StreetClass::Arterial && !l.surface;
                    streets.push_back(l.street);
                }
                if (arterials < 3)
                    continue;
                std::sort(streets.begin(), streets.end());
                std::string ids;
                for (uint64_t s : streets)
                    ids += std::to_string(s) + ",";
                const std::string id = key + "/roundabout/" + linkHex(linkHash(ids));
                json c = {{"id", id},
                          {"layer", roadLayer},
                          {"at", json::array({j.p.x, j.p.z})},
                          {"radius", std::clamp(j.radius * 1.5, 1200.0, 4000.0)},
                          {"profile", "urban"},
                          {"lanes", 1}};
                std::string joined;
                for (uint64_t s : streets)
                    if (auto it = idOfStreet.find(s); it != idOfStreet.end())
                    {
                        ringRoads[id].push_back(it->second);
                        joined += it->second + ";";
                    }
                rings.push_back({id, "terrainAddRoundabout", c, linkHex(linkHash(c.dump() + joined))});
            }
        }
    }

    if (a.get<bool>("lots", true))
    {
        std::map<int, const Envelope*> byLot;
        for (const Envelope& e : r.layout.envelopes)
            byLot[e.lot] = &e;
        for (const Lot& lot : r.layout.lots)
        {
            // Farmland, parks and cemeteries follow the ground; only built lots get a pad.
            if (lot.use == Use::Open || lot.use == Use::Plaza || lot.use == Use::Farm || lot.use == Use::Park || lot.use == Use::Cemetery)
                continue;
            auto it = byLot.find(lot.id);
            const Polygon& shape = it != byLot.end() ? it->second->footprint : lot.outline;
            const std::string id = key + "/lot/" + (lot.key ? linkHex(lot.key) : std::to_string(lot.id));
            json c = {{"footprint", xzLine(shape.pts)}, {"id", id}, {"layer", layer}, {"mode", padMode}, {"masks", json::array({"cityFootprint"})}};
            stamps.push_back({id, "terrainStampPad", c, linkHex(linkHash(c.dump()))});
        }
    }
    if (a.get<bool>("plazas", true))
        for (const Lot& lot : r.layout.lots)
            if (lot.use == Use::Plaza)
            {
                const std::string id = key + "/plaza/" + (lot.key ? linkHex(lot.key) : std::to_string(lot.id));
                json c = {{"footprint", xzLine(lot.outline.pts)}, {"id", id}, {"layer", layer}, {"mode", "flatten"}, {"masks", json::array({"cityFootprint", "paved"})}};
                stamps.push_back({id, "terrainStampRegion", c, linkHex(linkHash(c.dump()))});
            }
    if (a.get<bool>("water", true))
        for (size_t k = 0; k < r.layout.water.size(); ++k)
        {
            const WaterLine& w = r.layout.water[k];
            if (w.kind.empty() || w.pts.size() < 2)
                continue;
            const std::string id = key + "/water/" + std::to_string(k);
            if (w.kind == "canal" || w.kind == "moat")
            {
                json c = {{"id", id}, {"layer", layer}, {"kind", w.kind}, {"path", xzLine(w.pts)}, {"width", w.width}, {"depth", 200.0}};
                stamps.push_back({id, "terrainAddFeature", c, linkHex(linkHash(c.dump()))});
            }
            else if (w.kind == "quay")
            {
                // A quay down each bank, the land beside the water on the outer side.
                for (const char* side : {"left", "right"})
                {
                    const double s = std::string_view(side) == "left" ? 1.0 : -1.0;
                    std::vector<Vec2> bank;
                    for (size_t i = 0; i < w.pts.size(); ++i)
                    {
                        const Vec2 d = glm::normalize(w.pts[std::min(i + 1, w.pts.size() - 1)] - w.pts[i > 0 ? i - 1 : 0]);
                        bank.push_back(w.pts[i] + leftNormal(d) * (0.5 * w.width * s));
                    }
                    const std::string qid = id + "/" + side;
                    json c = {{"id", qid}, {"layer", layer}, {"kind", "quay"}, {"side", side}, {"path", xzLine(bank)}, {"width", 400.0}};
                    stamps.push_back({qid, "terrainStampQuay", c, linkHex(linkHash(c.dump()))});
                }
            }
            else if (w.kind == "river")
            {
                const std::string rid = key + "/river/" + std::to_string(k);
                json c = {{"id", rid}, {"path", xzLine(w.pts)}, {"width", w.width}};
                rivers.push_back({rid, "terrainAddRiver", c, linkHex(linkHash(c.dump()))});
            }
        }

    // What the terrain has now, to tell unchanged parts from missing ones and find the stale ones.
    std::set<std::string> haveStamps, haveRoads;
    for (const json& s : terrainCall("terrainStamps", terrain, json::object(), cmd).value("layers", json::object()).value(layer, json::array()))
        haveStamps.insert(s.value("id", std::string()));
    for (const json& s : terrainCall("terrainRoads", terrain, json::object(), cmd).value("layers", json::object()).value(roadLayer, json::array()))
        haveRoads.insert(s.value("id", std::string()));
    auto unchanged = [&](const Send& s, const std::set<std::string>& have) { return graded.value(s.id, std::string()) == s.hash && have.contains(s.id); };

    std::set<std::string> resend;
    for (const Send& s : roads)
        if (!unchanged(s, haveRoads))
            resend.insert(s.id);
    std::set<std::string> ringsToSend;
    for (const Send& ring : rings)
    {
        bool changed = !unchanged(ring, haveRoads);
        for (const std::string& rid : ringRoads[ring.id])
            changed = changed || resend.contains(rid);
        if (changed)
        {
            ringsToSend.insert(ring.id);
            // A ring cuts its roads back, so they go again in full for the new ring to cut.
            for (const std::string& rid : ringRoads[ring.id])
                resend.insert(rid);
        }
    }

    json made = json::object();
    int sentRoads = 0, sentRings = 0, sentStamps = 0, sentRivers = 0, kept = 0;
    auto send = [&](const Send& s) {
        try
        {
            terrainCall(s.command.c_str(), terrain, s.flags, cmd);
            return true;
        }
        catch (const CommandError& e)
        {
            if (std::string_view(e.what()).find("is not available") != std::string_view::npos)
                throw;
            if (errors.size() < 20)
                errors.push_back(s.command + " " + s.id + ": " + e.what());
        }
        catch (const std::exception& e)
        {
            if (errors.size() < 20)
                errors.push_back(s.command + " " + s.id + ": " + e.what());
        }
        return false;
    };
    auto isPart = [&](const std::string& id, const std::string& base) {
        return id.size() == base.size() + 2 && id.starts_with(base) && id[base.size()] == '_' && std::isalpha(static_cast<unsigned char>(id.back()));
    };
    std::vector<std::string> partsToRemove;
    for (const Send& s : roads)
    {
        made[s.id] = s.hash;
        if (!resend.contains(s.id))
        {
            ++kept;
            continue;
        }
        for (const std::string& have : haveRoads)
            if (isPart(have, s.id))
                partsToRemove.push_back(have);
    }
    if (!partsToRemove.empty())
        terrainCall("terrainRoadRemove", terrain, {{"ids", partsToRemove}, {"layer", roadLayer}}, cmd);
    for (const Send& s : roads)
        if (resend.contains(s.id))
            sentRoads += send(s);
    for (const Send& s : rings)
    {
        made[s.id] = s.hash;
        if (ringsToSend.contains(s.id))
            sentRings += send(s);
        else
            ++kept;
    }
    for (const Send& s : stamps)
    {
        made[s.id] = s.hash;
        if (unchanged(s, haveStamps))
            ++kept;
        else
            sentStamps += send(s);
    }
    const json prevRivers = edits.value("terrain", json::object()).value("rivers", json::array());
    json riverIds = json::array();
    for (const Send& s : rivers)
    {
        made[s.id] = s.hash;
        riverIds.push_back(s.id);
        if (graded.value(s.id, std::string()) == s.hash)
            ++kept;
        else if (!ops::find("terrainAddRiver"))
            errors.push_back(cmd + ": the Terrain command terrainAddRiver is not available, so river " + s.id + " was not added");
        else
            sentRivers += send(s);
    }

    // Parts this settlement sent before and no longer has.
    json staleStamps = json::array(), staleRoads = json::array(), staleRivers = json::array();
    for (const std::string& id : haveStamps)
        if (id.starts_with(key + "/") && !made.contains(id))
            staleStamps.push_back(id);
    for (const std::string& id : haveRoads)
    {
        if (!id.starts_with(key + "/") || made.contains(id))
            continue;
        bool part = false;
        for (const Send& s : roads)
            part = part || isPart(id, s.id);
        if (!part)
            staleRoads.push_back(id);
    }
    for (const json& id : prevRivers)
        if (!made.contains(id.get<std::string>()))
            staleRivers.push_back(id);
    if (!staleStamps.empty())
        terrainCall("terrainStampRemove", terrain, {{"ids", staleStamps}, {"layer", layer}}, cmd);
    if (!staleRoads.empty())
        terrainCall("terrainRoadRemove", terrain, {{"ids", staleRoads}, {"layer", roadLayer}}, cmd);
    if (!staleRivers.empty() && ops::find("terrainWaterRemove"))
        terrainCall("terrainWaterRemove", terrain, {{"ids", staleRivers}}, cmd);

    // Every street the terrain could not lay within its grade is reported with the road solver's reason.
    json overGrade = json::array();
    if (a.get<bool>("streets", true))
        for (const json& road : terrainCall("terrainRoads", terrain, json::object(), cmd).value("layers", json::object()).value(roadLayer, json::array()))
        {
            const std::string id = road.value("id", std::string());
            const double limit = road.value("design", json::object()).value("maxGrade", 0.0), grade = road.value("maxGrade", 0.0);
            if (!id.starts_with(key + "/street/") || limit <= 0.0 || grade <= limit * 1.01 + 0.05)
                continue;
            std::string why;
            for (const json& w : road.value("warnings", json::array()))
                why += (why.empty() ? "" : "; ") + w.get<std::string>();
            if (why.empty())
                why = "the ground under it is steeper than the limit over a stretch the road cannot lengthen";
            log::warn("{}: {} climbs {:.1f}% over its {:.1f}%: {}", cmd, id, grade, limit, why);
            overGrade.push_back({{"id", id}, {"length", road.value("length", 0.0)}, {"grade", grade}, {"maxGrade", limit}, {"why", why}});
        }
    out["overGrade"] = overGrade;

    const std::string rev = revisionOf(terrain, cmd);
    edits["terrain"] ={{"terrain", terrain}, {"revision", rev}, {"graded", made}, {"rivers", riverIds}, {"keys", keys}};
    // The plan is pinned at the graded terrain, so the grading itself never counts as a change to re-plan for.
    if (edits.contains("plan") && !edits["plan"].is_null() && r.layout.plan == "pinned")
    {
        edits["plan"]["revision"] = rev;
        out["plan"] = "pinned";
    }
    else
    {
        edits["plan"] = planToJson(r.layout, planKey(r.site, r.options), rev, terrain);
        out["plan"] = r.layout.plan == "stale" ? "repinned" : "pinned";
    }
    setEditsOf(*mod, edits);

    int bridges = 0, tunnels = 0;
    for (const Send& s : roads)
    {
        bridges += s.flags.value("bridgeHeight", 0.0) > 0.0;
        tunnels += s.flags.value("tunnelDepth", 0.0) > 0.0;
    }
    out["streets"] = roads.size();
    out["streetsSent"] = sentRoads;
    out["bridgeStreets"] = bridges;
    out["tunnelStreets"] = tunnels;
    out["roundabouts"] = rings.size();
    out["roundaboutsSent"] = sentRings;
    int pads = 0, regions = 0, water = 0;
    for (const Send& s : stamps)
    {
        pads += s.command == "terrainStampPad";
        regions += s.command == "terrainStampRegion";
        water += s.command == "terrainAddFeature" || s.command == "terrainStampQuay";
    }
    out["pads"] = pads;
    out["regions"] = regions;
    out["waterFeatures"] = water + int(rivers.size());
    out["stampsSent"] = sentStamps;
    out["riversSent"] = sentRivers;
    out["unchanged"] = kept;
    out["removed"] = staleStamps.size() + staleRoads.size() + staleRivers.size();
    out["roadLayer"] = roadLayer;
    out["terrain"] = terrain;
    out["revision"] = rev;
    out["errors"] = errors;
    return out;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Siting on a Terrain

bool isTerrainGround(const std::string& node)
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

std::vector<WaterLine> terrainRivers(const std::string& terrain, const json& analysis)
{
    std::vector<WaterLine> out;
    const json w = terrainCall("terrainWaterInfo", terrain, analysis, "settlement");
    for (const json& r : w.value("rivers", json::array()))
    {
        WaterLine line;
        double width = 0.0;
        for (const json& p : r.value("points", json::array()))
        {
            line.pts.push_back(groundPoint(p.value("position", json())));
            width += p.value("width", 0.0);
        }
        if (line.pts.size() < 2)
            continue;
        line.width = std::max(100.0, width / double(line.pts.size()));
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<Vec2> terrainRoad(const std::string& terrain, Vec2 a, Vec2 b, double maxGrade, std::string* note)
{
    TerrainWay w = routeTerrain(terrain, a, b, maxGrade, "settlementRegion");
    if (note)
        *note = w.note.empty() && !w.within ? "nothing kept within grade, so the gentlest way was taken" : w.note;
    return w.pts;
}

std::vector<SiteScore> suggestOnTerrain(const std::string& terrain, const Polygon& outline, const Ground& ground, const SiteOptions& o,
                                        const json& analysis, json* report)
{
    LILA_ZONE();

    const std::string cmd = "settlementSuggest";
    if (outline.pts.size() < 3)
        return {};
    const int count = std::max(1, o.count);
    const double spacing = o.spacing > 0.0 ? o.spacing : std::sqrt(area(outline) / count) * 0.8;
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : outline.pts)
        lo = glm::min(lo, q), hi = glm::max(hi, q);
    const double cell = std::max({spacing / 8.0, (hi.x - lo.x) / 100.0, (hi.y - lo.y) / 100.0, 1000.0});

    std::vector<Vec2> cands;
    for (double y = lo.y + cell * 0.5; y < hi.y; y += cell)
        for (double x = lo.x + cell * 0.5; x < hi.x; x += cell)
            if (contains(outline.pts, {x, y}) && ground.solid({x, y}))
                cands.push_back({x, y});
    for (const json& s : terrainCall("terrainSites", terrain, merged(analysis, {{"kind", "townCentre"}, {"count", 24}, {"spacing", spacing * 0.5}}), cmd)
                             .value("sites", json::array()))
        if (const Vec2 q = groundPoint(s.value("position", json())); contains(outline.pts, q))
            cands.push_back(q);
    if (cands.empty())
        return {};

    json pts = json::array();
    for (const Vec2& q : cands)
        pts.push_back(linkXz(q));
    const json samples = terrainCall("terrainSample", terrain,
                                     merged(analysis, {{"points", pts},
                                                       {"fields", {"floodRisk", "distanceToWater", "prominence", "viewshedScore", "solar", "windExposure"}}}),
                                     cmd);
    const json water = terrainCall("terrainWaterInfo", terrain, analysis, cmd);

    struct Feature
    {
        Vec2 p;
        double weight;
        std::string what;
    };
    std::vector<Feature> trade, resource;
    for (const json& f : water.value("fords", json::array()))
        trade.push_back({groundPoint(f["position"]), 1.0, std::format("ford on river {}", f.value("river", -1))});
    for (const json& f : water.value("bridgeSites", json::array()))
        trade.push_back({groundPoint(f["position"]), std::clamp(f.value("score", 0.5), 0.0, 1.0), std::format("bridge site on river {}", f.value("river", -1))});
    for (const json& f : water.value("confluences", json::array()))
        trade.push_back({groundPoint(f["position"]), 0.9, std::format("confluence of rivers {} and {}", f.value("from", -1), f.value("into", -1))});
    std::vector<Feature> harbours;
    for (const json& l : water.value("lakes", json::array()))
        for (const json& h : l.value("harbourSites", json::array()))
            harbours.push_back({groundPoint(h["position"]), std::clamp(h.value("score", 0.5), 0.0, 1.0), "lake harbour"});
    if (water.contains("sea") && water["sea"].is_object())
        for (const json& h : water["sea"].value("harbourSites", json::array()))
            harbours.push_back({groundPoint(h["position"]), std::clamp(h.value("score", 0.5), 0.0, 1.0), "sea harbour"});
    for (const Feature& h : harbours)
        trade.push_back(h);

    const std::string& kind = o.rules.kind;
    double resourceReach = o.waterReach;
    std::string resourceName;
    if (kind == "port")
    {
        resource = harbours;
        resourceReach = o.waterReach * 0.5;
        resourceName = "harbour site";
    }
    else if (kind != "fort")
    {
        const char* site = kind == "mining" ? "quarry" : kind == "mill" ? "mill" : "farmland";
        resourceName = kind == "mining" ? "quarry rock" : kind == "mill" ? "mill race" : "farmland";
        if (kind == "mill")
            resourceReach = o.waterReach * 0.5;
        for (const json& s : terrainCall("terrainSites", terrain, merged(analysis, {{"kind", site}, {"count", 30}, {"spacing", spacing * 0.25}}), cmd)
                                 .value("sites", json::array()))
            resource.push_back({groundPoint(s["position"]), std::clamp(s.value("score", 0.5), 0.0, 1.0), resourceName});
    }

    struct Region
    {
        std::vector<Vec2> outline;
        std::vector<std::vector<Vec2>> holes;
    };
    std::vector<Region> buildable;
    const double maxSlope = std::atan(o.maxGrade * 1.5) * 180.0 / kPi;
    for (const json& reg : terrainCall("terrainBuildable", terrain, merged(analysis, {{"minArea", 2000.0}, {"maxSlope", maxSlope}, {"avoidFlood", false}}), cmd)
                               .value("regions", json::array()))
    {
        Region rg;
        for (const json& p : reg.value("outline", json::array()))
            rg.outline.push_back(groundPoint(p));
        for (const json& h : reg.value("holes", json::array()))
        {
            rg.holes.emplace_back();
            for (const json& p : h)
                rg.holes.back().push_back(groundPoint(p));
        }
        if (rg.outline.size() >= 3)
            buildable.push_back(std::move(rg));
    }

    auto nearest = [](const std::vector<Feature>& list, Vec2 q, double reach, const Feature** best) {
        double v = 0.0;
        for (const Feature& f : list)
        {
            const double d = glm::length(f.p - q) / std::max(reach, 1.0);
            if (const double s = f.weight * std::exp(-d * d); s > v)
            {
                v = s;
                *best = &f;
            }
        }
        return v;
    };

    std::vector<SiteFacts> facts;
    for (size_t i = 0; i < cands.size() && i < samples.size(); ++i)
    {
        const json& s = samples[i];
        if (!s.value("inside", true))
            continue;
        const Vec2 q = cands[i];
        SiteFacts f;
        f.p = q;
        const double pct = std::tan(s.value("slope", 0.0) * kPi / 180.0) * 100.0;
        f.set(SiteTerm::Flat, std::clamp(1.0 - pct / std::max(o.maxGrade * 100.0, 0.1), 0.0, 1.0), std::format("slope {:.1f}%", pct));
        const double d = s.value("distanceToWater", 1e9);
        if (d < 0.0)
            f.set(SiteTerm::Water, 0.0, "in the water");
        else if (d >= 1e8)
            f.set(SiteTerm::Water, 0.0, "no water on the map");
        else
        {
            const double t = d / std::max(o.waterReach * 0.5, 1.0);
            f.set(SiteTerm::Water, std::exp(-t * t), std::format("water {:.0f} m away", d / 100.0));
        }
        const Feature* best = nullptr;
        const double tv = nearest(trade, q, o.waterReach * 0.5, &best);
        f.set(SiteTerm::Trade, tv, best ? std::format("{}, {:.0f} m away", best->what, glm::length(best->p - q) / 100.0) : std::string("no crossing or harbour near"));
        const double prom = s.value("prominence", 0.0), view = s.value("viewshedScore", 0.0);
        f.set(SiteTerm::Defense, std::clamp(0.7 * prom + 0.3 * view, 0.0, 1.0), std::format("prominence {:.2f}, seen from afar {:.2f}", prom, view));
        if (kind != "fort")
        {
            const Feature* rb = nullptr;
            const double rv = nearest(resource, q, resourceReach, &rb);
            f.set(SiteTerm::Resource, rv,
                  rb ? std::format("{} {:.0f} m away", rb->what, glm::length(rb->p - q) / 100.0) : std::format("no {} on the map", resourceName));
        }
        const double solar = s.value("solar", 0.5), wind = s.value("windExposure", 0.5);
        f.set(SiteTerm::Climate, std::clamp(0.7 * solar + 0.3 * (1.0 - wind), 0.0, 1.0), std::format("sun {:.2f}, wind {:.2f}", solar, wind));
        const double flood = std::clamp(s.value("floodRisk", 0.0), 0.0, 1.0);
        f.set(SiteTerm::Flood, flood, std::format("flood risk {:.2f}", flood));
        f.edge = std::clamp(boundaryDistance(outline.pts, q) / std::max(spacing * 0.25, 1.0), 0.0, 1.0);
        if (!buildable.empty())
        {
            bool in = false;
            for (const Region& rg : buildable)
                if (contains(rg.outline, q))
                {
                    in = true;
                    for (const auto& h : rg.holes)
                        in = in && !contains(h, q);
                }
            if (!in)
            {
                f.gate = 0.25;
                f.gateWhy = "outside buildable land (terrainBuildable)";
            }
        }
        facts.push_back(std::move(f));
    }

    auto scoreAll = [&]() {
        std::vector<SiteScore> all;
        for (size_t i = 0; i < facts.size(); ++i)
            all.push_back(scoreSite(facts[i], o.rules, 1e-3 * unitRandom(hash64(o.seed) ^ hash64(uint64_t(i)))));
        return all;
    };
    std::vector<SiteScore> all = scoreAll();

    // The best few get their footprint's statistics, not just the ground at their centre, until the best are all measured so.
    const double radius = std::clamp(spacing * 0.15, 2000.0, 20000.0);
    std::vector<char> measured(facts.size(), 0);
    int refined = 0;
    for (int round = 0; round < 6; ++round)
    {
        std::vector<size_t> order(all.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return all[x].score > all[y].score; });
        const size_t top = std::min(order.size(), size_t(round == 0 ? std::max(count * 4, 12) : std::max(count * 2, 6)));
        bool any = false;
        for (size_t k = 0; k < top; ++k)
        {
            if (measured[order[k]])
                continue;
            measured[order[k]] = 1;
            any = true;
            SiteFacts& f = facts[order[k]];
            json ring = json::array();
            for (int i = 0; i < 12; ++i)
            {
                const double t = 2.0 * kPi * i / 12.0;
                ring.push_back(linkXz(f.p + Vec2(std::cos(t), std::sin(t)) * radius));
            }
            const json st = terrainCall("terrainRegionStats", terrain, merged(analysis, {{"polygon", ring}, {"fields", {"slope", "floodRisk"}}}), cmd);
            const json fields = st.value("fields", json::object());
            if (fields.contains("slope"))
            {
                const double pct = std::tan(fields["slope"].value("mean", 0.0) * kPi / 180.0) * 100.0;
                f.set(SiteTerm::Flat, std::clamp(1.0 - pct / std::max(o.maxGrade * 100.0, 0.1), 0.0, 1.0),
                      std::format("mean slope {:.1f}% within {:.0f} m (terrainRegionStats)", pct, radius / 100.0));
            }
            if (fields.contains("floodRisk"))
            {
                const double flood = std::clamp(fields["floodRisk"].value("p90", 0.0), 0.0, 1.0);
                f.set(SiteTerm::Flood, flood, std::format("flood risk {:.2f} over the worst tenth within {:.0f} m", flood, radius / 100.0));
            }
            ++refined;
        }
        if (!any)
            break;
        all = scoreAll();
    }
    for (SiteScore& s : all)
        s.reasons["ground"] = "terrain";
    std::vector<SiteScore> out = pickSites(std::move(all), count, spacing);
    if (report)
        *report = {{"ground", "terrain"}, {"candidates", facts.size()}, {"refined", refined}, {"crossings", trade.size()}, {"resources", resource.size()}};
    return out;
}

json followTerrain(Graph& g, const std::string& settlement, const std::string& ground, bool rivers, const char* cmd)
{
    LILA_ZONE();

    if (ground.empty() || !isTerrainGround(ground))
        return nullptr;
    Node* mod = modifierOf(g, settlement, cmd);
    json edits = editsOf(*mod, cmd);
    json out = {{"terrain", ground}};
    if (rivers)
    {
        const Rebuilt r = rebuildSettlement(g, settlement, cmd, &edits);
        int added = 0;
        if (r.layout.water.empty())
            for (const WaterLine& w : terrainRivers(ground, json::object()))
            {
                bool reaches = false;
                for (size_t k = 0; k < w.pts.size() && !reaches; ++k)
                    reaches = contains(r.layout.outline.pts, w.pts[k]) || (k + 1 < w.pts.size() && segmentCrosses(r.layout.outline.pts, w.pts[k], w.pts[k + 1]));
                if (!reaches)
                    continue;
                edits["water"].push_back({{"points", xzLine(w.pts)}, {"width", w.width}, {"source", "terrain"}});
                ++added;
            }
        out["rivers"] = added;
    }
    out["routes"] = routeMainRoads(g, settlement, ground, edits, {}, true, cmd);
    setEditsOf(*mod, edits);
    return out;
}

void registerTerrainCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "settlementGrade",
        .doc = "Shapes a Terrain under the settlement through the terrain contract. Streets go as terrain roads (terrainAddRoad, "
               "in roadLayer): cross-section from their road profile (lanes, lane width, sidewalk, kerb), the class's steepest "
               "grade, banking from the design speed, cut and fill, and bridge or tunnel spans where their edges are flagged "
               "(or an arterial crosses water); junctions where three or more main roads meet become roundabouts "
               "(terrainAddRoundabout). Lots become building pads (padMode), squares flat paved region stamps, and water the "
               "settlement builds goes in too: canals and moats as carved channels, quays down both banks (terrainStampQuay), "
               "rivers as terrain rivers (terrainAddRiver). Everything is keyed by the settlement and the part's stable id: "
               "grading again sends only what changed and removes what is gone. With replan, a pinned plan first catches up "
               "with terrain changes (settlementReplan). Grading pins the plan at the graded terrain, so the settlement no "
               "longer moves when the ground under it changes until it is re-planned. Needs the Terrain extension. Returns "
               "counts per kind (made and sent), what was unchanged and removed, the plan state, the revision and any errors.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"terrain", "tr", LinkT::String, nullptr, "The terrain to shape (default the settlement's ground)."},
                  {"streets", "st", LinkT::Bool, true, "Streets as terrain roads."},
                  {"lots", "lt", LinkT::Bool, true, "Lots as building pads (the envelope's footprint where there is one)."},
                  {"plazas", "pz", LinkT::Bool, true, "Squares as region stamps."},
                  {"water", "wa", LinkT::Bool, true, "Canals, moats, quays and rivers the settlement builds."},
                  {"roundabouts", "ra", LinkT::String, "arterial", "Junctions made roundabouts: where three or more main roads meet, or none.",
                   {"arterial", "none"}},
                  {"roadLayer", "rl", LinkT::String, "CityRoads", "Terrain roads layer the streets go in."},
                  {"replan", "rp", LinkT::Bool, true, "Re-plan the regions the terrain changed in first."},
                  {"padMode", "pm", LinkT::String, "flatten", "How pads meet the slope.", {"flatten", "carve", "raise", "blend", "plinth", "terraced", "stilts"}}},
        .run = [](const Args& a) -> json {
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("settlementGrade: give one settlement");
            return grade(Graph::current(), names[0], a, "settlementGrade");
        },
        .examples = {"settlementGrade('Settlement', terrain='Terrain', padMode='terraced')",
                     "g = settlementGrade('Settlement')\ng['streetsSent'], g['unchanged'], g['removed']"},
    });

    ctx.registerCommand({
        .name = "settlementReplan",
        .doc = "Brings a settlement on a Terrain up to the terrain under it. The first time (or with full=True, or after the "
               "settlement's own inputs changed) it routes its main roads with terrainRoute and pins the plan. After that it "
               "asks terrainDirtyRegions what changed since the plan's revision and re-plans only there: blocks touching a "
               "changed region are laid out again (with new ids), every other block keeps its id, lots and envelopes exactly, "
               "and main roads crossing a changed region are routed again. Returns the mode (full, dirty or unchanged) and why, "
               "the regions, blocks kept, dropped and new, street edges removed and added, the routes and the revision.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"terrain", "tr", LinkT::String, nullptr, "The terrain (default the settlement's ground)."},
                  {"routes", "ro", LinkT::Bool, true, "Route main roads with terrainRoute."},
                  {"full", "fu", LinkT::Bool, false, "Plan everything afresh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "settlementReplan";
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one settlement");
            Graph& g = Graph::current();
            Node* mod = modifierOf(g, names[0], cmd);
            json edits = editsOf(*mod, cmd);
            const Rebuilt r = rebuildSettlement(g, names[0], cmd.c_str(), &edits);
            const std::string terrain = terrainOf(r, a, cmd);
            json out = replan(g, names[0], terrain, edits, a.get<bool>("routes", true), a.get<bool>("full", false), cmd);
            setEditsOf(*mod, edits);
            out["terrain"] = terrain;
            return out;
        },
        .examples = {"settlementReplan('Town')",
                     "terrainSculpt('Terrain', op='raise', points=[[12000, 3000]], radius=3000, strength=800)\nsettlementReplan('Town')['keptBlocks']"},
    });

    ctx.registerCommand({
        .name = "settlementExplain",
        .doc = "Why a part of a settlement is where it is, as reasons an agent can read: id='centre' (the market square, its "
               "ground and water, the site it was chosen as), 'site' (every siting term with its value, weight and why, for "
               "settlements placed by settlementRegion), a street id or 'street:<id>' (its class and the rule that laid it: "
               "main road to a gate or attractor, routed by terrainRoute or Settlement's own router, grown or grid street, lane "
               "through a large block; bridges; its steepest grade against the class's limit), 'lot:<id>' and 'landmark:<id>' "
               "(why it has its use, its envelope), 'block:<id>'. Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"id", "i", LinkT::String, "centre", "centre, site, a street id, street:<id>, lot:<id>, landmark:<id> or block:<id>."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "settlementExplain";
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one settlement");
            Graph& g = Graph::current();
            Node* mod = modifierOf(g, names[0], cmd);
            const json edits = editsOf(*mod, cmd);
            const Rebuilt r = rebuildSettlement(g, names[0], cmd.c_str(), &edits);
            try
            {
                return explain(r.layout, r.ground, edits.value("siting", json()), a.get<std::string>("id", "centre"));
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
        },
        .examples = {"settlementExplain('Town', id='centre')['reasons']",
                     "s = settlementStreets('Town')['streets'][0]['id']\nsettlementExplain('Town', id=str(s))"},
    });

    ctx.registerCommand({
        .name = "settlementCheck",
        .doc = "Runs the settlement's verification checks and lists what fails, each with its check, severity, id (lot:<id>, "
               "street:<id>, junction:<n>, mode:<vehicles|people>) and where: frontage (a built lot with no street), overlap "
               "(lots sharing ground), grade (a street steeper than its class allows, bridges excepted), water (a street "
               "through water without a bridge), envelope (outside its lot's setbacks), foundation (a building's base above "
               "the ground under it), lanes (a mode's lane graph not strongly connected), phases (signal phases letting "
               "crossing approaches go together). report=True also lists them in the Validation panel on the settlement's "
               "mesh. Returns ok, counts per check, the issues, warnings (checks that could not run) and the plan state.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"report", "rp", LinkT::Bool, false, "Also report the issues in the Validation panel (replacing this check's earlier ones)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "settlementCheck";
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one settlement");
            const Rebuilt r = rebuildSettlement(Graph::current(), names[0], cmd.c_str());
            json out = checkLayout(r.layout, r.ground, r.options);
            out["plan"] = r.layout.plan;
            if (a.get<bool>("report", false))
            {
                const std::string target = r.mesh.empty() ? r.modifier : r.mesh;
                if (!ops::find("reportIssue") || !ops::find("clearIssues"))
                    throw CommandError(cmd + ": the Validation commands (reportIssue, clearIssues) are not available");
                ops::execute("clearIssues", {{"args", {target}}, {"source", cmd}});
                int reported = 0;
                for (const json& i : out["issues"])
                    if (reported++ < 200)
                        ops::execute("reportIssue", {{"args", {target}},
                                                     {"message", i["id"].get<std::string>() + ": " + i["message"].get<std::string>()},
                                                     {"severity", i["severity"]},
                                                     {"source", cmd}});
                out["reported"] = std::min(reported, 200);
            }
            return out;
        },
        .examples = {"c = settlementCheck('Town')\nc['ok'], c['counts']", "settlementCheck('Town', report=True)"},
    });
}
} // namespace settlement
