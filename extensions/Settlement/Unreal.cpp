// settlementSendToUnreal sends roads (landscape splines, spline actors or ribbon meshes), massing and placements per World
// Partition cell, lots and envelopes as PCG data, and the lane graph as ZoneGraph shapes, resending only what changed since
// the last send.

#include "Export.h"
#include "Generators.h"
#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/Graph.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <string_view>

using namespace lila;

namespace settlement
{
namespace
{
using Arg = ArgType;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------------------
// Unreal

json ue(const glm::dvec3& p)
{
    const math::Vector v = math::toUnreal(math::Vector(p));
    return json::array({v.x, v.y, v.z});
}

json uePoints(std::span<const glm::dvec3> pts)
{
    json a = json::array();
    for (const glm::dvec3& p : pts)
        a.push_back(ue(p));
    return a;
}

json ueRing(const Polygon& p, double y)
{
    json a = json::array();
    for (const Vec2& q : p.pts)
        a.push_back(ue({q.x, y, q.y}));
    return a;
}

/** A smooth spline through the points (Catmull-Rom tangents), radius r at every point. */
curve::Spline smoothSpline(std::span<const glm::dvec3> pts, double radius)
{
    curve::Spline s;
    s.kind = curve::Kind::Bezier;
    s.points.assign(pts.begin(), pts.end());
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 prev = pts[i > 0 ? i - 1 : i], next = pts[i + 1 < n ? i + 1 : i];
        // A sixth of the neighbour span, shortened at sharp corners so handles never overshoot the next point.
        glm::dvec3 t = (next - prev) / 6.0;
        const double room = std::min(i > 0 ? glm::length(pts[i] - prev) : 1e30, i + 1 < n ? glm::length(next - pts[i]) : 1e30);
        if (glm::length(t) > 0.45 * room)
            t *= 0.45 * room / glm::length(t);
        s.handleL.push_back(pts[i] - t);
        s.handleR.push_back(pts[i] + t);
    }
    s.radius.assign(n, radius);
    return s;
}

std::shared_ptr<link::RpcClient> client(const char* cmd, std::initializer_list<const char*> features)
{
    try
    {
        link::Link::get().ensureConnected();
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": not connected to Unreal: " + e.what());
    }
    std::shared_ptr<link::RpcClient> c = link::Link::get().client();
    if (!c)
        throw CommandError(std::string(cmd) + ": not connected to Unreal");
    const json have = link::Link::get().status().value("features", json::array());
    for (const char* f : features)
        if (!have.is_array() || std::ranges::find(have, json(f)) == have.end())
        {
            if (std::string_view(f) == "settlement")
                throw CommandError(std::string(cmd) + ": enable the LilaLinkSettlement plugin in the Unreal project (copy Lila's "
                                                      "ue/LilaLinkSettlement next to LilaLink); it brings PCG and ZoneGraph");
            throw CommandError(std::string(cmd) + ": the LilaLink plugin in Unreal predates '" + f + "'; rebuild it from Lila's ue/LilaLink");
        }
    return c;
}

json call(link::RpcClient& c, const char* method, const json& params, const char* cmd, int timeoutMs = 300000)
{
    link::RpcResult r = c.call(method, params, timeoutMs);
    if (!r.ok)
        throw CommandError(std::string(cmd) + ": " + method + ": " + r.error);
    return r.result;
}

/**
 * ZoneGraph lanes of a cross-section: sidewalks, driving lanes, median, listed right to left looking along the road as
 * ZoneGraph lays them out. Driving lanes of an access direction are tagged Access instead of Vehicle, so Mass traffic
 * never routes into them.
 */
json lanesOf(const RoadProfile& p, bool accessForward = false, bool accessBackward = false)
{
    json lanes = json::array();
    auto lane = [&](double w, const char* dir, std::initializer_list<const char*> tags) {
        if (w <= 0.0)
            return;
        json t = json::array();
        for (const char* tag : tags)
            t.push_back(tag);
        lanes.push_back({{"width", w}, {"direction", dir}, {"tags", t}});
    };
    lane(p.sidewalk, "forward", {"Pedestrian"});
    lane(p.parking, "none", {"Parking"});
    // A single lane that is not one-way carries both directions, as the lane graph has it: half its width each way.
    const bool shared = p.lanes == 1 && !p.oneWay;
    const int back = shared ? 1 : p.oneWay ? 0 : p.lanes / 2, fwd = shared ? 1 : p.lanes - back;
    const double width = shared ? 0.5 * p.laneWidth : p.laneWidth;
    for (int k = 0; k < back; ++k)
        lane(width, "backward", {accessBackward ? "Access" : "Vehicle"});
    lane(p.median, "none", {});
    for (int k = 0; k < fwd; ++k)
        lane(width, "forward", {accessForward ? "Access" : "Vehicle"});
    lane(p.parking, "none", {"Parking"});
    lane(p.sidewalk, "forward", {"Pedestrian"});
    if (lanes.empty())
        lane(std::max(p.width(), 100.0), "forward", {"Pedestrian"});
    std::reverse(lanes.begin(), lanes.end());
    return lanes;
}

/** Indices 0..n-1, or the subset when one is given. */
template <class F> void eachIndex(const std::vector<int>* subset, size_t n, F&& f)
{
    if (subset)
        for (int k : *subset)
            f(size_t(k));
    else
        for (size_t k = 0; k < n; ++k)
            f(k);
}

/**
 * The PCG data: lots and envelopes in Unreal space with their attributes, and road centrelines; only what lies in one
 * cell when only is given (roads cut at its borders).
 */
json pcgData(const Rebuilt& r, const RoadNetwork& net, const CellContents* only = nullptr)
{
    const Layout& l = r.layout;
    std::map<int, std::string> lotKeyOf;
    for (const Lot& lot : l.lots)
        lotKeyOf[lot.id] = lotKey(lot);
    json lots = json::array();
    eachIndex(only ? &only->lots : nullptr, l.lots.size(), [&](size_t k) {
        const Lot& lot = l.lots[k];
        double lo = std::numeric_limits<double>::max();
        for (const Vec2& q : lot.outline.pts)
            lo = std::min(lo, r.ground.height(q));
        lots.push_back({{"id", lot.id},
                        {"key", lotKeyOf[lot.id]},
                        {"block", lot.block},
                        {"outline", ueRing(lot.outline, lo)},
                        {"use", useName(lot.use)},
                        {"area", lot.area},
                        {"frontages", lot.frontages},
                        {"corner", lot.corner},
                        {"frontClass", className(lot.frontClass)},
                        {"seed", int64_t(lot.random * 1e6)}});
    });
    json envelopes = json::array();
    eachIndex(only ? &only->envelopes : nullptr, l.envelopes.size(), [&](size_t k) {
        const Envelope& e = l.envelopes[k];
        const Vec2 c = centroid(e.footprint);
        json sides = json::array();
        for (int t : e.footprint.tag)
            sides.push_back(t == 0 ? "street" : t == 1 ? "party" : "open");
        const auto key = lotKeyOf.find(e.lot);
        envelopes.push_back({{"lot", e.lot},
                             {"key", key != lotKeyOf.end() ? "envelope:" + key->second.substr(4) : std::string()},
                             {"footprint", ueRing(e.footprint, e.ground)},
                             {"sides", sides},
                             {"center", ue({c.x, e.ground, c.y})},
                             {"front", ue({e.front.x, 0.0, e.front.y})},
                             {"base", e.base},
                             {"ground", e.ground},
                             {"top", e.top},
                             {"maxHeight", e.top - e.ground},
                             {"height", e.height},
                             {"floors", e.floors},
                             {"floorHeight", l.profile.floorHeight},
                             {"use", useName(e.use)},
                             {"roof", e.roof},
                             {"era", 0},
                             {"style", l.profile.name},
                             {"seed", int64_t(e.random * 1e6)}});
    });
    json roads = json::array();
    auto road = [&](size_t k, std::span<const glm::dvec3> pts) {
        const StreetLine& s = net.lines[k];
        const RoadProfile& p = l.roads[size_t(s.profile)];
        roads.push_back({{"id", int(k)},
                         {"key", roadKey(s)},
                         {"class", className(s.cls)},
                         {"profile", p.name},
                         {"width", s.width},
                         {"lanes", p.lanes},
                         {"street", s.street},
                         {"surface", s.surface},
                         {"path", s.path},
                         {"points", uePoints(pts)}});
    };
    if (only)
        for (const RoadPiece& piece : only->roads)
            road(size_t(piece.line), piece.pts);
    else
        for (size_t k = 0; k < net.lines.size(); ++k)
            road(k, net.lines[k].pts);
    json intersections = json::array();
    eachIndex(only ? &only->junctions : nullptr, net.junctions.size(), [&](size_t k) {
        const Junction& j = net.junctions[k];
        intersections.push_back({{"center", ue(j.p)},
                                 {"key", junctionKey(j)},
                                 {"control", controlName(j.control)},
                                 {"approaches", int64_t(j.ends.size())},
                                 {"phases", int64_t(j.phases.size())},
                                 {"radius", j.radius}});
    });
    json parking = json::array();
    eachIndex(only ? &only->parking : nullptr, net.parking.size(), [&](size_t k) {
        const ParkingSpot& p = net.parking[k];
        parking.push_back({{"position", ue(p.p)}, {"forward", ue(p.dir)}, {"length", p.length}, {"width", p.width}});
    });
    return {{"lots", lots}, {"envelopes", envelopes}, {"roads", roads}, {"intersections", intersections}, {"parking", parking}};
}

glm::dvec3 ueVec(const glm::dvec3& p)
{
    const math::Vector v = math::toUnreal(math::Vector(p));
    return {v.x, v.y, v.z};
}

json jsonVec(const glm::dvec3& v)
{
    return json::array({v.x, v.y, v.z});
}

/** The polyline cut back by d at its start (front) or its end. */
std::vector<glm::dvec3> trimmed(std::vector<glm::dvec3> pts, double d, bool front)
{
    if (front)
        std::reverse(pts.begin(), pts.end());
    while (pts.size() >= 2 && d > 0.0)
    {
        const glm::dvec3 a = pts[pts.size() - 2];
        const double len = glm::length(pts.back() - a);
        if (len > d)
        {
            pts.back() = a + (pts.back() - a) * ((len - d) / len);
            d = 0.0;
        }
        else
        {
            d -= len;
            pts.pop_back();
        }
    }
    if (front)
        std::reverse(pts.begin(), pts.end());
    return pts;
}

double polylineLength(const std::vector<glm::dvec3>& pts)
{
    double len = 0.0;
    for (size_t k = 1; k < pts.size(); ++k)
        len += glm::length(pts[k] - pts[k - 1]);
    return len;
}

/**
 * A polygon point where a road meets a junction, Unreal space. ZoneGraph links a polygon point to a spline end only when
 * their lane profiles face opposite ways, so the profile is reversed where the road starts at the junction.
 */
json polygonEnd(const glm::dvec3& position, const glm::dvec3& into, const std::string& profile, bool vehicle, bool reverse)
{
    return {{"position", jsonVec(position)}, {"forward", jsonVec(into)}, {"profile", profile}, {"vehicle", vehicle}, {"reverse", reverse}};
}

/** Largest strongly connected set of a directed graph's nodes, as a flag per node (Tarjan, iterative). */
std::vector<char> largestStronglyConnected(const std::vector<std::vector<int>>& next)
{
    const int n = int(next.size());
    std::vector<int> index(size_t(n), -1), low(size_t(n), 0), stack, comp(size_t(n), -1);
    std::vector<char> onStack(size_t(n), 0);
    int counter = 0, comps = 0;
    std::vector<int> sizes;
    for (int root = 0; root < n; ++root)
    {
        if (index[size_t(root)] >= 0)
            continue;
        std::vector<std::pair<int, size_t>> work{{root, 0}};
        while (!work.empty())
        {
            auto& [v, child] = work.back();
            if (child == 0 && index[size_t(v)] < 0)
            {
                index[size_t(v)] = low[size_t(v)] = counter++;
                stack.push_back(v);
                onStack[size_t(v)] = 1;
            }
            if (child < next[size_t(v)].size())
            {
                const int w = next[size_t(v)][child++];
                if (index[size_t(w)] < 0)
                    work.push_back({w, 0});
                else if (onStack[size_t(w)])
                    low[size_t(v)] = std::min(low[size_t(v)], index[size_t(w)]);
                continue;
            }
            if (low[size_t(v)] == index[size_t(v)])
            {
                int size = 0, w = -1;
                do
                {
                    w = stack.back();
                    stack.pop_back();
                    onStack[size_t(w)] = 0;
                    comp[size_t(w)] = comps;
                    ++size;
                } while (w != v);
                sizes.push_back(size);
                ++comps;
            }
            const int done = v;
            work.pop_back();
            if (!work.empty())
                low[size_t(work.back().first)] = std::min(low[size_t(work.back().first)], low[size_t(done)]);
        }
    }
    std::vector<char> out(size_t(n), 0);
    if (sizes.empty())
        return out;
    const int best = int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
    for (int v = 0; v < n; ++v)
        out[size_t(v)] = comp[size_t(v)] == best && sizes[size_t(best)] > 1;
    return out;
}

/**
 * ZoneGraph shapes: a spline per road with its lane profile, a polygon per junction joining the road ends. Mass traffic
 * has no exits and cannot turn back where a lane ends, so a two-way road's dead end gets a turning loop, two roads of
 * different profiles meeting end to end get a polygon between them, and the driving lanes of road directions cars could
 * enter but never leave (one-way stubs, one-way roads carrying on from two-way ones) are Access lanes; accessLines counts
 * the roads with any. Junctions a few metres apart are one polygon.
 */
json zoneData(const Rebuilt& r, const RoadNetwork& net)
{
    const Layout& l = r.layout;
    json profiles = json::object();
    json roads = json::array();
    json junctions = json::array();
    const size_t lineCount = net.lines.size();
    std::vector<std::vector<glm::dvec3>> linePts(lineCount);
    for (size_t k = 0; k < lineCount; ++k)
        linePts[k] = net.lines[k].pts;
    auto profileOf = [&](int line) -> const RoadProfile& { return l.roads[size_t(net.lines[size_t(line)].profile)]; };
    auto vehicleLine = [&](int line) { return profileOf(line).lanes > 0 && net.lines[size_t(line)].cls != StreetClass::Path; };
    auto backLanes = [&](int line) {
        const RoadProfile& p = profileOf(line);
        return p.oneWay ? 0 : p.lanes == 1 ? 1 : p.lanes / 2;
    };
    auto forwardLanes = [&](int line) {
        const RoadProfile& p = profileOf(line);
        return p.lanes == 1 && !p.oneWay ? 1 : p.lanes - backLanes(line);
    };

    // Road ends that no junction takes: pairs where two roads meet end to end, and dead ends.
    struct FreeEnd
    {
        int line;
        bool start;
    };
    std::set<std::pair<int, bool>> atJunction;
    for (const Junction& j : net.junctions)
        for (const RoadEnd& e : j.ends)
            atJunction.insert({e.line, e.start});
    std::vector<std::vector<FreeEnd>> groups;
    for (size_t k = 0; k < lineCount; ++k)
        for (bool start : {true, false})
        {
            const StreetLine& s = net.lines[k];
            if (s.pts.size() < 2 || !vehicleLine(int(k)) || atJunction.contains({int(k), start}))
                continue;
            const glm::dvec3 p = start ? s.pts.front() : s.pts.back();
            auto at = std::ranges::find_if(groups, [&](const std::vector<FreeEnd>& g) {
                const StreetLine& o = net.lines[size_t(g[0].line)];
                return glm::length((g[0].start ? o.pts.front() : o.pts.back()) - p) < 1.0;
            });
            if (at == groups.end())
                groups.push_back({{int(k), start}});
            else
                at->push_back({int(k), start});
        }
    constexpr double kJoinTrim = 300.0;
    std::map<std::pair<int, bool>, std::pair<int, bool>> linked;
    std::set<std::pair<int, bool>> turning;
    for (const std::vector<FreeEnd>& g : groups)
    {
        if (g.size() == 2 && polylineLength(linePts[size_t(g[0].line)]) > 3.0 * kJoinTrim &&
            polylineLength(linePts[size_t(g[1].line)]) > 3.0 * kJoinTrim)
        {
            linked[{g[0].line, g[0].start}] = {g[1].line, g[1].start};
            linked[{g[1].line, g[1].start}] = {g[0].line, g[0].start};
            continue;
        }
        for (const FreeEnd& e : g)
            if (forwardLanes(e.line) > 0 && backLanes(e.line) > 0)
                turning.insert({e.line, e.start});
    }

    // The road directions Mass traffic can drive round forever: arc 2k runs line k start to end, 2k + 1 end to start.
    auto leaving = [&](int line, bool start) {
        const int arc = 2 * line + (start ? 0 : 1);
        return (start ? forwardLanes(line) : backLanes(line)) > 0 ? arc : -1;
    };
    std::vector<std::vector<int>> next(2 * lineCount);
    auto link = [&](int from, int to) {
        if (to >= 0)
            next[size_t(from)].push_back(to);
    };
    std::map<std::pair<int, bool>, const Junction*> junctionAt;
    for (const Junction& j : net.junctions)
        for (const RoadEnd& e : j.ends)
            junctionAt[{e.line, e.start}] = &j;
    for (size_t k = 0; k < lineCount; ++k)
        for (int dir = 0; dir < 2; ++dir)
        {
            const int line = int(k), arc = 2 * line + dir;
            if (!vehicleLine(line) || (dir == 0 ? forwardLanes(line) : backLanes(line)) == 0)
                continue;
            const std::pair<int, bool> head{line, dir == 1}; // forward arrives at the end, backward at the start
            if (const auto j = junctionAt.find(head); j != junctionAt.end())
            {
                for (const RoadEnd& e : j->second->ends)
                    if (e.vehicle && (e.line != head.first || e.start != head.second))
                        link(arc, leaving(e.line, e.start));
            }
            else if (turning.contains(head))
                link(arc, leaving(head.first, head.second));
            else if (const auto partner = linked.find(head); partner != linked.end())
                link(arc, leaving(partner->second.first, partner->second.second));
        }
    const std::vector<char> driven = largestStronglyConnected(next);
    std::vector<std::string> laneProfile(lineCount);
    int accessLines = 0;
    for (size_t k = 0; k < lineCount; ++k)
    {
        const RoadProfile& p = profileOf(int(k));
        const bool vehicle = vehicleLine(int(k));
        const bool accessForward = vehicle && forwardLanes(int(k)) > 0 && !driven[2 * k];
        const bool accessBackward = vehicle && backLanes(int(k)) > 0 && !driven[2 * k + 1];
        laneProfile[k] = p.name + (accessForward && accessBackward ? "_access" : accessForward ? "_accessForward" : accessBackward ? "_accessBackward" : "");
        profiles[laneProfile[k]] = {{"lanes", lanesOf(p, accessForward, accessBackward)}};
        accessLines += accessForward || accessBackward;
    }

    for (const auto& [end, partner] : linked)
    {
        if (end > partner)
            continue;
        const StreetLine& s0 = net.lines[size_t(end.first)];
        const glm::dvec3 node = end.second ? s0.pts.front() : s0.pts.back();
        json ends = json::array();
        for (const auto& [line, start] : {end, partner})
        {
            std::vector<glm::dvec3>& pts = linePts[size_t(line)];
            pts = trimmed(pts, kJoinTrim, start);
            const glm::dvec3 at = ueVec(start ? pts.front() : pts.back());
            ends.push_back(polygonEnd(at, glm::normalize(ueVec(node) - at), laneProfile[size_t(line)], true, start));
        }
        junctions.push_back({{"center", ue(node)}, {"points", ends}, {"tags", json::array()}, {"control", "none"}});
    }
    for (const auto& [line, start] : turning)
    {
        const RoadProfile& p = profileOf(line);
        const std::vector<glm::dvec3>& pts = linePts[size_t(line)];
        const glm::dvec3 end = ueVec(start ? pts.front() : pts.back());
        glm::dvec3 out = end - ueVec(start ? pts[1] : pts[pts.size() - 2]);
        out.z = 0.0;
        if (glm::length(out) < 1e-6)
            continue;
        // A loop past the road's end turning onto the other side: in on the right, out on the left.
        out = glm::normalize(out);
        const glm::dvec3 right(-out.y, out.x, 0.0);
        const double radius = std::max(300.0, 0.5 * (p.lanes * p.laneWidth + p.median));
        const glm::dvec3 centre = end + out * 200.0;
        json loop = json::array();
        constexpr int kSteps = 8;
        for (int i = 0; i <= kSteps; ++i)
        {
            const double a = glm::pi<double>() * double(i) / kSteps;
            loop.push_back(jsonVec(centre + right * (radius * std::cos(a)) + out * (radius * std::sin(a))));
        }
        roads.push_back({{"points", loop}, {"profile", "turnaround"}, {"tags", json::array()}});
        profiles["turnaround"] = {{"lanes", json::array({{{"width", 300.0}, {"direction", "forward"}, {"tags", json::array({"Vehicle"})}}})}};
        json ends = json::array({polygonEnd(end, out, laneProfile[size_t(line)], true, start),
                                 polygonEnd(centre + right * radius, -out, "turnaround", true, true),
                                 polygonEnd(centre - right * radius, -out, "turnaround", true, false)});
        junctions.push_back({{"center", jsonVec(end + out * 100.0)}, {"points", ends}, {"tags", json::array()}, {"control", "none"}});
    }

    // Junctions joined by a road too short for a car to wait on are one polygon: MassTraffic lets a car into a junction
    // only when it fits on the road beyond, so a stub shorter than a car would hold its approach for good.
    constexpr double kMinLink = 1500.0;
    std::vector<size_t> group(net.junctions.size());
    std::iota(group.begin(), group.end(), size_t(0));
    auto root = [&](size_t j) {
        while (group[j] != j)
            j = group[j] = group[group[j]];
        return j;
    };
    std::vector<char> internal(lineCount, 0);
    for (size_t k = 0; k < lineCount; ++k)
    {
        const auto a = junctionAt.find({int(k), true}), b = junctionAt.find({int(k), false});
        if (a == junctionAt.end() || b == junctionAt.end() || a->second == b->second || polylineLength(net.lines[k].pts) >= kMinLink)
            continue;
        internal[k] = 1;
        group[root(size_t(a->second - net.junctions.data()))] = root(size_t(b->second - net.junctions.data()));
    }

    for (size_t k = 0; k < lineCount; ++k)
    {
        if (internal[k])
            continue;
        const StreetLine& s = net.lines[k];
        // Vehicle and Access are on the lanes: ZoneGraph adds a shape's tags to all its lanes.
        json tags = json::array();
        if (s.cls == StreetClass::Path)
            tags.push_back("Pedestrian");
        if (s.surface)
            tags.push_back("Structure");
        roads.push_back({{"points", uePoints(linePts[k])}, {"profile", laneProfile[k]}, {"tags", tags}});
    }
    std::map<size_t, std::vector<size_t>> merged;
    for (size_t j = 0; j < net.junctions.size(); ++j)
        merged[root(j)].push_back(j);
    for (const auto& [rootJunction, members] : merged)
    {
        json ends = json::array(), crosswalks = json::array();
        glm::dvec3 centre(0.0);
        Control control = Control::None;
        for (size_t m : members)
        {
            const Junction& j = net.junctions[m];
            centre += j.p / double(members.size());
            control = std::max(control, j.control);
            // ZoneGraph's lane profile points face into the shape, so the polygon's points point at the junction.
            for (const RoadEnd& e : j.ends)
                if (!internal[size_t(e.line)])
                    ends.push_back(polygonEnd(ueVec(e.p), ueVec(-e.out), laneProfile[size_t(e.line)], e.vehicle, e.start));
            for (const Crosswalk& c : j.crosswalks)
            {
                crosswalks.push_back({{"a", ue(c.a)}, {"b", ue(c.b)}, {"end", c.end}});
                // People cross on a lane of their own, which MassCrowd and the signal phases both see.
                const glm::dvec3 ab[] = {c.a, c.b};
                roads.push_back({{"points", uePoints(ab)}, {"profile", "crosswalk"}, {"tags", json::array({"Pedestrian", "Crossing"})}});
                profiles["crosswalk"] = {{"lanes", json::array({{{"width", 300.0}, {"direction", "forward"}, {"tags", json::array({"Pedestrian", "Crossing"})}}})}};
            }
        }
        if (ends.size() < 2)
            continue;
        json tags = json::array({"Intersection"});
        if (control == Control::Signal)
            tags.push_back("TrafficLight");
        if (control == Control::Stop || control == Control::Yield)
            tags.push_back("StopSign");
        // Phases and walks index a junction's own ends, so a merged junction leaves its signals to the engine.
        const bool single = members.size() == 1;
        junctions.push_back({{"center", ue(centre)},
                             {"points", ends},
                             {"tags", tags},
                             {"control", controlName(control)},
                             {"phases", single ? json(net.junctions[members[0]].phases) : json::array()},
                             {"walks", single ? json(net.junctions[members[0]].walks) : json::array()},
                             {"crosswalks", crosswalks},
                             {"merged", members.size()}});
    }
    // Levels above the ground: each deck's own road network, and the elevated roads, ramps and skybridges joining them.
    auto levelRoad = [&](std::span<const glm::dvec3> pts, StreetClass cls, int layer, const std::string& kind) {
        const RoadProfile& p = l.roads[size_t(std::min(cls, StreetClass::Path))];
        profiles[p.name] = {{"lanes", lanesOf(p)}};
        roads.push_back({{"points", uePoints(pts)},
                         {"profile", p.name},
                         {"tags", json::array({cls == StreetClass::Path ? "Pedestrian" : "Vehicle", "Level"})},
                         {"layer", layer},
                         {"kind", kind}});
    };
    if (l.gen)
    {
        for (size_t k = 0; k < l.gen->levels.size(); ++k)
            if (l.gen->levels[k].kind == "deck")
            {
                const RoadNetwork deck = levelNetwork(l, int(k));
                for (const StreetLine& s : deck.lines)
                    levelRoad(s.pts, s.cls, int(k) + 1, "deck");
                for (const Junction& j : deck.junctions)
                {
                    json ends = json::array();
                    for (const RoadEnd& e : j.ends)
                        ends.push_back(polygonEnd(ueVec(e.p), ueVec(-e.out), l.roads[size_t(deck.lines[size_t(e.line)].profile)].name, e.vehicle, e.start));
                    if (ends.size() >= 2)
                        junctions.push_back({{"center", ue(j.p)},
                                             {"points", ends},
                                             {"tags", json::array({"Intersection", "Vehicle", "Level"})},
                                             {"control", controlName(j.control)},
                                             {"phases", j.phases},
                                             {"walks", j.walks},
                                             {"crosswalks", json::array()},
                                             {"layer", int(k) + 1}});
                }
            }
        for (const LevelLine& line : levelLines(l, r.ground))
            if (line.kind != "deck" && line.kind != "air")
                levelRoad(line.pts, line.cls, line.layer, line.kind);
    }
    json parking = json::array();
    for (const ParkingSpot& p : net.parking)
        parking.push_back({{"position", ue(p.p)}, {"forward", ue(p.dir)}, {"length", p.length}, {"width", p.width}});
    return {{"profiles", profiles}, {"roads", roads}, {"junctions", junctions}, {"parking", parking}, {"accessLines", accessLines}};
}

std::string assetName(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out.empty() ? std::string("Settlement") : out;
}

/** An asset's object path: a package path gets ".<name>" when it has no object name. */
std::string objectPath(const std::string& asset)
{
    const size_t slash = asset.find_last_of('/');
    const std::string last = slash == std::string::npos ? asset : asset.substr(slash + 1);
    return last.find('.') != std::string::npos ? asset : asset + "." + last;
}

/** A placeInstances placement: Unreal space location, quaternion and scale. */
json placementJson(const Placement& p)
{
    const glm::dquat q(p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z);
    const glm::dmat4 world = glm::translate(glm::dmat4(1.0), p.position) * glm::mat4_cast(q) * glm::scale(glm::dmat4(1.0), p.scale);
    const math::Transform t = math::toUnreal(math::Transform(math::Matrix(world)));
    return {{"id", p.id},
            {"name", p.id},
            {"location", json::array({t.translation.x, t.translation.y, t.translation.z})},
            {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
            {"scale", json::array({t.scale3d.x, t.scale3d.y, t.scale3d.z})}};
}

/** Writes a mesh (positions about the cell centre) as FBX, imports it as a static mesh and places it as one actor at centre. */
json sendCellMesh(link::RpcClient& c, const geo::Mesh& mesh, const std::string& name, const std::string& group, const glm::dvec3& centre,
                  const std::string& folder, const fs::path& dir, const char* cmd)
{
    const fs::path file = dir / (name + ".fbx");
    fbx::ExportScene scene;
    fbx::ExportNode node;
    node.name = name;
    node.mesh = std::make_shared<const geo::Mesh>(mesh);
    scene.nodes.push_back(std::move(node));
    std::string err;
    if (!fbx::writeFbx(scene, file, fbx::ExportOptions{}, &err))
        throw CommandError(std::string(cmd) + ": writing " + file.string() + ": " + err);
    const json imported = call(c, "importAsset", link::importParams(file, {{"contentPath", folder + "/" + name}, {"kind", "static"}, {"reapplyOptions", true}}), cmd);
    std::error_code ec;
    fs::remove(file, ec);
    std::string asset = folder + "/" + name + "." + name;
    if (const json made = imported.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
        asset = made[0].get<std::string>();
    return call(c, "placeInstances",
                {{"group", group},
                 {"label", name},
                 {"mesh", asset},
                 {"mode", "actors"},
                 {"placements", json::array({{{"id", group}, {"name", name}, {"location", ue(centre)}, {"rotation", json::array({0, 0, 0, 1})}, {"scale", json::array({1, 1, 1})}}})}},
                cmd);
}

/** Ground roads (surface=false) on the landscape or as one spline actor, or the surface paths as their own actor. */
json sendSplines(link::RpcClient& c, const Rebuilt& r, const Args& a, const std::string& key, const std::string& label, bool surface,
                 const char* cmd)
{
    LILA_ZONE();

    curve::Splines splines;
    for (const StreetLine& s : streetLines(r.layout, r.ground))
        if (s.surface == surface)
            splines.push_back(smoothSpline(s.pts, 0.5 * s.width / 100.0));
    const bool landscape = !surface && a.get<std::string>("roads", "landscape") == "landscape";
    json params = {{"id", key + (surface ? "/paths" : "/roads")},
                   {"label", label + (surface ? "_Paths" : "_Roads")},
                   {"mode", landscape ? "landscape" : "actor"},
                   {"location", json::array({0, 0, 0})},
                   {"splines", link::splinePoints(splines, glm::dvec3(0.0))},
                   {"folder", "Lila/Settlement"}};
    if (!surface)
    {
        params["width"] = 100.0;
        params["join"] = true;
        params["deformTerrain"] = a.get<bool>("deformTerrain", true);
        if (a.has("landscape"))
            params["landscape"] = a.get<std::string>("landscape");
    }
    if (a.has("roadMesh"))
        params["mesh"] = {{"asset", a.get<std::string>("roadMesh")}, {"forwardAxis", "x"}};
    return call(c, "setSpline", params, cmd);
}

/** Levels above the ground as one spline actor: deck streets, elevated roads, ramps and skybridges at their heights. */
json sendLevels(link::RpcClient& c, const Rebuilt& r, const std::string& key, const std::string& label, const char* cmd)
{
    curve::Splines splines;
    for (const LevelLine& line : levelLines(r.layout, r.ground))
        if (line.kind != "air")
            splines.push_back(smoothSpline(line.pts, 0.0));
    return call(c, "setSpline",
                {{"id", key + "/levels"},
                 {"label", label + "_Levels"},
                 {"mode", "actor"},
                 {"location", json::array({0, 0, 0})},
                 {"splines", link::splinePoints(splines, glm::dvec3(0.0))},
                 {"folder", "Lila/Settlement"}},
                cmd);
}

json readJsonFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return json();
    try
    {
        return json::parse(f);
    }
    catch (const std::exception&)
    {
        return json();
    }
}

/** The cell named at the end of an item key ("<key>/pcg/<x>_<z>"). */
std::string cellNameOf(const std::string& itemKey) { return itemKey.substr(itemKey.find_last_of('/') + 1); }

json sendToUnreal(const Args& a)
{
    LILA_ZONE();

    const char* cmd = "settlementSendToUnreal";
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one settlement");
    Rebuilt r = rebuildSettlement(scene::Graph::current(), names[0], cmd);
    addPlacements(r.layout, a, cmd);
    const std::string key = r.mesh.empty() ? r.modifier : r.mesh;
    std::string label = key;
    if (const size_t slash = label.find_last_of("|/"); slash != std::string::npos)
        label = label.substr(slash + 1);
    const std::string mode = a.get<std::string>("roads", "landscape");
    if (mode != "landscape" && mode != "splines" && mode != "meshes" && mode != "none")
        throw CommandError(std::string(cmd) + ": roads is landscape, splines, meshes or none, not '" + mode + "'");
    const std::string placeMode = a.get<std::string>("placeMode", "hism");
    if (placeMode != "hism" && placeMode != "ism" && placeMode != "actors")
        throw CommandError(std::string(cmd) + ": placeMode is hism, ism or actors, not '" + placeMode + "'");
    const double cellSize = std::max(1000.0, a.get<double>("cell", 12800.0));
    const std::string folder = a.get<std::string>("folder", "/Game/Lila/Settlement");
    const std::string cellFolder = folder + "/" + assetName(label);
    const std::string graph = a.get<std::string>("pcgGraph", "");

    const RoadNetwork net = roadNetwork(r.layout, r.ground);
    const std::map<Cell, CellContents> cells = partition(r.layout, net, cellSize);
    std::map<Cell, geo::Mesh> roadMeshes;
    std::map<Cell, uint64_t> roadHashes;
    if (mode == "meshes")
    {
        roadMeshes = meshByCell(toMeshes(r.layout, r.ground).streets, cellSize);
        for (const auto& [cell, m] : roadMeshes)
            roadHashes[cell] = meshHash(m);
    }
    SyncOptions o;
    o.key = key;
    o.roads = mode;
    o.massing = a.get<bool>("massing", true);
    o.pcg = a.get<bool>("pcg", true);
    o.partition = a.get<bool>("partition", true);
    o.zoneGraph = a.get<bool>("zoneGraph", true);
    o.settings = json({{"folder", folder},
                       {"cell", cellSize},
                       {"placeMode", placeMode},
                       {"pcgGraph", graph},
                       {"landscape", a.get<std::string>("landscape", "")},
                       {"roadMesh", a.get<std::string>("roadMesh", "")},
                       {"deformTerrain", a.get<bool>("deformTerrain", true)}})
                     .dump();
    const std::vector<SyncItem> items = syncItems(r.layout, net, cells, o, roadHashes);
    const std::map<std::string, uint64_t> elements = elementHashes(r.layout, net);

    // One manifest per settlement and Unreal project: what was last sent there, for the diff.
    const std::string project = link::Link::get().connected() ? link::Link::get().status().value("project", std::string()) : std::string();
    const fs::path manifestPath = link::cacheDir() / "settlement" / ("sync_" + assetName(project) + "_" + assetName(key) + ".json");
    const json previous = a.get<bool>("resend", false) ? json() : readJsonFile(manifestPath);
    const SyncDiff d = diffSync(previous, items, elements);
    json kinds = json::object();
    for (const SyncItem& i : items)
        kinds[i.kind] = kinds.value(i.kind, 0) + 1;
    json out = {{"cells", cells.size()}, {"cellSize", cellSize}, {"items", items.size()}, {"kinds", kinds}, {"diff", toJson(d)}};
    if (a.get<bool>("dryRun", false))
    {
        out["dryRun"] = true;
        out["manifest"] = manifestPath.generic_string();
        return out;
    }

    std::map<std::string, const SyncItem*> byKey;
    for (const SyncItem& i : items)
        byKey[i.key] = &i;
    std::vector<const char*> features;
    bool splines = false, settlement = false, cellData = false;
    for (const std::vector<std::string>* keys : {&d.added, &d.changed})
        for (const std::string& k : *keys)
        {
            const std::string& kind = byKey[k]->kind;
            splines |= (kind == "roads" && mode != "meshes") || kind == "paths" || kind == "levels";
            settlement |= kind == "pcg" || kind == "zoneGraph";
            cellData |= kind == "pcg" && o.partition;
        }
    if (splines)
        features.push_back(link::kSplineFeature);
    if (settlement)
        features.push_back("settlement");
    std::shared_ptr<link::RpcClient> c = client(cmd, std::initializer_list<const char*>{});
    for (const char* f : features)
        client(cmd, {f});
    if (cellData)
    {
        const json have = link::Link::get().status().value("features", json::array());
        if (!have.is_array() || std::ranges::find(have, json("settlementCells")) == have.end())
            throw CommandError(std::string(cmd) + ": the LilaLinkSettlement plugin in Unreal predates PCG cells; rebuild it from Lila's "
                                                  "ue/LilaLinkSettlement, or send with partition=False");
    }

    const fs::path dir = link::cacheDir() / "settlement" / assetName(label);
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::map<Cell, geo::Mesh> massingCells;
    if (o.massing)
        massingCells = massingByCell(r.layout, cellSize);
    json sent = json::object(), errors = json::array();
    json warnings = json::array();
    std::set<std::string> failed;
    auto send = [&](const SyncItem& i) -> json {
        const Vec2 mid = i.cell.center(cellSize);
        if (i.kind == "roads" && mode == "meshes")
            return sendCellMesh(*c, roadMeshes.at(i.cell), "SM_" + assetName(label) + "_Roads_" + i.cell.name(), i.key,
                                {mid.x, 0.0, mid.y}, cellFolder, dir, cmd);
        if (i.kind == "roads" || i.kind == "paths")
            return sendSplines(*c, r, a, key, label, i.kind == "paths", cmd);
        if (i.kind == "levels")
            return sendLevels(*c, r, key, label, cmd);
        if (i.kind == "massing")
            return sendCellMesh(*c, massingCells.at(i.cell), "SM_" + assetName(label) + "_Massing_" + i.cell.name(), i.key,
                                {mid.x, 0.0, mid.y}, cellFolder, dir, cmd);
        if (i.kind == "pcg" && o.partition)
        {
            json params = pcgData(r, net, &cells.at(i.cell));
            params["id"] = key;
            params["cell"] = i.cell.name();
            params["asset"] = cellFolder + "/PCG_" + assetName(label) + "_" + i.cell.name();
            params["label"] = label + "_Cell_" + i.cell.name();
            params["center"] = ue({mid.x, r.ground.height(mid), mid.y});
            params["size"] = cellSize;
            if (!graph.empty())
            {
                params["graph"] = graph;
                params["makeGraph"] = a.get<bool>("makeGraph", false);
            }
            return call(*c, "setSettlementCell", params, cmd);
        }
        if (i.kind == "pcg")
        {
            json params = pcgData(r, net);
            params["id"] = key;
            params["asset"] = folder + "/PCG_" + assetName(label);
            params["label"] = label;
            return call(*c, "setSettlementData", params, cmd);
        }
        if (i.kind == "place")
        {
            json list = json::array();
            for (int k : cells.at(i.cell).placements)
                if (r.layout.placements[size_t(k)].asset == i.asset)
                    list.push_back(placementJson(r.layout.placements[size_t(k)]));
            return call(*c, "placeInstances",
                        {{"group", i.key},
                         {"label", label + "_" + safeName(i.asset.substr(i.asset.find_last_of('/') + 1)) + "_" + i.cell.name()},
                         {"mesh", objectPath(i.asset)},
                         {"mode", placeMode},
                         {"placements", list}},
                        cmd, 600000);
        }
        if (i.kind == "zoneGraph")
        {
            json params = zoneData(r, net);
            if (const int access = params.value("accessLines", 0); access > 0)
                warnings.push_back(std::format("{} roads carry Access lanes Mass traffic stays off: cars could drive in but never back out "
                                               "(one-way dead ends, one-way roads carrying on from two-way ones)", access));
            params["id"] = key;
            params["label"] = label + "_Lanes";
            params["folder"] = folder;
            return call(*c, "setZoneGraph", params, cmd);
        }
        throw CommandError(std::string(cmd) + ": no sender for " + i.kind);
    };
    // Placements of an asset the project doesn't have are one finding per asset, not one error per cell.
    std::map<std::string, int> missingAssets;
    constexpr std::string_view kNoMesh = "no static mesh at ";
    for (const std::vector<std::string>* keys : {&d.added, &d.changed})
        for (const std::string& k : *keys)
            try
            {
                sent[k] = send(*byKey[k]);
            }
            catch (const std::exception& e)
            {
                failed.insert(k);
                const std::string what = e.what();
                if (const size_t at = what.find(kNoMesh); byKey[k]->kind == "place" && at != std::string::npos)
                    ++missingAssets[what.substr(at + kNoMesh.size())];
                else
                    errors.push_back(k + ": " + what);
            }

    // What the last send made that this layout no longer has.
    const json before = previous.is_object() ? previous.value("items", json::object()) : json::object();
    json groups = json::array(), meshGroups = json::array(), pcgCells = json::array(), removed = json::object();
    std::set<std::string> keptRemoved;
    for (const std::string& k : d.removed)
    {
        const std::string kind = before[k].value("kind", std::string());
        try
        {
            // Massing and road mesh cells own their static meshes, which go with them; placements show the project's.
            if (kind == "massing" || (kind == "roads" && k.find("/roads/") != std::string::npos))
                meshGroups.push_back(k);
            else if (kind == "place")
                groups.push_back(k);
            else if (kind == "roads" || kind == "paths" || kind == "levels")
                removed[k] = call(*c, "removeSpline", {{"id", k}}, cmd);
            else if (kind == "pcg" && k.find("/pcg/") != std::string::npos)
                pcgCells.push_back(cellNameOf(k));
            else if (kind == "zoneGraph")
                removed[k] = call(*c, "removeSettlement", {{"id", key}}, cmd);
        }
        catch (const std::exception& e)
        {
            keptRemoved.insert(k);
            errors.push_back(k + ": " + e.what());
        }
    }
    auto removeBatch = [&](const json& list, const char* method, json params, const char* field) {
        if (list.empty())
            return;
        try
        {
            removed[field] = call(*c, method, params, cmd);
        }
        catch (const std::exception& e)
        {
            for (const json& k : list)
                keptRemoved.insert(k.get<std::string>().find('/') != std::string::npos ? k.get<std::string>() : key + "/pcg/" + k.get<std::string>());
            errors.push_back(std::string(method) + ": " + e.what());
        }
    };
    removeBatch(groups, "removePlacements", {{"groups", groups}}, "placements");
    removeBatch(meshGroups, "removePlacements", {{"groups", meshGroups}, {"deleteMeshes", true}, {"meshFolder", cellFolder}}, "meshCells");
    removeBatch(pcgCells, "removeSettlementCells", {{"id", key}, {"cells", pcgCells}}, "pcgCells");

    // The manifest records what Unreal holds now: failed sends keep their old hash (or none) so the next send retries them.
    std::vector<SyncItem> held;
    for (const SyncItem& i : items)
        if (!failed.contains(i.key))
            held.push_back(i);
    json manifest = syncManifest(held, elements);
    for (const std::string& k : failed)
        if (before.contains(k))
            manifest["items"][k] = before[k];
    for (const std::string& k : keptRemoved)
        manifest["items"][k] = before[k];
    fs::create_directories(manifestPath.parent_path(), ec);
    std::ofstream(manifestPath, std::ios::binary) << manifest.dump(1);

    out["sent"] = sent;
    out["removed"] = removed;
    out["errors"] = errors;
    if (!missingAssets.empty())
    {
        out["missingAssets"] = missingAssets;
        warnings.push_back(std::format("placements of {} assets not sent: the project has no static mesh at those paths (missingAssets); "
                                       "name the project's assets with settlementFurniture(engineAssets=...) or add them, then send again",
                                       missingAssets.size()));
    }
    if (!warnings.empty())
        out["warnings"] = warnings;
    out["manifest"] = manifestPath.generic_string();
    return out;
}
} // namespace

void registerUnrealCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "settlementSendToUnreal",
        .doc = "Sends a settlement to the connected Unreal editor and keeps it in sync: each send compares what it would make "
               "with what the last send to that project made (a manifest keyed by stable ids: streets, lots, envelopes, "
               "junctions, placements) and sends only the objects whose contents changed, removing those that are gone. "
               "Everything spatial is cut into World Partition cells of side cell, one actor per cell, so the level streams "
               "it. Roads go as landscape splines (roads='landscape', joined at junctions, shaping the landscape), as spline "
               "actors (roads='splines', with spline meshes of roadMesh along them), as ribbon meshes per cell "
               "(roads='meshes') or not at all; paths on the walkable mesh go as a spline actor. massing=True sends each "
               "cell's envelopes as a static mesh. pcg=True writes PCG data: per cell (partition=True) a data asset and a cell "
               "actor whose PCG component runs pcgGraph with the asset as its SettlementData parameter, else one data asset "
               "for the whole settlement. Pins: Lots (polygons: Key, Use, Area, Frontages, Corner, FrontClass, Seed), "
               "Envelopes (footprints: MaxHeight, Floors, FloorHeight, Base, Ground, Use, Roof, Era, Style, Seed), "
               "EnvelopePoints, Roads (splines: Key, Class, Profile, Width, Lanes), Intersections and ParkingSpots. "
               "zoneGraph=True writes ZoneGraph lane shapes and rebuilds the ZoneGraph. The settlement's placements (library "
               "buildings, furniture, and the placements flag) go as HISM instances per cell and asset. dryRun=True only "
               "returns the cells and the diff. Returns the diff, what Unreal made and removed, and any errors.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"roads", "rd", Arg::String, "landscape", "How roads go.", {"landscape", "splines", "meshes", "none"}},
                  {"roadMesh", "rm", Arg::String, nullptr, "Static mesh asset laid along road splines, such as /Game/Roads/SM_Road."},
                  {"landscape", "ls", Arg::String, nullptr, "roads='landscape': the landscape's label (default the first one)."},
                  {"deformTerrain", "dt", Arg::Bool, true, "roads='landscape': the splines shape the landscape."},
                  {"cell", "ce", Arg::Float, 12800.0, "World Partition cell side, cm: road meshes, massing, PCG data and placements are cut by it.",
                   {}, {}, 1000.0},
                  {"massing", "ma", Arg::Bool, true, "Envelopes as a static mesh per cell."},
                  {"pcg", "pc", Arg::Bool, true, "Lots, envelopes and roads as PCG data."},
                  {"partition", "pt", Arg::Bool, true, "PCG data per cell (a data asset and a cell actor each); off for one asset."},
                  {"pcgGraph", "pg", Arg::String, nullptr, "PCG graph each cell actor runs; it reads the cell's data asset from its SettlementData parameter."},
                  {"makeGraph", "mg", Arg::Bool, false, "Make pcgGraph when the project has none: an empty PCG graph with the SettlementData parameter, ready for its nodes."},
                  {"zoneGraph", "zg", Arg::Bool, true, "Lanes as ZoneGraph shapes, for Mass traffic and crowds."},
                  {"placeMode", "pm", Arg::String, "hism", "How placements go.", {"hism", "ism", "actors"}},
                  {"placements", "pl", Arg::Any, nullptr,
                   "Extra placements, a list or its JSON text: [{id, asset, position: [x, y, z], rotation: [x, y, z, w], scale, tags}], cm."},
                  {"resend", "rs", Arg::Bool, false, "Send everything, ignoring what the last send made (after the level was reset)."},
                  {"dryRun", "dr", Arg::Bool, false, "Return the cells and the diff without sending."},
                  {"folder", "f", Arg::String, "/Game/Lila/Settlement", "Content folder for assets."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return sendToUnreal(a); },
        .examples = {"settlementSendToUnreal('Settlement', roads='landscape', pcg=True, zoneGraph=True)",
                     "settlementSendToUnreal('Settlement', roads='meshes', cell=12800, pcgGraph='/Game/PCG/PCG_Buildings')",
                     "settlementSendToUnreal('Settlement', dryRun=True)['diff']"},
    });

    ctx.registerCommand({
        .name = "settlementRoadProfileToUnreal",
        .doc = "Writes a road profile to Unreal as a road shape grammar: a PCG data asset with a ModulesInfo pin (Symbol, Size, "
               "Scalable per slot: Verge, Sidewalk, Parking, Lane, Median) and a Grammar pin holding the grammar string, ready "
               "for Subdivide Segment and Duplicate Cross Sections on road splines. The profile is a built-in one, or one of "
               "the settlement's own when a settlement is given. Returns the asset, slots and grammar.",
        .positionalDoc = "settlement mesh (optional, for its own road profiles)",
        .flags = {{"profile", "pf", Arg::String, "street", "Road profile name."},
                  {"asset", "as", Arg::String, nullptr, "Content path (default /Game/Lila/Settlement/RoadGrammar_<profile>)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementRoadProfileToUnreal";
            const std::string name = a.get<std::string>("profile", "street");
            RoadProfile p;
            bool found = builtinRoadProfile(name, &p);
            if (!found && !a.objects().empty())
            {
                Site site;
                const Rebuilt r = rebuildSettlement(scene::Graph::current(), a.objects()[0], cmd);
                for (const RoadProfile& own : r.layout.roads)
                    if (own.name == name)
                    {
                        p = own;
                        found = true;
                    }
                if (!found)
                    try
                    {
                        applyEdits(site, parseEdits(r.values.value("edits", std::string())));
                    }
                    catch (const std::exception&)
                    {
                    }
                for (const RoadProfile& own : site.roadProfiles)
                    if (own.name == name)
                    {
                        p = own;
                        found = true;
                    }
            }
            if (!found)
                throw CommandError(std::string(cmd) + ": no road profile '" + name + "'");
            const std::vector<CrossSlot> slots = crossSection(p);
            json modules = json::array();
            for (const CrossSlot& s : slots)
                modules.push_back({{"symbol", s.symbol}, {"size", s.size}, {"scalable", s.symbol == "Verge"}});
            const std::string asset = a.get<std::string>("asset", "/Game/Lila/Settlement/RoadGrammar_" + assetName(name));
            std::shared_ptr<link::RpcClient> c = client(cmd, {"settlement"});
            json out = call(*c, "setRoadGrammar", {{"asset", asset}, {"label", name}, {"modules", modules}, {"grammar", grammarOf(slots)}}, cmd);
            out["slots"] = modules;
            out["grammar"] = grammarOf(slots);
            return out;
        },
        .examples = {"settlementRoadProfileToUnreal(profile='avenue')"},
    });

    ctx.registerCommand({
        .name = "settlementRoadProfileFromUnreal",
        .doc = "Reads a road definition in Unreal (a PCG data asset with ModulesInfo, or any asset holding slots with a name and "
               "a size, such as a City Sample shape grammar definition) and adds it to the settlement as a road profile, so "
               "streets laid with it match the road kit exactly. Slots are placed by their names (lane, median, parking, "
               "sidewalk or curb, verge); others count as verge and are listed. Returns the profile and the slots read.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"asset", "as", Arg::String, nullptr, "Content path of the definition."},
                  {"name", "n", Arg::String, nullptr, "Profile name (default the asset's name)."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementRoadProfileFromUnreal";
            if (a.objects().size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            if (!a.has("asset"))
                throw CommandError(std::string(cmd) + ": asset= required");
            const std::string asset = a.get<std::string>("asset");
            std::shared_ptr<link::RpcClient> c = client(cmd, {"settlement"});
            const json read = call(*c, "readRoadGrammar", {{"asset", asset}}, cmd);
            std::vector<CrossSlot> modules;
            for (const json& m : read.value("modules", json::array()))
                modules.push_back({m.value("symbol", std::string()), m.value("size", 0.0)});
            // The grammar orders and repeats the modules; without one each module is one slot.
            std::vector<CrossSlot> slots;
            const std::vector<std::string> symbols = grammarSymbols(read.value("grammar", std::string()));
            for (const std::string& s : symbols)
                for (const CrossSlot& m : modules)
                    if (m.symbol == s)
                    {
                        slots.push_back(m);
                        break;
                    }
            if (slots.empty())
                slots = modules;
            if (slots.empty())
                throw CommandError(std::string(cmd) + ": " + asset + " holds no slots with a name and a size");
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = assetName(asset.substr(asset.find_last_of('/') + 1));
            std::vector<std::string> unknown;
            const RoadProfile p = profileFromSlots(name, slots, &unknown);
            const std::string modifier = rebuildSettlement(scene::Graph::current(), a.objects()[0], cmd).modifier;
            json edits = parseEdits(lila::ops::execute("getAttr", {{"args", {modifier + ".edits"}}}).get<std::string>());
            json fields = toJson(p);
            fields.erase("name");
            fields.erase("width");
            edits["roadProfiles"][name] = fields;
            lila::ops::execute("setAttr", {{"args", {modifier + ".edits", edits.dump()}}});
            json slotsOut = json::array();
            for (const CrossSlot& s : slots)
                slotsOut.push_back({{"symbol", s.symbol}, {"size", s.size}});
            return {{"profile", toJson(p)}, {"slots", slotsOut}, {"unknown", unknown}};
        },
        .examples = {"settlementRoadProfileFromUnreal('Settlement', asset='/Game/CitySample/Roads/SG_Avenue')"},
    });
}
} // namespace settlement
