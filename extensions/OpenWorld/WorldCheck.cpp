// worldCheck and worldMap: every part's own check gathered into one list with the cross-part checks only a whole world
// can make (real names in any output text, swap progress, buildings over roads or water, props off the ground, stale
// buildings), and the whole map drawn as one image plus JSON for an in-game map.

#include "OpenWorld.h"
#include "WorldScene.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Names.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Slots.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

using namespace lila;

namespace openworld::world
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;
using T = ArgType;

namespace
{
json callCmd(const char* cmd, json flags) { return ops::execute(cmd, std::move(flags)); }

std::string pyString(const std::string& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\\' || c == '\'')
            out += '\\';
        out += c;
    }
    return out + "'";
}

std::string firstString(const json& j, std::initializer_list<const char*> keys)
{
    for (const char* k : keys)
        if (j.contains(k) && j[k].is_string() && !j[k].get<std::string>().empty())
            return j[k].get<std::string>();
    return {};
}

json pointOf(const json& j)
{
    for (const char* k : {"at", "where", "position", "point"})
        if (j.contains(k) && j[k].is_array() && j[k].size() >= 2 && j[k][0].is_number())
            return j[k].size() >= 3 ? json::array({j[k][0], j[k][2]}) : json::array({j[k][0], j[k][1]});
    return nullptr;
}

/** The world check's issue list with counts by kind. */
class Issues
{
public:
    void add(std::string kind, std::string check, std::string severity, std::string what, json at = nullptr, std::string target = {}, std::string select = {},
             std::string fix = {})
    {
        mCounts[kind] = mCounts.value(kind, 0) + 1;
        if (severity != "info")
            mBad = true;
        json i = {{"kind", kind}, {"check", check}, {"severity", severity}, {"what", what}};
        if (!at.is_null())
            i["at"] = at;
        if (!target.empty())
            i["target"] = target;
        if (!select.empty())
            i["select"] = select;
        if (!fix.empty())
            i["fix"] = fix;
        mList.push_back(std::move(i));
    }
    /** A child check's issues, kinds kept as "<check>.<kind>" unless mapped. */
    void child(const std::string& check, const json& result, const std::string& target, const std::function<std::string(const json&)>& kindOf = {})
    {
        for (const json& i : result.value("issues", json::array()))
        {
            const std::string raw = firstString(i, {"kind", "type", "check"});
            std::string kind = kindOf ? kindOf(i) : std::string();
            if (kind.empty())
                kind = check + (raw.empty() ? std::string() : "." + raw);
            std::string severity = firstString(i, {"severity"});
            if (severity.empty())
                severity = "warning";
            std::string what = firstString(i, {"what", "message", "issue", "why", "text"});
            if (what.empty())
                what = i.dump();
            add(kind, check, severity, check + ": " + what, pointOf(i), target, {}, firstString(i, {"fix"}));
        }
    }
    bool ok() const { return !mBad; }
    const json& list() const { return mList; }
    const json& counts() const { return mCounts; }

private:
    json mList = json::array(), mCounts = json::object();
    bool mBad = false;
};

/** Runs a child check, recording its summary; a check that fails to run is an issue of its own. */
json runChild(Issues& issues, json& children, const std::string& label, const char* cmd, json flags, const std::string& target)
{
    LILA_ZONE_N("world.check.child");
    LILA_ZONE_TEXT(label.c_str(), label.size());
    const auto t0 = std::chrono::steady_clock::now();
    try
    {
        json r = callCmd(cmd, std::move(flags));
        children[label] = {{"ok", r.value("ok", true)}, {"counts", r.value("counts", json::object())},
                           {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()}};
        return r;
    }
    catch (const std::exception& e)
    {
        children[label] = {{"ok", false}, {"error", e.what()}};
        issues.add("checkFailed", label, "warning", std::string(cmd) + " could not run: " + e.what(), nullptr, target);
        return json::object();
    }
}

bool swapped(const slots::Payload& p)
{
    return !p.item.empty() || !p.pool.empty() || !p.poolTag.empty() || !p.mesh.empty() || !p.unrealMesh.empty() || !p.unrealClass.empty();
}

bool hasTag(const slots::SlotType* t, std::string_view tag) { return t && std::find(t->tags.begin(), t->tags.end(), tag) != t->tags.end(); }

using Ring = std::vector<glm::dvec2>;
Ring ringOf(const json& j)
{
    Ring r;
    for (const json& p : j)
        if (p.is_array() && p.size() >= 2)
            r.push_back(p.size() >= 3 ? glm::dvec2(p[0].get<double>(), p[2].get<double>()) : glm::dvec2(p[0].get<double>(), p[1].get<double>()));
    return r;
}
glm::dvec2 centroidOf(const Ring& r)
{
    glm::dvec2 c(0.0);
    for (const glm::dvec2& p : r)
        c += p;
    return r.empty() ? c : c / double(r.size());
}

/** Every building envelope in the world: settlements' and sites', with where it came from. */
struct Envelope
{
    std::string id, from, mesh;
    Ring footprint;
};
std::vector<Envelope> envelopes(const std::vector<std::string>& settlements)
{
    std::vector<Envelope> out;
    for (const std::string& s : settlements)
        try
        {
            for (const json& e : callCmd("settlementEnvelopes", {{"args", {s}}, {"limit", 0}}).value("envelopes", json::array()))
                out.push_back({"lot:" + std::to_string(e.value("lotId", uint64_t(0))), s, s, ringOf(e.value("footprint", json::array()))});
        }
        catch (const std::exception&)
        {
        }
    if (ops::find("siteEnvelopes"))
        try
        {
            for (const json& e : callCmd("siteEnvelopes", json::object()).value("envelopes", json::array()))
                out.push_back({e.value("id", std::string()), "site:" + e.value("site", std::string()), {}, ringOf(e.value("footprint", json::array()))});
        }
        catch (const std::exception&)
        {
        }
    return out;
}

/** The terrain's roads as paved bands round their centrelines, bucketed for footprint tests. */
struct RoadIndex
{
    struct Segment
    {
        glm::dvec2 a, b;
        double half = 0.0;
        std::string id;
    };
    std::vector<Segment> segments;
    std::map<std::pair<int, int>, std::vector<size_t>> cells;
    static constexpr double kCell = 5000.0;

    static double segmentDistance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
    {
        const glm::dvec2 ab = b - a;
        const double t = std::clamp(glm::dot(p - a, ab) / std::max(1e-9, glm::dot(ab, ab)), 0.0, 1.0);
        return glm::length(p - (a + ab * t));
    }
    static bool segmentsCross(glm::dvec2 p, glm::dvec2 q, glm::dvec2 a, glm::dvec2 b)
    {
        auto side = [](glm::dvec2 o, glm::dvec2 u, glm::dvec2 v) { return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x); };
        return side(p, q, a) * side(p, q, b) < 0.0 && side(a, b, p) * side(a, b, q) < 0.0;
    }
    static bool inside(const Ring& r, glm::dvec2 p)
    {
        bool in = false;
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
        return in;
    }

    /** The id of a road whose paved band reaches into the footprint, else empty. */
    std::string crossing(const Ring& fp) const
    {
        glm::dvec2 lo(1e300), hi(-1e300);
        for (const glm::dvec2& q : fp)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        std::set<size_t> seen;
        for (int cz = int(std::floor(lo.y / kCell)); cz <= int(std::floor(hi.y / kCell)); ++cz)
            for (int cx = int(std::floor(lo.x / kCell)); cx <= int(std::floor(hi.x / kCell)); ++cx)
            {
                auto it = cells.find({cx, cz});
                if (it == cells.end())
                    continue;
                for (size_t k : it->second)
                {
                    if (!seen.insert(k).second)
                        continue;
                    const Segment& s = segments[k];
                    // Inside by the band less a margin, so a building fronting the road's edge isn't flagged.
                    const double reach = std::max(0.0, s.half - 50.0);
                    if (inside(fp, s.a) || inside(fp, s.b))
                        return s.id;
                    for (size_t i = 0; i < fp.size(); ++i)
                    {
                        const glm::dvec2 p = fp[i], q = fp[(i + 1) % fp.size()];
                        if (segmentsCross(p, q, s.a, s.b) || segmentDistance(p, s.a, s.b) < reach || segmentDistance(q, s.a, s.b) < reach ||
                            segmentDistance(s.a, p, q) < reach || segmentDistance(s.b, p, q) < reach)
                            return s.id;
                    }
                }
            }
        return {};
    }
};

RoadIndex roadIndex(const std::string& terrain)
{
    RoadIndex idx;
    json roads;
    try
    {
        roads = callCmd("terrainRoads", {{"args", {terrain}}, {"detail", true}});
    }
    catch (const std::exception&)
    {
        return idx;
    }
    const json layers = roads.value("layers", json::object());
    for (const auto& [layer, list] : layers.items())
        for (const json& r : list)
        {
            const double width = r.value("pavedWidth", r.value("width", 0.0));
            const json line = r.value("centreline", json::array());
            for (size_t i = 0; i + 1 < line.size(); ++i)
            {
                const json& p = line[i];
                const json& q = line[i + 1];
                if (p.size() < 3 || q.size() < 3)
                    continue;
                RoadIndex::Segment s{{p[0].get<double>(), p[2].get<double>()}, {q[0].get<double>(), q[2].get<double>()}, width * 0.5, r.value("id", std::string())};
                const glm::dvec2 lo = glm::min(s.a, s.b) - glm::dvec2(s.half), hi = glm::max(s.a, s.b) + glm::dvec2(s.half);
                const size_t k = idx.segments.size();
                idx.segments.push_back(std::move(s));
                for (int cz = int(std::floor(lo.y / RoadIndex::kCell)); cz <= int(std::floor(hi.y / RoadIndex::kCell)); ++cz)
                    for (int cx = int(std::floor(lo.x / RoadIndex::kCell)); cx <= int(std::floor(hi.x / RoadIndex::kCell)); ++cx)
                        idx.cells[{cx, cz}].push_back(k);
            }
        }
    return idx;
}

std::string meshName(const std::string& path)
{
    Node* n = Graph::current().find(path);
    return n ? n->name() : path;
}
} // namespace

json checkWorld(const WorldRef* w, const json& options)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    Issues issues;
    json children = json::object();
    double tol = options.value("propTolerance", 60.0);
    const std::string terrain = worldTerrain(w);
    json notes = json::array();

    // Children: every part's own check.
    if (!terrain.empty())
    {
        json tc = runChild(issues, children, "terrainCheck", "terrainCheck", {{"args", {terrain}}}, terrain);
        // A site kit's pads and yards are earthworks by design and sit by water by design: siteCheck judges their grading
        // (steepGround, water), so terrainCheck's cut/fill and flood findings on site stamps are counted, not listed.
        int siteEarthworks = 0;
        if (tc.contains("issues"))
        {
            json kept = json::array();
            for (const json& i : tc["issues"])
            {
                const std::string t = firstString(i, {"type", "kind"});
                if ((t == "cutFill" || t == "flood") && i.dump().find("site:") != std::string::npos)
                    ++siteEarthworks;
                else
                    kept.push_back(i);
            }
            tc["issues"] = kept;
        }
        if (siteEarthworks > 0)
            notes.push_back(std::to_string(siteEarthworks) + " terrainCheck cut/fill and flood findings on site kit stamps left to siteCheck");
        issues.child("terrain", tc, terrain, [](const json& i) {
            const std::string t = firstString(i, {"type", "kind"});
            if (t == "laneDisconnected" || t == "harbourUnreachable" || t == "channelShallow" || t == "channelNarrow" || t == "laneShallow" || t == "laneOverLand")
                return std::string("unreachableWater");
            return std::string();
        });
    }
    else
        notes.push_back(terrainMeshes().empty() ? "no terrain in the scene" : "several terrains and no world node: terrainCheck skipped (name the world)");
    const std::string remix = w ? w->built.value("remix", std::string()) : std::string();
    if (!remix.empty() && g.find(remix))
        issues.child("remix", runChild(issues, children, "remixCheck", "remixCheck", {{"args", {remix}}}, remix), remix);
    const std::vector<std::string> settlements = settlementMeshes();
    for (const std::string& s : settlements)
        issues.child("settlement", runChild(issues, children, "settlementCheck:" + meshName(s), "settlementCheck", {{"args", {s}}}, s), s, [](const json& i) {
            return firstString(i, {"check"}) == "realName" ? std::string("realName") : std::string();
        });
    if (ops::find("buildingCheck"))
    {
        const json b = runChild(issues, children, "buildingCheck", "buildingCheck", json::object(), {});
        issues.child("building", b, {}, [](const json& i) {
            const std::string k = firstString(i, {"kind"});
            if (k == "realName")
                return std::string("realName");
            if (k.find("ntry") != std::string::npos || k.find("nterior") != std::string::npos)
                return std::string("interiorNoEntry");
            return std::string();
        });
        try
        {
            const json stale = callCmd("buildingRefresh", {{"dryRun", true}});
            for (const json& s : stale.value("stale", json::array()))
                issues.add("buildingsStale", "buildingRefresh", "warning",
                           "buildings of " + s.value("settlement", std::string()) + " were made before the settlement last changed", nullptr,
                           s.value("group", std::string()), {}, "from lila import ops\nops.buildingRefresh(" + pyString(s.value("settlement", std::string())) + ")");
        }
        catch (const std::exception& e)
        {
            notes.push_back(std::string("buildingRefresh(dryRun) failed: ") + e.what());
        }
    }
    if (ops::find("siteList") && !callCmd("siteList", json::object()).empty())
        issues.child("site", runChild(issues, children, "siteCheck", "siteCheck", json::object(), {}), {}, [](const json& i) {
            const std::string k = firstString(i, {"kind"});
            return k == "realName" ? std::string("realName") : std::string();
        });
    std::vector<std::string> zoneNames, settlementZoneNames;
    for (const std::string& set : zoneSetNodes())
    {
        issues.child("zone", runChild(issues, children, "zoneCheck:" + meshName(set), "zoneCheck", {{"set", set}}, set), set);
        const json spec = callCmd("zoneSpec", {{"set", set}});
        for (const json& l : spec.value("layers", json::array()))
            for (const json& z : l.value("zones", json::array()))
            {
                // Names Settlement gave its districts are curated words, checked against imported names only (as settlementCheck does).
                (z.value("nameSource", std::string()) == "settlement" ? settlementZoneNames : zoneNames).push_back(z.value("name", std::string()));
                if (z.value("nameSource", std::string()) == "numbered")
                    issues.add("zoneNumberedName", "zones", "warning", "zone " + z.value("id", std::string()) + " in " + l.value("id", std::string()) + " has a numbered name",
                               nullptr, set, {},
                               "from lila import ops\nops.zoneEdit(set=" + pyString(set) + ", layer=" + pyString(l.value("id", std::string())) + ", zone=" +
                                   pyString(z.value("id", std::string())) + ", name='')");
            }
    }

    // Slots: orphans and duplicates from the report, swap progress and ground props from every slot.
    const json report = slots::report(g);
    children["slotReport"] = {{"total", report["total"]}, {"orphans", report["orphans"].size()}, {"duplicates", report["duplicates"].size()}};
    for (const json& o : report["orphans"])
        issues.add("slotOrphan", "slots", "warning", "slot override " + o["id"].get<std::string>() + " has no slot", o.value("at", json()), {}, {},
                   o.contains("nearest") ? "from lila import ops\nops.slotSet(id=" + pyString(o["id"].get<std::string>()) + ", reassign=" + pyString(o["nearest"].get<std::string>()) + ")"
                                         : std::string());
    for (const json& d : report["duplicates"])
        issues.add("slotDuplicate", "slots", "error", "slot id " + d.get<std::string>() + " is made more than once");

    std::vector<lila::names::Text> texts;
    json byType = json::object();
    size_t onPlaceholder = 0, swappedCount = 0, shell = 0, hidden = 0;
    std::shared_ptr<const geo::Heightfield> hf = terrain.empty() ? nullptr : terrainHeights(terrain);
    // A prop and this check sample the ground differently between grid points; on coarse cells that differs by more.
    if (hf)
        tol = std::max(tol, hf->cell * 0.05);
    double seaLevel = -1e30;
    if (!terrain.empty())
        try
        {
            const json water = callCmd("terrainWater", {{"args", {terrain}}});
            if (water.contains("features") && water["features"].contains("settings"))
                seaLevel = water["features"]["settings"].value("seaLevel", seaLevel);
        }
        catch (const std::exception&)
        {
        }
    const auto tSlots = std::chrono::steady_clock::now();
    {
        LILA_ZONE_N("world.check.slots");
        const slots::StatePtr st = slots::state();
        int floating = 0, buried = 0;
        struct GroundProp
        {
            glm::dvec3 at;
            std::string type, id;
        };
        std::vector<GroundProp> ground;
        // Every slot is looked at as it is gathered and only ground props are kept: a city holds millions of slots.
        auto look = [&](const slots::Slot& s) {
            const slots::Resolved r = slots::resolve(s, *st);
            for (const char* k : {"text", "name", "label"})
                if (r.payload.data.contains(k) && r.payload.data[k].is_string())
                    texts.push_back({"slot " + s.id, r.payload.data[k].get<std::string>()});
            if (r.payload.hidden && *r.payload.hidden)
            {
                ++hidden;
                return false;
            }
            const slots::SlotType* type = slots::resolveType(s.type);
            if (hasTag(type, "shell"))
            {
                ++shell;
                return false;
            }
            json& t = byType[s.type];
            if (t.is_null())
                t = {{"placeholder", 0}, {"swapped", 0}};
            if (swapped(r.payload))
            {
                ++swappedCount;
                t["swapped"] = t["swapped"].get<int>() + 1;
            }
            else
            {
                ++onPlaceholder;
                t["placeholder"] = t["placeholder"].get<int>() + 1;
            }
            // Ground props only: wall, roof and free parts sit where their building or kit put them, and an interior's
            // furnishing stands on its floor.
            return hf && type && type->attach == slots::Attach::Ground && !s.owner.starts_with("building:") && !s.owner.starts_with("interior:");
        };
        for (const slots::Slot& s : slots::collect(g, look))
        {
            const glm::dvec3 p(s.transform[3]);
            if (hf->contains(p.x, p.z))
                ground.push_back({p, s.type, s.id});
        }
        // A prop with another standing right under it is stacked (container tiers, crates), not floating.
        std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> cells;
        auto cellOf = [](const glm::dvec3& p) { return std::pair<int64_t, int64_t>{int64_t(std::floor(p.x / 200.0)), int64_t(std::floor(p.z / 200.0))}; };
        for (size_t i = 0; i < ground.size(); ++i)
            cells[cellOf(ground[i].at)].push_back(i);
        auto stacked = [&](size_t i) {
            const glm::dvec3& p = ground[i].at;
            const auto [cx, cz] = cellOf(p);
            for (int64_t dz = -1; dz <= 1; ++dz)
                for (int64_t dx = -1; dx <= 1; ++dx)
                    if (auto it = cells.find({cx + dx, cz + dz}); it != cells.end())
                        for (size_t j : it->second)
                            if (j != i && ground[j].at.y < p.y - 50.0 && glm::length(glm::dvec2(ground[j].at.x - p.x, ground[j].at.z - p.z)) < 150.0)
                                return true;
            return false;
        };
        for (size_t i = 0; i < ground.size(); ++i)
        {
            const glm::dvec3& p = ground[i].at;
            const double groundAt = hf->heightAt(p.x, p.z);
            const double dy = p.y - groundAt;
            // Ground under the sea level counts as water too: a dredged berth is cut after the sea mask is made.
            const bool overWater = hf->maskAt("water", p.x, p.z) > 0.3 || hf->maskAt("sea", p.x, p.z) > 0.3 || hf->maskAt("channel", p.x, p.z) > 0.3 ||
                                   groundAt < seaLevel - 50.0;
            if (dy > tol && !overWater && !stacked(i))
            {
                if (++floating <= 200)
                    issues.add("propFloating", "slots", "warning", ground[i].type + " " + ground[i].id + " stands " + std::to_string(int(dy)) + " cm above the ground",
                               json::array({p.x, p.z}), {}, "from lila import ops\nops.slotFind(id=" + pyString(ground[i].id) + ", select=True)");
            }
            else if (dy < -tol)
            {
                if (++buried <= 200)
                    issues.add("propBuried", "slots", "warning", ground[i].type + " " + ground[i].id + " is " + std::to_string(int(-dy)) + " cm under the ground",
                               json::array({p.x, p.z}), {}, "from lila import ops\nops.slotFind(id=" + pyString(ground[i].id) + ", select=True)");
            }
        }
        if (floating > 200 || buried > 200)
            notes.push_back(std::to_string(floating) + " floating and " + std::to_string(buried) + " buried props; the first 200 of each are listed");
    }

    children["slots"] = {{"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - tSlots).count()}};
    const auto tEnv = std::chrono::steady_clock::now();
    // Buildings over water (the terrain's water mask) or a road (a terrain road's paved width round its centreline).
    if (hf)
    {
        LILA_ZONE_N("world.check.envelopes");
        const RoadIndex roads = roadIndex(terrain);
        for (const Envelope& e : envelopes(settlements))
        {
            if (e.footprint.size() < 3)
                continue;
            const glm::dvec2 c = centroidOf(e.footprint);
            if (!hf->contains(c.x, c.y))
                continue;
            int wet = 0;
            for (const glm::dvec2& q : e.footprint)
                if (hf->maskAt("water", q.x, q.y) > 0.5)
                    ++wet;
            if (hf->maskAt("water", c.x, c.y) > 0.5 && wet * 2 >= int(e.footprint.size()))
                issues.add("buildingOverWater", "world", "warning", "building " + e.id + " (" + e.from + ") stands on water", json::array({c.x, c.y}), e.mesh);
            else if (const std::string road = roads.crossing(e.footprint); !road.empty())
                issues.add("buildingOverRoad", "world", "warning", "building " + e.id + " (" + e.from + ") stands on road " + road, json::array({c.x, c.y}), e.mesh);
        }
    }

    children["envelopes"] = {{"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - tEnv).count()}};
    const auto tNames = std::chrono::steady_clock::now();
    // Real names in any output text: the scene's names and name attributes, plus zone names, site names and sign texts.
    {
        LILA_ZONE_N("world.check.realNames");
        for (const std::string& z : zoneNames)
            if (!z.empty())
                texts.push_back({"zone name", z});
        if (ops::find("siteList"))
            for (const json& s : callCmd("siteList", json::object()))
                texts.push_back({"site " + s.value("id", std::string()), s.value("name", std::string())});
        for (const lila::names::Leak& l : lila::names::findRealNames(g, texts))
            issues.add("realName", "names", "error", "\"" + l.text + "\" (" + l.where + ") matches the real name \"" + l.real + "\" from " + l.source);
        std::vector<lila::names::Text> districtTexts;
        for (const std::string& z : settlementZoneNames)
            if (!z.empty())
                districtTexts.push_back({"district zone name", z});
        if (!districtTexts.empty())
            for (const lila::names::Leak& l : lila::names::findRealNames(districtTexts, lila::names::sceneRealNames(g), false))
                issues.add("realName", "names", "error", "\"" + l.text + "\" (" + l.where + ") matches the real name \"" + l.real + "\" from " + l.source);
    }

    children["realNames"] = {{"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - tNames).count()}};
    if (onPlaceholder > 0)
        issues.add("placeholders", "slots", "info",
                   std::to_string(onPlaceholder) + " of " + std::to_string(onPlaceholder + swappedCount) + " slots still use placeholders (slotRule swaps them by type)");

    json out = {{"ok", issues.ok()},
                {"counts", issues.counts()},
                {"issues", issues.list()},
                {"children", children},
                {"placeholders", {{"onPlaceholder", onPlaceholder}, {"swapped", swappedCount}, {"shell", shell}, {"hidden", hidden}, {"byType", byType},
                                  {"progress", onPlaceholder + swappedCount == 0 ? 1.0 : double(swappedCount) / double(onPlaceholder + swappedCount)}}},
                {"terrain", terrain},
                {"notes", notes}};
    if (w)
        out["world"] = w->node;
    return out;
}

// Overview map ----------------------------------------------------------------------------------------------------------

namespace
{
struct Canvas
{
    int w = 0, h = 0;
    std::vector<float> rgba;
    Canvas(int width, int height) : w(width), h(height), rgba(size_t(width) * height * 4, 1.0f) {}

    void blend(int x, int y, const glm::vec3& c, float a)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        float* p = &rgba[(size_t(y) * w + x) * 4];
        for (int k = 0; k < 3; ++k)
            p[k] = p[k] * (1.0f - a) + c[k] * a;
    }
    void disc(double cx, double cy, double r, const glm::vec3& c, float a = 1.0f)
    {
        for (int y = int(std::floor(cy - r)); y <= int(std::ceil(cy + r)); ++y)
            for (int x = int(std::floor(cx - r)); x <= int(std::ceil(cx + r)); ++x)
                if ((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy) <= r * r)
                    blend(x, y, c, a);
    }
    void line(glm::dvec2 a, glm::dvec2 b, double width, const glm::vec3& c, float alpha = 1.0f)
    {
        const double len = glm::length(b - a);
        const int steps = std::max(1, int(len / std::max(0.5, width * 0.5)));
        for (int i = 0; i <= steps; ++i)
        {
            const glm::dvec2 p = a + (b - a) * (double(i) / steps);
            disc(p.x, p.y, std::max(0.5, width * 0.5), c, alpha);
        }
    }
    /** Even-odd scanline fill. */
    void fill(const std::vector<glm::dvec2>& poly, const glm::vec3& c, float a)
    {
        if (poly.size() < 3)
            return;
        double y0 = poly[0].y, y1 = poly[0].y;
        for (const glm::dvec2& p : poly)
            y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
        std::vector<double> xs;
        for (int y = std::max(0, int(std::floor(y0))); y <= std::min(h - 1, int(std::ceil(y1))); ++y)
        {
            const double sy = y + 0.5;
            xs.clear();
            for (size_t i = 0; i < poly.size(); ++i)
            {
                const glm::dvec2 p = poly[i], q = poly[(i + 1) % poly.size()];
                if ((p.y <= sy) != (q.y <= sy))
                    xs.push_back(p.x + (sy - p.y) / (q.y - p.y) * (q.x - p.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2)
                for (int x = std::max(0, int(std::ceil(xs[i] - 0.5))); x <= std::min(w - 1, int(std::floor(xs[i + 1] - 0.5))); ++x)
                    blend(x, y, c, a);
        }
    }
};

glm::vec3 hashColour(const std::string& key)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key)
        h = (h ^ c) * 1099511628211ull;
    const float hue = float(h % 360) / 60.0f;
    const float x = 1.0f - std::abs(std::fmod(hue, 2.0f) - 1.0f);
    glm::vec3 c = hue < 1 ? glm::vec3(1, x, 0) : hue < 2 ? glm::vec3(x, 1, 0) : hue < 3 ? glm::vec3(0, 1, x) : hue < 4 ? glm::vec3(0, x, 1) : hue < 5 ? glm::vec3(x, 0, 1) : glm::vec3(1, 0, x);
    return glm::mix(glm::vec3(0.5f), c, 0.6f);
}

std::string hexOf(const glm::vec3& c)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", int(c.r * 255), int(c.g * 255), int(c.b * 255));
    return buf;
}

json ptJson(glm::dvec2 p) { return json::array({std::round(p.x), std::round(p.y)}); }

/** Drops points closer than step to the last kept one (the ends stay). */
std::vector<glm::dvec2> thin(const std::vector<glm::dvec2>& in, double step)
{
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i < in.size(); ++i)
        if (out.empty() || i + 1 == in.size() || glm::length(in[i] - out.back()) >= step)
            out.push_back(in[i]);
    return out;
}
} // namespace

json writeWorldMap(const WorldRef* w, const json& options)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    const std::string terrain = worldTerrain(w);
    if (terrain.empty())
        throw CommandError("worldMap: no terrain (build a world, or keep one terrain in the scene)");
    std::shared_ptr<const geo::Heightfield> hf = terrainHeights(terrain);
    if (!hf || hf->empty())
        throw CommandError("worldMap: " + terrain + " has no heights yet");
    json notes = json::array();
    const std::string name = w ? w->spec.value("name", std::string("World")) : meshName(terrain);
    const fs::path folder = options.contains("folder") ? paths::fromUtf8(options["folder"].get<std::string>()) : outputFolder("world", name, &notes) / "map";
    std::error_code ec;
    fs::create_directories(folder, ec);

    const glm::dvec2 lo = hf->origin, ext = hf->extent();
    const int W = std::clamp(int(options.value("resolution", 2048)), 64, 8192);
    const int H = std::max(1, int(std::round(W * ext.y / std::max(1.0, ext.x))));
    const double cmPerPixel = ext.x / W;
    auto toPx = [&](glm::dvec2 p) { return (p - lo) / cmPerPixel; };
    Canvas c(W, H);

    // Ground: tinted by height, hill shaded; water blue by depth; road surfaces grey.
    {
        LILA_ZONE_N("world.map.ground");
        const glm::dvec2 range = hf->range();
        double sea = 0.0;
        try
        {
            const json wi = callCmd("terrainWater", {{"args", {terrain}}});
            if (wi.contains("features") && wi["features"].contains("settings"))
                sea = wi["features"]["settings"].value("seaLevel", 0.0);
        }
        catch (const std::exception&)
        {
        }
        const glm::dvec3 light = glm::normalize(glm::dvec3(-1.0, 1.4, -1.0));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const double wx = lo.x + (x + 0.5) * cmPerPixel, wz = lo.y + (y + 0.5) * cmPerPixel;
                const double ht = hf->heightAt(wx, wz);
                const bool water = hf->maskAt("water", wx, wz) > 0.5 || hf->maskAt("sea", wx, wz) > 0.5 || ht < sea;
                glm::vec3 col;
                if (water)
                {
                    const float depth = float(std::clamp((sea - ht) / 3000.0, 0.0, 1.0));
                    col = glm::mix(glm::vec3(0.55f, 0.75f, 0.85f), glm::vec3(0.18f, 0.35f, 0.55f), depth);
                }
                else
                {
                    const float t = float(std::clamp((ht - std::max(sea, range.x)) / std::max(1.0, range.y - std::max(sea, range.x)), 0.0, 1.0));
                    col = t < 0.5f ? glm::mix(glm::vec3(0.62f, 0.72f, 0.50f), glm::vec3(0.80f, 0.74f, 0.55f), t * 2.0f)
                                   : glm::mix(glm::vec3(0.80f, 0.74f, 0.55f), glm::vec3(0.93f, 0.92f, 0.90f), (t - 0.5f) * 2.0f);
                    const double shade = std::clamp(glm::dot(hf->normalAt(wx, wz), light), 0.0, 1.0);
                    col *= float(0.55 + 0.45 * shade);
                    if (hf->maskAt("roadSurface", wx, wz) > 0.5)
                        col = glm::vec3(0.35f);
                }
                float* p = &c.rgba[(size_t(y) * W + x) * 4];
                p[0] = col.r, p[1] = col.g, p[2] = col.b, p[3] = 1.0f;
            }
    }

    json districts = json::array(), zonesOut = json::array(), settlementsOut = json::array(), pois = json::array(), roads = json::array(), water = json::array(), sitesOut = json::array();

    // Districts: the zone sets' district layers, else each settlement's districts.
    bool zoneDistricts = false;
    for (const std::string& set : zoneSetNodes())
    {
        const json spec = callCmd("zoneSpec", {{"set", set}});
        for (const json& l : spec.value("layers", json::array()))
        {
            json layer = {{"id", l.value("id", std::string())}, {"type", l.value("type", std::string())}, {"zones", json::array()}};
            for (const json& z : l.value("zones", json::array()))
            {
                json zz = {{"id", z.value("id", std::string())}, {"name", z.value("name", std::string())}, {"polygons", json::array()}};
                for (const json& poly : z.value("polygons", json::array()))
                {
                    const Ring r = ringOf(poly);
                    json pj = json::array();
                    for (const glm::dvec2& q : r)
                        pj.push_back(ptJson(q));
                    zz["polygons"].push_back(pj);
                    if (l.value("type", std::string()) == "district")
                    {
                        std::vector<glm::dvec2> px;
                        for (const glm::dvec2& q : r)
                            px.push_back(toPx(q));
                        const glm::vec3 col = hashColour(z.value("name", z.value("id", std::string())));
                        c.fill(px, col, 0.35f);
                        for (size_t i = 0; i < px.size(); ++i)
                            c.line(px[i], px[(i + 1) % px.size()], 1.5, col * 0.6f, 0.9f);
                        zoneDistricts = true;
                        if (!r.empty())
                            districts.push_back({{"name", z.value("name", std::string())}, {"zone", z.value("id", std::string())}, {"color", hexOf(col)}, {"polygon", pj},
                                                 {"label", ptJson(centroidOf(r))}});
                    }
                }
                layer["zones"].push_back(std::move(zz));
            }
            zonesOut.push_back(std::move(layer));
        }
    }
    for (const std::string& s : settlementMeshes())
    {
        json names;
        try
        {
            names = callCmd("settlementNames", {{"args", {s}}});
        }
        catch (const std::exception&)
        {
            continue;
        }
        glm::dvec2 slo(1e300), shi(-1e300);
        for (const json& d : names.value("districts", json::array()))
        {
            const Ring r = ringOf(d.value("outline", json::array()));
            for (const glm::dvec2& q : r)
                slo = glm::min(slo, q), shi = glm::max(shi, q);
            if (zoneDistricts || r.size() < 3)
                continue;
            std::vector<glm::dvec2> px;
            json pj = json::array();
            for (const glm::dvec2& q : r)
                px.push_back(toPx(q)), pj.push_back(ptJson(q));
            const glm::vec3 col = hashColour(d.value("name", std::string()));
            c.fill(px, col, 0.35f);
            for (size_t i = 0; i < px.size(); ++i)
                c.line(px[i], px[(i + 1) % px.size()], 1.5, col * 0.6f, 0.9f);
            districts.push_back({{"name", d.value("name", std::string())}, {"settlement", meshName(s)}, {"color", hexOf(col)}, {"polygon", pj}, {"label", ptJson(centroidOf(r))}});
        }
        json e = {{"name", meshName(s)}};
        if (slo.x < shi.x)
        {
            e["bounds"] = {ptJson(slo), ptJson(shi)};
            e["label"] = ptJson((slo + shi) * 0.5);
        }
        settlementsOut.push_back(std::move(e));
    }

    // Roads, rivers and coast from every OSM import's curves (the remix's and each settlement's), and their places.
    {
        LILA_ZONE_N("world.map.curves");
        for (scene::NodeId id : g.nodes())
        {
            Node* n = g.node(id);
            if (!n || !n->type().isA("curve"))
                continue;
            const bool features = n->type().attrIndex("osmFeatures") >= 0, places = n->type().attrIndex("osmPlaces") >= 0;
            if (!features && !places)
                continue;
            if (places)
            {
                const json pl = json::parse(callCmd("getAttr", {{"args", {n->path() + ".osmPlaces"}}}).get<std::string>(), nullptr, false);
                if (pl.is_array())
                    for (const json& p : pl)
                    {
                        const json at = pointOf(p);
                        if (at.is_null())
                            continue;
                        const glm::dvec2 q(at[0].get<double>(), at[1].get<double>());
                        const std::string kind = firstString(p, {"program", "kind", "amenity", "shop"});
                        c.disc(toPx(q).x, toPx(q).y, 2.5, glm::vec3(0.75f, 0.25f, 0.2f));
                        pois.push_back({{"name", p.value("name", std::string())}, {"kind", kind}, {"at", ptJson(q)}});
                    }
            }
            if (!features)
                continue;
            const json feats = json::parse(callCmd("getAttr", {{"args", {n->path() + ".osmFeatures"}}}).get<std::string>(), nullptr, false);
            const json data = callCmd("curveData", {{"args", {n->path()}}, {"worldSpace", true}});
            if (!data.is_array())
                continue;
            for (size_t k = 0; k < data.size(); ++k)
            {
                const json f = feats.is_array() && k < feats.size() && feats[k].is_object() ? feats[k] : json::object();
                const std::string kind = firstString(f, {"kind"});
                if (kind == "building" || data[k].value("closed", false))
                    continue;
                std::vector<glm::dvec2> pts;
                for (const json& p : data[k].value("points", json::array()))
                    if (p.is_array() && p.size() >= 3)
                        pts.push_back({p[0].get<double>(), p[2].get<double>()});
                if (pts.size() < 2)
                    continue;
                pts = thin(pts, cmPerPixel);
                const bool isWater = kind == "river" || kind == "stream" || kind == "coastline" || kind == "canal";
                const std::string cls = firstString(f, {"class", "highway", "waterway"});
                const double width = isWater ? (kind == "river" ? 2.0 : 1.2) : (cls == "motorway" || cls == "trunk" || cls == "primary") ? 2.5 : 1.3;
                const glm::vec3 col = isWater ? glm::vec3(0.25f, 0.45f, 0.7f) : (cls == "motorway" || cls == "trunk") ? glm::vec3(0.85f, 0.55f, 0.2f) : glm::vec3(0.98f, 0.97f, 0.92f);
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                    c.line(toPx(pts[i]), toPx(pts[i + 1]), width, col, 0.95f);
                json pj = json::array();
                for (const glm::dvec2& q : pts)
                    pj.push_back(ptJson(q));
                json item = {{"kind", kind.empty() ? std::string("road") : kind}, {"class", cls}, {"name", f.value("name", std::string())}, {"points", pj}};
                (isWater ? water : roads).push_back(std::move(item));
            }
        }
    }

    // Sites as points of interest.
    if (ops::find("siteList"))
        for (const json& s : callCmd("siteList", json::object()))
        {
            json info = callCmd("siteInfo", {{"args", {s["mesh"]}}});
            const json at = info.value("spec", json::object()).value("at", json());
            if (!at.is_array() || at.size() < 2)
                continue;
            const glm::dvec2 q(at[0].get<double>(), at[1].get<double>());
            const glm::dvec2 px = toPx(q);
            c.disc(px.x, px.y, 5.0, glm::vec3(0.1f));
            c.disc(px.x, px.y, 3.5, hashColour(s.value("kind", std::string())));
            sitesOut.push_back({{"id", s.value("id", std::string())}, {"kind", s.value("kind", std::string())}, {"name", s.value("name", std::string())}, {"at", ptJson(q)}});
        }

    const fs::path image = folder / (name + "_map.png");
    const fs::path jsonFile = folder / (name + "_map.json");
    image::writePng(image, W, H, c.rgba, 8);
    const json doc = {{"format", "lila.worldMap"},
                      {"version", 1},
                      {"name", name},
                      {"image", paths::toUtf8(image.filename())},
                      {"size", {W, H}},
                      {"bounds", {{"min", ptJson(lo)}, {"max", ptJson(lo + ext)}}},
                      {"cmPerPixel", cmPerPixel},
                      {"pixel", "px = (x - bounds.min[0]) / cmPerPixel, py = (z - bounds.min[1]) / cmPerPixel; row 0 is the lowest z (north, -Z, at the top)"},
                      {"districts", districts},
                      {"zones", zonesOut},
                      {"settlements", settlementsOut},
                      {"sites", sitesOut},
                      {"pois", pois},
                      {"roads", roads},
                      {"water", water}};
    std::ofstream(jsonFile, std::ios::binary) << doc.dump(1);
    return {{"image", paths::toUtf8(image)},
            {"json", paths::toUtf8(jsonFile)},
            {"size", {W, H}},
            {"cmPerPixel", cmPerPixel},
            {"counts", {{"districts", districts.size()}, {"settlements", settlementsOut.size()}, {"sites", sitesOut.size()}, {"pois", pois.size()}, {"roads", roads.size()}, {"water", water.size()}}},
            {"notes", notes}};
}

void registerCheckCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "worldCheck",
        .doc = "Check a whole world at once: every part's own check (terrainCheck with waterCheck, remixCheck, settlementCheck per "
               "settlement, buildingCheck, siteCheck, zoneCheck per zone set, the slot report's orphans and duplicates) plus what only "
               "the whole world shows: real brand or place names in any output text (names, signs, zones, sites), buildings over water or "
               "a road surface, ground props floating above or buried under the terrain, buildings made before their settlement last "
               "changed (buildingRefresh), interiors without an entry, unreachable boat channels and lanes, zone gaps and numbered zone "
               "names. placeholders is swap progress: slots still on placeholders by type. report=True lists the issues in the "
               "Validation panel with select and fix scripts where there is one. Returns ok (no warnings or errors), counts by kind, "
               "issues, each child's summary and the placeholders.",
        .positionalDoc = "world node or name (default the only world, else the scene)",
        .flags = {{"report", "rp", T::Bool, false, "Also list the issues in the Validation panel."},
                  {"propTolerance", "pt", T::Float, 60.0, "Gap or depth at which a ground prop counts as floating or buried, cm.", {}, {}, 0.0},
                  {"limit", "lm", T::Int, 500, "Most issues listed in the Validation panel.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const std::string name = a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>();
            std::optional<WorldRef> w = findWorld(name);
            if (!name.empty() && !w)
                throw CommandError("worldCheck: no world named " + name);
            json out = checkWorld(w ? &*w : nullptr, {{"propTolerance", a.get<double>("propTolerance", 60.0)}});
            if (a.get<bool>("report", false))
            {
                callCmd("clearIssues", {{"source", "worldCheck"}});
                int n = 0;
                const int limit = int(a.get<int64_t>("limit", 500));
                for (const json& i : out["issues"])
                {
                    if (n++ >= limit)
                        break;
                    json call = {{"message", i["what"]}, {"severity", i["severity"]}, {"source", "worldCheck"}};
                    const std::string target = i.value("target", std::string());
                    if (!target.empty() && Graph::current().find(target))
                        call["args"] = {target};
                    if (i.contains("fix"))
                        call["fix"] = i["fix"];
                    if (i.contains("select"))
                        call["select"] = i["select"];
                    callCmd("reportIssue", call);
                }
                out["reported"] = std::min<int>(n, limit);
            }
            return out;
        },
        .examples = {"c = worldCheck(report=True)\nc['ok'], c['counts'], c['placeholders']['byType']"},
    });

    ctx.registerCommand({
        .name = "worldMap",
        .doc = "Write the whole map as one image plus JSON for an in-game map: the terrain tinted by height and hill shaded, water by "
               "depth, road surfaces, district colours (zone sets' district layers, else each settlement's districts), OSM roads, rivers "
               "and coast, points of interest and sites as markers. The JSON (format lila.worldMap) carries the bounds and the pixel "
               "mapping, districts with colours and label points, every zone layer's polygons, settlements, sites, POIs, roads and water "
               "lines in world cm, so a game can draw its own labels. Returns the files and counts.",
        .positionalDoc = "world node or name (default the only world, else the only terrain)",
        .flags = {{"resolution", "r", T::Int, 2048, "Image width, pixels.", {}, {}, 64.0, 8192.0},
                  {"folder", "f", T::String, nullptr, "Output folder; default the workspace's openworld/world/<name>/map."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string name = a.positional.empty() || !a.positional[0].is_string() ? std::string() : a.positional[0].get<std::string>();
            std::optional<WorldRef> w = findWorld(name);
            if (!name.empty() && !w)
                throw CommandError("worldMap: no world named " + name);
            json o = {{"resolution", a.get<int64_t>("resolution", 2048)}};
            if (a.has("folder"))
                o["folder"] = a.get<std::string>("folder");
            return writeWorldMap(w ? &*w : nullptr, o);
        },
        .examples = {"m = worldMap(resolution=2048)\nm['image'], m['counts']"},
    });
}
} // namespace openworld::world
