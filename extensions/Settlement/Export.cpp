// Settlement export data: World Partition cells, live-sync manifests, and the GeoJSON, USD, DXF and engine JSON files.

#include "Export.h"
#include "Generators.h"
#include "Parcels.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <format>
#include <set>

namespace settlement
{
namespace xport
{
/** GeoJSON position: metres, x east, -z north. */
json geoPoint(Vec2 q) { return json::array({q.x / 100.0, -q.y / 100.0}); }

/** A closed GeoJSON ring, counter-clockwise as RFC 7946 asks (flipping z reverses Lila's winding). */
json geoRing(const Polygon& p)
{
    json ring = json::array();
    for (auto it = p.pts.rbegin(); it != p.pts.rend(); ++it)
        ring.push_back(geoPoint(*it));
    if (!p.pts.empty())
        ring.push_back(geoPoint(p.pts.back()));
    return ring;
}

json geoLine(std::span<const glm::dvec3> pts)
{
    json a = json::array();
    for (const glm::dvec3& q : pts)
        a.push_back(geoPoint({q.x, q.z}));
    return a;
}

json feature(json geometry, json properties) { return {{"type", "Feature"}, {"geometry", std::move(geometry)}, {"properties", std::move(properties)}}; }

double lowestGround(const Ground& ground, const Polygon& p)
{
    double lo = std::numeric_limits<double>::max();
    for (const Vec2& q : p.pts)
        lo = std::min(lo, ground.height(q));
    return p.pts.empty() ? 0.0 : lo;
}

std::string hex(uint64_t v) { return std::format("{:016x}", v); }

const char* sideKind(int t) { return t == 0 ? "street" : t == 1 ? "party" : "open"; }

json lotFields(const Lot& lot)
{
    return {{"id", lot.id},     {"key", lotKey(lot)},   {"block", lot.block}, {"use", useName(lot.use)}, {"areaM2", lot.area / kM2},
            {"frontages", lot.frontages}, {"corner", lot.corner}, {"frontClass", className(lot.frontClass)}, {"era", lot.era},
            {"landmark", lot.landmark}, {"wealth", lot.wealth}, {"route", routeName(lot.route)}};
}

json envelopeFields(const Layout& l, const Envelope& e, const std::string& key)
{
    return {{"lot", e.lot},
            {"key", key},
            {"use", useName(e.use)},
            {"floors", e.floors},
            {"floorHeight", l.profile.floorHeight},
            {"height", e.height},
            {"base", e.base},
            {"ground", e.ground},
            {"top", e.top},
            {"roof", e.roof},
            {"roofHeight", e.roofHeight},
            {"era", e.era},
            {"eraName", e.eraName},
            {"groundUse", useName(e.groundUse)},
            {"corner", e.corner},
            {"landmark", e.landmark},
            {"route", routeName(e.route)}};
}

json roadFields(const Layout& l, const StreetLine& s)
{
    const RoadProfile& p = l.roads[size_t(s.profile)];
    return {{"key", roadKey(s)}, {"class", className(s.cls)}, {"profile", p.name}, {"width", s.width}, {"lanes", p.lanes},
            {"oneWay", p.oneWay}, {"speed", p.speed}, {"surface", s.surface}, {"path", s.path}};
}

/** Lot keys by lot id, for envelopes. */
std::map<int, std::string> lotKeys(const Layout& l)
{
    std::map<int, std::string> out;
    for (const Lot& lot : l.lots)
        out[lot.id] = lotKey(lot);
    return out;
}

/** Shortest round-trip text of a number, for USD. */
std::string num(double v) { return std::format("{}", std::abs(v) < 1e-12 ? 0.0 : v); }

/** A USD prim name: letters, digits and underscores, not starting with a digit. */
std::string primName(std::string_view s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    return out;
}

std::string usdString(std::string_view s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

/** Unique prim names within one scope. */
struct PrimNames
{
    std::set<std::string> taken;
    std::string operator()(std::string base)
    {
        base = primName(base);
        std::string n = base;
        for (int k = 2; !taken.insert(n).second; ++k)
            n = base + "_" + std::to_string(k);
        return n;
    }
};

struct Usda
{
    std::string text;
    int depth = 0;

    void line(std::string_view s)
    {
        text.append(size_t(depth) * 4, ' ');
        text += s;
        text += '\n';
    }
    void open(std::string_view header)
    {
        line(header);
        line("{");
        ++depth;
    }
    void close()
    {
        --depth;
        line("}");
    }
    void points(std::span<const glm::dvec3> pts)
    {
        std::string s = "point3f[] points = [";
        for (size_t k = 0; k < pts.size(); ++k)
            s += std::format("{}({}, {}, {})", k ? ", " : "", num(pts[k].x), num(pts[k].y), num(pts[k].z));
        line(s + "]");
    }
    void ints(std::string_view decl, std::span<const int> v)
    {
        std::string s = std::string(decl) + " = [";
        for (size_t k = 0; k < v.size(); ++k)
            s += (k ? ", " : "") + std::to_string(v[k]);
        line(s + "]");
    }
    void attr(std::string_view type, std::string_view name, const json& v)
    {
        std::string value;
        if (v.is_string())
            value = usdString(v.get<std::string>());
        else if (v.is_boolean())
            value = v.get<bool>() ? "true" : "false";
        else if (v.is_number_integer())
            value = std::to_string(v.get<int64_t>());
        else if (v.is_number())
            value = num(v.get<double>());
        else
            return;
        line(std::format("custom {} lila:{} = {}", type, name, value));
    }
    /** The fields of a JSON object as lila: attributes, typed by their values. */
    void attrs(const json& fields)
    {
        for (const auto& [k, v] : fields.items())
            attr(v.is_string() ? "string" : v.is_boolean() ? "bool" : v.is_number_integer() ? "int64" : "double", k, v);
    }
    void polygonMesh(const Polygon& p, double y)
    {
        std::vector<glm::dvec3> pts;
        for (const Vec2& q : p.pts)
            pts.push_back({q.x, y, q.y});
        std::vector<int> idx;
        for (int k = int(pts.size()) - 1; k >= 0; --k)
            idx.push_back(k); // reversed, so the face points up (+Y)
        const int count[] = {int(pts.size())};
        ints("int[] faceVertexCounts", count);
        ints("int[] faceVertexIndices", idx);
        points(pts);
    }
};

std::vector<glm::dvec3> flat(const Polygon& p, double y)
{
    std::vector<glm::dvec3> pts;
    for (const Vec2& q : p.pts)
        pts.push_back({q.x, y, q.y});
    return pts;
}

void addPrism(geo::Mesh& m, const Envelope& e, glm::dvec3 offset)
{
    const size_t n = e.footprint.size();
    if (n < 3)
        return;
    std::vector<geo::Index> lo, hi;
    for (const Vec2& q : e.footprint.pts)
    {
        lo.push_back(m.addPoint(glm::dvec3(q.x, e.base, q.y) - offset));
        hi.push_back(m.addPoint(glm::dvec3(q.x, e.top, q.y) - offset));
    }
    for (size_t k = 0; k < n; ++k)
    {
        const size_t j = (k + 1) % n;
        m.addFace({lo[k], hi[k], hi[j], lo[j]});
    }
    m.addFace(std::vector<geo::Index>(hi.rbegin(), hi.rend()));
    m.addFace(lo);
}

/** Engine space, metres: Unity flips z (left-handed). */
struct EngineSpace
{
    EngineAxes axes;
    glm::dvec3 operator()(glm::dvec3 p) const { return glm::dvec3(p.x, p.y, axes == EngineAxes::Unity ? -p.z : p.z) / 100.0; }
    void push(json& a, glm::dvec3 p) const
    {
        const glm::dvec3 q = (*this)(p);
        a.push_back(q.x);
        a.push_back(q.y);
        a.push_back(q.z);
    }
    json flatPoints(std::span<const glm::dvec3> pts) const
    {
        json a = json::array();
        for (const glm::dvec3& p : pts)
            push(a, p);
        return a;
    }
    json vec(glm::dvec3 p) const
    {
        json a = json::array();
        push(a, p);
        return a;
    }
    /** Mirroring z negates the quaternion's x and y. */
    json quat(glm::dvec4 q) const
    {
        return axes == EngineAxes::Unity ? json::array({-q.x, -q.y, q.z, q.w}) : json::array({q.x, q.y, q.z, q.w});
    }
};
} // namespace xport

using namespace xport;

// ---------------------------------------------------------------------------------------------------------------------
// Cells

std::string Cell::name() const { return std::to_string(x) + "_" + std::to_string(z); }

Cell cellAt(Vec2 p, double size) { return {int64_t(std::floor(p.x / size)), int64_t(std::floor(p.y / size))}; }

std::vector<std::pair<Cell, std::vector<glm::dvec3>>> cutByCells(std::span<const glm::dvec3> pts, double size)
{
    std::vector<std::pair<Cell, std::vector<glm::dvec3>>> out;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
    {
        const glm::dvec3 a = pts[k], b = pts[k + 1];
        std::vector<double> ts = {0.0, 1.0};
        auto borders = [&](double a0, double b0) {
            if (std::abs(b0 - a0) < 1e-9)
                return;
            const double lo = std::min(a0, b0), hi = std::max(a0, b0);
            for (double i = std::floor(lo / size) + 1.0; i * size < hi; i += 1.0)
                ts.push_back((i * size - a0) / (b0 - a0));
        };
        borders(a.x, b.x);
        borders(a.z, b.z);
        std::ranges::sort(ts);
        for (size_t i = 0; i + 1 < ts.size(); ++i)
        {
            if (ts[i + 1] - ts[i] < 1e-9)
                continue;
            const glm::dvec3 p0 = a + (b - a) * ts[i], p1 = a + (b - a) * ts[i + 1], mid = 0.5 * (p0 + p1);
            const Cell c = cellAt({mid.x, mid.z}, size);
            if (!out.empty() && out.back().first == c && glm::length(out.back().second.back() - p0) < 1e-6)
                out.back().second.push_back(p1);
            else
                out.push_back({c, {p0, p1}});
        }
    }
    return out;
}

std::map<Cell, CellContents> partition(const Layout& l, const RoadNetwork& net, double size)
{
    LILA_ZONE();

    std::map<Cell, CellContents> out;
    for (size_t k = 0; k < l.lots.size(); ++k)
        out[cellAt(centroid(l.lots[k].outline), size)].lots.push_back(int(k));
    for (size_t k = 0; k < l.envelopes.size(); ++k)
        out[cellAt(centroid(l.envelopes[k].footprint), size)].envelopes.push_back(int(k));
    for (size_t k = 0; k < net.junctions.size(); ++k)
        out[cellAt({net.junctions[k].p.x, net.junctions[k].p.z}, size)].junctions.push_back(int(k));
    for (size_t k = 0; k < net.parking.size(); ++k)
        out[cellAt({net.parking[k].p.x, net.parking[k].p.z}, size)].parking.push_back(int(k));
    for (size_t k = 0; k < l.placements.size(); ++k)
        out[cellAt({l.placements[k].position.x, l.placements[k].position.z}, size)].placements.push_back(int(k));
    for (size_t k = 0; k < net.lines.size(); ++k)
        for (auto& [cell, pts] : cutByCells(net.lines[k].pts, size))
            out[cell].roads.push_back({int(k), std::move(pts)});
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Stable ids and live sync

std::string lotKey(const Lot& lot)
{
    if (lot.key)
        return "lot:" + hex(lot.key);
    const Vec2 c = centroid(lot.outline);
    const uint64_t x = uint64_t(std::llround(c.x / 100.0)), z = uint64_t(std::llround(c.y / 100.0));
    return "lot:" + hex(hash64(x * 0x9E3779B97F4A7C15ull ^ hash64(z)));
}

std::string roadKey(const StreetLine& s)
{
    if (s.surface || s.street == 0)
        return "path:" + std::to_string(s.path);
    if (s.pts.empty())
        return "street:" + hex(s.street);
    // A street runs through junctions as several lines; its ends name the line.
    auto q = [](const glm::dvec3& p) { return std::pair{std::llround(p.x / 10.0), std::llround(p.z / 10.0)}; };
    const auto [a, b] = std::minmax(q(s.pts.front()), q(s.pts.back()));
    const uint64_t ends = hash64(hash64(hash64(hash64(uint64_t(a.first)) ^ uint64_t(a.second)) ^ uint64_t(b.first)) ^ uint64_t(b.second));
    return "street:" + hex(s.street) + "." + hex(ends & 0xffffffffull);
}

std::string junctionKey(const Junction& j)
{
    const uint64_t x = uint64_t(std::llround(j.p.x / 100.0)), z = uint64_t(std::llround(j.p.z / 100.0));
    return "junction:" + hex(hash64(x * 0x9E3779B97F4A7C15ull ^ hash64(z)));
}

std::vector<Placement> placementsFrom(const json& list)
{
    if (!list.is_array())
        throw std::runtime_error("give a list of placements");
    auto vec = [](const json& e, const char* field, size_t n, size_t k) {
        const json& v = e[field];
        if (!v.is_array() || v.size() != n)
            throw std::runtime_error("placement " + std::to_string(k) + ": " + field + " takes " + std::to_string(n) + " numbers");
        std::vector<double> out;
        for (const json& x : v)
        {
            if (!x.is_number())
                throw std::runtime_error("placement " + std::to_string(k) + ": " + field + " takes numbers");
            out.push_back(x.get<double>());
        }
        return out;
    };
    std::vector<Placement> out;
    for (size_t k = 0; k < list.size(); ++k)
    {
        const json& e = list[k];
        if (!e.is_object())
            throw std::runtime_error("placement " + std::to_string(k) + " is not an object");
        Placement p;
        p.id = e.contains("id") ? (e["id"].is_string() ? e["id"].get<std::string>() : e["id"].dump()) : std::string();
        p.asset = e.value("asset", std::string());
        if (p.id.empty() || p.asset.empty())
            throw std::runtime_error("placement " + std::to_string(k) + " needs an id and an asset");
        if (e.contains("position"))
        {
            const auto v = vec(e, "position", 3, k);
            p.position = {v[0], v[1], v[2]};
        }
        if (e.contains("rotation"))
        {
            const auto v = vec(e, "rotation", 4, k);
            p.rotation = {v[0], v[1], v[2], v[3]};
        }
        if (e.contains("scale") && e["scale"].is_number())
            p.scale = glm::dvec3(e["scale"].get<double>());
        else if (e.contains("scale"))
        {
            const auto v = vec(e, "scale", 3, k);
            p.scale = {v[0], v[1], v[2]};
        }
        for (const json& t : e.value("tags", json::array()))
            if (t.is_string())
                p.tags.push_back(t.get<std::string>());
        out.push_back(std::move(p));
    }
    return out;
}

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

std::map<std::string, uint64_t> elementHashes(const Layout& l, const RoadNetwork& net)
{
    LILA_ZONE();

    std::map<std::string, uint64_t> out;
    // Two elements with one id (a street split into lines) count as one, their hashes combined in order.
    auto add = [&](const std::string& id, const json& data) {
        const uint64_t h = hashText(data.dump());
        auto [it, fresh] = out.try_emplace(id, h);
        if (!fresh)
            it->second = hash64(it->second ^ h);
    };
    for (const Lot& lot : l.lots)
    {
        json d = lotFields(lot);
        d.erase("id");
        d.erase("block");
        json pts = json::array();
        for (const Vec2& q : lot.outline.pts)
            pts.push_back({q.x, q.y});
        d["outline"] = pts;
        add(lotKey(lot), d);
    }
    const std::map<int, std::string> keys = lotKeys(l);
    for (const Envelope& e : l.envelopes)
    {
        const auto it = keys.find(e.lot);
        const std::string key = it != keys.end() ? it->second : "lot:" + std::to_string(e.lot);
        json d = envelopeFields(l, e, key);
        d.erase("lot");
        json pts = json::array();
        for (const Vec2& q : e.footprint.pts)
            pts.push_back({q.x, q.y});
        d["footprint"] = pts;
        add("envelope:" + key.substr(4), d);
    }
    for (const StreetLine& s : net.lines)
    {
        json d = roadFields(l, s);
        json pts = json::array();
        for (const glm::dvec3& q : s.pts)
            pts.push_back({q.x, q.y, q.z});
        d["points"] = pts;
        add(roadKey(s), d);
    }
    for (const Junction& j : net.junctions)
        add(junctionKey(j), {{"control", controlName(j.control)}, {"ends", j.ends.size()}, {"radius", j.radius}, {"y", j.p.y}});
    for (const Placement& p : l.placements)
        add("place:" + p.id, {{"asset", p.asset},
                              {"p", {p.position.x, p.position.y, p.position.z}},
                              {"r", {p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w}},
                              {"s", {p.scale.x, p.scale.y, p.scale.z}},
                              {"tags", p.tags}});
    return out;
}

json syncManifest(std::span<const SyncItem> items, const std::map<std::string, uint64_t>& elements)
{
    json it = json::object();
    for (const SyncItem& i : items)
        it[i.key] = {{"kind", i.kind}, {"hash", hex(i.hash)}};
    json el = json::object();
    for (const auto& [id, h] : elements)
        el[id] = hex(h);
    return {{"format", "lila.settlement.sync"}, {"version", 1}, {"items", it}, {"elements", el}};
}

SyncDiff diffSync(const json& previous, std::span<const SyncItem> items, const std::map<std::string, uint64_t>& elements)
{
    LILA_ZONE();

    SyncDiff d;
    const json before = previous.is_object() ? previous.value("items", json::object()) : json::object();
    std::set<std::string> now;
    for (const SyncItem& i : items)
    {
        now.insert(i.key);
        if (!before.contains(i.key))
            d.added.push_back(i.key);
        else if (before[i.key].value("hash", std::string()) != hex(i.hash))
            d.changed.push_back(i.key);
        else
            d.unchanged.push_back(i.key);
    }
    for (const auto& [key, v] : before.items())
        if (!now.contains(key))
            d.removed.push_back(key);
    const json beforeEl = previous.is_object() ? previous.value("elements", json::object()) : json::object();
    for (const auto& [id, h] : elements)
    {
        if (!beforeEl.contains(id))
            d.addedIds.push_back(id);
        else if (beforeEl[id].get<std::string>() != hex(h))
            d.changedIds.push_back(id);
    }
    for (const auto& [id, v] : beforeEl.items())
        if (!elements.contains(id))
            d.removedIds.push_back(id);
    std::ranges::sort(d.removed);
    return d;
}

std::string safeName(std::string_view s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    while (!out.empty() && out.front() == '_')
        out.erase(out.begin());
    return out.empty() ? std::string("asset") : out;
}

uint64_t meshHash(const geo::Mesh& m)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        for (geo::Index p : m.facePoints(f))
        {
            const glm::dvec3 q = m.position(p);
            mix(&q, sizeof(q));
        }
        const uint64_t sep = 0xFFFFFFFFull;
        mix(&sep, sizeof(sep));
    }
    return h;
}

std::map<Cell, geo::Mesh> meshByCell(const geo::Mesh& m, double size)
{
    LILA_ZONE();

    std::map<Cell, std::vector<geo::Index>> byCell;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        glm::dvec3 c(0.0);
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (geo::Index p : pts)
            c += m.position(p);
        c /= double(pts.size());
        byCell[cellAt({c.x, c.z}, size)].push_back(f);
    }
    std::map<Cell, geo::Mesh> out;
    for (const auto& [cell, faces] : byCell)
    {
        const Vec2 mid = cell.center(size);
        const glm::dvec3 centre(mid.x, 0.0, mid.y);
        geo::Mesh& chunk = out[cell];
        std::map<geo::Index, geo::Index> remap;
        for (geo::Index f : faces)
        {
            std::vector<geo::Index> ids;
            for (geo::Index p : m.facePoints(f))
            {
                auto [it, fresh] = remap.try_emplace(p, 0);
                if (fresh)
                    it->second = chunk.addPoint(m.position(p) - centre);
                ids.push_back(it->second);
            }
            chunk.addFace(ids);
        }
    }
    return out;
}

std::vector<SyncItem> syncItems(const Layout& l, const RoadNetwork& net, const std::map<Cell, CellContents>& cells, const SyncOptions& o,
                                const std::map<Cell, uint64_t>& roadMeshes)
{
    LILA_ZONE();

    const std::map<std::string, uint64_t> elements = elementHashes(l, net);
    const std::map<int, std::string> keys = lotKeys(l);
    const uint64_t settings = hashText(o.settings);
    std::vector<std::string> roadIds(net.lines.size()), lotIds(l.lots.size()), envelopeIds(l.envelopes.size()), junctionIds(net.junctions.size());
    for (size_t k = 0; k < net.lines.size(); ++k)
        roadIds[k] = roadKey(net.lines[k]);
    for (size_t k = 0; k < l.lots.size(); ++k)
        lotIds[k] = lotKey(l.lots[k]);
    for (size_t k = 0; k < l.envelopes.size(); ++k)
    {
        const auto it = keys.find(l.envelopes[k].lot);
        envelopeIds[k] = "envelope:" + (it != keys.end() ? it->second.substr(4) : std::to_string(l.envelopes[k].lot));
    }
    for (size_t k = 0; k < net.junctions.size(); ++k)
        junctionIds[k] = junctionKey(net.junctions[k]);

    std::vector<SyncItem> items;
    // An item's hash: the settings, then its elements' hashes in order, then anything else it holds.
    auto make = [&](std::string key, const char* kind, std::vector<std::string> ids, uint64_t extra, Cell cell = {}) {
        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        uint64_t h = hash64(settings ^ hashText(kind));
        for (const std::string& id : ids)
        {
            const auto it = elements.find(id);
            h = hash64(h ^ hashText(id) ^ (it != elements.end() ? it->second : 0));
        }
        items.push_back({std::move(key), kind, hash64(h ^ extra), std::move(ids), cell, {}});
    };
    auto piecesHash = [](const std::vector<RoadPiece>& pieces) {
        uint64_t h = 0;
        for (const RoadPiece& p : pieces)
            for (const glm::dvec3& q : p.pts)
                h = hash64(h ^ hashText(std::format("{},{},{}", q.x, q.y, q.z)));
        return h;
    };

    uint64_t levelHash = 0;
    if (l.gen)
        for (const LevelLine& line : levelLines(l, Ground::plane()))
        {
            levelHash = hash64(levelHash ^ hashText(line.kind) ^ uint64_t(line.layer) * 131 ^ uint64_t(line.cls));
            for (const glm::dvec3& q : line.pts)
                levelHash = hash64(levelHash ^ hashText(std::format("{},{},{}", q.x, q.y, q.z)));
        }
    if (levelHash && o.roads != "none")
        make(o.key + "/levels", "levels", {}, levelHash);

    if (o.roads == "meshes")
        for (const auto& [cell, mesh] : roadMeshes)
        {
            std::vector<std::string> ids;
            if (const auto it = cells.find(cell); it != cells.end())
                for (const RoadPiece& p : it->second.roads)
                    ids.push_back(roadIds[size_t(p.line)]);
            make(o.key + "/roads/" + cell.name(), "roads", std::move(ids), mesh, cell);
        }
    else if (o.roads != "none")
    {
        std::vector<std::string> ground, surface;
        for (size_t k = 0; k < net.lines.size(); ++k)
            (net.lines[k].surface ? surface : ground).push_back(roadIds[k]);
        if (!ground.empty())
            make(o.key + "/roads", "roads", std::move(ground), hashText(o.roads));
        if (!surface.empty())
            make(o.key + "/paths", "paths", std::move(surface), 0);
    }
    for (const auto& [cell, c] : cells)
    {
        if (o.massing && !c.envelopes.empty())
        {
            std::vector<std::string> ids;
            for (int e : c.envelopes)
                ids.push_back(envelopeIds[size_t(e)]);
            make(o.key + "/massing/" + cell.name(), "massing", std::move(ids), 0, cell);
        }
        if (o.pcg && o.partition && (!c.lots.empty() || !c.envelopes.empty() || !c.roads.empty() || !c.junctions.empty() || !c.parking.empty()))
        {
            std::vector<std::string> ids;
            for (int k : c.lots)
                ids.push_back(lotIds[size_t(k)]);
            for (int k : c.envelopes)
                ids.push_back(envelopeIds[size_t(k)]);
            for (const RoadPiece& p : c.roads)
                ids.push_back(roadIds[size_t(p.line)]);
            for (int k : c.junctions)
                ids.push_back(junctionIds[size_t(k)]);
            make(o.key + "/pcg/" + cell.name(), "pcg", std::move(ids), hash64(piecesHash(c.roads) ^ c.parking.size()), cell);
        }
        std::map<std::string, std::vector<std::string>> byAsset;
        for (int k : c.placements)
            byAsset[l.placements[size_t(k)].asset].push_back("place:" + l.placements[size_t(k)].id);
        for (auto& [asset, ids] : byAsset)
        {
            make(o.key + "/place/" + cell.name() + "/" + safeName(asset), "place", std::move(ids), hashText(asset), cell);
            items.back().asset = asset;
        }
    }
    if (o.pcg && !o.partition)
    {
        std::vector<std::string> ids = roadIds;
        ids.insert(ids.end(), lotIds.begin(), lotIds.end());
        ids.insert(ids.end(), envelopeIds.begin(), envelopeIds.end());
        ids.insert(ids.end(), junctionIds.begin(), junctionIds.end());
        make(o.key + "/pcg", "pcg", std::move(ids), net.parking.size());
    }
    if (o.zoneGraph && !net.lines.empty())
    {
        std::vector<std::string> ids = roadIds;
        ids.insert(ids.end(), junctionIds.begin(), junctionIds.end());
        make(o.key + "/zoneGraph", "zoneGraph", std::move(ids), hash64(net.parking.size() ^ levelHash));
    }
    return items;
}

json toJson(const SyncDiff& d)
{
    return {{"added", d.added},       {"changed", d.changed},       {"removed", d.removed},      {"unchanged", d.unchanged.size()},
            {"addedIds", d.addedIds}, {"changedIds", d.changedIds}, {"removedIds", d.removedIds}};
}

// ---------------------------------------------------------------------------------------------------------------------
// Files

std::vector<LaneLine> laneLines(const Layout& l, const RoadNetwork& net)
{
    LILA_ZONE();

    std::vector<LaneLine> out;
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& s = net.lines[k];
        const RoadProfile& p = l.roads[size_t(s.profile)];
        if (s.cls == StreetClass::Path || p.lanes <= 0 || s.pts.size() < 2)
            continue;
        const int back = p.oneWay ? 0 : p.lanes / 2, fwd = p.lanes - back;
        const double total = p.lanes * p.laneWidth + p.median;
        // Per point, the right-hand normal in XZ averaged over the segments either side.
        std::vector<Vec2> right(s.pts.size(), Vec2(0.0));
        for (size_t i = 0; i + 1 < s.pts.size(); ++i)
        {
            Vec2 d(s.pts[i + 1].x - s.pts[i].x, s.pts[i + 1].z - s.pts[i].z);
            const double len = glm::length(d);
            if (len < 1e-9)
                continue;
            d /= len;
            const Vec2 r(-d.y, d.x);
            right[i] += r;
            right[i + 1] += r;
        }
        for (Vec2& r : right)
            if (const double len = glm::length(r); len > 1e-9)
                r /= len;
        auto lane = [&](int index, double across, bool forward) {
            LaneLine ln{int(k), index, forward, p.laneWidth, {}};
            const double off = across - 0.5 * total;
            for (size_t i = 0; i < s.pts.size(); ++i)
                ln.pts.push_back(s.pts[i] + glm::dvec3(right[i].x * off, 0.0, right[i].y * off));
            if (!forward)
                std::ranges::reverse(ln.pts);
            out.push_back(std::move(ln));
        };
        int index = 0;
        for (int b = 0; b < back; ++b)
            lane(index++, (b + 0.5) * p.laneWidth, false);
        for (int f = 0; f < fwd; ++f)
            lane(index++, back * p.laneWidth + p.median + (f + 0.5) * p.laneWidth, true);
    }
    return out;
}

json toGeoJson(const Layout& l, const RoadNetwork& net, const std::string& name)
{
    LILA_ZONE();

    json features = json::array();
    const StreetGraph& g = l.graph;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const StreetEdge& x = g.edges[e];
        features.push_back(feature({{"type", "LineString"}, {"coordinates", json::array({geoPoint(g.nodes[size_t(x.a)].p), geoPoint(g.nodes[size_t(x.b)].p)})}},
                                   {{"layer", "street"},
                                    {"edge", int(e)},
                                    {"a", x.a},
                                    {"b", x.b},
                                    {"key", x.street ? "street:" + hex(x.street) : std::string()},
                                    {"class", className(x.cls)},
                                    {"width", edgeWidth(l, int(e))},
                                    {"profile", edgeProfile(l, int(e)).name},
                                    {"era", x.era}}));
    }
    for (const Junction& j : net.junctions)
        features.push_back(feature({{"type", "Point"}, {"coordinates", geoPoint({j.p.x, j.p.z})}},
                                   {{"layer", "junction"},
                                    {"key", junctionKey(j)},
                                    {"control", controlName(j.control)},
                                    {"approaches", j.ends.size()},
                                    {"phases", j.phases},
                                    {"walks", j.walks},
                                    {"radius", j.radius},
                                    {"height", j.p.y}}));
    for (const Block& b : l.blocks)
        features.push_back(feature({{"type", "Polygon"}, {"coordinates", json::array({geoRing(b.outline)})}},
                                   {{"layer", "block"}, {"id", b.id}, {"use", useName(b.use)}, {"areaM2", b.area / kM2}}));
    for (const Lot& lot : l.lots)
    {
        json props = lotFields(lot);
        props["layer"] = "lot";
        features.push_back(feature({{"type", "Polygon"}, {"coordinates", json::array({geoRing(lot.outline)})}}, props));
    }
    const std::map<int, std::string> keys = lotKeys(l);
    for (const Envelope& e : l.envelopes)
    {
        const auto it = keys.find(e.lot);
        json props = envelopeFields(l, e, it != keys.end() ? "envelope:" + it->second.substr(4) : std::string());
        props["layer"] = "envelope";
        json sides = json::array();
        for (int t : e.footprint.tag)
            sides.push_back(sideKind(t));
        props["sides"] = sides;
        features.push_back(feature({{"type", "Polygon"}, {"coordinates", json::array({geoRing(e.footprint)})}}, props));
    }
    for (const LaneLine& ln : laneLines(l, net))
    {
        const StreetLine& s = net.lines[size_t(ln.line)];
        features.push_back(feature({{"type", "LineString"}, {"coordinates", geoLine(ln.pts)}},
                                   {{"layer", "lane"},
                                    {"road", roadKey(s)},
                                    {"lane", ln.lane},
                                    {"forward", ln.forward},
                                    {"width", ln.width},
                                    {"speed", l.roads[size_t(s.profile)].speed}}));
    }
    for (const Placement& p : l.placements)
        features.push_back(feature({{"type", "Point"}, {"coordinates", geoPoint({p.position.x, p.position.z})}},
                                   {{"layer", "placement"},
                                    {"key", "place:" + p.id},
                                    {"asset", p.asset},
                                    {"height", p.position.y},
                                    {"rotation", {p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w}},
                                    {"scale", {p.scale.x, p.scale.y, p.scale.z}},
                                    {"tags", p.tags}}));
    return {{"type", "FeatureCollection"},
            {"name", name},
            {"lila", {{"format", "lila.settlement.geojson"}, {"version", 1}, {"units", "m"}, {"axes", "x east, -z north; heights are world Y in cm"}, {"profile", l.profile.name}}},
            {"features", features}};
}

std::string toUsda(const Layout& l, const Ground& ground, const RoadNetwork& net, const std::string& name)
{
    LILA_ZONE();

    Usda u;
    u.line("#usda 1.0");
    u.line("(");
    u.line(std::format("    defaultPrim = {}", usdString(primName(name))));
    u.line("    metersPerUnit = 0.01");
    u.line("    upAxis = \"Y\"");
    u.line(")");
    u.line("");
    u.open(std::format("def Xform {} (kind = \"assembly\")", usdString(primName(name))));
    u.attr("string", "profile", l.profile.name);
    u.attr("string", "format", "lila.settlement.usd");

    auto curves = [&](std::span<const glm::dvec3> pts, double width) {
        u.line("uniform token type = \"linear\"");
        const int count[] = {int(pts.size())};
        u.ints("int[] curveVertexCounts", count);
        u.points(pts);
        u.line(std::format("float[] widths = [{}] (interpolation = \"constant\")", num(width)));
    };

    u.open("def Scope \"Streets\"");
    {
        PrimNames names;
        for (const StreetLine& s : net.lines)
        {
            if (s.pts.size() < 2)
                continue;
            u.open(std::format("def BasisCurves \"{}\"", names(roadKey(s))));
            curves(s.pts, s.width);
            u.attrs(roadFields(l, s));
            u.close();
        }
    }
    u.close();

    u.open("def Scope \"Lanes\"");
    {
        PrimNames names;
        for (const LaneLine& ln : laneLines(l, net))
        {
            const StreetLine& s = net.lines[size_t(ln.line)];
            u.open(std::format("def BasisCurves \"{}\"", names(roadKey(s) + "_lane" + std::to_string(ln.lane))));
            curves(ln.pts, ln.width);
            u.attrs({{"road", roadKey(s)}, {"lane", ln.lane}, {"forward", ln.forward}, {"speed", l.roads[size_t(s.profile)].speed}});
            u.close();
        }
    }
    u.close();

    u.open("def Scope \"Junctions\"");
    {
        PrimNames names;
        for (const Junction& j : net.junctions)
        {
            u.open(std::format("def Xform \"{}\"", names(junctionKey(j))));
            u.line(std::format("double3 xformOp:translate = ({}, {}, {})", num(j.p.x), num(j.p.y), num(j.p.z)));
            u.line("uniform token[] xformOpOrder = [\"xformOp:translate\"]");
            u.attrs({{"key", junctionKey(j)}, {"control", controlName(j.control)}, {"approaches", int64_t(j.ends.size())}, {"phases", int64_t(j.phases.size())}, {"radius", j.radius}});
            u.close();
        }
    }
    u.close();

    u.open("def Scope \"Blocks\"");
    for (const Block& b : l.blocks)
    {
        u.open(std::format("def Mesh \"block_{}\"", b.id));
        u.polygonMesh(b.outline, lowestGround(ground, b.outline));
        u.attrs({{"id", b.id}, {"use", useName(b.use)}, {"areaM2", b.area / kM2}});
        u.close();
    }
    u.close();

    u.open("def Scope \"Lots\"");
    for (const Lot& lot : l.lots)
    {
        u.open(std::format("def Mesh \"lot_{}\"", lot.id));
        u.polygonMesh(lot.outline, lowestGround(ground, lot.outline));
        u.attrs(lotFields(lot));
        u.close();
    }
    u.close();

    u.open("def Scope \"Envelopes\"");
    {
        const std::map<int, std::string> keys = lotKeys(l);
        for (const Envelope& e : l.envelopes)
        {
            geo::Mesh m;
            addPrism(m, e, glm::dvec3(0.0));
            std::vector<glm::dvec3> pts;
            for (geo::Index p = 0; p < m.pointCapacity(); ++p)
                pts.push_back(m.position(p));
            std::vector<int> counts, idx;
            for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            {
                if (!m.faceValid(f))
                    continue;
                const std::vector<geo::Index> fp = m.facePoints(f);
                counts.push_back(int(fp.size()));
                for (geo::Index p : fp)
                    idx.push_back(int(p));
            }
            u.open(std::format("def Mesh \"envelope_{}\"", e.lot));
            u.ints("int[] faceVertexCounts", counts);
            u.ints("int[] faceVertexIndices", idx);
            u.points(pts);
            const auto it = keys.find(e.lot);
            u.attrs(envelopeFields(l, e, it != keys.end() ? "envelope:" + it->second.substr(4) : std::string()));
            u.close();
        }
    }
    u.close();

    u.open("def Scope \"Placements\"");
    {
        PrimNames names;
        for (const Placement& p : l.placements)
        {
            u.open(std::format("def Xform \"{}\"", names("place_" + p.id)));
            u.line(std::format("double3 xformOp:translate = ({}, {}, {})", num(p.position.x), num(p.position.y), num(p.position.z)));
            u.line(std::format("quatf xformOp:orient = ({}, {}, {}, {})", num(p.rotation.w), num(p.rotation.x), num(p.rotation.y), num(p.rotation.z)));
            u.line(std::format("float3 xformOp:scale = ({}, {}, {})", num(p.scale.x), num(p.scale.y), num(p.scale.z)));
            u.line("uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:orient\", \"xformOp:scale\"]");
            u.attrs({{"key", "place:" + p.id}, {"asset", p.asset}});
            std::string tags = "custom string[] lila:tags = [";
            for (size_t k = 0; k < p.tags.size(); ++k)
                tags += (k ? ", " : "") + usdString(p.tags[k]);
            u.line(tags + "]");
            u.close();
        }
    }
    u.close();

    u.close();
    return u.text;
}

std::vector<std::pair<std::string, lila::curve::Splines>> dxfLayers(const Layout& l, const Ground& ground, const RoadNetwork& net, double cmPerUnit)
{
    LILA_ZONE();

    namespace curve = lila::curve;
    auto toFile = [&](glm::dvec3 p) { return glm::dvec3(p.x, -p.z, p.y) / cmPerUnit; };
    auto poly = [&](std::span<const glm::dvec3> pts, bool closed) {
        curve::Spline s;
        s.kind = curve::Kind::Poly;
        s.closed = closed;
        for (const glm::dvec3& p : pts)
            s.points.push_back(toFile(p));
        return s;
    };
    curve::Splines streets, paths, water, blocks, lots, buildings, lanes;
    for (const StreetLine& s : streetLines(l, ground))
        if (s.pts.size() >= 2)
            (s.surface || s.cls == StreetClass::Path ? paths : streets).push_back(poly(s.pts, false));
    for (const WaterLine& w : l.water)
    {
        std::vector<glm::dvec3> pts;
        for (const Vec2& q : w.pts)
            pts.push_back({q.x, ground.height(q), q.y});
        if (pts.size() >= 2)
            water.push_back(poly(pts, false));
    }
    for (const Block& b : l.blocks)
        blocks.push_back(poly(flat(b.outline, lowestGround(ground, b.outline)), true));
    for (const Lot& lot : l.lots)
        lots.push_back(poly(flat(lot.outline, lowestGround(ground, lot.outline)), true));
    for (const Envelope& e : l.envelopes)
        buildings.push_back(poly(flat(e.footprint, e.ground), true));
    for (const LaneLine& ln : laneLines(l, net))
        lanes.push_back(poly(ln.pts, false));
    std::vector<std::pair<std::string, curve::Splines>> out;
    for (auto& [n, c] : std::initializer_list<std::pair<const char*, curve::Splines*>>{
             {"Streets", &streets}, {"Paths", &paths}, {"Water", &water}, {"Blocks", &blocks}, {"Lots", &lots}, {"Buildings", &buildings}, {"Lanes", &lanes}})
        if (!c->empty())
            out.push_back({n, std::move(*c)});
    return out;
}

json toEngineJson(const Layout& l, const Ground& ground, const RoadNetwork& net, const std::string& name, EngineAxes axes)
{
    LILA_ZONE();

    const EngineSpace es{axes};
    json streets = json::array();
    for (const StreetLine& s : net.lines)
    {
        const RoadProfile& p = l.roads[size_t(s.profile)];
        streets.push_back({{"key", roadKey(s)},
                           {"cls", className(s.cls)},
                           {"profile", p.name},
                           {"width", s.width / 100.0},
                           {"lanes", p.lanes},
                           {"surface", s.surface},
                           {"points", es.flatPoints(s.pts)}});
    }
    json junctions = json::array();
    for (const Junction& j : net.junctions)
        junctions.push_back({{"key", junctionKey(j)}, {"control", controlName(j.control)}, {"approaches", int(j.ends.size())}, {"position", es.vec(j.p)}, {"phases", j.phases}, {"walks", j.walks}});
    json blocks = json::array();
    for (const Block& b : l.blocks)
        blocks.push_back({{"id", b.id}, {"use", useName(b.use)}, {"area", b.area / kM2}, {"outline", es.flatPoints(flat(b.outline, lowestGround(ground, b.outline)))}});
    json lots = json::array();
    for (const Lot& lot : l.lots)
        lots.push_back({{"id", lot.id},
                        {"key", lotKey(lot)},
                        {"block", lot.block},
                        {"use", useName(lot.use)},
                        {"area", lot.area / kM2},
                        {"frontages", lot.frontages},
                        {"corner", lot.corner},
                        {"frontClass", className(lot.frontClass)},
                        {"outline", es.flatPoints(flat(lot.outline, lowestGround(ground, lot.outline)))}});
    const std::map<int, std::string> keys = lotKeys(l);
    json envelopes = json::array();
    for (const Envelope& e : l.envelopes)
    {
        const auto it = keys.find(e.lot);
        envelopes.push_back({{"lot", e.lot},
                             {"key", it != keys.end() ? "envelope:" + it->second.substr(4) : std::string()},
                             {"use", useName(e.use)},
                             {"floors", e.floors},
                             {"floorHeight", l.profile.floorHeight / 100.0},
                             {"height", e.height / 100.0},
                             {"base", e.base / 100.0},
                             {"ground", e.ground / 100.0},
                             {"top", e.top / 100.0},
                             {"roof", e.roof},
                             {"footprint", es.flatPoints(flat(e.footprint, e.ground))}});
    }
    json lanes = json::array();
    for (const LaneLine& ln : laneLines(l, net))
        lanes.push_back({{"road", roadKey(net.lines[size_t(ln.line)])}, {"lane", ln.lane}, {"forward", ln.forward}, {"width", ln.width / 100.0}, {"points", es.flatPoints(ln.pts)}});
    json placements = json::array();
    for (const Placement& p : l.placements)
        placements.push_back({{"id", p.id}, {"asset", p.asset}, {"position", es.vec(p.position)}, {"rotation", es.quat(p.rotation)}, {"scale", {p.scale.x, p.scale.y, p.scale.z}}, {"tags", p.tags}});
    return {{"format", "lila.settlement.engine"},
            {"version", 1},
            {"target", axes == EngineAxes::Unity ? "unity" : "godot"},
            {"name", name},
            {"units", "m"},
            {"profile", l.profile.name},
            {"streets", streets},
            {"junctions", junctions},
            {"blocks", blocks},
            {"lots", lots},
            {"envelopes", envelopes},
            {"lanes", lanes},
            {"placements", placements}};
}

std::map<Cell, geo::Mesh> massingByCell(const Layout& l, double size)
{
    LILA_ZONE();

    std::map<Cell, geo::Mesh> out;
    for (const Envelope& e : l.envelopes)
    {
        const Cell c = cellAt(centroid(e.footprint), size);
        const Vec2 centre = c.center(size);
        addPrism(out[c], e, glm::dvec3(centre.x, 0.0, centre.y));
    }
    return out;
}

geo::Mesh massing(const Layout& l)
{
    geo::Mesh m;
    for (const Envelope& e : l.envelopes)
        addPrism(m, e, glm::dvec3(0.0));
    return m;
}
} // namespace settlement
