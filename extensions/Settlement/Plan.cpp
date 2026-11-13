// Settlement: main roads booked from the terrain router, the pinned plan kept in the edits, and re-planning only the
// regions of ground that changed.

#include "Parcels.h"
#include "SettlementTerrain.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace settlement
{
namespace
{
uint64_t fnv(std::string_view s, uint64_t h = 1469598103934665603ull)
{
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

json planXz(Vec2 p) { return json::array({p.x, p.y}); }

Vec2 vec2Of(const json& j)
{
    if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number())
        throw std::runtime_error("plan points are [x, z]");
    return {j[0].get<double>(), j[1].get<double>()};
}

json polyJson(const Polygon& p)
{
    json a = json::array();
    for (size_t k = 0; k < p.pts.size(); ++k)
        a.push_back(json::array({p.pts[k].x, p.pts[k].y, k < p.tag.size() ? p.tag[k] : -1}));
    return a;
}

Polygon polyOf(const json& j)
{
    Polygon p;
    for (const json& q : j)
    {
        p.pts.push_back(vec2Of(q));
        p.tag.push_back(q.size() > 2 ? q[2].get<int>() : -1);
    }
    return p;
}

json polysJson(const std::vector<Polygon>& ps)
{
    json a = json::array();
    for (const Polygon& p : ps)
        a.push_back(polyJson(p));
    return a;
}

std::vector<Polygon> polysOf(const json& j)
{
    std::vector<Polygon> ps;
    for (const json& p : j)
        ps.push_back(polyOf(p));
    return ps;
}

json vec2sJson(const std::vector<Vec2>& ps)
{
    json a = json::array();
    for (Vec2 p : ps)
        a.push_back(planXz(p));
    return a;
}

std::vector<Vec2> vec2sOf(const json& j)
{
    std::vector<Vec2> ps;
    for (const json& p : j)
        ps.push_back(vec2Of(p));
    return ps;
}

// Every field round-trips: a pinned plan replaces what build() made, so anything left out here is lost from the layout.
json planBlockJson(const Block& b)
{
    return {{"id", b.id},
            {"outline", polyJson(b.outline)},
            {"area", b.area},
            {"use", int(b.use)},
            {"key", b.key},
            {"type", int(b.type)},
            {"method", int(b.method)},
            {"courtyards", polysJson(b.courtyards)},
            {"water", b.water},
            {"reasons", b.reasons},
            {"era", b.era}};
}

Block blockOf(const json& j)
{
    Block b;
    b.id = j.at("id").get<int>();
    b.outline = polyOf(j.at("outline"));
    b.area = j.value("area", 0.0);
    b.use = Use(j.value("use", 0));
    b.key = j.value("key", uint64_t(0));
    b.type = BlockType(j.value("type", 0));
    b.method = LotMethod(j.value("method", 0));
    b.courtyards = polysOf(j.value("courtyards", json::array()));
    b.water = j.value("water", false);
    b.reasons = j.value("reasons", std::vector<std::string>());
    b.era = j.value("era", 0);
    return b;
}

json lotJson(const Lot& l)
{
    return {{"id", l.id},
            {"block", l.block},
            {"outline", polyJson(l.outline)},
            {"area", l.area},
            {"frontages", l.frontages},
            {"corner", l.corner},
            {"frontClass", int(l.frontClass)},
            {"use", int(l.use)},
            {"random", l.random},
            {"key", l.key},
            {"wealth", l.wealth},
            {"access", l.access},
            {"landmark", l.landmark},
            {"reasons", l.reasons},
            {"route", int(l.route)},
            {"era", l.era}};
}

Lot lotOf(const json& j)
{
    Lot l;
    l.id = j.at("id").get<int>();
    l.block = j.value("block", 0);
    l.outline = polyOf(j.at("outline"));
    l.area = j.value("area", 0.0);
    l.frontages = j.value("frontages", 0);
    l.corner = j.value("corner", false);
    l.frontClass = StreetClass(j.value("frontClass", int(StreetClass::Local)));
    l.use = Use(j.value("use", 0));
    l.random = j.value("random", 0.0f);
    l.key = j.value("key", uint64_t(0));
    l.wealth = j.value("wealth", 1);
    l.access = j.value("access", 0.0);
    l.landmark = j.value("landmark", std::string());
    l.reasons = j.value("reasons", std::vector<std::string>());
    l.route = BuildingRoute(j.value("route", 0));
    l.era = j.value("era", 0);
    return l;
}

json envelopeJson(const Envelope& e)
{
    json tiers = json::array();
    for (const Envelope::Tier& t : e.tiers)
        tiers.push_back({{"footprint", polyJson(t.footprint)}, {"bottom", t.bottom}, {"top", t.top}});
    return {{"lot", e.lot},
            {"footprint", polyJson(e.footprint)},
            {"base", e.base},
            {"ground", e.ground},
            {"top", e.top},
            {"height", e.height},
            {"floors", e.floors},
            {"use", int(e.use)},
            {"roof", e.roof},
            {"front", planXz(e.front)},
            {"random", e.random},
            {"key", e.key},
            {"tiers", tiers},
            {"partyWith", e.partyWith},
            {"frontSetback", e.frontSetback},
            {"sideSetback", e.sideSetback},
            {"ridge", planXz(e.ridge)},
            {"roofHeight", e.roofHeight},
            {"entrances", vec2sJson(e.entrances)},
            {"groundUse", int(e.groundUse)},
            {"corner", e.corner},
            {"foundationDepth", e.foundationDepth},
            {"foundationSource", e.foundationSource},
            {"landmark", e.landmark},
            {"eraName", e.eraName},
            {"era", e.era},
            {"wealth", e.wealth},
            {"route", int(e.route)}};
}

Envelope envelopeOf(const json& j)
{
    Envelope e;
    e.lot = j.at("lot").get<int>();
    e.footprint = polyOf(j.at("footprint"));
    e.base = j.value("base", 0.0);
    e.ground = j.value("ground", 0.0);
    e.top = j.value("top", 0.0);
    e.height = j.value("height", 0.0);
    e.floors = j.value("floors", 0);
    e.use = Use(j.value("use", 0));
    e.roof = j.value("roof", std::string());
    e.front = vec2Of(j.value("front", json::array({0.0, 1.0})));
    e.random = j.value("random", 0.0f);
    e.key = j.value("key", uint64_t(0));
    for (const json& t : j.value("tiers", json::array()))
        e.tiers.push_back({polyOf(t.at("footprint")), t.value("bottom", 0.0), t.value("top", 0.0)});
    e.partyWith = j.value("partyWith", std::vector<int>());
    e.frontSetback = j.value("frontSetback", 0.0);
    e.sideSetback = j.value("sideSetback", 0.0);
    e.ridge = vec2Of(j.value("ridge", json::array({1.0, 0.0})));
    e.roofHeight = j.value("roofHeight", 0.0);
    e.entrances = vec2sOf(j.value("entrances", json::array()));
    e.groundUse = Use(j.value("groundUse", 0));
    e.corner = j.value("corner", std::string("none"));
    e.foundationDepth = j.value("foundationDepth", 0.0);
    e.foundationSource = j.value("foundationSource", std::string());
    e.landmark = j.value("landmark", std::string());
    e.eraName = j.value("eraName", std::string());
    e.era = j.value("era", 0);
    e.wealth = j.value("wealth", 1);
    e.route = BuildingRoute(j.value("route", 0));
    return e;
}

/** Liang-Barsky: does the segment a-b reach into the rectangle? */
bool segmentTouches(Vec2 a, Vec2 b, const DirtyRect& r)
{
    double t0 = 0.0, t1 = 1.0;
    const Vec2 d = b - a;
    const double p[4] = {-d.x, d.x, -d.y, d.y};
    const double q[4] = {a.x - r.lo.x, r.hi.x - a.x, a.y - r.lo.y, r.hi.y - a.y};
    for (int k = 0; k < 4; ++k)
    {
        if (std::abs(p[k]) < 1e-12)
        {
            if (q[k] < 0.0)
                return false;
            continue;
        }
        const double t = q[k] / p[k];
        if (p[k] < 0.0)
            t0 = std::max(t0, t);
        else
            t1 = std::min(t1, t);
        if (t0 > t1)
            return false;
    }
    return true;
}

bool edgeTouches(const StreetGraph& g, const StreetEdge& e, std::span<const DirtyRect> dirty)
{
    for (const DirtyRect& r : dirty)
        if (segmentTouches(g.nodes[size_t(e.a)].p, g.nodes[size_t(e.b)].p, r))
            return true;
    return false;
}

bool touchesAny(const Polygon& p, std::span<const DirtyRect> dirty)
{
    for (const DirtyRect& r : dirty)
        if (touches(p, r))
            return true;
    return false;
}

void bounds(const Polygon& p, Vec2* lo, Vec2* hi)
{
    *lo = Vec2(std::numeric_limits<double>::max());
    *hi = Vec2(-std::numeric_limits<double>::max());
    for (const Vec2& q : p.pts)
    {
        *lo = glm::min(*lo, q);
        *hi = glm::max(*hi, q);
    }
}

/** True when the polygons share area (beyond a centimetre at their edges). */
bool overlaps(const Polygon& a, const Polygon& b)
{
    Vec2 alo, ahi, blo, bhi;
    bounds(a, &alo, &ahi);
    bounds(b, &blo, &bhi);
    if (alo.x > bhi.x || blo.x > ahi.x || alo.y > bhi.y || blo.y > ahi.y)
        return false;
    for (const Vec2& q : a.pts)
        if (contains(b.pts, q) && boundaryDistance(b.pts, q) > 1.0)
            return true;
    for (const Vec2& q : b.pts)
        if (contains(a.pts, q) && boundaryDistance(a.pts, q) > 1.0)
            return true;
    const Vec2 ca = centroid(a), cb = centroid(b);
    return (contains(b.pts, ca) && boundaryDistance(b.pts, ca) > 1.0) || (contains(a.pts, cb) && boundaryDistance(a.pts, cb) > 1.0);
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Route book

const std::vector<Vec2>* RouteBook::use(Vec2 a, Vec2 b, double maxGrade) const
{
    for (const Entry& e : entries)
        if (glm::length(e.a - a) <= tolerance && glm::length(e.b - b) <= tolerance && std::abs(e.maxGrade - maxGrade) < 1e-6 && e.pts.size() >= 2)
            return &e.pts;
    if (!recording)
        return nullptr;
    std::lock_guard lock(mLock);
    for (const Entry& m : mMissing)
        if (glm::length(m.a - a) <= tolerance && glm::length(m.b - b) <= tolerance && std::abs(m.maxGrade - maxGrade) < 1e-6)
            return nullptr;
    Entry m;
    m.a = a;
    m.b = b;
    m.maxGrade = maxGrade;
    mMissing.push_back(m);
    return nullptr;
}

std::vector<RouteBook::Entry> RouteBook::missing() const
{
    std::lock_guard lock(mLock);
    return mMissing;
}

void RouteBook::clearMissing() const
{
    std::lock_guard lock(mLock);
    mMissing.clear();
}

json toJson(const RouteBook& b)
{
    json routes = json::array();
    for (const RouteBook::Entry& e : b.entries)
    {
        json pts = json::array();
        for (const Vec2& q : e.pts)
            pts.push_back(planXz(q));
        routes.push_back({{"a", planXz(e.a)},
                          {"b", planXz(e.b)},
                          {"maxGrade", e.maxGrade},
                          {"points", pts},
                          {"source", e.source},
                          {"gradeFound", e.gradeFound},
                          {"withinGrade", e.withinGrade},
                          {"note", e.note}});
    }
    return {{"routes", routes}};
}

std::shared_ptr<RouteBook> routeBookFromJson(const json& j)
{
    auto b = std::make_shared<RouteBook>();
    if (!j.is_object())
        throw std::runtime_error("terrainRoutes must be a JSON object");
    for (const json& r : j.value("routes", json::array()))
    {
        RouteBook::Entry e;
        e.a = vec2Of(r.at("a"));
        e.b = vec2Of(r.at("b"));
        e.maxGrade = r.value("maxGrade", 0.12);
        for (const json& q : r.value("points", json::array()))
            e.pts.push_back(vec2Of(q));
        e.source = r.value("source", std::string("terrain"));
        e.gradeFound = r.value("gradeFound", 0.0);
        e.withinGrade = r.value("withinGrade", true);
        e.note = r.value("note", std::string());
        b->entries.push_back(std::move(e));
    }
    return b;
}

double classMaxGrade(const Profile& p, StreetClass c)
{
    switch (c)
    {
    case StreetClass::Local: return p.maxGrade * 1.25;
    case StreetClass::Alley: return p.maxGrade * 1.5;
    case StreetClass::Path: return p.maxGrade * 2.5; // paths take stairs
    default: return p.maxGrade;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pinned plan

uint64_t planKey(const Site& site, const Options& o)
{
    json j = {{"profile", toJson(o.profile)},
              {"seed", o.seed},
              {"growth", o.growth},
              {"scales", {o.heightScale, o.widthScale, o.lotScale}},
              {"blocks", o.blocks},
              {"buildings", o.buildings},
              {"walk", {o.walk.maxSlope, o.walk.step, o.walk.gap, o.walk.spacing}},
              {"size", site.size},
              {"hasCenter", site.hasCenter},
              {"edits", site.editsKey}};
    auto line = [](std::span<const Vec2> pts) {
        json a = json::array();
        for (const Vec2& q : pts)
            a.push_back(planXz(q));
        return a;
    };
    j["outline"] = line(site.outline);
    j["attractors"] = line(site.attractors);
    for (const auto& s : site.streets)
        j["streets"].push_back(line(s));
    for (const auto& r : site.roads)
        j["roads"].push_back(line(r));
    for (const WaterLine& w : site.water)
        j["water"].push_back({line(w.pts), w.width, w.kind});
    return fnv(j.dump());
}

json planToJson(const Layout& l, uint64_t key, const std::string& revision, const std::string& terrain)
{
    LILA_ZONE();

    json nodes = json::array(), edges = json::array(), blocks = json::array(), lots = json::array(), envelopes = json::array(), bridges = json::array();
    for (const StreetNode& n : l.graph.nodes)
        nodes.push_back(json::array({n.p.x, n.p.y, n.alive}));
    for (const StreetEdge& e : l.graph.edges)
        edges.push_back(json::array({e.a, e.b, int(e.cls), e.era, e.alive, e.width, e.profile, e.path, e.street, e.bridge, e.tunnel, e.layer, e.moat}));
    for (const Block& b : l.blocks)
        blocks.push_back(planBlockJson(b));
    for (const Lot& lot : l.lots)
        lots.push_back(lotJson(lot));
    for (const Envelope& e : l.envelopes)
        envelopes.push_back(envelopeJson(e));
    for (const Vec2& b : l.bridges)
        bridges.push_back(planXz(b));
    return {{"key", std::format("{:016x}", key)},
            {"revision", revision},
            {"terrain", terrain},
            {"nodes", nodes},
            {"edges", edges},
            {"blocks", blocks},
            {"lots", lots},
            {"envelopes", envelopes},
            {"bridges", bridges}};
}

std::shared_ptr<PlanSnapshot> planFromJson(const json& j)
{
    LILA_ZONE();

    if (!j.is_object() || !j.contains("key") || !j["key"].is_string())
        throw std::runtime_error("plan must be an object with a key");
    auto s = std::make_shared<PlanSnapshot>();
    s->key = std::stoull(j["key"].get<std::string>(), nullptr, 16);
    s->revision = j.value("revision", std::string());
    s->terrain = j.value("terrain", std::string());
    for (const json& n : j.value("nodes", json::array()))
        s->nodes.push_back({vec2Of(n), n.size() > 2 ? n[2].get<bool>() : true});
    for (const json& e : j.value("edges", json::array()))
    {
        if (!e.is_array() || e.size() < 9)
            throw std::runtime_error("plan edges are [a, b, class, era, alive, width, profile, path, street, bridge, tunnel, layer, moat]");
        StreetEdge x;
        x.a = e[0].get<int>();
        x.b = e[1].get<int>();
        x.cls = StreetClass(e[2].get<int>());
        x.era = e[3].get<int>();
        x.alive = e[4].get<bool>();
        x.width = e[5].get<double>();
        x.profile = e[6].get<int>();
        x.path = e[7].get<int>();
        x.street = e[8].get<uint64_t>();
        x.bridge = e.size() > 9 && e[9].get<bool>();
        x.tunnel = e.size() > 10 && e[10].get<bool>();
        x.layer = e.size() > 11 ? e[11].get<int>() : 0;
        x.moat = e.size() > 12 && e[12].get<bool>();
        if (x.a < 0 || x.b < 0 || size_t(x.a) >= s->nodes.size() || size_t(x.b) >= s->nodes.size())
            throw std::runtime_error("plan edge names a node it does not have");
        s->edges.push_back(x);
    }
    for (const json& b : j.value("blocks", json::array()))
        s->blocks.push_back(blockOf(b));
    for (const json& l : j.value("lots", json::array()))
        s->lots.push_back(lotOf(l));
    for (const json& e : j.value("envelopes", json::array()))
        s->envelopes.push_back(envelopeOf(e));
    for (const json& b : j.value("bridges", json::array()))
        s->bridges.push_back(vec2Of(b));
    return s;
}

void applySnapshot(Layout& l, const PlanSnapshot& s)
{
    l.graph.nodes = s.nodes;
    l.graph.edges = s.edges;
    l.graph.reindex();
    l.blocks = s.blocks;
    l.lots = s.lots;
    l.envelopes = s.envelopes;
    l.bridges = s.bridges;
}

/**
 * A pinned plan's buildings stood on the ground it was planned on; grading (pads, cut and fill) moves that ground. Each
 * envelope keeps its footprint, floors and foundation depth and is set on the ground now under it.
 */
static void reseatEnvelopes(Layout& l, const Site& site, const Ground& ground)
{
    for (Envelope& e : l.envelopes)
    {
        if (e.footprint.pts.empty())
            continue;
        double lo = 0.0, hi = 0.0;
        envelopeGround(e.footprint, ground, lo, hi);
        if (auto it = site.foundations.find(e.key); it != site.foundations.end())
        {
            lo = std::min(lo, it->second.min);
            hi = std::max(hi, it->second.max);
        }
        const double below = e.ground - e.base, rise = hi + e.height - e.top;
        // Ground that didn't move (beyond sampling noise) leaves the building as planned, byte for byte.
        if (std::abs(lo - e.ground) < 5.0 && std::abs(rise) < 5.0)
            continue;
        e.ground = lo;
        e.base = lo - below;
        e.top = hi + e.height;
        e.foundationDepth = hi - e.base;
        for (size_t t = 0; t < e.tiers.size(); ++t)
        {
            e.tiers[t].bottom = t == 0 ? e.base : e.tiers[t].bottom + rise;
            e.tiers[t].top += rise;
        }
    }
}

Layout buildPlanned(const Site& site, const Ground& ground, const Options& o, const WalkSurface* walk)
{
    LILA_ZONE();

    Layout l = build(site, ground, o, walk);
    l.plan = "none";
    if (site.plan)
    {
        if (site.plan->key == planKey(site, o))
        {
            applySnapshot(l, *site.plan);
            reseatEnvelopes(l, site, ground);
            applyBuildingRoutes(l, site);
            l.plan = "pinned";
        }
        else
            l.plan = "stale";
    }
    return l;
}

bool touches(const Polygon& p, const DirtyRect& r)
{
    if (p.pts.empty())
        return false;
    Vec2 lo, hi;
    bounds(p, &lo, &hi);
    if (lo.x > r.hi.x || r.lo.x > hi.x || lo.y > r.hi.y || r.lo.y > hi.y)
        return false;
    for (size_t k = 0; k < p.pts.size(); ++k)
        if (segmentTouches(p.pts[k], p.pts[(k + 1) % p.pts.size()], r))
            return true;
    // The rectangle wholly inside the polygon.
    return contains(p.pts, (r.lo + r.hi) * 0.5);
}

Layout mergeDirty(const Layout& before, const Layout& fresh, std::span<const DirtyRect> dirty, json* report)
{
    LILA_ZONE();

    Layout out = fresh;
    out.graph = before.graph;
    int removed = 0, added = 0;
    for (size_t e = 0; e < out.graph.edges.size(); ++e)
        if (out.graph.isStreet(int(e)) && edgeTouches(out.graph, out.graph.edges[e], dirty))
        {
            out.graph.removeEdge(int(e));
            ++removed;
        }
    for (size_t e = 0; e < fresh.graph.edges.size(); ++e)
    {
        const StreetEdge& x = fresh.graph.edges[e];
        if (!fresh.graph.isStreet(int(e)) || !edgeTouches(fresh.graph, x, dirty))
            continue;
        out.graph.addStreet(fresh.graph.nodes[size_t(x.a)].p, fresh.graph.nodes[size_t(x.b)].p, x);
        ++added;
    }

    out.blocks.clear();
    out.lots.clear();
    out.envelopes.clear();
    int nextBlock = 0, nextLot = 0;
    std::set<int> keptBlocks, keptLots;
    for (const Block& b : before.blocks)
    {
        nextBlock = std::max(nextBlock, b.id + 1);
        if (!touchesAny(b.outline, dirty))
        {
            out.blocks.push_back(b);
            keptBlocks.insert(b.id);
        }
    }
    for (const Lot& lot : before.lots)
    {
        nextLot = std::max(nextLot, lot.id + 1);
        if (keptBlocks.contains(lot.block))
        {
            out.lots.push_back(lot);
            keptLots.insert(lot.id);
        }
    }
    for (const Envelope& e : before.envelopes)
        if (keptLots.contains(e.lot))
            out.envelopes.push_back(e);
    const size_t keptCount = out.blocks.size();

    std::map<int, int> blockMap, lotMap;
    for (const Block& b : fresh.blocks)
    {
        if (!touchesAny(b.outline, dirty))
            continue;
        bool clear = true;
        for (size_t k = 0; k < keptCount && clear; ++k)
            clear = !overlaps(out.blocks[k].outline, b.outline);
        if (!clear)
            continue;
        Block nb = b;
        nb.id = nextBlock++;
        blockMap[b.id] = nb.id;
        out.blocks.push_back(std::move(nb));
    }
    for (const Lot& lot : fresh.lots)
        if (auto it = blockMap.find(lot.block); it != blockMap.end())
        {
            Lot nl = lot;
            nl.id = nextLot++;
            nl.block = it->second;
            lotMap[lot.id] = nl.id;
            out.lots.push_back(std::move(nl));
        }
    for (const Envelope& e : fresh.envelopes)
        if (auto it = lotMap.find(e.lot); it != lotMap.end())
        {
            Envelope ne = e;
            ne.lot = it->second;
            out.envelopes.push_back(std::move(ne));
        }

    auto inDirty = [&](Vec2 q) {
        for (const DirtyRect& r : dirty)
            if (q.x >= r.lo.x && q.x <= r.hi.x && q.y >= r.lo.y && q.y <= r.hi.y)
                return true;
        return false;
    };
    out.bridges.clear();
    for (const Vec2& b : before.bridges)
        if (!inDirty(b))
            out.bridges.push_back(b);
    for (const Vec2& b : fresh.bridges)
        if (inDirty(b))
            out.bridges.push_back(b);

    if (report)
        *report = {{"dirtyRegions", dirty.size()},
                   {"keptBlocks", keptCount},
                   {"droppedBlocks", before.blocks.size() - keptCount},
                   {"newBlocks", out.blocks.size() - keptCount},
                   {"streetEdgesRemoved", removed},
                   {"streetEdgesAdded", added}};
    return out;
}

uint64_t blockHash(const Layout& l, int block)
{
    uint64_t h = fnv("block");
    for (const Block& b : l.blocks)
        if (b.id == block)
            h = fnv(planBlockJson(b).dump(), h);
    std::set<int> lots;
    for (const Lot& lot : l.lots)
        if (lot.block == block)
        {
            h = fnv(lotJson(lot).dump(), h);
            lots.insert(lot.id);
        }
    for (const Envelope& e : l.envelopes)
        if (lots.contains(e.lot))
            h = fnv(envelopeJson(e).dump(), h);
    return h;
}

void applyTerrainEdits(Site& site, const json& edits)
{
    if (!edits.is_object())
        return;
    for (const json& w : edits.value("water", json::array()))
    {
        if (!w.is_object())
            throw std::runtime_error("water edits are objects {points, width, kind}");
        WaterLine line;
        for (const json& q : w.value("points", json::array()))
            line.pts.push_back(vec2Of(q.size() == 3 ? json::array({q[0], q[2]}) : q));
        if (line.pts.size() < 2)
            throw std::runtime_error("water edits need two or more points");
        line.width = std::max(1.0, w.value("width", 2000.0));
        line.kind = w.value("kind", std::string());
        if (line.kind != "" && line.kind != "river" && line.kind != "canal" && line.kind != "moat" && line.kind != "quay")
            throw std::runtime_error("unknown water kind '" + line.kind + "' (river, canal, moat, quay, or empty for natural water)");
        site.water.push_back(std::move(line));
    }
    if (edits.contains("terrainRoutes"))
        site.routeBook = routeBookFromJson(edits["terrainRoutes"]);
    if (edits.contains("plan") && !edits["plan"].is_null())
        site.plan = planFromJson(edits["plan"]);
    json rest = edits;
    for (const char* k : {"plan", "terrain", "terrainRoutes", "siting", "buildingRoutes", "libraryPicks"})
        rest.erase(k);
    site.editsKey = fnv(rest.dump());
}
} // namespace settlement
