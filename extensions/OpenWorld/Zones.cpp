// Zones: the spec and its JSON, painted masks in sparse tiles, point queries with priority, rasters for the overlay and
// the check (gaps and overlaps), mask outlines and convex pieces for engines, and the engine JSON both ways.

#include "Zones.h"

#include <clipper2/clipper.h>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace openworld::zones
{
namespace
{
[[noreturn]] void bad(const std::string& field, const std::string& what) { throw std::runtime_error("zones: " + field + " " + what); }

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint64_t hashText(std::string_view s)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : s)
    {
        h ^= uint8_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

glm::vec3 hsv(float h, float s, float v)
{
    const float c = v * s, x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f)), m = v - c;
    const int k = int(h * 6.0f) % 6;
    const glm::vec3 rgb = k == 0 ? glm::vec3(c, x, 0) : k == 1 ? glm::vec3(x, c, 0) : k == 2 ? glm::vec3(0, c, x)
                        : k == 3 ? glm::vec3(0, x, c) : k == 4 ? glm::vec3(x, 0, c) : glm::vec3(c, 0, x);
    return rgb + glm::vec3(m);
}

json ringJson(const Polygon& r)
{
    json a = json::array();
    for (Vec2 p : r)
        a.push_back({p.x, p.y});
    return a;
}

Polygon ringFrom(const json& j, const std::string& field)
{
    if (!j.is_array())
        bad(field, "is a list of [x, z] points");
    Polygon r;
    for (const json& p : j)
    {
        if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[p.size() == 3 ? 2 : 1].is_number())
            bad(field, "is a list of [x, z] points");
        r.push_back(p.size() == 3 ? Vec2(p[0].get<double>(), p[2].get<double>()) : Vec2(p[0].get<double>(), p[1].get<double>()));
    }
    if (r.size() < 3)
        bad(field, "needs at least 3 points");
    return r;
}

std::vector<Polygon> ringsFrom(const json& j, const std::string& field)
{
    std::vector<Polygon> out;
    if (j.is_null())
        return out;
    if (!j.is_array())
        bad(field, "is a list of rings");
    for (size_t i = 0; i < j.size(); ++i)
        out.push_back(ringFrom(j[i], field + "[" + std::to_string(i) + "]"));
    return out;
}

void onlyKeys(const json& j, const std::string& what, std::initializer_list<const char*> keys)
{
    if (!j.is_object())
        bad(what, "is an object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return it.key() == k; }))
            bad(what, "has an unknown key '" + it.key() + "'");
}

glm::vec3 colorFrom(const json& j, glm::vec3 fallback)
{
    if (!j.is_array() || j.size() < 3)
        return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

bool ringBounds(const Polygon& r, Vec2& lo, Vec2& hi)
{
    for (Vec2 p : r)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    return !r.empty();
}

bool insidePolygons(const Zone& z, Vec2 p)
{
    for (const Polygon& r : z.polygons)
        if (inside(r, p))
        {
            for (const Polygon& h : z.holes)
                if (inside(h, p))
                    return false;
            return true;
        }
    return false;
}

/** Zones in winning order: priority, then the later. */
std::vector<int> winOrder(const Layer& l)
{
    std::vector<int> order(l.zones.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (l.zones[size_t(a)].priority != l.zones[size_t(b)].priority)
            return l.zones[size_t(a)].priority > l.zones[size_t(b)].priority;
        return a > b;
    });
    return order;
}

double perpendicular(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double len = glm::length(d);
    if (len < 1e-12)
        return glm::length(p - a);
    return std::abs(d.x * (a.y - p.y) - d.y * (a.x - p.x)) / len;
}

void douglasPeucker(const Polygon& pts, size_t a, size_t b, double tol, std::vector<bool>& keep)
{
    double best = -1.0;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i)
        if (const double d = perpendicular(pts[i], pts[a], pts[b]); d > best)
        {
            best = d;
            at = i;
        }
    if (best > tol)
    {
        keep[at] = true;
        douglasPeucker(pts, a, at, tol, keep);
        douglasPeucker(pts, at, b, tol, keep);
    }
}

Polygon simplifyRing(const Polygon& ring, double tol)
{
    if (ring.size() <= 4 || tol <= 0.0)
        return ring;
    // Split the closed ring at its two farthest-apart points and simplify each half.
    size_t far = 0;
    double best = -1.0;
    for (size_t i = 1; i < ring.size(); ++i)
        if (const double d = glm::length(ring[i] - ring[0]); d > best)
        {
            best = d;
            far = i;
        }
    Polygon closed = ring;
    closed.push_back(ring[0]);
    std::vector<bool> keep(closed.size(), false);
    keep[0] = keep[far] = keep[closed.size() - 1] = true;
    douglasPeucker(closed, 0, far, tol, keep);
    douglasPeucker(closed, far, closed.size() - 1, tol, keep);
    Polygon out;
    for (size_t i = 0; i + 1 < closed.size(); ++i)
        if (keep[i])
            out.push_back(closed[i]);
    return out.size() >= 3 ? out : ring;
}

double cross(Vec2 o, Vec2 a, Vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

bool convex(const Polygon& p)
{
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++i)
        if (cross(p[i], p[(i + 1) % n], p[(i + 2) % n]) < -1e-6)
            return false;
    return true;
}

bool inTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c) { return cross(a, b, p) >= 0.0 && cross(b, c, p) >= 0.0 && cross(c, a, p) >= 0.0; }
} // namespace

uint8_t Mask::cellValue(int cx, int cz) const
{
    const auto it = tiles.find({floorDiv(cx, kTile), floorDiv(cz, kTile)});
    if (it == tiles.end())
        return 0;
    const int lx = cx - floorDiv(cx, kTile) * kTile, lz = cz - floorDiv(cz, kTile) * kTile;
    return it->second[size_t(lz * kTile + lx)];
}

void Mask::setCell(int cx, int cz, uint8_t v)
{
    const std::pair<int, int> key{floorDiv(cx, kTile), floorDiv(cz, kTile)};
    auto it = tiles.find(key);
    if (it == tiles.end())
    {
        if (v == 0)
            return;
        it = tiles.emplace(key, std::vector<uint8_t>(size_t(kTile * kTile), 0)).first;
    }
    it->second[size_t((cz - key.second * kTile) * kTile + (cx - key.first * kTile))] = v;
}

glm::ivec2 Mask::cellOf(Vec2 p) const { return {int(std::floor(p.x / cell)), int(std::floor(p.y / cell))}; }

double Mask::at(Vec2 p) const
{
    const glm::ivec2 c = cellOf(p);
    return cellValue(c.x, c.y) / 255.0;
}

void Mask::prune()
{
    for (auto it = tiles.begin(); it != tiles.end();)
        if (std::all_of(it->second.begin(), it->second.end(), [](uint8_t v) { return v == 0; }))
            it = tiles.erase(it);
        else
            ++it;
}

bool Mask::cellBounds(glm::ivec2& lo, glm::ivec2& hi) const
{
    lo = glm::ivec2(INT32_MAX);
    hi = glm::ivec2(INT32_MIN);
    for (const auto& [key, v] : tiles)
        for (int z = 0; z < kTile; ++z)
            for (int x = 0; x < kTile; ++x)
                if (v[size_t(z * kTile + x)])
                {
                    lo = glm::min(lo, glm::ivec2(key.first * kTile + x, key.second * kTile + z));
                    hi = glm::max(hi, glm::ivec2(key.first * kTile + x, key.second * kTile + z));
                }
    return lo.x <= hi.x;
}

size_t Mask::setCells(uint8_t threshold) const
{
    size_t n = 0;
    for (const auto& [key, v] : tiles)
        for (uint8_t c : v)
            n += c >= threshold ? 1 : 0;
    return n;
}

size_t paint(Mask& m, const Dab& d)
{
    const double r = std::max(d.radius, m.cell * 0.5);
    const glm::ivec2 lo = m.cellOf(d.center - Vec2(r)), hi = m.cellOf(d.center + Vec2(r));
    const double soft = std::clamp(d.softness, 0.0, 1.0);
    const double target = d.erase ? 0.0 : std::clamp(d.value, 0.0, 1.0) * 255.0;
    size_t changed = 0;
    for (int z = lo.y; z <= hi.y; ++z)
        for (int x = lo.x; x <= hi.x; ++x)
        {
            const Vec2 c = (Vec2(x, z) + 0.5) * m.cell;
            const double t = glm::length(c - d.center) / r;
            if (t > 1.0)
                continue;
            double f = soft <= 0.0 || t <= 1.0 - soft ? 1.0 : (1.0 - t) / soft;
            f = f * f * (3.0 - 2.0 * f);
            const double old = m.cellValue(x, z);
            const double now = old + (target - old) * std::clamp(d.strength, 0.0, 1.0) * f;
            // Round toward the target so repeated soft dabs reach it.
            const uint8_t v = uint8_t(std::clamp(target > old ? std::ceil(now - 1e-9) : std::floor(now + 1e-9), 0.0, 255.0));
            if (v != uint8_t(old))
            {
                m.setCell(x, z, v);
                ++changed;
            }
        }
    if (d.erase)
        m.prune();
    return changed;
}

Zone* Layer::zone(std::string_view zoneId)
{
    for (Zone& z : zones)
        if (z.id == zoneId)
            return &z;
    return nullptr;
}

const Zone* Layer::zone(std::string_view zoneId) const { return const_cast<Layer*>(this)->zone(zoneId); }

Layer* ZoneSpec::layer(std::string_view layerId)
{
    for (Layer& l : layers)
        if (l.id == layerId)
            return &l;
    return nullptr;
}

const Layer* ZoneSpec::layer(std::string_view layerId) const { return const_cast<ZoneSpec*>(this)->layer(layerId); }

const LayerType* ZoneSpec::type(std::string_view typeId) const
{
    for (const LayerType& t : types)
        if (t.id == typeId)
            return &t;
    for (const LayerType& t : builtInTypes())
        if (t.id == typeId)
            return &t;
    return nullptr;
}

const std::vector<LayerType>& builtInTypes()
{
    static const std::vector<LayerType> all = {
        {"district", "Districts", false, true, true, json::object(), "district", {0.55f, 0.7f, 0.95f},
         "Named districts and neighbourhoods; the map's place names."},
        {"police", "Police", false, true, true, {{"agency", "municipal"}, {"station", ""}, {"responseTime", 60}}, "government", {0.35f, 0.5f, 1.0f},
         "Police jurisdiction: which agency answers, from which station, how fast (s)."},
        {"military", "Military", false, false, false, {{"branch", "army"}, {"restricted", true}, {"shootOnSight", false}}, "government",
         {0.45f, 0.6f, 0.35f}, "Military jurisdiction and restricted ground."},
        {"territory", "Territory", false, false, false, {{"faction", ""}, {"control", 1.0}, {"hostility", 0.5}}, "district", {0.9f, 0.3f, 0.3f},
         "Gang and cartel territory: which faction holds it and how hard; overlaps are contested ground."},
        {"pedSpawn", "Pedestrian Spawns", false, false, false, {{"set", "default"}, {"models", json::array()}, {"weight", 1.0}, {"hours", "any"}},
         "district", {0.95f, 0.75f, 0.3f}, "Ambient pedestrian spawn sets."},
        {"vehicleSpawn", "Vehicle Spawns", false, false, false, {{"set", "default"}, {"models", json::array()}, {"weight", 1.0}, {"hours", "any"}},
         "district", {0.95f, 0.55f, 0.25f}, "Ambient vehicle spawn sets."},
        {"boatSpawn", "Boat Spawns", false, false, false, {{"set", "default"}, {"craft", "skiff"}, {"models", json::array()}, {"weight", 1.0}},
         "district", {0.25f, 0.75f, 0.9f}, "Ambient boat spawn sets, often along water routes."},
        {"aircraftSpawn", "Aircraft Spawns", false, false, false, {{"set", "default"}, {"craft", "light"}, {"models", json::array()}, {"weight", 1.0}},
         "district", {0.7f, 0.7f, 0.95f}, "Ambient aircraft spawn sets, often along air lanes."},
        {"traffic", "Traffic Density", true, false, false, {{"density", 1.0}, {"maxVehiclesPerKm", 30}}, "district", {0.95f, 0.6f, 0.2f},
         "Painted traffic density: density times the painted weight."},
        {"pedestrians", "Pedestrian Density", true, false, false, {{"density", 1.0}, {"maxPerHectare", 40}}, "district", {0.4f, 0.85f, 0.5f},
         "Painted pedestrian density: density times the painted weight."},
        {"wanted", "Wanted Response", false, true, false, {{"level", 1}, {"responders", {"police"}}, {"responseTime", 60}, {"heat", 1.0}}, "district",
         {0.95f, 0.35f, 0.6f}, "How the law answers a crime here: base wanted level, responders, response time (s), heat multiplier."},
        {"interiors", "Interiors", false, false, false, {{"interior", ""}, {"building", ""}, {"enterable", true}, {"floor", 0}}, "business",
         {0.8f, 0.6f, 0.9f}, "Index of interiors: which interior and building a footprint holds."},
        {"missions", "Mission Markers", false, false, false, {{"mission", ""}, {"trigger", "enter"}, {"marker", "area"}}, "business", {1.0f, 0.85f, 0.2f},
         "Mission start and trigger areas."},
    };
    return all;
}

json toJson(const Mask& m)
{
    json tiles = json::array();
    for (const auto& [key, v] : m.tiles)
    {
        mz_ulong len = mz_compressBound(mz_ulong(v.size()));
        std::vector<uint8_t> packed(len);
        if (mz_compress2(packed.data(), &len, v.data(), mz_ulong(v.size()), 6) != MZ_OK)
            throw std::runtime_error("zones: could not pack a mask tile");
        packed.resize(len);
        tiles.push_back({{"at", {key.first, key.second}}, {"data", base64(packed)}});
    }
    return {{"cell", m.cell}, {"tiles", tiles}};
}

Mask maskFromJson(const json& j, double cell)
{
    Mask m;
    m.cell = cell;
    if (j.is_null())
        return m;
    if (!j.is_object() || !j.value("tiles", json()).is_array())
        bad("mask", "is {cell, tiles: [{at, data}]}");
    if (const double c = j.value("cell", cell); std::abs(c - cell) > 1e-6)
        bad("mask", "cell " + std::to_string(c) + " differs from the set's maskCell " + std::to_string(cell));
    for (const json& t : j["tiles"])
    {
        if (!t.contains("at") || !t["at"].is_array() || t["at"].size() != 2 || !t.value("data", json()).is_string())
            bad("mask.tiles", "are {at: [tx, tz], data}");
        const std::vector<uint8_t> packed = unbase64(t["data"].get<std::string>());
        std::vector<uint8_t> v(size_t(kTile * kTile));
        mz_ulong len = mz_ulong(v.size());
        if (mz_uncompress(v.data(), &len, packed.data(), mz_ulong(packed.size())) != MZ_OK || len != v.size())
            bad("mask.tiles", "hold a tile that does not unpack");
        m.tiles[{t["at"][0].get<int>(), t["at"][1].get<int>()}] = std::move(v);
    }
    m.prune();
    return m;
}

json toJson(const LayerType& t)
{
    return {{"id", t.id}, {"label", t.label}, {"density", t.density}, {"exclusive", t.exclusive}, {"mustCover", t.mustCover},
            {"defaults", t.defaults}, {"nameKind", t.nameKind}, {"color", {t.color.r, t.color.g, t.color.b}}, {"doc", t.doc}, {"builtIn", t.builtIn}};
}

LayerType typeFromJson(const json& j)
{
    onlyKeys(j, "types[]", {"id", "label", "density", "exclusive", "mustCover", "defaults", "nameKind", "color", "doc", "builtIn"});
    LayerType t;
    t.id = j.value("id", std::string());
    if (t.id.empty())
        bad("types[].id", "is required");
    t.label = j.value("label", t.id);
    t.density = j.value("density", false);
    t.exclusive = j.value("exclusive", false);
    t.mustCover = j.value("mustCover", false);
    t.defaults = j.value("defaults", json::object());
    if (!t.defaults.is_object())
        bad("types[].defaults", "is an object");
    t.nameKind = j.value("nameKind", std::string("district"));
    t.color = colorFrom(j.value("color", json()), hsv(float(hashText(t.id) % 1000) / 1000.0f, 0.5f, 0.9f));
    t.doc = j.value("doc", std::string());
    t.builtIn = false;
    return t;
}

Layer makeLayer(const LayerType& t, std::string id)
{
    Layer l;
    l.id = std::move(id);
    l.type = t.id;
    l.label = t.label;
    l.exclusive = t.exclusive;
    l.mustCover = t.mustCover;
    l.density = t.density;
    l.color = t.color;
    l.defaults = t.defaults;
    return l;
}

json toJson(const ZoneSpec& s)
{
    json layers = json::array();
    for (const Layer& l : s.layers)
    {
        json zones = json::array();
        for (const Zone& z : l.zones)
        {
            json zj = {{"id", z.id}, {"name", z.name}, {"priority", z.priority}, {"attrs", z.attrs}};
            if (!z.nameSource.empty())
                zj["nameSource"] = z.nameSource;
            if (!z.polygons.empty())
            {
                json p = json::array();
                for (const Polygon& r : z.polygons)
                    p.push_back(ringJson(r));
                zj["polygons"] = p;
            }
            if (!z.holes.empty())
            {
                json h = json::array();
                for (const Polygon& r : z.holes)
                    h.push_back(ringJson(r));
                zj["holes"] = h;
            }
            if (z.hasMask())
                zj["mask"] = toJson(z.mask);
            if (!z.source.is_null())
                zj["source"] = z.source;
            zones.push_back(std::move(zj));
        }
        json lj = {{"id", l.id},
                   {"type", l.type},
                   {"label", l.label},
                   {"priority", l.priority},
                   {"exclusive", l.exclusive},
                   {"mustCover", l.mustCover},
                   {"visible", l.visible},
                   {"density", l.density},
                   {"color", {l.color.r, l.color.g, l.color.b}},
                   {"defaults", l.defaults},
                   {"zones", zones}};
        if (!l.dataLayer.empty())
            lj["dataLayer"] = l.dataLayer;
        layers.push_back(std::move(lj));
    }
    json out = {{"version", s.version}, {"culture", s.culture}, {"maskCell", s.maskCell}, {"layers", layers}};
    if (!s.playArea.empty())
    {
        json p = json::array();
        for (const Polygon& r : s.playArea)
            p.push_back(ringJson(r));
        out["playArea"] = p;
    }
    if (!s.types.empty())
    {
        json t = json::array();
        for (const LayerType& x : s.types)
            t.push_back(toJson(x));
        out["types"] = t;
    }
    return out;
}

ZoneSpec zonesFromJson(const json& j)
{
    ZoneSpec s;
    if (j.is_null())
        return s;
    onlyKeys(j, "spec", {"version", "culture", "maskCell", "playArea", "types", "layers"});
    s.version = j.value("version", 1);
    if (s.version != 1)
        bad("version", "is " + std::to_string(s.version) + "; this Lila reads version 1");
    s.culture = j.value("culture", s.culture);
    s.maskCell = j.value("maskCell", s.maskCell);
    if (!(s.maskCell >= 1.0))
        bad("maskCell", "is at least 1 cm");
    s.playArea = ringsFrom(j.value("playArea", json()), "playArea");
    for (const json& t : j.value("types", json::array()))
        s.types.push_back(typeFromJson(t));
    std::set<std::string> layerIds;
    const json layers = j.value("layers", json::array());
    if (!layers.is_array())
        bad("layers", "is a list");
    for (size_t li = 0; li < layers.size(); ++li)
    {
        const json& lj = layers[li];
        const std::string where = "layers[" + std::to_string(li) + "]";
        onlyKeys(lj, where, {"id", "type", "label", "priority", "exclusive", "mustCover", "visible", "density", "color", "defaults", "dataLayer", "zones"});
        const std::string type = lj.value("type", std::string());
        const LayerType* t = s.type(type);
        if (!t)
            bad(where + ".type", "'" + type + "' is not a known layer type (zoneTypes lists them; define a new one in types)");
        Layer l = makeLayer(*t, lj.value("id", type));
        if (l.id.empty() || !layerIds.insert(l.id).second)
            bad(where + ".id", "'" + l.id + "' is empty or used twice");
        l.label = lj.value("label", l.label);
        l.priority = lj.value("priority", 0);
        l.exclusive = lj.value("exclusive", l.exclusive);
        l.mustCover = lj.value("mustCover", l.mustCover);
        l.visible = lj.value("visible", true);
        l.density = lj.value("density", l.density);
        l.color = colorFrom(lj.value("color", json()), l.color);
        l.defaults = lj.value("defaults", l.defaults);
        l.dataLayer = lj.value("dataLayer", std::string());
        std::set<std::string> zoneIds;
        const json zones = lj.value("zones", json::array());
        for (size_t zi = 0; zi < zones.size(); ++zi)
        {
            const json& zj = zones[zi];
            const std::string zw = where + ".zones[" + std::to_string(zi) + "]";
            onlyKeys(zj, zw, {"id", "name", "nameSource", "priority", "attrs", "polygons", "holes", "mask", "source"});
            Zone z;
            z.id = zj.value("id", std::string());
            if (z.id.empty() || !zoneIds.insert(z.id).second)
                bad(zw + ".id", "'" + z.id + "' is empty or used twice in the layer");
            z.name = zj.value("name", std::string());
            z.nameSource = zj.value("nameSource", std::string());
            z.priority = zj.value("priority", 0);
            z.attrs = zj.value("attrs", json::object());
            if (!z.attrs.is_object())
                bad(zw + ".attrs", "is an object");
            z.polygons = ringsFrom(zj.value("polygons", json()), zw + ".polygons");
            z.holes = ringsFrom(zj.value("holes", json()), zw + ".holes");
            z.mask = maskFromJson(zj.value("mask", json()), s.maskCell);
            z.source = zj.value("source", json());
            l.zones.push_back(std::move(z));
        }
        s.layers.push_back(std::move(l));
    }
    return s;
}

json zonesSchema()
{
    const json ring = {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 3}}}, {"minItems", 3}};
    const json rings = {{"type", "array"}, {"items", ring}};
    const json zone = {{"type", "object"},
                       {"required", {"id"}},
                       {"properties",
                        {{"id", {{"type", "string"}, {"description", "Stable id within the layer; the same id replaces."}}},
                         {"name", {{"type", "string"}, {"description", "Display name; generated from the naming culture when empty."}}},
                         {"nameSource", {{"type", "string"}}},
                         {"priority", {{"type", "integer"}, {"description", "Higher wins inside the layer; ties go to the later zone."}}},
                         {"attrs", {{"type", "object"}, {"description", "Typed attributes; the layer's defaults fill what is left out."}}},
                         {"polygons", rings},
                         {"holes", rings},
                         {"mask", {{"type", "object"}, {"description", "Painted mask: {cell, tiles: [{at: [tx, tz], data: base64 deflate of 64x64 bytes}]}."}}},
                         {"source", {{"type", "object"}, {"description", "What it was derived from: {kind: district|terrain|water|drawn|painted, ...}."}}}}}};
    const json layer = {{"type", "object"},
                        {"required", {"type"}},
                        {"properties",
                         {{"id", {{"type", "string"}}},
                          {"type", {{"type", "string"}, {"description", "A built-in type (zoneTypes) or one defined in types."}}},
                          {"label", {{"type", "string"}}},
                          {"priority", {{"type", "integer"}, {"description", "Layer order: higher draws on top and gets higher Unreal volume priority."}}},
                          {"exclusive", {{"type", "boolean"}}},
                          {"mustCover", {{"type", "boolean"}}},
                          {"visible", {{"type", "boolean"}}},
                          {"density", {{"type", "boolean"}}},
                          {"color", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                          {"defaults", {{"type", "object"}}},
                          {"dataLayer", {{"type", "string"}, {"description", "Unreal data layer name; empty for the label."}}},
                          {"zones", {{"type", "array"}, {"items", zone}}}}}};
    return {{"$schema", "https://json-schema.org/draft/2020-12/schema"},
            {"title", "Lila zones"},
            {"description", "The gameplay layer: typed zone layers over the world XZ plane, cm (+X east, -Z north). The world spec's zones section."},
            {"type", "object"},
            {"properties",
             {{"version", {{"const", 1}}},
              {"culture", {{"type", "string"}, {"description", "Naming culture of generated zone names."}}},
              {"maskCell", {{"type", "number"}, {"description", "Painted mask cell size, cm."}}},
              {"playArea", rings},
              {"types", {{"type", "array"}, {"items", {{"type", "object"}, {"required", {"id"}}}}}},
              {"layers", {{"type", "array"}, {"items", layer}}}}}};
}

double weight(const Zone& z, Vec2 p)
{
    if (insidePolygons(z, p))
        return 1.0;
    return z.hasMask() ? z.mask.at(p) : 0.0;
}

bool contains(const Layer& l, const Zone& z, Vec2 p)
{
    const double w = weight(z, p);
    return l.density ? w > 0.0 : w >= 0.5;
}

bool bounds(const Zone& z, Vec2& lo, Vec2& hi)
{
    lo = Vec2(1e300);
    hi = Vec2(-1e300);
    bool any = false;
    for (const Polygon& r : z.polygons)
        any = ringBounds(r, lo, hi) || any;
    glm::ivec2 a, b;
    if (z.mask.cellBounds(a, b))
    {
        lo = glm::min(lo, Vec2(a) * z.mask.cell);
        hi = glm::max(hi, Vec2(b + 1) * z.mask.cell);
        any = true;
    }
    return any;
}

double areaM2(const Zone& z, const Layer& l)
{
    double a = 0.0;
    for (const Polygon& r : z.polygons)
        a += std::abs(area(r));
    for (const Polygon& h : z.holes)
        a -= std::abs(area(h));
    const double cellArea = z.mask.cell * z.mask.cell;
    for (const auto& [key, v] : z.mask.tiles)
        for (int i = 0; i < kTile * kTile; ++i)
            if (v[size_t(i)] && (l.density || v[size_t(i)] >= 128))
            {
                const Vec2 c = (Vec2(key.first * kTile + i % kTile, key.second * kTile + i / kTile) + 0.5) * z.mask.cell;
                if (!insidePolygons(z, c))
                    a += l.density ? cellArea * v[size_t(i)] / 255.0 : cellArea;
            }
    return std::max(0.0, a) / 10000.0;
}

std::vector<const Zone*> zonesAt(const Layer& l, Vec2 p)
{
    std::vector<const Zone*> out;
    for (int i : winOrder(l))
        if (contains(l, l.zones[size_t(i)], p))
            out.push_back(&l.zones[size_t(i)]);
    return out;
}

json queryLayer(const Layer& l, Vec2 p)
{
    const std::vector<const Zone*> at = zonesAt(l, p);
    json out = {{"type", l.type}, {"label", l.label}, {"zone", nullptr}};
    json all = json::array();
    for (const Zone* z : at)
        all.push_back(z->id);
    out["all"] = all;
    if (l.density)
        out["density"] = 0.0;
    if (at.empty())
        return out;
    const Zone& z = *at.front();
    json attrs = l.defaults;
    attrs.update(z.attrs);
    const double w = weight(z, p);
    out["zone"] = z.id;
    out["name"] = z.name;
    out["priority"] = z.priority;
    out["weight"] = w;
    out["attrs"] = attrs;
    if (l.density)
    {
        const json d = attrs.value("density", json(1.0));
        out["density"] = (d.is_number() ? d.get<double>() : 1.0) * w;
    }
    return out;
}

json query(const ZoneSpec& s, Vec2 p)
{
    json out = json::object();
    for (const Layer& l : s.layers)
        out[l.id] = queryLayer(l, p);
    return out;
}

Raster rasterize(const Layer& l, Vec2 lo, Vec2 hi, double cell)
{
    Raster r;
    r.cell = std::max(cell, 1.0);
    r.origin = lo;
    r.nx = std::max(1, int(std::ceil((hi.x - lo.x) / r.cell)));
    r.nz = std::max(1, int(std::ceil((hi.y - lo.y) / r.cell)));
    r.winner.assign(size_t(r.nx) * size_t(r.nz), -1);
    r.count.assign(r.winner.size(), 0);
    const std::vector<int> order = winOrder(l);
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const Zone& z = l.zones[size_t(*it)];
        Vec2 zl, zh;
        if (!bounds(z, zl, zh))
            continue;
        const int x0 = std::max(0, int(std::floor((zl.x - lo.x) / r.cell - 0.5))), x1 = std::min(r.nx - 1, int(std::ceil((zh.x - lo.x) / r.cell)));
        const int z0 = std::max(0, int(std::floor((zl.y - lo.y) / r.cell - 0.5))), z1 = std::min(r.nz - 1, int(std::ceil((zh.y - lo.y) / r.cell)));
        for (int zz = z0; zz <= z1; ++zz)
            for (int xx = x0; xx <= x1; ++xx)
                if (contains(l, z, r.at(xx, zz)))
                {
                    const size_t i = size_t(zz) * size_t(r.nx) + size_t(xx);
                    r.winner[i] = *it;
                    r.count[i] = uint8_t(std::min(255, r.count[i] + 1));
                }
    }
    return r;
}

bool specBounds(const ZoneSpec& s, Vec2& lo, Vec2& hi)
{
    lo = Vec2(1e300);
    hi = Vec2(-1e300);
    bool any = false;
    for (const Layer& l : s.layers)
        for (const Zone& z : l.zones)
        {
            Vec2 a, b;
            if (bounds(z, a, b))
            {
                lo = glm::min(lo, a);
                hi = glm::max(hi, b);
                any = true;
            }
        }
    return any;
}

std::vector<Issue> check(const ZoneSpec& s, const std::vector<Polygon>& playArea, double cell, int minCells)
{
    std::vector<Issue> issues;
    for (const Layer& l : s.layers)
        for (const Zone& z : l.zones)
        {
            Vec2 a, b;
            if (!bounds(z, a, b))
                issues.push_back({"empty", l.id, {z.id}, Vec2(0.0), Vec2(0.0), Vec2(0.0), 0.0, "zone '" + z.id + "' has no polygon and nothing painted"});
            else if (z.name.empty())
                issues.push_back({"unnamed", l.id, {z.id}, (a + b) * 0.5, a, b, 0.0, "zone '" + z.id + "' has no name"});
        }

    Vec2 lo, hi;
    if (!playArea.empty())
    {
        lo = Vec2(1e300);
        hi = Vec2(-1e300);
        for (const Polygon& r : playArea)
            ringBounds(r, lo, hi);
    }
    else if (!specBounds(s, lo, hi))
        return issues;
    if (cell <= 0.0)
        cell = std::max({(hi.x - lo.x) / 512.0, (hi.y - lo.y) / 512.0, 100.0});

    for (const Layer& l : s.layers)
    {
        if (!(l.mustCover || l.exclusive))
            continue;
        const Raster r = rasterize(l, lo, hi, cell);
        std::vector<uint8_t> inPlay(r.winner.size(), 1);
        if (!playArea.empty())
            for (int z = 0; z < r.nz; ++z)
                for (int x = 0; x < r.nx; ++x)
                {
                    const Vec2 p = r.at(x, z);
                    inPlay[size_t(z) * size_t(r.nx) + size_t(x)] = std::any_of(playArea.begin(), playArea.end(), [&](const Polygon& a) { return inside(a, p); });
                }
        auto components = [&](auto&& want, const char* kind) {
            std::vector<uint8_t> seen(r.winner.size(), 0);
            for (size_t start = 0; start < r.winner.size(); ++start)
            {
                if (seen[start] || !want(start))
                    continue;
                std::deque<size_t> q{start};
                seen[start] = 1;
                std::vector<size_t> cells;
                while (!q.empty())
                {
                    const size_t i = q.front();
                    q.pop_front();
                    cells.push_back(i);
                    const int x = int(i % size_t(r.nx)), z = int(i / size_t(r.nx));
                    const int nb[4][2] = {{x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1}};
                    for (const auto& n : nb)
                        if (n[0] >= 0 && n[1] >= 0 && n[0] < r.nx && n[1] < r.nz)
                            if (const size_t j = size_t(n[1]) * size_t(r.nx) + size_t(n[0]); !seen[j] && want(j))
                            {
                                seen[j] = 1;
                                q.push_back(j);
                            }
                }
                if (int(cells.size()) < minCells)
                    continue;
                Issue is{kind, l.id, {}, Vec2(0.0), Vec2(1e300), Vec2(-1e300), double(cells.size()) * r.cell * r.cell / 10000.0, {}};
                std::set<std::string> zones;
                Vec2 sum(0.0);
                for (size_t i : cells)
                {
                    const Vec2 p = r.at(int(i % size_t(r.nx)), int(i / size_t(r.nx)));
                    sum += p;
                    is.lo = glm::min(is.lo, p - Vec2(r.cell * 0.5));
                    is.hi = glm::max(is.hi, p + Vec2(r.cell * 0.5));
                    if (std::string_view(kind) == "overlap")
                        for (const Zone* z : zonesAt(l, p))
                            zones.insert(z->id);
                }
                // The sample nearest the mean, so the marker sits on the issue even for a ring-shaped one.
                const Vec2 mean = sum / double(cells.size());
                double best = 1e300;
                for (size_t i : cells)
                    if (const Vec2 p = r.at(int(i % size_t(r.nx)), int(i / size_t(r.nx))); glm::length(p - mean) < best)
                    {
                        best = glm::length(p - mean);
                        is.at = p;
                    }
                is.zones.assign(zones.begin(), zones.end());
                char area[64];
                std::snprintf(area, sizeof area, "%.0f m2", is.areaM2);
                if (is.kind == "gap")
                    is.what = l.label + " leaves " + area + " of the play area uncovered";
                else
                {
                    std::string names;
                    for (const std::string& z : is.zones)
                        names += (names.empty() ? "" : ", ") + z;
                    is.what = l.label + " zones " + names + " overlap over " + area;
                }
                issues.push_back(std::move(is));
            }
        };
        if (l.mustCover)
            components([&](size_t i) { return inPlay[i] && r.count[i] == 0; }, "gap");
        if (l.exclusive)
            components([&](size_t i) { return r.count[i] >= 2; }, "overlap");
    }
    return issues;
}

json toJson(const Issue& i)
{
    json j = {{"kind", i.kind}, {"layer", i.layer}, {"zones", i.zones}, {"what", i.what}};
    if (i.kind == "gap" || i.kind == "overlap" || i.kind == "unnamed")
    {
        j["at"] = {i.at.x, i.at.y};
        j["bounds"] = {{i.lo.x, i.lo.y}, {i.hi.x, i.hi.y}};
    }
    if (i.areaM2 > 0.0)
        j["areaM2"] = i.areaM2;
    return j;
}

void maskOutline(const Mask& m, uint8_t threshold, double tolerance, std::vector<Polygon>& rings, std::vector<Polygon>& holes)
{
    glm::ivec2 lo, hi;
    if (!m.cellBounds(lo, hi))
        return;
    auto on = [&](int x, int z) { return m.cellValue(x, z) >= threshold; };
    // Directed boundary edges, counter-clockwise around each set cell seen with +X right and +Z up, keyed by start corner.
    std::multimap<std::pair<int, int>, std::pair<int, int>> edges;
    for (int z = lo.y; z <= hi.y; ++z)
        for (int x = lo.x; x <= hi.x; ++x)
        {
            if (!on(x, z))
                continue;
            if (!on(x, z - 1))
                edges.insert({{x, z}, {x + 1, z}});
            if (!on(x + 1, z))
                edges.insert({{x + 1, z}, {x + 1, z + 1}});
            if (!on(x, z + 1))
                edges.insert({{x + 1, z + 1}, {x, z + 1}});
            if (!on(x - 1, z))
                edges.insert({{x, z + 1}, {x, z}});
        }
    while (!edges.empty())
    {
        auto it = edges.begin();
        const std::pair<int, int> start = it->first;
        std::pair<int, int> at = it->second, prev = start;
        edges.erase(it);
        std::vector<glm::ivec2> loop{{start.first, start.second}};
        while (at != start)
        {
            loop.push_back({at.first, at.second});
            auto [a, b] = edges.equal_range(at);
            if (a == b)
                break;
            // At a corner two cells touch diagonally: take the left turn, so the cells stay separate loops.
            auto pick = a;
            const glm::ivec2 in(at.first - prev.first, at.second - prev.second);
            for (auto e = a; e != b; ++e)
            {
                const glm::ivec2 out(e->second.first - at.first, e->second.second - at.second);
                if (in.x * out.y - in.y * out.x > 0)
                    pick = e;
            }
            prev = at;
            at = pick->second;
            edges.erase(pick);
        }
        Polygon ring;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            const glm::ivec2 a = loop[(i + loop.size() - 1) % loop.size()], b = loop[i], c = loop[(i + 1) % loop.size()];
            const glm::ivec2 d1 = b - a, d2 = c - b;
            if (d1.x * d2.y - d1.y * d2.x != 0)
                ring.push_back(Vec2(b) * m.cell);
        }
        if (ring.size() < 3)
            continue;
        ring = simplifyRing(ring, tolerance);
        (area(ring) > 0.0 ? rings : holes).push_back(std::move(ring));
    }
}

std::vector<Polygon> convexPieces(const Polygon& poly0)
{
    Polygon poly = poly0;
    if (poly.size() < 3)
        return {};
    if (area(poly) < 0.0)
        std::reverse(poly.begin(), poly.end());
    if (convex(poly))
        return {poly};
    const size_t n = poly.size();
    // Ear clipping into triangles over a linked ring, resuming where the last ear was cut; only reflex corners can
    // lie inside an ear, so only they are tested.
    std::vector<size_t> prev(n), next(n);
    for (size_t i = 0; i < n; ++i)
        prev[i] = (i + n - 1) % n, next[i] = (i + 1) % n;
    auto reflex = [&](size_t i) { return cross(poly[prev[i]], poly[i], poly[next[i]]) <= 1e-9; };
    std::vector<char> isReflex(n);
    std::vector<size_t> reflexList;
    for (size_t i = 0; i < n; ++i)
        if ((isReflex[i] = reflex(i)))
            reflexList.push_back(i);
    std::vector<char> gone(n, 0);
    auto isEar = [&](size_t b) {
        if (isReflex[b])
            return false;
        const size_t a = prev[b], c = next[b];
        for (size_t k : reflexList)
            if (!gone[k] && isReflex[k] && k != a && k != b && k != c && inTriangle(poly[k], poly[a], poly[b], poly[c]))
                return false;
        return true;
    };
    std::vector<std::array<size_t, 3>> tris;
    size_t left = n, at = 0, sinceCut = 0;
    while (left > 3 && sinceCut <= left)
    {
        if (isEar(at))
        {
            const size_t a = prev[at], c = next[at];
            tris.push_back({a, at, c});
            gone[at] = 1;
            next[a] = c;
            prev[c] = a;
            --left;
            sinceCut = 0;
            isReflex[a] = reflex(a);
            isReflex[c] = reflex(c);
            at = a;
            continue;
        }
        at = next[at];
        ++sinceCut;
    }
    if (left == 3)
        tris.push_back({prev[at], at, next[at]});

    // Hertel-Mehlhorn: drop each diagonal whose two pieces stay convex together, one pass over the diagonals.
    std::vector<std::vector<size_t>> pieces;
    for (const auto& t : tris)
        pieces.push_back({t[0], t[1], t[2]});
    std::vector<char> alive(pieces.size(), 1);
    auto key = [](size_t u, size_t v) { return u < v ? (uint64_t(u) << 32) | v : (uint64_t(v) << 32) | u; };
    std::unordered_map<uint64_t, std::vector<size_t>> byEdge;
    for (size_t i = 0; i < pieces.size(); ++i)
        for (size_t e = 0; e < 3; ++e)
            byEdge[key(pieces[i][e], pieces[i][(e + 1) % 3])].push_back(i);
    auto asPoly = [&](const std::vector<size_t>& p) {
        Polygon out;
        out.reserve(p.size());
        for (size_t i : p)
            out.push_back(poly[i]);
        return out;
    };
    std::vector<uint64_t> diagonals;
    for (const auto& [k, owners] : byEdge)
        if (owners.size() == 2)
            diagonals.push_back(k);
    std::sort(diagonals.begin(), diagonals.end());
    for (uint64_t d : diagonals)
    {
        std::vector<size_t>& owners = byEdge[d];
        if (owners.size() != 2 || owners[0] == owners[1] || !alive[owners[0]] || !alive[owners[1]])
            continue;
        const size_t i = owners[0], j = owners[1];
        const std::vector<size_t>&A = pieces[i], &B = pieces[j];
        std::vector<size_t> joined;
        for (size_t ai = 0; ai < A.size() && joined.empty(); ++ai)
        {
            const size_t u = A[ai], v = A[(ai + 1) % A.size()];
            if (key(u, v) != d)
                continue;
            for (size_t bi = 0; bi < B.size(); ++bi)
                if (B[bi] == v && B[(bi + 1) % B.size()] == u)
                {
                    // A up to u, then B from u round to v, then A from v on.
                    for (size_t k = 0; k <= ai; ++k)
                        joined.push_back(A[k]);
                    for (size_t k = 2; k < B.size(); ++k)
                        joined.push_back(B[(bi + k) % B.size()]);
                    for (size_t k = ai + 1; k < A.size(); ++k)
                        joined.push_back(A[k]);
                    break;
                }
        }
        if (joined.empty() || !convex(asPoly(joined)))
            continue;
        // j's edges now belong to i.
        for (size_t e = 0; e < B.size(); ++e)
            for (size_t& o : byEdge[key(B[e], B[(e + 1) % B.size()])])
                if (o == j)
                    o = i;
        owners.clear();
        pieces[i] = std::move(joined);
        alive[j] = 0;
    }
    std::vector<Polygon> out;
    for (size_t i = 0; i < pieces.size(); ++i)
        if (alive[i])
            out.push_back(asPoly(pieces[i]));
    return out;
}

std::vector<Polygon> convexPieces(const Polygon& ring, const std::vector<Polygon>& holes)
{
    Vec2 lo(1e300), hi(-1e300);
    for (Vec2 p : ring)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    auto path = [](const Polygon& p) {
        Clipper2Lib::PathD out;
        for (Vec2 q : p)
            out.push_back({q.x, q.y});
        return out;
    };
    Clipper2Lib::PathsD cut;
    // A vertical cut through each hole's middle leaves every strip between cuts with no hole of its own.
    std::vector<double> xs = {lo.x - 1.0, hi.x + 1.0};
    for (const Polygon& h : holes)
    {
        Vec2 a(1e300), b(-1e300);
        for (Vec2 p : h)
        {
            a = glm::min(a, p);
            b = glm::max(b, p);
        }
        if (h.size() < 3 || b.x <= lo.x || a.x >= hi.x || b.y <= lo.y || a.y >= hi.y)
            continue;
        cut.push_back(path(h));
        xs.push_back((a.x + b.x) * 0.5);
    }
    if (cut.empty())
        return convexPieces(ring);
    std::sort(xs.begin(), xs.end());
    std::vector<Polygon> out;
    for (size_t i = 0; i + 1 < xs.size(); ++i)
    {
        if (xs[i + 1] - xs[i] < 1e-6)
            continue;
        const Clipper2Lib::PathsD strip = {{{xs[i], lo.y - 1.0}, {xs[i + 1], lo.y - 1.0}, {xs[i + 1], hi.y + 1.0}, {xs[i], hi.y + 1.0}}};
        const Clipper2Lib::PathsD inside = Clipper2Lib::Intersect({path(ring)}, strip, Clipper2Lib::FillRule::NonZero, 4);
        for (const Clipper2Lib::PathD& piece : Clipper2Lib::Difference(inside, cut, Clipper2Lib::FillRule::NonZero, 4))
        {
            if (Clipper2Lib::Area(piece) <= 0.0)
                continue;
            Polygon p;
            for (const Clipper2Lib::PointD& q : piece)
                p.push_back({q.x, q.y});
            for (Polygon& c : convexPieces(p))
                out.push_back(std::move(c));
        }
    }
    return out;
}

glm::vec3 zoneColor(const Layer& l, const Zone& z)
{
    if (l.density)
        return l.color;
    // Golden-ratio hue steps from the layer's own hue, so neighbours in one layer differ.
    const float base = float(hashText(l.type) % 997) / 997.0f;
    const float step = float(hashText(z.id) % 1000) * 0.618034f;
    return hsv(std::fmod(base + step, 1.0f), 0.55f, 0.95f);
}

json exportJson(const ZoneSpec& s, const std::string& maskPrefix, std::vector<ExportedMask>* masks)
{
    json layers = json::array();
    for (const Layer& l : s.layers)
    {
        json zones = json::array();
        for (const Zone& z : l.zones)
        {
            json attrs = l.defaults;
            attrs.update(z.attrs);
            json polys = json::array(), holes = json::array();
            for (const Polygon& r : z.polygons)
                polys.push_back(ringJson(r));
            for (const Polygon& r : z.holes)
                holes.push_back(ringJson(r));
            json zj = {{"id", z.id}, {"name", z.name}, {"priority", z.priority}, {"attrs", attrs}, {"ownAttrs", z.attrs}, {"polygons", polys}, {"holes", holes},
                       {"areaM2", areaM2(z, l)}};
            if (!z.nameSource.empty())
                zj["nameSource"] = z.nameSource;
            if (!z.source.is_null())
                zj["source"] = z.source;
            Vec2 lo, hi;
            if (bounds(z, lo, hi))
                zj["bounds"] = {{lo.x, lo.y}, {hi.x, hi.y}};
            glm::ivec2 a, b;
            if (z.mask.cellBounds(a, b))
            {
                std::vector<Polygon> outline, outlineHoles;
                maskOutline(z.mask, l.density ? 1 : 128, z.mask.cell * 0.5, outline, outlineHoles);
                json o = json::array(), oh = json::array();
                for (const Polygon& r : outline)
                    o.push_back(ringJson(r));
                for (const Polygon& r : outlineHoles)
                    oh.push_back(ringJson(r));
                ExportedMask em{l.id, z.id, a, b - a + 1, {}};
                em.values.resize(size_t(em.size.x) * size_t(em.size.y));
                for (int y = 0; y < em.size.y; ++y)
                    for (int x = 0; x < em.size.x; ++x)
                        em.values[size_t(y) * size_t(em.size.x) + size_t(x)] = z.mask.cellValue(a.x + x, a.y + y);
                const std::string file = maskPrefix + l.id + "_" + z.id + ".png";
                zj["mask"] = {{"image", file},
                              {"origin", {a.x * z.mask.cell, a.y * z.mask.cell}},
                              {"cell", z.mask.cell},
                              {"size", {em.size.x, em.size.y}},
                              {"rows", "+z"},
                              {"threshold", l.density ? 0.0 : 0.5},
                              {"outline", o},
                              {"outlineHoles", oh}};
                if (masks)
                    masks->push_back(std::move(em));
            }
            zones.push_back(std::move(zj));
        }
        layers.push_back({{"id", l.id},
                          {"type", l.type},
                          {"label", l.label},
                          {"priority", l.priority},
                          {"exclusive", l.exclusive},
                          {"mustCover", l.mustCover},
                          {"visible", l.visible},
                          {"density", l.density},
                          {"color", {l.color.r, l.color.g, l.color.b}},
                          {"defaults", l.defaults},
                          {"dataLayer", l.dataLayer.empty() ? l.label : l.dataLayer},
                          {"zones", zones}});
    }
    json types = json::array();
    for (const LayerType& t : s.types)
        types.push_back(toJson(t));
    json play = json::array();
    for (const Polygon& r : s.playArea)
        play.push_back(ringJson(r));
    return {{"format", "lila.zones"}, {"version", 1}, {"units", "cm"}, {"axes", "x east, z south (Lila world, Y up)"},
            {"culture", s.culture}, {"maskCell", s.maskCell}, {"playArea", play}, {"types", types}, {"layers", layers}};
}

ZoneSpec importJson(const json& doc, const std::function<std::vector<uint8_t>(const std::string&, glm::ivec2)>& readMask)
{
    if (doc.value("format", std::string()) != "lila.zones")
        bad("format", "is not lila.zones");
    ZoneSpec s;
    s.culture = doc.value("culture", s.culture);
    s.maskCell = doc.value("maskCell", s.maskCell);
    s.playArea = ringsFrom(doc.value("playArea", json::array()), "playArea");
    for (const json& t : doc.value("types", json::array()))
        s.types.push_back(typeFromJson(t));
    for (const json& lj : doc.value("layers", json::array()))
    {
        const LayerType* t = s.type(lj.value("type", std::string()));
        if (!t)
            bad("layers[].type", "'" + lj.value("type", std::string()) + "' is not a known layer type");
        Layer l = makeLayer(*t, lj.value("id", t->id));
        l.label = lj.value("label", l.label);
        l.priority = lj.value("priority", 0);
        l.exclusive = lj.value("exclusive", l.exclusive);
        l.mustCover = lj.value("mustCover", l.mustCover);
        l.visible = lj.value("visible", true);
        l.density = lj.value("density", l.density);
        l.color = colorFrom(lj.value("color", json()), l.color);
        l.defaults = lj.value("defaults", l.defaults);
        if (const std::string dl = lj.value("dataLayer", std::string()); dl != l.label)
            l.dataLayer = dl;
        for (const json& zj : lj.value("zones", json::array()))
        {
            Zone z;
            z.id = zj.value("id", std::string());
            z.name = zj.value("name", std::string());
            z.nameSource = zj.value("nameSource", std::string());
            z.priority = zj.value("priority", 0);
            z.attrs = zj.value("ownAttrs", zj.value("attrs", json::object()));
            z.polygons = ringsFrom(zj.value("polygons", json::array()), "polygons");
            z.holes = ringsFrom(zj.value("holes", json::array()), "holes");
            z.source = zj.value("source", json());
            z.mask.cell = s.maskCell;
            if (const json m = zj.value("mask", json()); m.is_object())
            {
                const glm::ivec2 size(m["size"][0].get<int>(), m["size"][1].get<int>());
                const double cell = m.value("cell", s.maskCell);
                if (std::abs(cell - s.maskCell) > 1e-6)
                    bad("mask.cell", "differs from maskCell");
                const glm::ivec2 a(int(std::lround(m["origin"][0].get<double>() / cell)), int(std::lround(m["origin"][1].get<double>() / cell)));
                const std::vector<uint8_t> v = readMask(m.value("image", std::string()), size);
                if (v.size() != size_t(size.x) * size_t(size.y))
                    bad("mask.image", "'" + m.value("image", std::string()) + "' is not " + std::to_string(size.x) + " x " + std::to_string(size.y));
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                        if (const uint8_t c = v[size_t(y) * size_t(size.x) + size_t(x)])
                            z.mask.setCell(a.x + x, a.y + y, c);
            }
            l.zones.push_back(std::move(z));
        }
        s.layers.push_back(std::move(l));
    }
    return s;
}

std::string base64(const std::vector<uint8_t>& bytes)
{
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t n = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += k[(n >> 18) & 63];
        out += k[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? k[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? k[n & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> unbase64(std::string_view text)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text)
    {
        const int v = val(c);
        if (v < 0)
            continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}
} // namespace openworld::zones
