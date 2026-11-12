// Settlement: measures and checks for agents, the layout as JSON, and meshes for the viewport and export.

#include "SettlementCore.h"
#include "Parcels.h"
#include "Streetscape.h"

#include <lila/Profile.h>

#include <algorithm>
#include <format>
#include <cmath>
#include <map>
#include <numeric>
#include <tuple>

namespace settlement
{
namespace
{
json xz(Vec2 p) { return json::array({p.x, p.y}); }

json ring(const Polygon& p)
{
    json a = json::array();
    for (const Vec2& q : p.pts)
        a.push_back(xz(q));
    return a;
}

const char* sideName(int tag)
{
    switch (tag)
    {
    case 0: return "street";
    case 1: return "party";
    default: return "open";
    }
}

int findRoot(std::vector<int>& parent, int x)
{
    while (parent[size_t(x)] != x)
        x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
    return x;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/** Writes per-face values once every face exists: adding layers or faces moves earlier spans. */
void setFaceInts(geo::Mesh& m, const std::vector<std::pair<const char*, std::vector<int32_t>>>& layers)
{
    for (const auto& [name, values] : layers)
        m.faceAttrs().add(name, geo::AttrType::Int);
    for (const auto& [name, values] : layers)
    {
        std::span<int32_t> out = m.faceAttrs().get<int32_t>(name);
        std::copy(values.begin(), values.begin() + std::min(values.size(), out.size()), out.begin());
    }
}
} // namespace

json metrics(const Layout& l)
{
    LILA_ZONE();

    const StreetGraph& g = l.graph;
    double bins[36] = {};
    double total = 0.0;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const Vec2 d = g.nodes[size_t(g.edges[e].b)].p - g.nodes[size_t(g.edges[e].a)].p;
        const double len = glm::length(d);
        double bearing = std::atan2(d.y, d.x) * 180.0 / kPi;
        for (double b : {bearing, bearing + 180.0})
        {
            const double w = std::fmod(b + 360.0, 360.0);
            bins[std::min(35, int(w / 10.0))] += len;
        }
        total += len;
    }
    // Boeing 2019: entropy over 36 bearings, from ln 4 (a perfect grid) to ln 36 (no order).
    double entropy = 0.0;
    for (double b : bins)
        if (b > 0.0)
        {
            const double p = b / (2.0 * total);
            entropy -= p * std::log(p);
        }
    const double hMax = std::log(36.0), hGrid = std::log(4.0);
    const double order = total > 0.0 ? std::clamp(1.0 - std::pow((entropy - hGrid) / (hMax - hGrid), 2.0), 0.0, 1.0) : 0.0;

    int counts[6] = {};
    int nodes = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        const int d = g.degree(int(n));
        if (d == 0)
            continue;
        ++nodes;
        ++counts[std::min(d, 5)];
    }
    const double km2 = area(l.outline) / kM2 / 1.0e6;
    const int intersections = counts[3] + counts[4] + counts[5];
    std::vector<double> blockAreas;
    for (const Block& b : l.blocks)
        blockAreas.push_back(b.area);
    return {{"streetLengthKm", total / 1.0e5},
            {"streetDensityKmPerKm2", km2 > 0.0 ? total / 1.0e5 / km2 : 0.0},
            {"orientationEntropy", entropy},
            {"orientationOrder", order},
            {"nodes", nodes},
            {"intersections", intersections},
            {"intersectionsPerKm2", km2 > 0.0 ? intersections / km2 : 0.0},
            {"deadEndRatio", nodes > 0 ? double(counts[1]) / nodes : 0.0},
            {"degree", {{"1", counts[1]}, {"2", counts[2]}, {"3", counts[3]}, {"4", counts[4]}, {"5+", counts[5]}}},
            {"meanDegree", nodes > 0 ? double(counts[1] + 2 * counts[2] + 3 * counts[3] + 4 * counts[4] + 5 * counts[5]) / nodes : 0.0},
            {"blocks", int64_t(l.blocks.size())},
            {"blockAreaMedianM2", median(blockAreas)},
            {"blockAreaMeanM2", blockAreas.empty() ? 0.0 : std::accumulate(blockAreas.begin(), blockAreas.end(), 0.0) / double(blockAreas.size())}};
}

json checks(const Layout& l, const Ground& ground, const Options& o)
{
    LILA_ZONE();

    const StreetGraph& g = l.graph;
    const Profile& p = l.profile;
    std::vector<int> parent(g.nodes.size());
    std::iota(parent.begin(), parent.end(), 0);
    int steep = 0, crossings = 0;
    json crossingAt = json::array();
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const StreetEdge& x = g.edges[e];
        parent[size_t(findRoot(parent, x.a))] = findRoot(parent, x.b);
        const Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
        if (ground.gradeAlong(a, b) > p.maxGrade * 1.5 + 1e-9)
            ++steep;
        Vec2 hit{0.0};
        if (g.firstCrossing(a, b, &hit, nullptr, x.a, x.b) >= 0)
        {
            if (++crossings <= 8)
                crossingAt.push_back(xz(hit));
        }
    }
    std::map<int, int> roots;
    for (size_t n = 0; n < g.nodes.size(); ++n)
        if (g.degree(int(n)) > 0)
            ++roots[findRoot(parent, int(n))];

    int sharp = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        std::vector<double> angles;
        for (int e : g.incident(int(n)))
        {
            if (!g.isStreet(e))
                continue;
            const StreetEdge& x = g.edges[size_t(e)];
            const Vec2 d = g.nodes[size_t(x.a == int(n) ? x.b : x.a)].p - g.nodes[n].p;
            angles.push_back(std::atan2(d.y, d.x));
        }
        if (angles.size() < 2)
            continue;
        std::sort(angles.begin(), angles.end());
        for (size_t k = 0; k < angles.size(); ++k)
        {
            double gap = (k + 1 < angles.size() ? angles[k + 1] : angles[0] + 2.0 * kPi) - angles[k];
            if (gap * 180.0 / kPi < p.minAngle * 0.5)
            {
                ++sharp;
                break;
            }
        }
    }
    int landlocked = 0, emptyEnvelopes = 0;
    for (const Lot& lot : l.lots)
        if (lot.frontages == 0 && lot.use != Use::Plaza)
            ++landlocked;
    for (const Envelope& env : l.envelopes)
        if (env.floors <= 0 || env.footprint.pts.size() < 3)
            ++emptyEnvelopes;
    // Anything inside an exclusion is a fault: streets through it, lots or buildings on it.
    auto inside = [&](Vec2 q) {
        for (const Polygon& x : l.exclusions)
            if (contains(x.pts, q) && boundaryDistance(x.pts, q) > 1.0)
                return true;
        return false;
    };
    int inExclusions = 0;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)) && inside((g.nodes[size_t(g.edges[e].a)].p + g.nodes[size_t(g.edges[e].b)].p) * 0.5))
            ++inExclusions;
    for (const Lot& lot : l.lots)
        if (std::ranges::any_of(lot.outline.pts, inside))
            ++inExclusions;
    json failed = json::array();
    for (const PathResult& r : l.pathResults)
        if (!r.ok)
            failed.push_back({{"id", r.id}, {"error", r.error}});
    const int components = int(roots.size());
    return {{"components", components},
            {"crossings", crossings},
            {"crossingAt", crossingAt},
            {"steepStreets", steep},
            {"sharpJunctions", sharp},
            {"landlockedLots", landlocked},
            {"badEnvelopes", emptyEnvelopes},
            {"inExclusions", inExclusions},
            {"failedPaths", failed},
            {"staleOverrides", l.staleOverrides},
            {"warnings", l.streetWarnings},
            {"ok", components <= 1 && crossings == 0 && emptyEnvelopes == 0 && inExclusions == 0 && failed.empty() &&
                       (!o.blocks || !l.lots.empty())}};
}

json stats(const Layout& l)
{
    const StreetGraph& g = l.graph;
    json byClass = json::object();
    int streets = 0;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)))
        {
            ++streets;
            const char* c = className(g.edges[e].cls);
            byClass[c] = byClass.value(c, 0.0) + g.length(int(e)) / 100.0;
        }
    json uses = json::object();
    for (const Lot& lot : l.lots)
        uses[useName(lot.use)] = uses.value(useName(lot.use), 0) + 1;
    int minF = 0, maxF = 0;
    double sumF = 0.0;
    for (const Envelope& env : l.envelopes)
    {
        minF = minF == 0 ? env.floors : std::min(minF, env.floors);
        maxF = std::max(maxF, env.floors);
        sumF += env.floors;
    }
    return {{"profile", l.profile.name},
            {"pattern", patternName(l.profile.pattern)},
            {"areaM2", area(l.outline) / kM2},
            {"radiusM", l.radius / 100.0},
            {"center", xz(l.center)},
            {"streetSegments", streets},
            {"streetMetresByClass", byClass},
            {"blocks", int64_t(l.blocks.size())},
            {"lots", int64_t(l.lots.size())},
            {"lotUses", uses},
            {"buildings", int64_t(l.envelopes.size())},
            {"floors", {{"min", minF}, {"max", maxF}, {"mean", l.envelopes.empty() ? 0.0 : sumF / double(l.envelopes.size())}}},
            {"refusedStreets", l.refused},
            {"bridges", int64_t(l.bridges.size())},
            {"exclusions", int64_t(l.exclusions.size())},
            {"surfacePaths", int64_t(l.surfacePaths.size())},
            {"paths", pathResults(l)}};
}

const char* useName(Use u)
{
    switch (u)
    {
    case Use::Residential: return "residential";
    case Use::Commercial: return "commercial";
    case Use::Civic: return "civic";
    case Use::Plaza: return "plaza";
    case Use::Open: return "open";
    case Use::Market: return "market";
    case Use::Religious: return "religious";
    case Use::Industrial: return "industrial";
    case Use::Port: return "port";
    case Use::Farm: return "farm";
    case Use::Park: return "park";
    case Use::Cemetery: return "cemetery";
    }
    return "open";
}

json toJson(const Layout& l)
{
    LILA_ZONE();

    const StreetGraph& g = l.graph;
    // Nodes renumbered over the ones streets use, so the file holds no dead entries.
    std::vector<int> remap(g.nodes.size(), -1);
    json nodes = json::array();
    json streets = json::array();
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const StreetEdge& x = g.edges[e];
        for (int n : {x.a, x.b})
            if (remap[size_t(n)] < 0)
            {
                remap[size_t(n)] = int(nodes.size());
                nodes.push_back(xz(g.nodes[size_t(n)].p));
            }
        streets.push_back({{"a", remap[size_t(x.a)]},
                           {"b", remap[size_t(x.b)]},
                           {"class", className(x.cls)},
                           {"width", edgeWidth(l, int(e))},
                           {"profile", edgeProfile(l, int(e)).name},
                           {"path", x.path},
                           {"street", x.street},
                           {"era", x.era},
                           {"name", streetName(l, x.street)}});
    }
    json blocks = json::array();
    for (const Block& b : l.blocks)
    {
        blocks.push_back({{"id", b.id}, {"use", useName(b.use)}, {"areaM2", b.area}, {"outline", ring(b.outline)}});
        blocks.back().update(blockJson(b));
    }
    json lots = json::array();
    for (const Lot& lot : l.lots)
    {
        json sides = json::array();
        for (int t : lot.outline.tag)
            sides.push_back(t >= 0 ? json(className(StreetClass(t))) : json());
        lots.push_back({{"id", lot.id},
                        {"block", lot.block},
                        {"use", useName(lot.use)},
                        {"areaM2", lot.area},
                        {"frontages", lot.frontages},
                        {"corner", lot.corner},
                        {"frontClass", className(lot.frontClass)},
                        {"outline", ring(lot.outline)},
                        {"sideStreets", sides},
                        {"random", lot.random}});
        lots.back().update(lotExtras(lot));
    }
    json envelopes = json::array();
    for (const Envelope& env : l.envelopes)
    {
        json sides = json::array();
        for (int t : env.footprint.tag)
            sides.push_back(sideName(t));
        envelopes.push_back({{"lot", env.lot},
                             {"use", useName(env.use)},
                             {"footprint", ring(env.footprint)},
                             {"sides", sides},
                             {"floors", env.floors},
                             {"floorHeight", l.profile.floorHeight},
                             {"height", env.height},
                             {"base", env.base},
                             {"ground", env.ground},
                             {"top", env.top},
                             {"roof", env.roof},
                             {"front", xz(env.front)},
                             {"random", env.random}});
        envelopes.back().update(envelopeExtras(env));
    }
    json water = json::array();
    for (const WaterLine& w : l.water)
    {
        json pts = json::array();
        for (const Vec2& q : w.pts)
            pts.push_back(xz(q));
        water.push_back({{"width", w.width}, {"points", pts}});
    }
    json bridges = json::array();
    for (const Vec2& b : l.bridges)
        bridges.push_back(xz(b));
    json exclusions = json::array();
    for (size_t k = 0; k < l.exclusions.size(); ++k)
        exclusions.push_back({{"id", k < l.exclusionIds.size() ? l.exclusionIds[k] : -1}, {"outline", ring(l.exclusions[k])}});
    json surface = json::array();
    for (const SurfacePath& s : l.surfacePaths)
    {
        json pts = json::array();
        for (const glm::dvec3& q : s.pts)
            pts.push_back(json::array({q.x, q.y, q.z}));
        surface.push_back({{"path", s.path}, {"class", className(s.cls)}, {"profile", l.roads[size_t(s.profile)].name}, {"width", s.width}, {"points", pts}});
    }
    json roads = json::array();
    for (const RoadProfile& r : l.roads)
        roads.push_back(toJson(r));
    json districts = json::array();
    for (const District& d : l.districts)
        districts.push_back({{"id", d.id}, {"name", d.name}, {"areaM2", d.area}, {"center", xz(d.center)}, {"outline", ring(d.outline)}});
    return {{"format", "lila.settlement"},
            {"version", 1},
            {"units", "cm"},
            {"up", "y"},
            {"note", "2D points are [x, z]; heights are world Y."},
            {"profile", toJson(l.profile)},
            {"center", xz(l.center)},
            {"outline", ring(l.outline)},
            {"nodes", nodes},
            {"streets", streets},
            {"blocks", blocks},
            {"lots", lots},
            {"envelopes", envelopes},
            {"water", water},
            {"bridges", bridges},
            {"exclusions", exclusions},
            {"surfacePaths", surface},
            {"roadProfiles", roads},
            {"culture", l.culture},
            {"districts", districts}};
}

MeshParts toMeshes(const Layout& l, const Ground& ground, const OutputOptions& o)
{
    LILA_ZONE();

    MeshParts out;
    const StreetGraph& g = l.graph;
    // Draped no coarser than the ground's own grid, on its exact surface, so ribbons follow it at any scale.
    const double seg = std::max(std::min(o.segment, ground.isPlane() ? o.segment : std::max(ground.cellSize(), 50.0)), 10.0);
    auto at = [&](Vec2 p, double lift) { return glm::dvec3(p.x, ground.surfaceHeight(p) + lift, p.y); };

    // A ribbon per edge, wound so its normal faces up (clockwise in XZ seen as x, z).
    std::vector<int32_t> streetId, streetClass, streetPath, waterId;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        const StreetEdge& x = g.edges[e];
        const bool water = x.alive && x.cls == StreetClass::Water;
        if (!g.isStreet(int(e)) && !water)
            continue;
        geo::Mesh& m = water ? out.water : out.streets;
        const Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
        const double len = glm::length(b - a);
        if (len < 1e-6)
            continue;
        const Vec2 d = (b - a) / len, n(-d.y, d.x);
        const double half = 0.5 * edgeWidth(l, int(e));
        const double lift = water ? o.lift * 0.5 : o.lift;
        const int steps = std::max(1, int(std::ceil(len / seg)));
        geo::Index prevL = 0, prevR = 0;
        for (int s = 0; s <= steps; ++s)
        {
            const Vec2 c = a + (b - a) * (double(s) / steps);
            const geo::Index L = m.addPoint(at(c + n * half, lift));
            const geo::Index R = m.addPoint(at(c - n * half, lift));
            if (s > 0)
            {
                m.addFace({prevL, L, R, prevR});
                if (water)
                    waterId.push_back(int32_t(e));
                else
                {
                    streetId.push_back(int32_t(e));
                    streetClass.push_back(int32_t(x.cls));
                    streetPath.push_back(int32_t(x.path));
                }
            }
            prevL = L;
            prevR = R;
        }
    }
    // Surface paths: ribbons on the walkable surface, level with each point, -1 for the street id.
    for (const SurfacePath& s : l.surfacePaths)
    {
        const double half = 0.5 * s.width;
        geo::Index prevL = 0, prevR = 0;
        for (size_t k = 0; k < s.pts.size(); ++k)
        {
            const glm::dvec3 prev = s.pts[k > 0 ? k - 1 : k], next = s.pts[k + 1 < s.pts.size() ? k + 1 : k];
            glm::dvec2 d(next.x - prev.x, next.z - prev.z);
            d = glm::length(d) > 1e-9 ? d / glm::length(d) : glm::dvec2(1.0, 0.0);
            const glm::dvec3 side(-d.y * half, 0.0, d.x * half), up(0.0, o.lift, 0.0);
            const geo::Index L = out.streets.addPoint(s.pts[k] + side + up);
            const geo::Index R = out.streets.addPoint(s.pts[k] - side + up);
            if (k > 0)
            {
                out.streets.addFace({prevL, L, R, prevR});
                streetId.push_back(-1);
                streetClass.push_back(int32_t(s.cls));
                streetPath.push_back(int32_t(s.path));
            }
            prevL = L;
            prevR = R;
        }
    }
    setFaceInts(out.streets, {{"street", streetId}, {"class", streetClass}, {"path", streetPath}});
    setFaceInts(out.water, {{"water", waterId}});

    // Lots: outlines as loose edges draped on the ground.
    std::vector<int32_t> edgeLot;
    for (const Lot& lot : l.lots)
    {
        const size_t n = lot.outline.size();
        std::vector<geo::Index> ids;
        for (size_t k = 0; k < n; ++k)
        {
            const Vec2 a = lot.outline.pts[k], b = lot.outline.pts[(k + 1) % n];
            const int steps = std::max(1, int(std::ceil(glm::length(b - a) / seg)));
            for (int s = 0; s < steps; ++s)
                ids.push_back(out.lots.addPoint(at(a + (b - a) * (double(s) / steps), o.lift * 1.5)));
        }
        for (size_t k = 0; k < ids.size(); ++k)
        {
            out.lots.addEdge(ids[k], ids[(k + 1) % ids.size()]);
            edgeLot.push_back(lot.id);
        }
    }
    out.lots.edgeAttrs().add("lot", geo::AttrType::Int);
    {
        std::span<int32_t> v = out.lots.edgeAttrs().get<int32_t>("lot");
        std::copy(edgeLot.begin(), edgeLot.begin() + std::min(edgeLot.size(), v.size()), v.begin());
    }

    // Envelopes: closed prisms from the foundation to the top, walls facing out, roof up, floor down.
    std::vector<int32_t> envLot, envUse, envFloors, envPart;
    std::vector<int> flatRoofs;
    for (const Envelope& env : l.envelopes)
    {
        if (env.route == BuildingRoute::Massing)
        {
            if (!appendMassing(env, l, out.envelopes, [&](int part) {
                    envLot.push_back(env.lot);
                    envUse.push_back(int32_t(env.use));
                    envFloors.push_back(env.floors);
                    envPart.push_back(part);
                }))
                flatRoofs.push_back(env.lot);
            continue;
        }
        const size_t n = env.footprint.size();
        std::vector<geo::Index> lo, hi;
        for (const Vec2& q : env.footprint.pts)
        {
            lo.push_back(out.envelopes.addPoint({q.x, env.base, q.y}));
            hi.push_back(out.envelopes.addPoint({q.x, env.top, q.y}));
        }
        auto tagFace = [&](int part) {
            envLot.push_back(env.lot);
            envUse.push_back(int32_t(env.use));
            envFloors.push_back(env.floors);
            envPart.push_back(part);
        };
        for (size_t k = 0; k < n; ++k)
        {
            const size_t j = (k + 1) % n;
            out.envelopes.addFace({lo[k], hi[k], hi[j], lo[j]});
            tagFace(0);
        }
        std::vector<geo::Index> top(hi.rbegin(), hi.rend());
        out.envelopes.addFace(top);
        tagFace(1);
        out.envelopes.addFace(lo);
        tagFace(2);
    }
    setFaceInts(out.envelopes, {{"lot", envLot}, {"use", envUse}, {"floors", envFloors}, {"part", envPart}});
    if (!flatRoofs.empty())
    {
        std::string lots;
        for (size_t k = 0; k < flatRoofs.size() && k < 20; ++k)
            lots += (k ? ", " : "") + std::to_string(flatRoofs[k]);
        out.warnings.push_back(std::format("{} massing roofs are flat caps: the straight skeleton found no roof on their footprints (lots {}{})", flatRoofs.size(), lots, flatRoofs.size() > 20 ? ", ..." : ""));
    }
    return out;
}

std::vector<StreetLine> streetLines(const Layout& l, const Ground& ground)
{
    const StreetGraph& g = l.graph;
    std::vector<char> used(g.edges.size(), 0);
    std::vector<StreetLine> lines;
    auto same = [&](int a, int b) {
        const StreetEdge &x = g.edges[size_t(a)], &y = g.edges[size_t(b)];
        return x.cls == y.cls && x.profile == y.profile && x.path == y.path && edgeWidth(l, a) == edgeWidth(l, b);
    };
    auto through = [&](int node, int e) {
        // The edge that carries a street straight on through a plain node: the same kind, the only other one.
        if (g.degree(node) != 2)
            return -1;
        for (int x : g.incident(node))
            if (x != e && g.isStreet(x) && same(x, e))
                return x;
        return -1;
    };
    auto walk = [&](int start, int e) {
        StreetLine line;
        const StreetEdge& first = g.edges[size_t(e)];
        line.cls = first.cls;
        line.profile = first.profile >= 0 ? first.profile : int(std::min(first.cls, StreetClass::Path));
        line.width = edgeWidth(l, e);
        line.path = first.path;
        line.street = first.street;
        line.startNode = start;
        int node = start;
        line.pts.push_back({g.nodes[size_t(node)].p.x, ground.surfaceHeight(g.nodes[size_t(node)].p), g.nodes[size_t(node)].p.y});
        while (e >= 0 && !used[size_t(e)])
        {
            used[size_t(e)] = 1;
            const StreetEdge& x = g.edges[size_t(e)];
            node = x.a == node ? x.b : x.a;
            const Vec2 p = g.nodes[size_t(node)].p;
            line.pts.push_back({p.x, ground.surfaceHeight(p), p.y});
            e = through(node, e);
        }
        line.endNode = node;
        if (line.pts.size() >= 2)
            lines.push_back(std::move(line));
    };
    for (size_t n = 0; n < g.nodes.size(); ++n)
        for (int e : g.incident(int(n)))
            if (g.isStreet(e) && !used[size_t(e)] && through(int(n), e) < 0)
                walk(int(n), e);
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)) && !used[e])
            walk(g.edges[e].a, int(e));
    for (const SurfacePath& s : l.surfacePaths)
    {
        StreetLine line;
        line.pts = s.pts;
        line.cls = s.cls;
        line.profile = s.profile;
        line.width = s.width;
        line.path = s.path;
        line.surface = true;
        lines.push_back(std::move(line));
    }
    return lines;
}

json pathResults(const Layout& l)
{
    json out = json::array();
    for (const PathResult& r : l.pathResults)
    {
        json j = {{"id", r.id}, {"ok", r.ok}, {"lengthM", r.length / 100.0}, {"surfaceLengthM", r.surfaceLength / 100.0}};
        if (!r.ok)
            j["error"] = r.error;
        out.push_back(j);
    }
    return out;
}

const char* controlName(Control c)
{
    switch (c)
    {
    case Control::None: return "none";
    case Control::Yield: return "yield";
    case Control::Stop: return "stop";
    case Control::Signal: return "signal";
    }
    return "none";
}

namespace
{
/**
 * How a junction is controlled, its signal phases and crosswalks: lights where an arterial meets another vehicle road or
 * two collectors cross, stop lines where four or more meet, yield otherwise. Opposite approaches share a phase.
 */
void controlJunction(Junction& j, const RoadNetwork& net)
{
    int vehicles = 0, arterials = 0, collectors = 0;
    for (const RoadEnd& e : j.ends)
    {
        if (!e.vehicle)
            continue;
        ++vehicles;
        const StreetClass c = net.lines[size_t(e.line)].cls;
        arterials += c == StreetClass::Arterial;
        collectors += c == StreetClass::Collector;
    }
    if (vehicles < 2)
        j.control = Control::None;
    else if (vehicles >= 3 && (arterials >= 1 && arterials + collectors >= 2))
        j.control = Control::Signal;
    else if (vehicles >= 4 || collectors >= 2)
        j.control = Control::Stop;
    else
        j.control = Control::Yield;
    if (j.control == Control::Signal)
    {
        // Approaches pair with the one most nearly opposite, the straightest pairs first, so a road that runs on
        // through the junction is never paired across a side road.
        std::vector<std::tuple<double, int, int>> pairs;
        for (size_t a = 0; a < j.ends.size(); ++a)
            for (size_t b = a + 1; b < j.ends.size(); ++b)
                if (j.ends[a].vehicle && j.ends[b].vehicle)
                    if (const double d = -glm::dot(j.ends[a].out, j.ends[b].out); d > 0.7)
                        pairs.emplace_back(-d, int(a), int(b));
        std::sort(pairs.begin(), pairs.end());
        std::vector<int> partner(j.ends.size(), -1);
        for (const auto& [d, a, b] : pairs)
            if (partner[size_t(a)] < 0 && partner[size_t(b)] < 0)
            {
                partner[size_t(a)] = b;
                partner[size_t(b)] = a;
            }
        for (size_t a = 0; a < j.ends.size(); ++a)
        {
            if (!j.ends[a].vehicle || (partner[a] >= 0 && partner[a] < int(a)))
                continue;
            std::vector<int> phase = {int(a)};
            if (partner[a] >= 0)
                phase.push_back(partner[a]);
            j.phases.push_back(std::move(phase));
        }
    }
    if (j.control == Control::Signal || j.control == Control::Stop)
        for (size_t k = 0; k < j.ends.size(); ++k)
        {
            const RoadEnd& e = j.ends[k];
            if (!e.vehicle)
                continue;
            const double half = 0.5 * net.lines[size_t(e.line)].width;
            const glm::dvec3 side = glm::normalize(glm::dvec3(-e.out.z, 0.0, e.out.x)) * half;
            j.crosswalks.push_back({int(k), e.p - side, e.p + side});
        }
}

/** Parking spaces along both kerbs of roads whose profile has a parking lane. */
void parkAlong(RoadNetwork& net, const Layout& l)
{
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& s = net.lines[k];
        const RoadProfile& p = l.roads[size_t(s.profile)];
        if (p.parking <= 0.0 || s.surface)
            continue;
        const double offset = 0.5 * (p.lanes * p.laneWidth + p.median) + 0.5 * p.parking;
        const double bay = 600.0;
        for (size_t i = 0; i + 1 < s.pts.size(); ++i)
        {
            const glm::dvec3 a = s.pts[i], b = s.pts[i + 1];
            const double len = glm::length(b - a);
            if (len < bay)
                continue;
            const glm::dvec3 dir = (b - a) / len;
            const glm::dvec3 side = glm::normalize(glm::dvec3(-dir.z, 0.0, dir.x));
            const int n = int(len / bay);
            const double start = 0.5 * (len - n * bay);
            for (int m = 0; m < n; ++m)
                for (double sign : {-1.0, 1.0})
                    net.parking.push_back({a + dir * (start + (m + 0.5) * bay) + side * (sign * offset), dir * sign, bay, p.parking, int(k)});
        }
    }
}
} // namespace

RoadNetwork roadNetwork(const Layout& l, const Ground& ground)
{
    LILA_ZONE();

    RoadNetwork net;
    net.lines = streetLines(l, ground);
    std::map<int, size_t> at; // graph node -> junction
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& line = net.lines[k];
        for (const int node : {line.startNode, line.endNode})
            if (node >= 0 && l.graph.degree(node) >= 3 && !at.contains(node))
            {
                at[node] = net.junctions.size();
                Junction j;
                const Vec2 p = l.graph.nodes[size_t(node)].p;
                j.p = {p.x, ground.surfaceHeight(p), p.y};
                // Wide enough for the widest road meeting there, and never past half of any road's length.
                for (int e : l.graph.incident(node))
                    if (l.graph.isStreet(e))
                        j.radius = std::max(j.radius, 0.5 * edgeWidth(l, e) + 100.0);
                net.junctions.push_back(j);
            }
    }
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        StreetLine& line = net.lines[k];
        for (int end = 0; end < 2; ++end)
        {
            const int node = end == 0 ? line.startNode : line.endNode;
            auto it = at.find(node);
            if (it == at.end())
                continue;
            Junction& j = net.junctions[it->second];
            double length = 0.0;
            for (size_t i = 0; i + 1 < line.pts.size(); ++i)
                length += glm::length(line.pts[i + 1] - line.pts[i]);
            const double cut = std::min(j.radius, 0.45 * length);
            // Walk in from this end by cut, dropping the points passed.
            std::vector<glm::dvec3>& pts = line.pts;
            if (end == 1)
                std::reverse(pts.begin(), pts.end());
            double left = cut;
            while (pts.size() >= 2 && glm::length(pts[1] - pts[0]) <= left)
            {
                left -= glm::length(pts[1] - pts[0]);
                pts.erase(pts.begin());
            }
            if (pts.size() >= 2)
                pts[0] += glm::normalize(pts[1] - pts[0]) * left;
            const glm::dvec3 out = pts.size() >= 2 ? glm::normalize(pts[1] - pts[0]) : glm::dvec3(1, 0, 0);
            j.ends.push_back({int(k), end == 0, pts[0], out, l.roads[size_t(line.profile)].lanes > 0 && line.cls != StreetClass::Path});
            if (end == 1)
                std::reverse(pts.begin(), pts.end());
        }
    }
    for (Junction& j : net.junctions)
        controlJunction(j, net);
    finishSignals(net, l);
    parkAlong(net, l);
    return net;
}
} // namespace settlement
