// Settlement: the plan's verification checks on a layout, and why each part is where it is.

#include "Streetscape.h"
#include "SettlementTerrain.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>

namespace settlement
{
namespace
{
json at(Vec2 p) { return json::array({p.x, p.y}); }

bool properCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    const double eps = 1e-6 * std::max({glm::length(b - a), glm::length(d - c), 1.0});
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

bool segmentsMeet(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 >= 0.0) != (d2 >= 0.0) || d1 == 0.0 || d2 == 0.0) && ((d3 >= 0.0) != (d4 >= 0.0) || d3 == 0.0 || d4 == 0.0);
}

/** True when the polygons share area: sides crossing, or a corner well inside the other. */
bool shareArea(const Polygon& a, const Polygon& b, double tol)
{
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
            if (properCross(a.pts[i], a.pts[(i + 1) % a.size()], b.pts[j], b.pts[(j + 1) % b.size()]))
                return true;
    for (const Vec2& q : a.pts)
        if (contains(b.pts, q) && boundaryDistance(b.pts, q) > tol)
            return true;
    for (const Vec2& q : b.pts)
        if (contains(a.pts, q) && boundaryDistance(a.pts, q) > tol)
            return true;
    return false;
}

/** Strongly connected components of a directed graph (iterative Tarjan); returns the component of every vertex. */
std::vector<int> components(int n, const std::vector<std::vector<int>>& out, int* count)
{
    std::vector<int> index(size_t(n), -1), low(size_t(n), 0), comp(size_t(n), -1);
    std::vector<char> onStack(size_t(n), 0);
    std::vector<int> stack;
    int next = 0, made = 0;
    for (int s = 0; s < n; ++s)
    {
        if (index[size_t(s)] >= 0)
            continue;
        std::vector<std::pair<int, size_t>> work = {{s, 0}};
        index[size_t(s)] = low[size_t(s)] = next++;
        stack.push_back(s);
        onStack[size_t(s)] = 1;
        while (!work.empty())
        {
            auto& [v, k] = work.back();
            if (k < out[size_t(v)].size())
            {
                const int w = out[size_t(v)][k++];
                if (index[size_t(w)] < 0)
                {
                    index[size_t(w)] = low[size_t(w)] = next++;
                    stack.push_back(w);
                    onStack[size_t(w)] = 1;
                    work.push_back({w, 0});
                }
                else if (onStack[size_t(w)])
                    low[size_t(v)] = std::min(low[size_t(v)], index[size_t(w)]);
                continue;
            }
            if (low[size_t(v)] == index[size_t(v)])
            {
                int w;
                do
                {
                    w = stack.back();
                    stack.pop_back();
                    onStack[size_t(w)] = 0;
                    comp[size_t(w)] = made;
                } while (w != v);
                ++made;
            }
            const int done = v;
            work.pop_back();
            if (!work.empty())
                low[size_t(work.back().first)] = std::min(low[size_t(work.back().first)], low[size_t(done)]);
        }
    }
    *count = made;
    return comp;
}

/** The reasons field another part of Settlement may give a struct, when it has one. */
template <class T> std::vector<std::string> reasonsOf(const T& x)
{
    if constexpr (requires { x.reasons; })
    {
        std::vector<std::string> out;
        for (const auto& r : x.reasons)
            out.push_back(std::string(r));
        return out;
    }
    else
        return {};
}

const char* useLabel(Use u) { return useName(u); }
} // namespace

bool polygonsShareArea(const Polygon& a, const Polygon& b, double tol) { return shareArea(a, b, tol); }

json streetscapeLaneChecks(const Layout& l, const Ground& ground)
{
    const json r = checkLanes(l, ground);
    json issues = json::array();
    if (!r.is_object())
        return issues;
    auto add = [&](const char* severity, const json& id, std::string message) {
        issues.push_back({{"check", "laneGraph"}, {"severity", severity}, {"id", id.is_string() ? id.get<std::string>() : id.is_null() ? std::string("lanes") : id.dump()}, {"message", std::move(message)}, {"at", nullptr}});
    };
    const json modes = r.value("modes", json::object());
    for (const auto& [mode, m] : modes.items())
    {
        if (m.value("components", 0) > 1)
            add("error", nullptr, std::format("{} lanes split into {} groups; {} lanes cannot reach the rest", mode, m.value("components", 0),
                                              m.value("isolatedCount", 0)));
        for (const json& x : m.value("cannotReachExit", json::array()))
            add("error", x, std::format("{} lane {} cannot reach an exit", mode, x.dump()));
        for (const json& x : m.value("unreachableExits", json::array()))
            add("error", x, std::format("{} exit lane {} cannot be reached", mode, x.dump()));
    }
    for (const json& c : r.contains("conflictingGreens") ? r["conflictingGreens"] : json::array())
    {
        auto move = [](const json& m) { return std::format("{} lane {} to {}", m.value("move", std::string()), m.value("from", -1), m.value("to", -1)); };
        add("error", c.value("junction", -1), std::format("junction {} phase {}: green movements {} ({} and {})", c.value("junction", -1), c.value("phase", -1),
                                                            c.value("kind", std::string("cross")), move(c.value("a", json::object())), move(c.value("b", json::object()))));
    }
    for (const json& u : r.contains("unphasedApproaches") ? r["unphasedApproaches"] : json::array())
        add("error", u.value("junction", -1), std::format("junction {}: approach {} has no signal phase", u.value("junction", -1), u.value("end", -1)));
    for (const json& t : r.contains("unreachableRoutes") ? r["unreachableRoutes"] : json::array())
        add("warning", t.value("route", -1), std::format("transit route {} is not reachable: {}", t.value("name", std::string()), t.value("note", std::string())));
    return issues;
}

bool edgeInWater(const Layout& l, int edge)
{
    const StreetEdge& e = l.graph.edges[size_t(edge)];
    const Vec2 a = l.graph.nodes[size_t(e.a)].p, b = l.graph.nodes[size_t(e.b)].p;
    if (l.water.empty())
        return false;
    for (const WaterLine& w : l.water)
        for (size_t k = 0; k + 1 < w.pts.size(); ++k)
            if (segmentsMeet(a, b, w.pts[k], w.pts[k + 1]))
                return true;
    return bankDistance((a + b) * 0.5, l.water) < 0.0;
}

bool edgeIsBridge(const Layout& l, int edge)
{
    const StreetEdge& e = l.graph.edges[size_t(edge)];
    if (e.bridge || e.tunnel)
        return true;
    if (e.cls != StreetClass::Arterial || l.water.empty())
        return false;
    if (edgeInWater(l, edge))
        return true;
    // A main road's approach whose end reaches into the channel is part of the span, not a climb up the bank.
    const Vec2 a = l.graph.nodes[size_t(e.a)].p, b = l.graph.nodes[size_t(e.b)].p;
    for (int k = 0; k <= 8; ++k)
        if (bankDistance(a + (b - a) * (k / 8.0), l.water) < 0.0)
            return true;
    return false;
}

json checkLayout(const Layout& l, const Ground& ground, const Options& o)
{
    LILA_ZONE();

    (void)o;
    const Profile& p = l.profile;
    json issues = json::array();
    std::map<std::string, int> counts;
    auto issue = [&](const char* check, const char* severity, const std::string& id, std::string message, json where) {
        ++counts[check];
        issues.push_back({{"check", check}, {"severity", severity}, {"id", id}, {"message", std::move(message)}, {"at", std::move(where)}});
    };

    for (const Lot& lot : l.lots)
        if (lot.frontages == 0 && lot.use != Use::Plaza && lot.use != Use::Open)
            issue("frontage", "error", "lot:" + std::to_string(lot.id), std::format("{} lot {} has no street frontage", useLabel(lot.use), lot.id),
                  at(centroid(lot.outline)));

    {
        // Lots binned by their bounds, each pair tested once.
        const double cell = std::max(2000.0, p.lotDepth);
        std::unordered_map<int64_t, std::vector<size_t>> bins;
        auto key = [](int x, int y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); };
        for (size_t i = 0; i < l.lots.size(); ++i)
        {
            Vec2 lo(std::numeric_limits<double>::max()), hi(-lo);
            for (const Vec2& q : l.lots[i].outline.pts)
                lo = glm::min(lo, q), hi = glm::max(hi, q);
            for (int y = int(std::floor(lo.y / cell)); y <= int(std::floor(hi.y / cell)); ++y)
                for (int x = int(std::floor(lo.x / cell)); x <= int(std::floor(hi.x / cell)); ++x)
                    bins[key(x, y)].push_back(i);
        }
        std::map<std::pair<size_t, size_t>, bool> seen;
        for (const auto& [k, list] : bins)
            for (size_t m = 0; m < list.size(); ++m)
                for (size_t n = m + 1; n < list.size(); ++n)
                {
                    const auto pr = std::minmax(list[m], list[n]);
                    if (seen.contains(pr))
                        continue;
                    seen[pr] = true;
                    const Lot &a = l.lots[pr.first], &b = l.lots[pr.second];
                    if (a.outline.pts.size() >= 3 && b.outline.pts.size() >= 3 && shareArea(a.outline, b.outline, 2.0))
                        issue("overlap", "error", std::format("lot:{}", a.id), std::format("lots {} and {} overlap", a.id, b.id), at(centroid(a.outline)));
                }
    }

    std::map<uint64_t, std::pair<double, int>> steepest;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        if (!l.graph.isStreet(int(e)))
            continue;
        const StreetEdge& x = l.graph.edges[e];
        const Vec2 a = l.graph.nodes[size_t(x.a)].p, b = l.graph.nodes[size_t(x.b)].p;
        const bool bridge = edgeIsBridge(l, int(e));
        if (!bridge && ground.gradeAlong(a, b) > classMaxGrade(p, x.cls) * 1.05 + 1e-9)
        {
            auto& s = steepest[x.street];
            if (ground.gradeAlong(a, b) > s.first)
                s = {ground.gradeAlong(a, b), int(e)};
        }
        if (edgeInWater(l, int(e)) && !bridge)
            issue("water", "error", "street:" + std::to_string(x.street),
                  std::format("{} street {} runs through water without a bridge", className(x.cls), x.street), at((a + b) * 0.5));
    }
    for (const auto& [street, s] : steepest)
    {
        const StreetEdge& x = l.graph.edges[size_t(s.second)];
        const Vec2 a = l.graph.nodes[size_t(x.a)].p, b = l.graph.nodes[size_t(x.b)].p;
        issue("grade", "warning", "street:" + std::to_string(street),
              std::format("{} street {} climbs {:.1f}% over {:.1f} m, over its {:.1f}%", className(x.cls), street, s.first * 100.0,
                          glm::length(b - a) / 100.0, classMaxGrade(p, x.cls) * 100.0),
              at((a + b) * 0.5));
    }

    std::map<int, const Lot*> lotById;
    for (const Lot& lot : l.lots)
        lotById[lot.id] = &lot;
    for (const Envelope& env : l.envelopes)
    {
        auto it = lotById.find(env.lot);
        if (it == lotById.end())
        {
            issue("envelope", "error", "lot:" + std::to_string(env.lot), std::format("envelope on lot {}, which does not exist", env.lot), at(centroid(env.footprint)));
            continue;
        }
        const Polygon& poly = it->second->outline;
        std::vector<double> d(poly.size());
        for (size_t k = 0; k < poly.size(); ++k)
            d[k] = poly.tag[k] >= 0 ? env.frontSetback : env.sideSetback; // the setbacks its block type gave it
        const Polygon allowed = inset(poly, d);
        bool inside = !allowed.pts.empty();
        for (const Vec2& q : env.footprint.pts)
            inside = inside && (contains(allowed.pts, q) || boundaryDistance(allowed.pts, q) < 2.0);
        if (!inside)
            issue("envelope", "error", "lot:" + std::to_string(env.lot), std::format("envelope on lot {} crosses the lot's setbacks", env.lot),
                  at(centroid(env.footprint)));
        std::vector<Vec2> samples = env.footprint.pts;
        for (size_t k = 0; k < env.footprint.size(); ++k)
            samples.push_back((env.footprint.pts[k] + env.footprint.pts[(k + 1) % env.footprint.size()]) * 0.5);
        samples.push_back(centroid(env.footprint));
        double lowest = std::numeric_limits<double>::max();
        for (const Vec2& q : samples)
            lowest = std::min(lowest, ground.height(q));
        if (env.base > lowest + 1.0)
            issue("foundation", "error", "lot:" + std::to_string(env.lot),
                  std::format("foundation on lot {} stands {:.0f} cm above the ground", env.lot, env.base - lowest), at(centroid(env.footprint)));
    }

    // Lane graph per mode: vehicles on roads with driving lanes (one way runs from the edge's first node), people on every street.
    for (const char* mode : {"vehicles", "people"})
    {
        const bool people = std::string_view(mode) == "people";
        const int n = int(l.graph.nodes.size());
        std::vector<std::vector<int>> out(static_cast<size_t>(n));
        std::vector<char> used(size_t(n), 0);
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
        {
            if (!l.graph.isStreet(int(e)))
                continue;
            const StreetEdge& x = l.graph.edges[e];
            const RoadProfile& rp = edgeProfile(l, int(e));
            if (!people && (x.cls == StreetClass::Path || rp.lanes <= 0))
                continue;
            out[size_t(x.a)].push_back(x.b);
            if (people || !rp.oneWay)
                out[size_t(x.b)].push_back(x.a);
            used[size_t(x.a)] = used[size_t(x.b)] = 1;
        }
        int count = 0;
        const std::vector<int> comp = components(n, out, &count);
        std::map<int, int> sizes;
        for (int v = 0; v < n; ++v)
            if (used[size_t(v)])
                ++sizes[comp[size_t(v)]];
        if (sizes.size() > 1)
        {
            int biggest = 0;
            for (const auto& [c, s] : sizes)
                biggest = std::max(biggest, s);
            json parts = json::array();
            for (int v = 0; v < n; ++v)
                if (used[size_t(v)] && sizes[comp[size_t(v)]] < biggest && parts.size() < 8)
                    parts.push_back(at(l.graph.nodes[size_t(v)].p));
            issue("lanes", "error", std::string("mode:") + mode,
                  std::format("the {} lane graph splits into {} parts that cannot all reach each other", mode, sizes.size()), parts);
        }
    }

    const RoadNetwork net = roadNetwork(l, ground);
    for (size_t k = 0; k < net.junctions.size(); ++k)
    {
        const Junction& j = net.junctions[k];
        if (j.control != Control::Signal)
            continue;
        std::vector<int> seen(j.ends.size(), 0);
        bool conflict = false;
        for (const std::vector<int>& phase : j.phases)
            for (size_t a = 0; a < phase.size(); ++a)
            {
                ++seen[size_t(phase[a])];
                for (size_t b = a + 1; b < phase.size(); ++b)
                    conflict = conflict || -glm::dot(j.ends[size_t(phase[a])].out, j.ends[size_t(phase[b])].out) < 0.7;
            }
        for (size_t e = 0; e < j.ends.size(); ++e)
            conflict = conflict || (j.ends[e].vehicle && seen[e] != 1);
        if (conflict)
            issue("phases", "error", "junction:" + std::to_string(k), std::format("signal phases at junction {} let crossing approaches go together", k),
                  json::array({j.p.x, j.p.z}));
    }

    json c = json::object();
    for (const char* k : {"frontage", "overlap", "grade", "water", "envelope", "foundation", "lanes", "phases"})
        c[k] = counts[k];
    json warnings = json::array();
    const json lanes = streetscapeLaneChecks(l, ground);
    for (const json& i : lanes)
    {
        ++counts["laneGraph"];
        issues.push_back(i);
    }
    c["laneGraph"] = counts["laneGraph"];
    return {{"ok", issues.empty()}, {"counts", c}, {"issues", issues}, {"warnings", warnings}};
}

json explain(const Layout& l, const Ground& ground, const json& siting, const std::string& id)
{
    LILA_ZONE();

    const Profile& p = l.profile;
    std::vector<std::string> reasons;
    json out = {{"id", id}};
    auto slopeAt = [&](Vec2 q) { return ground.grade(q) * 100.0; };
    auto number = [&](std::string_view s) -> int64_t {
        try
        {
            return std::stoll(std::string(s));
        }
        catch (...)
        {
            throw std::runtime_error("'" + id + "' has no number after its kind");
        }
    };

    if (id == "site")
    {
        if (!siting.is_object())
            throw std::runtime_error("this settlement keeps no site record: it was not placed from settlementSuggest or settlementRegion");
        out["kind"] = "site";
        out["site"] = siting;
        const json terms = siting.value("reasons", json::object()).value("terms", json::object());
        for (auto& [term, t] : terms.items())
            if (!t.value("value", json()).is_null())
                reasons.push_back(std::format("{}: {} (value {:.2f}, weight {:.2f})", term, t.value("why", std::string()), t["value"].get<double>(),
                                              t.value("weight", 0.0)));
    }
    else if (id == "centre" || id == "center")
    {
        out["kind"] = "centre";
        out["position"] = json::array({l.center.x, ground.height(l.center), l.center.y});
        bool plaza = false;
        for (const Block& b : l.blocks)
            plaza = plaza || (b.use == Use::Plaza && contains(b.outline.pts, l.center));
        reasons.push_back(plaza ? "the market square: the block at the centre kept open" : "the point the main roads gather at");
        reasons.push_back(std::format("ground grade {:.1f}% here", slopeAt(l.center)));
        if (!l.water.empty())
            reasons.push_back(std::format("{:.0f} m from the nearest water", std::max(0.0, bankDistance(l.center, l.water)) / 100.0));
        if (siting.is_object())
        {
            reasons.push_back(std::format("site chosen as a {} with score {:.3f} (see settlementExplain id='site')", siting.value("kind", std::string("town")),
                                          siting.value("score", 0.0)));
            out["site"] = siting;
        }
    }
    else if (id.starts_with("lot:") || id.starts_with("landmark:"))
    {
        const int lotId = int(number(id.substr(id.find(':') + 1)));
        const Lot* lot = nullptr;
        for (const Lot& x : l.lots)
            if (x.id == lotId)
                lot = &x;
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        out["kind"] = id.starts_with("lot:") ? "lot" : "landmark";
        out["use"] = useName(lot->use);
        out["area"] = lot->area;
        out["frontages"] = lot->frontages;
        out["frontClass"] = className(lot->frontClass);
        const double dist = glm::length(centroid(lot->outline) - l.center) / std::max(l.radius, 1.0);
        switch (lot->use)
        {
        case Use::Plaza: reasons.push_back("the block at the centre, kept open as the market square"); break;
        case Use::Open:
            if (lot->frontages == 0)
                reasons.push_back("open ground: no street frontage");
            else if (lot->area < p.minLot)
                reasons.push_back(std::format("open ground: {:.0f} m2 is under the profile's minLot {:.0f} m2", lot->area, p.minLot));
            else
                reasons.push_back("open ground: too irregular to build on");
            break;
        case Use::Commercial:
            reasons.push_back(std::format("commercial: faces a {} street at {:.0f}% of the radius from the centre, inside the profile's {:.0f}%",
                                          className(lot->frontClass), dist * 100.0, p.commercial * 100.0));
            break;
        case Use::Civic: reasons.push_back("civic: the largest lot on a main street near the centre"); break;
        case Use::Residential:
            reasons.push_back(std::format("residential: faces a {} street at {:.0f}% of the radius from the centre", className(lot->frontClass), dist * 100.0));
            break;
        }
        for (const std::string& r : reasonsOf(*lot))
            reasons.push_back(r);
        for (const Envelope& e : l.envelopes)
            if (e.lot == lotId)
                reasons.push_back(std::format("envelope: {} floors, foundation {:.0f} cm below the lowest ground", e.floors, e.ground - e.base));
    }
    else if (id.starts_with("block:"))
    {
        const int blockId = int(number(id.substr(6)));
        const Block* block = nullptr;
        for (const Block& b : l.blocks)
            if (b.id == blockId)
                block = &b;
        if (!block)
            throw std::runtime_error("no block " + std::to_string(blockId));
        out["kind"] = "block";
        out["use"] = useName(block->use);
        out["area"] = block->area;
        int lots = 0;
        for (const Lot& lot : l.lots)
            lots += lot.block == blockId;
        out["lots"] = lots;
        reasons.push_back("a face of the street network, pulled in by half of each street's width");
        if (block->area > p.maxBlock)
            reasons.push_back(std::format("{:.0f} m2 is over maxBlock {:.0f} m2, where a lane would cut it if one fitted", block->area, p.maxBlock));
    }
    else
    {
        const uint64_t street = uint64_t(number(id.starts_with("street:") ? id.substr(7) : id));
        std::vector<int> edges;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
            if (l.graph.isStreet(int(e)) && l.graph.edges[e].street == street)
                edges.push_back(int(e));
        if (edges.empty())
            throw std::runtime_error("no street " + std::to_string(street) + " (settlementStreets lists them)");
        const StreetEdge& first = l.graph.edges[size_t(edges.front())];
        double length = 0.0, grade = 0.0;
        int bridges = 0;
        bool atCentre = false, atOutline = false;
        std::vector<Vec2> pts;
        for (int e : edges)
        {
            const StreetEdge& x = l.graph.edges[size_t(e)];
            const Vec2 a = l.graph.nodes[size_t(x.a)].p, b = l.graph.nodes[size_t(x.b)].p;
            length += glm::length(b - a);
            grade = std::max(grade, ground.gradeAlong(a, b));
            bridges += edgeIsBridge(l, e) && edgeInWater(l, e);
            for (Vec2 q : {a, b})
            {
                atCentre = atCentre || glm::length(q - l.center) < std::max(p.widthArterial * 3.0, 3000.0);
                atOutline = atOutline || boundaryDistance(l.outline.pts, q) < std::max(p.widthArterial, 500.0);
                pts.push_back(q);
            }
        }
        out["kind"] = "street";
        out["class"] = className(first.cls);
        out["length"] = length;
        out["maxGrade"] = grade * 100.0;
        out["gradeLimit"] = classMaxGrade(p, first.cls) * 100.0;
        if (first.path >= 0)
            reasons.push_back(std::format("laid for edit {} (a drawn street or route)", first.path));
        else
            switch (first.cls)
            {
            case StreetClass::Arterial:
                if (atCentre && atOutline)
                    reasons.push_back("main road from the centre out to a gate on the outline");
                else if (atOutline)
                    reasons.push_back("main road arriving from outside the settlement");
                else if (atCentre)
                    reasons.push_back("main road from the centre to an attractor");
                else
                    reasons.push_back("main road (ring or through road of the pattern)");
                break;
            case StreetClass::Collector:
            case StreetClass::Local:
                reasons.push_back(p.pattern == Pattern::Grid || p.pattern == Pattern::OrganicGrid
                                      ? std::format("grid street: blocks of {:.0f} x {:.0f} m at {:.0f} degrees", p.gridWidth / 100.0, p.gridDepth / 100.0, p.gridAngle)
                                      : std::format("grown off the main roads: {:.0f} m segments, turns up to {:.0f} degrees, {:.0f} m apart",
                                                    p.segment / 100.0, p.turn, p.spacing / 100.0));
                break;
            case StreetClass::Alley: reasons.push_back(std::format("lane cutting a block over maxBlock {:.0f} m2", p.maxBlock)); break;
            default: reasons.push_back("footpath"); break;
            }
        if (first.cls == StreetClass::Arterial && l.routeBook)
            for (const RouteBook::Entry& r : l.routeBook->entries)
            {
                int near = 0;
                for (const Vec2& q : pts)
                    for (size_t k = 0; k + 1 < r.pts.size() && near < int(pts.size()); ++k)
                    {
                        const Vec2 d = r.pts[k + 1] - r.pts[k];
                        const double t = std::clamp(glm::dot(q - r.pts[k], d) / std::max(glm::dot(d, d), 1e-9), 0.0, 1.0);
                        if (glm::length(r.pts[k] + d * t - q) < std::max(p.widthArterial, 300.0))
                        {
                            ++near;
                            break;
                        }
                    }
                if (near * 2 >= int(pts.size()))
                {
                    reasons.push_back(r.source == "terrain"
                                          ? std::format("routed over the terrain by terrainRoute: steepest {:.1f}% against {:.1f}%{}", r.gradeFound * 100.0,
                                                        r.maxGrade * 100.0, r.withinGrade ? "" : " (no way kept within grade; the gentlest was taken)")
                                          : "routed by Settlement's own router: " + r.note);
                    out["route"] = r.source;
                    break;
                }
            }
        if (bridges > 0)
            reasons.push_back(std::format("bridges water at {} place{}", bridges, bridges == 1 ? "" : "s"));
        reasons.push_back(std::format("steepest {:.1f}% against the {} limit of {:.1f}%", grade * 100.0, className(first.cls), classMaxGrade(p, first.cls) * 100.0));
    }
    out["reasons"] = reasons;
    return out;
}
} // namespace settlement
