// OpenStreetMap import: .osm XML, Overpass JSON and .osm.pbf read without libraries, sorted into terrain features (roads,
// paths, rivers, streams, lakes, coastline) and land cover rasterised into masks.

#include "Osm.h"

#include <lila/Profile.h>

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace terrain
{
namespace osmdetail
{
/** A point key from a position, for data that gives positions without node ids (never collides with real ids, which are positive). */
int64_t positionKey(LatLon p) { return -(int64_t(std::llround((p.lat + 90.0) * 1e7)) * 4000000000LL + int64_t(std::llround((p.lon + 180.0) * 1e7))) - 1; }

/** Resolves ways' node refs to positions once every node is known. */
struct Builder
{
    OsmData data;
    std::unordered_map<int64_t, LatLon> nodes;
    std::vector<std::vector<int64_t>> refs;

    void addWay(int64_t id, std::vector<int64_t> r, OsmTags tags)
    {
        data.ways.push_back({id, {}, std::move(tags)});
        refs.push_back(std::move(r));
    }
    OsmData finish()
    {
        for (size_t i = 0; i < data.ways.size(); ++i)
        {
            if (!data.ways[i].points.empty())
                continue;
            for (int64_t ref : refs[i])
            {
                auto it = nodes.find(ref);
                if (it == nodes.end())
                {
                    ++data.missingNodes;
                    continue;
                }
                data.ways[i].points.push_back({ref, it->second});
            }
        }
        data.nodes = nodes.size();
        return std::move(data);
    }
};

std::string decodeEntities(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out += s[i];
            continue;
        }
        const size_t end = s.find(';', i);
        if (end == std::string_view::npos || end - i > 10)
        {
            out += s[i];
            continue;
        }
        const std::string_view e = s.substr(i + 1, end - i - 1);
        uint32_t cp = 0;
        if (e == "amp")
            cp = '&';
        else if (e == "lt")
            cp = '<';
        else if (e == "gt")
            cp = '>';
        else if (e == "quot")
            cp = '"';
        else if (e == "apos")
            cp = '\'';
        else if (!e.empty() && e[0] == '#')
            cp = uint32_t(std::strtoul(std::string(e.substr(e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? 2 : 1)).c_str(), nullptr,
                                       e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? 16 : 10));
        if (!cp)
        {
            out += s[i];
            continue;
        }
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800)
        {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        else
        {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        i = end;
    }
    return out;
}

struct XmlTag
{
    std::string_view name;
    bool closing = false;
    bool selfClosing = false;
    std::vector<std::pair<std::string_view, std::string_view>> attrs;

    std::string_view attr(std::string_view k) const
    {
        for (const auto& [n, v] : attrs)
            if (n == k)
                return v;
        return {};
    }
};

/** The next tag at or after pos, skipping declarations, comments and text; false at the end. */
bool nextXmlTag(std::string_view s, size_t& pos, XmlTag& t)
{
    while (true)
    {
        const size_t lt = s.find('<', pos);
        if (lt == std::string_view::npos)
            return false;
        if (s.compare(lt, 4, "<!--") == 0)
        {
            const size_t end = s.find("-->", lt);
            pos = end == std::string_view::npos ? s.size() : end + 3;
            continue;
        }
        const size_t gt = s.find('>', lt);
        if (gt == std::string_view::npos)
            return false;
        pos = gt + 1;
        if (s[lt + 1] == '?' || s[lt + 1] == '!')
            continue;
        size_t i = lt + 1;
        t = XmlTag{};
        if (s[i] == '/')
        {
            t.closing = true;
            ++i;
        }
        size_t nameEnd = i;
        while (nameEnd < gt && !std::isspace((unsigned char)s[nameEnd]) && s[nameEnd] != '/')
            ++nameEnd;
        t.name = s.substr(i, nameEnd - i);
        t.selfClosing = s[gt - 1] == '/';
        i = nameEnd;
        // Attribute values may hold '>' only escaped, so the tag ends at the first '>'.
        while (i < gt)
        {
            while (i < gt && (std::isspace((unsigned char)s[i]) || s[i] == '/'))
                ++i;
            const size_t eq = s.find('=', i);
            if (eq == std::string_view::npos || eq >= gt)
                break;
            std::string_view key = s.substr(i, eq - i);
            while (!key.empty() && std::isspace((unsigned char)key.back()))
                key.remove_suffix(1);
            size_t q = eq + 1;
            while (q < gt && std::isspace((unsigned char)s[q]))
                ++q;
            if (q >= gt || (s[q] != '"' && s[q] != '\''))
                break;
            const size_t close = s.find(s[q], q + 1);
            if (close == std::string_view::npos || close > gt)
                break;
            t.attrs.push_back({key, s.substr(q + 1, close - q - 1)});
            i = close + 1;
        }
        return true;
    }
}

double toDouble(std::string_view v) { return std::strtod(std::string(v).c_str(), nullptr); }
int64_t toInt(std::string_view v) { return std::strtoll(std::string(v).c_str(), nullptr, 10); }

// Protocol buffers ------------------------------------------------------------------------------------------------

struct Pb
{
    const uint8_t* p;
    const uint8_t* end;

    explicit Pb(std::string_view s) : p(reinterpret_cast<const uint8_t*>(s.data())), end(p + s.size()) {}

    bool more() const { return p < end; }
    uint64_t varint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if (p >= end)
                throw std::runtime_error("truncated protobuf");
            const uint8_t b = *p++;
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80))
                return v;
        }
        throw std::runtime_error("bad protobuf varint");
    }
    static int64_t zigzag(uint64_t v) { return int64_t(v >> 1) ^ -int64_t(v & 1); }
    void key(uint32_t& field, uint32_t& wire)
    {
        const uint64_t k = varint();
        field = uint32_t(k >> 3);
        wire = uint32_t(k & 7);
    }
    std::string_view bytes()
    {
        const uint64_t n = varint();
        if (n > uint64_t(end - p))
            throw std::runtime_error("truncated protobuf");
        std::string_view s(reinterpret_cast<const char*>(p), size_t(n));
        p += n;
        return s;
    }
    void skip(uint32_t wire)
    {
        switch (wire)
        {
        case 0: varint(); break;
        case 1: p += 8; break;
        case 2: bytes(); break;
        case 5: p += 4; break;
        default: throw std::runtime_error("unsupported protobuf wire type");
        }
        if (p > end)
            throw std::runtime_error("truncated protobuf");
    }
    /** A repeated varint field, packed (wire 2) or one value (wire 0). */
    void repeated(uint32_t wire, std::vector<uint64_t>& out)
    {
        if (wire == 0)
        {
            out.push_back(varint());
            return;
        }
        Pb sub(bytes());
        while (sub.more())
            out.push_back(sub.varint());
    }
};

void pbfTags(const std::vector<uint64_t>& keys, const std::vector<uint64_t>& vals, const std::vector<std::string>& strings, OsmTags& tags)
{
    for (size_t i = 0; i < keys.size() && i < vals.size(); ++i)
        if (keys[i] < strings.size() && vals[i] < strings.size())
            tags[strings[keys[i]]] = strings[vals[i]];
}

void pbfBlock(std::string_view block, Builder& b)
{
    Pb pb(block);
    std::vector<std::string> strings;
    std::vector<std::string_view> groups;
    int64_t granularity = 100, latOffset = 0, lonOffset = 0;
    while (pb.more())
    {
        uint32_t f, w;
        pb.key(f, w);
        if (f == 1 && w == 2)
        {
            Pb st(pb.bytes());
            while (st.more())
            {
                uint32_t sf, sw;
                st.key(sf, sw);
                if (sf == 1 && sw == 2)
                    strings.emplace_back(st.bytes());
                else
                    st.skip(sw);
            }
        }
        else if (f == 2 && w == 2)
            groups.push_back(pb.bytes());
        else if (f == 17 && w == 0)
            granularity = int64_t(pb.varint());
        else if (f == 19 && w == 0)
            latOffset = int64_t(pb.varint());
        else if (f == 20 && w == 0)
            lonOffset = int64_t(pb.varint());
        else
            pb.skip(w);
    }
    auto deg = [&](int64_t v, int64_t offset) { return 1e-9 * double(offset + granularity * v); };
    for (std::string_view g : groups)
    {
        Pb gp(g);
        while (gp.more())
        {
            uint32_t f, w;
            gp.key(f, w);
            if (w != 2)
            {
                gp.skip(w);
                continue;
            }
            Pb e(gp.bytes());
            std::vector<uint64_t> ids, lats, lons, keys, vals, refs, roles, memids, types, keysVals;
            int64_t id = 0, lat = 0, lon = 0;
            while (e.more())
            {
                uint32_t ef, ew;
                e.key(ef, ew);
                if (f == 1)
                {
                    if (ef == 1 && ew == 0)
                        id = Pb::zigzag(e.varint());
                    else if (ef == 2)
                        e.repeated(ew, keys);
                    else if (ef == 3)
                        e.repeated(ew, vals);
                    else if (ef == 8 && ew == 0)
                        lat = Pb::zigzag(e.varint());
                    else if (ef == 9 && ew == 0)
                        lon = Pb::zigzag(e.varint());
                    else
                        e.skip(ew);
                }
                else if (f == 2)
                {
                    if (ef == 1)
                        e.repeated(ew, ids);
                    else if (ef == 8)
                        e.repeated(ew, lats);
                    else if (ef == 9)
                        e.repeated(ew, lons);
                    else if (ef == 10)
                        e.repeated(ew, keysVals);
                    else
                        e.skip(ew);
                }
                else if (f == 3 || f == 4)
                {
                    if (ef == 1 && ew == 0)
                        id = int64_t(e.varint());
                    else if (ef == 2)
                        e.repeated(ew, keys);
                    else if (ef == 3)
                        e.repeated(ew, vals);
                    else if (ef == 8)
                        e.repeated(ew, f == 3 ? refs : roles);
                    else if (ef == 9 && f == 4)
                        e.repeated(ew, memids);
                    else if (ef == 10 && f == 4)
                        e.repeated(ew, types);
                    else
                        e.skip(ew);
                }
                else
                    e.skip(ew);
            }
            if (f == 1)
            {
                b.nodes[id] = {deg(lat, latOffset), deg(lon, lonOffset)};
                if (!keys.empty())
                {
                    OsmData::Node n{id, b.nodes[id], {}};
                    pbfTags(keys, vals, strings, n.tags);
                    b.data.tagged.push_back(std::move(n));
                }
            }
            else if (f == 2)
            {
                int64_t did = 0, dlat = 0, dlon = 0;
                // keys_vals: per node, key and value string ids in pairs, each node's run ended by a 0.
                size_t kv = 0;
                for (size_t i = 0; i < ids.size() && i < lats.size() && i < lons.size(); ++i)
                {
                    did += Pb::zigzag(ids[i]);
                    dlat += Pb::zigzag(lats[i]);
                    dlon += Pb::zigzag(lons[i]);
                    b.nodes[did] = {deg(dlat, latOffset), deg(dlon, lonOffset)};
                    OsmTags tags;
                    while (kv < keysVals.size() && keysVals[kv] != 0)
                    {
                        if (kv + 1 < keysVals.size() && keysVals[kv] < strings.size() && keysVals[kv + 1] < strings.size())
                            tags[strings[keysVals[kv]]] = strings[keysVals[kv + 1]];
                        kv += 2;
                    }
                    ++kv;
                    if (!tags.empty())
                        b.data.tagged.push_back({did, b.nodes[did], std::move(tags)});
                }
            }
            else if (f == 3)
            {
                OsmTags tags;
                pbfTags(keys, vals, strings, tags);
                std::vector<int64_t> r;
                int64_t ref = 0;
                for (uint64_t v : refs)
                    r.push_back(ref += Pb::zigzag(v));
                b.addWay(id, std::move(r), std::move(tags));
            }
            else if (f == 4)
            {
                OsmData::Relation rel;
                rel.id = id;
                pbfTags(keys, vals, strings, rel.tags);
                int64_t ref = 0;
                for (size_t i = 0; i < memids.size(); ++i)
                {
                    OsmData::Member m;
                    m.ref = ref += Pb::zigzag(memids[i]);
                    const uint64_t t = i < types.size() ? types[i] : 1;
                    m.type = t == 0 ? 'n' : t == 1 ? 'w' : 'r';
                    if (i < roles.size() && roles[i] < strings.size())
                        m.role = strings[roles[i]];
                    rel.members.push_back(std::move(m));
                }
                b.data.relations.push_back(std::move(rel));
            }
        }
    }
}

// Features --------------------------------------------------------------------------------------------------------

std::string_view tag(const OsmTags& t, std::string_view k)
{
    auto it = t.find(k);
    return it != t.end() ? std::string_view(it->second) : std::string_view();
}

/** Metres from a width tag ("12", "12 m", "40'"); 0 when it isn't a number. */
double widthTag(std::string_view v)
{
    if (v.empty())
        return 0.0;
    const std::string s(v);
    char* end = nullptr;
    const double x = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || !(x > 0.0))
        return 0.0;
    return s.find('\'') != std::string::npos || s.find("ft") != std::string::npos ? x * 0.3048 : x;
}

double defaultRoadWidth(std::string_view cls)
{
    static const std::pair<const char*, double> widths[] = {
        {"motorway", 25.0}, {"trunk", 20.0},       {"primary", 14.0},   {"secondary", 11.0}, {"tertiary", 9.0},     {"unclassified", 7.0},
        {"residential", 7.0}, {"living_street", 6.0}, {"service", 4.5}, {"road", 7.0},       {"track", 3.5},        {"busway", 7.0},
    };
    std::string_view base = cls.substr(0, cls.find("_link"));
    for (const auto& [n, w] : widths)
        if (base == n)
            return w;
    return 6.0;
}

bool isPath(std::string_view hw)
{
    return hw == "footway" || hw == "path" || hw == "cycleway" || hw == "bridleway" || hw == "steps" || hw == "pedestrian" || hw == "track";
}

std::string landcoverOf(const OsmTags& t)
{
    const std::string_view lu = tag(t, "landuse"), nat = tag(t, "natural"), leisure = tag(t, "leisure"), ww = tag(t, "waterway");
    if (lu == "forest" || nat == "wood")
        return "forest";
    if (nat == "grassland" || lu == "meadow" || lu == "grass" || lu == "recreation_ground" || leisure == "park" || lu == "village_green")
        return "grassland";
    if (lu == "farmland" || lu == "orchard" || lu == "vineyard" || lu == "allotments" || lu == "farmyard" || lu == "plant_nursery")
        return "farmland";
    if (nat == "scrub" || nat == "heath")
        return "scrub";
    if (nat == "bare_rock" || nat == "scree" || nat == "shingle")
        return "rock";
    if (nat == "sand" || nat == "beach" || nat == "dune")
        return "sand";
    if (lu == "residential" || lu == "commercial" || lu == "industrial" || lu == "retail" || lu == "construction")
        return "urban";
    if (nat == "wetland")
        return "wetland";
    if (nat == "water" || ww == "riverbank" || lu == "reservoir" || lu == "basin")
        return "water";
    if (nat == "glacier")
        return "glacier";
    return {};
}

/** Lake class when the tags make a lake: lake, reservoir, pond or water (river surfaces aren't lakes). */
std::string lakeOf(const OsmTags& t)
{
    const std::string_view nat = tag(t, "natural"), water = tag(t, "water"), lu = tag(t, "landuse");
    if (lu == "reservoir")
        return "reservoir";
    if (nat != "water")
        return {};
    if (water == "river" || water == "canal" || water == "stream" || water == "ditch" || water == "wastewater")
        return {};
    return water.empty() ? "water" : std::string(water);
}

/** Joins ways end to end into as few chains as possible; a chain that ends where it starts is closed. */
std::vector<std::vector<OsmPoint>> joinWays(std::vector<std::vector<OsmPoint>> parts)
{
    std::vector<std::vector<OsmPoint>> out;
    std::erase_if(parts, [](const std::vector<OsmPoint>& p) { return p.size() < 2; });
    std::vector<uint8_t> used(parts.size(), 0);
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (used[i])
            continue;
        used[i] = 1;
        std::vector<OsmPoint> chain = parts[i];
        bool grew = true;
        while (grew && chain.front().key != chain.back().key)
        {
            grew = false;
            for (size_t j = 0; j < parts.size(); ++j)
            {
                if (used[j])
                    continue;
                const std::vector<OsmPoint>& p = parts[j];
                if (p.front().key == chain.back().key)
                    chain.insert(chain.end(), p.begin() + 1, p.end());
                else if (p.back().key == chain.back().key)
                    chain.insert(chain.end(), p.rbegin() + 1, p.rend());
                else if (p.back().key == chain.front().key)
                    chain.insert(chain.begin(), p.begin(), p.end() - 1);
                else if (p.front().key == chain.front().key)
                    chain.insert(chain.begin(), p.rbegin(), p.rend() - 1);
                else
                    continue;
                used[j] = 1;
                grew = true;
            }
        }
        out.push_back(std::move(chain));
    }
    return out;
}

std::vector<LatLon> positions(const std::vector<OsmPoint>& pts, bool dropClosingPoint)
{
    std::vector<LatLon> out;
    out.reserve(pts.size());
    for (const OsmPoint& p : pts)
        out.push_back(p.at);
    if (dropClosingPoint && out.size() > 2 && pts.front().key == pts.back().key)
        out.pop_back();
    return out;
}

bool isClosed(const std::vector<OsmPoint>& pts) { return pts.size() > 3 && pts.front().key == pts.back().key; }

/** Every real name the tags give: name and its language, official, alternative, old and short forms, brand and operator. */
std::vector<std::string> realNamesOf(const OsmTags& t)
{
    std::vector<std::string> out;
    for (const auto& [k, v] : t)
    {
        if (v.empty())
            continue;
        const bool named = k == "name" || k.starts_with("name:") || k == "official_name" || k == "alt_name" || k == "old_name" ||
                           k == "short_name" || k == "loc_name" || k == "reg_name" || k == "int_name" || k == "nat_name";
        const bool owner = k == "brand" || k.starts_with("brand:") || k == "operator" || k.starts_with("operator:") || k == "network";
        if (!named && !owner)
            continue;
        if (std::ranges::find(out, v) == out.end())
            out.push_back(v);
    }
    return out;
}

/** The chain a branch belongs to: its brand, else its operator, as lower-case text; empty for an independent business. */
std::string chainOf(const OsmTags& t)
{
    std::string_view c = tag(t, "brand");
    if (c.empty())
        c = tag(t, "operator");
    std::string out;
    for (unsigned char ch : c)
        out += char(std::tolower(ch));
    return out;
}

/** Metres from a height tag ("12", "12 m", "40'"); NaN when it isn't a number. */
double metresTag(std::string_view v)
{
    if (v.empty())
        return std::numeric_limits<double>::quiet_NaN();
    const double m = widthTag(v);
    return m > 0.0 ? m : (v.front() == '0' ? 0.0 : std::numeric_limits<double>::quiet_NaN());
}

int levelsTag(std::string_view v)
{
    if (v.empty())
        return -1;
    const double x = std::strtod(std::string(v).c_str(), nullptr);
    return x >= 0.0 && x < 500.0 ? int(std::lround(x)) : -1;
}

LatLon ringCentre(const std::vector<LatLon>& r)
{
    double lat = 0.0, lon = 0.0, a = 0.0;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
    {
        const double c = r[j].lon * r[i].lat - r[i].lon * r[j].lat;
        a += c;
        lon += (r[j].lon + r[i].lon) * c;
        lat += (r[j].lat + r[i].lat) * c;
    }
    if (std::abs(a) < 1e-18)
    {
        LatLon m{0.0, 0.0};
        for (const LatLon& p : r)
            m.lat += p.lat / double(r.size()), m.lon += p.lon / double(r.size());
        return m;
    }
    return {lat / (3.0 * a), lon / (3.0 * a)};
}

bool inRing(const std::vector<LatLon>& r, LatLon q)
{
    bool in = false;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        if ((r[i].lat > q.lat) != (r[j].lat > q.lat) && q.lon < (r[j].lon - r[i].lon) * (q.lat - r[i].lat) / (r[j].lat - r[i].lat) + r[i].lon)
            in = !in;
    return in;
}

bool isStructure(std::string_view mm) { return mm == "pier" || mm == "breakwater" || mm == "quay" || mm == "groyne"; }

} // namespace osmdetail

using namespace osmdetail;

OsmData parseOsmXml(std::string_view s)
{
    LILA_ZONE();

    Builder b;
    size_t pos = 0;
    XmlTag t;
    enum
    {
        None,
        InNode,
        InWay,
        InRelation
    } in = None;
    int64_t id = 0;
    std::vector<int64_t> refs;
    OsmTags tags;
    OsmData::Relation rel;
    LatLon nodeAt;
    bool sawOsm = false;
    while (nextXmlTag(s, pos, t))
    {
        if (t.closing)
        {
            if (t.name == "way" && in == InWay)
                b.addWay(id, std::move(refs), std::move(tags));
            else if (t.name == "relation" && in == InRelation)
            {
                rel.tags = std::move(tags);
                b.data.relations.push_back(std::move(rel));
            }
            else if (t.name == "node" && in == InNode && !tags.empty())
                b.data.tagged.push_back({id, nodeAt, std::move(tags)});
            if (t.name == "node" || t.name == "way" || t.name == "relation")
            {
                in = None;
                refs.clear();
                tags.clear();
                rel = {};
            }
            continue;
        }
        if (t.name == "osm")
            sawOsm = true;
        else if (t.name == "node")
        {
            id = toInt(t.attr("id"));
            nodeAt = {toDouble(t.attr("lat")), toDouble(t.attr("lon"))};
            b.nodes[id] = nodeAt;
            in = t.selfClosing ? None : InNode;
        }
        else if (t.name == "way")
        {
            id = toInt(t.attr("id"));
            in = InWay;
            if (t.selfClosing)
            {
                b.addWay(id, {}, {});
                in = None;
            }
        }
        else if (t.name == "relation")
        {
            rel = {};
            rel.id = toInt(t.attr("id"));
            in = InRelation;
            if (t.selfClosing)
            {
                b.data.relations.push_back(std::move(rel));
                in = None;
            }
        }
        else if (t.name == "nd" && in == InWay)
            refs.push_back(toInt(t.attr("ref")));
        else if (t.name == "tag" && in != None)
            tags[decodeEntities(t.attr("k"))] = decodeEntities(t.attr("v"));
        else if (t.name == "member" && in == InRelation)
        {
            const std::string_view type = t.attr("type");
            rel.members.push_back({type.empty() ? 'w' : type[0], toInt(t.attr("ref")), decodeEntities(t.attr("role")), {}});
        }
    }
    if (!sawOsm)
        throw std::runtime_error("not OSM XML (no <osm> element)");
    return b.finish();
}

OsmData parseOverpassJson(std::string_view text)
{
    LILA_ZONE();

    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("elements") || !j["elements"].is_array())
        throw std::runtime_error("not Overpass JSON (no elements list)");
    Builder b;
    auto geometry = [](const nlohmann::json& g, const nlohmann::json* ids) {
        std::vector<OsmPoint> pts;
        for (size_t i = 0; i < g.size(); ++i)
        {
            if (!g[i].is_object() || !g[i].contains("lat"))
                continue;
            const LatLon p{g[i]["lat"].get<double>(), g[i]["lon"].get<double>()};
            const int64_t key = ids && i < ids->size() ? (*ids)[i].get<int64_t>() : positionKey(p);
            pts.push_back({key, p});
        }
        return pts;
    };
    for (const nlohmann::json& e : j["elements"])
    {
        const std::string type = e.value("type", std::string());
        OsmTags tags;
        if (e.contains("tags") && e["tags"].is_object())
            for (const auto& [k, v] : e["tags"].items())
                tags[k] = v.is_string() ? v.get<std::string>() : v.dump();
        const int64_t id = e.value("id", int64_t(0));
        if (type == "node")
        {
            b.nodes[id] = {e.value("lat", 0.0), e.value("lon", 0.0)};
            if (!tags.empty())
                b.data.tagged.push_back({id, b.nodes[id], std::move(tags)});
        }
        else if (type == "way")
        {
            std::vector<int64_t> refs;
            if (e.contains("nodes") && e["nodes"].is_array())
                refs = e["nodes"].get<std::vector<int64_t>>();
            b.addWay(id, refs, std::move(tags));
            if (e.contains("geometry") && e["geometry"].is_array())
                b.data.ways.back().points = geometry(e["geometry"], e.contains("nodes") ? &e["nodes"] : nullptr);
        }
        else if (type == "relation")
        {
            OsmData::Relation rel;
            rel.id = id;
            rel.tags = std::move(tags);
            if (e.contains("members") && e["members"].is_array())
                for (const nlohmann::json& m : e["members"])
                {
                    const std::string mt = m.value("type", std::string("way"));
                    OsmData::Member mem{mt.empty() ? 'w' : mt[0], m.value("ref", int64_t(0)), m.value("role", std::string()), {}};
                    if (m.contains("geometry") && m["geometry"].is_array())
                        mem.points = geometry(m["geometry"], nullptr);
                    rel.members.push_back(std::move(mem));
                }
            b.data.relations.push_back(std::move(rel));
        }
    }
    return b.finish();
}

OsmData parseOsmPbf(std::string_view bytes)
{
    LILA_ZONE();

    Builder b;
    size_t pos = 0;
    bool sawHeader = false;
    while (pos + 4 <= bytes.size())
    {
        const uint32_t len = uint32_t(uint8_t(bytes[pos])) << 24 | uint32_t(uint8_t(bytes[pos + 1])) << 16 | uint32_t(uint8_t(bytes[pos + 2])) << 8 |
                             uint32_t(uint8_t(bytes[pos + 3]));
        pos += 4;
        if (len > 64 * 1024 || pos + len > bytes.size())
            throw std::runtime_error("not an OSM PBF file (bad blob header)");
        Pb header(bytes.substr(pos, len));
        pos += len;
        std::string type;
        uint64_t dataSize = 0;
        while (header.more())
        {
            uint32_t f, w;
            header.key(f, w);
            if (f == 1 && w == 2)
                type = std::string(header.bytes());
            else if (f == 3 && w == 0)
                dataSize = header.varint();
            else
                header.skip(w);
        }
        if (pos + dataSize > bytes.size())
            throw std::runtime_error("truncated OSM PBF file");
        Pb blob(bytes.substr(pos, size_t(dataSize)));
        pos += size_t(dataSize);
        std::string_view raw, zlib;
        uint64_t rawSize = 0;
        bool otherCompression = false;
        while (blob.more())
        {
            uint32_t f, w;
            blob.key(f, w);
            if (f == 1 && w == 2)
                raw = blob.bytes();
            else if (f == 2 && w == 0)
                rawSize = blob.varint();
            else if (f == 3 && w == 2)
                zlib = blob.bytes();
            else
            {
                otherCompression = otherCompression || (w == 2 && f >= 4);
                blob.skip(w);
            }
        }
        std::string inflated;
        if (!zlib.empty())
        {
            if (rawSize > (uint64_t(1) << 28))
                throw std::runtime_error("OSM PBF block too large");
            inflated.resize(size_t(rawSize));
            mz_ulong got = mz_ulong(rawSize);
            if (mz_uncompress(reinterpret_cast<unsigned char*>(inflated.data()), &got, reinterpret_cast<const unsigned char*>(zlib.data()), mz_ulong(zlib.size())) != MZ_OK)
                throw std::runtime_error("bad zlib data in OSM PBF file");
            inflated.resize(got);
            raw = inflated;
        }
        else if (raw.empty() && otherCompression)
            throw std::runtime_error("OSM PBF blocks compressed with lzma or zstd aren't read; re-save with zlib (osmium cat -f pbf,pbf_compression=zlib)");
        if (type == "OSMHeader")
        {
            sawHeader = true;
            Pb h(raw);
            while (h.more())
            {
                uint32_t f, w;
                h.key(f, w);
                // required_features (4): DenseNodes and OsmSchema-V0.6 are read; HistoricalInformation isn't.
                if (f == 4 && w == 2)
                {
                    const std::string_view feature = h.bytes();
                    if (feature != "OsmSchema-V0.6" && feature != "DenseNodes")
                        throw std::runtime_error("OSM PBF needs feature " + std::string(feature));
                }
                else
                    h.skip(w);
            }
        }
        else if (type == "OSMData")
            pbfBlock(raw, b);
    }
    if (!sawHeader)
        throw std::runtime_error("not an OSM PBF file (no OSMHeader)");
    return b.finish();
}

OsmData readOsm(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("can't open " + path.string());
    const std::string bytes((std::istreambuf_iterator<char>(f)), {});
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    try
    {
        // Overpass answers are JSON or XML even when saved under a .pbf name, so text content wins over the extension.
        const size_t first = bytes.find_first_not_of(" \t\r\n");
        const bool text = first != std::string::npos && (bytes[first] == '{' || bytes[first] == '<');
        if (ext == ".pbf" && !text)
            return parseOsmPbf(bytes);
        if (ext == ".json" || ext == ".geojson" || (!bytes.empty() && bytes.find_first_not_of(" \t\r\n") != std::string::npos &&
                                                    bytes[bytes.find_first_not_of(" \t\r\n")] == '{'))
            return parseOverpassJson(bytes);
        return parseOsmXml(bytes);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(path.filename().string() + ": " + e.what());
    }
}

const std::vector<std::string>& osmFeatureKindNames()
{
    static const std::vector<std::string> n = {"road", "path", "river", "stream", "lake", "coastline", "pier", "breakwater", "quay", "dam"};
    return n;
}

const std::vector<std::string>& landcoverClasses()
{
    static const std::vector<std::string> n = {"forest", "grassland", "farmland", "scrub", "rock", "sand", "urban", "wetland", "water", "glacier"};
    return n;
}

std::pair<std::string, std::string> osmProgram(const OsmTags& t)
{
    const std::string_view amenity = tag(t, "amenity"), shop = tag(t, "shop"), tourism = tag(t, "tourism"), office = tag(t, "office");
    const std::string_view leisure = tag(t, "leisure"), landuse = tag(t, "landuse"), aeroway = tag(t, "aeroway"), manMade = tag(t, "man_made");
    const std::string_view cuisine = tag(t, "cuisine"), building = tag(t, "building"), place = tag(t, "place"), nat = tag(t, "natural");
    const std::string_view harbour = tag(t, "harbour"), industrial = tag(t, "industrial"), craft = tag(t, "craft");
    auto has = [](std::string_view list, std::string_view w) { return list.find(w) != std::string_view::npos; };
    using P = std::pair<std::string, std::string>;
    if (!amenity.empty())
    {
        if ((amenity == "restaurant" || amenity == "fast_food") && (has(cuisine, "taco") || has(cuisine, "mexican")))
            return P{"taqueria", "business"};
        if (amenity == "restaurant" || amenity == "food_court")
            return has(cuisine, "seafood") || has(cuisine, "fish") ? P{"seafood", "business"} : P{"restaurant", "business"};
        static const std::pair<const char*, std::pair<const char*, const char*>> kAmenity[] = {
            {"fast_food", {"fastFood", "business"}}, {"cafe", {"cafe", "business"}}, {"bar", {"bar", "business"}},
            {"pub", {"bar", "business"}}, {"biergarten", {"bar", "business"}}, {"nightclub", {"nightclub", "business"}},
            {"ice_cream", {"iceCream", "business"}}, {"pharmacy", {"pharmacy", "business"}}, {"bank", {"bank", "business"}},
            {"fuel", {"fuel", "business"}}, {"hospital", {"hospital", "hospital"}}, {"clinic", {"clinic", "hospital"}},
            {"doctors", {"clinic", "business"}}, {"dentist", {"dentist", "business"}}, {"veterinary", {"vet", "business"}},
            {"school", {"school", "school"}}, {"kindergarten", {"school", "school"}}, {"college", {"school", "school"}},
            {"university", {"university", "school"}}, {"police", {"police", "police"}}, {"fire_station", {"fireStation", "government"}},
            {"townhall", {"townhall", "government"}}, {"courthouse", {"courthouse", "government"}}, {"prison", {"government", "government"}},
            {"post_office", {"postOffice", "government"}}, {"library", {"library", "government"}}, {"place_of_worship", {"church", "religious"}},
            {"marketplace", {"market", "market"}}, {"bus_station", {"busStation", "business"}}, {"ferry_terminal", {"ferryTerminal", "port"}},
            {"grave_yard", {"cemetery", "cemetery"}}, {"theatre", {"theatre", "landmark"}}, {"cinema", {"cinema", "business"}},
            {"car_wash", {"carWash", "business"}}, {"car_repair", {"carRepair", "business"}}, {"arts_centre", {"museum", "landmark"}},
            {"community_centre", {"government", "government"}}, {"social_facility", {"government", "government"}},
            {"public_building", {"government", "government"}}, {"bureau_de_change", {"bank", "business"}},
        };
        for (const auto& [k, v] : kAmenity)
            if (amenity == k)
                return amenity == "place_of_worship" && building == "cathedral" ? P{"cathedral", "religious"} : P{v.first, v.second};
    }
    if (!shop.empty())
    {
        static const std::pair<const char*, const char*> kShop[] = {
            {"convenience", "grocery"}, {"general", "grocery"}, {"kiosk", "grocery"}, {"variety_store", "grocery"}, {"supermarket", "supermarket"},
            {"butcher", "butcher"}, {"car_parts", "carParts"}, {"tyres", "tyres"}, {"car_repair", "carRepair"}, {"car", "carRepair"},
            {"hardware", "hardware"}, {"doityourself", "hardware"}, {"trade", "hardware"}, {"clothes", "clothes"}, {"boutique", "clothes"},
            {"fashion", "clothes"}, {"shoes", "shoes"}, {"mobile_phone", "electronics"}, {"electronics", "electronics"}, {"computer", "electronics"},
            {"furniture", "furniture"}, {"stationery", "stationery"}, {"hairdresser", "hairdresser"}, {"beauty", "hairdresser"},
            {"laundry", "laundry"}, {"dry_cleaning", "laundry"}, {"alcohol", "liquor"}, {"beverages", "liquor"}, {"florist", "florist"},
            {"jewelry", "jewelry"}, {"optician", "optician"}, {"bakery", "bakery"}, {"pastry", "bakery"}, {"confectionery", "bakery"},
            {"mall", "mall"}, {"department_store", "mall"}, {"seafood", "seafood"}, {"tortilla", "tortilleria"}, {"ice_cream", "iceCream"},
            {"chemist", "pharmacy"}, {"medical_supply", "pharmacy"}, {"greengrocer", "grocery"},
        };
        for (const auto& [k, v] : kShop)
            if (shop == k)
                return P{v, "business"};
        return P{"shop", "business"};
    }
    if (!tourism.empty())
    {
        if (tourism == "hotel" || tourism == "guest_house" || tourism == "hostel" || tourism == "apartment")
            return P{"hotel", "business"};
        if (tourism == "motel")
            return P{"motel", "business"};
        if (tourism == "museum" || tourism == "gallery")
            return P{"museum", "landmark"};
        if (tourism == "attraction" || tourism == "viewpoint" || tourism == "artwork")
            return P{"", "landmark"};
    }
    if (!office.empty())
        return office == "government" ? P{"government", "government"} : P{"office", "business"};
    if (!leisure.empty())
    {
        if (leisure == "stadium")
            return P{"stadium", "stadium"};
        if (leisure == "park" || leisure == "garden")
            return P{"park", "park"};
        if (leisure == "marina")
            return P{"marina", "port"};
        if (leisure == "fitness_centre")
            return P{"gym", "business"};
        if (leisure == "sports_centre" || leisure == "pitch" || leisure == "track")
            return P{"sports", "park"};
    }
    if (harbour == "yes" || landuse == "port" || tag(t, "seamark:type") == "harbour" || industrial == "port")
        return P{"port", "port"};
    if (!aeroway.empty())
    {
        if (aeroway == "aerodrome" || aeroway == "airport")
            return tag(t, "aerodrome:type") == "private" || tag(t, "aerodrome") == "private" ? P{"airstrip", "airport"} : P{"airport", "airport"};
        if (aeroway == "airstrip")
            return P{"airstrip", "airport"};
        if (aeroway == "terminal")
            return P{"airport", "airport"};
        if (aeroway == "hangar")
            return P{"warehouse", "building"};
    }
    if (manMade == "lighthouse")
        return P{"lighthouse", "landmark"};
    if (manMade == "works")
        return P{"factory", "business"};
    if (!industrial.empty())
        return industrial == "warehouse" ? P{"warehouse", "business"} : P{"factory", "business"};
    if (!craft.empty())
        return P{"shop", "business"};
    if (landuse == "cemetery")
        return P{"cemetery", "cemetery"};
    if (landuse == "farmland" || landuse == "orchard" || landuse == "farmyard" || landuse == "vineyard")
        return P{"farm", "business"};
    if (landuse == "greenhouse_horticulture")
        return P{"greenhouse", "business"};
    if (landuse == "industrial")
        return P{"factory", "business"};
    if (landuse == "military")
        return P{"military", "government"};
    if (!place.empty())
    {
        if (place == "city" || place == "town" || place == "village" || place == "hamlet" || place == "isolated_dwelling" || place == "municipality")
            return P{"town", "town"};
        if (place == "suburb" || place == "neighbourhood" || place == "quarter" || place == "borough" || place == "city_block")
            return P{"district", "district"};
        if (place == "island" || place == "islet")
            return P{"island", "island"};
    }
    if (nat == "beach")
        return P{"beach", "beach"};
    if (nat == "bay")
        return P{"bay", "bay"};
    if (nat == "peak" || nat == "volcano" || nat == "hill" || nat == "ridge")
        return P{"mountain", "mountain"};
    if (!building.empty())
    {
        static const std::pair<const char*, std::pair<const char*, const char*>> kBuilding[] = {
            {"church", {"church", "religious"}}, {"chapel", {"church", "religious"}}, {"cathedral", {"cathedral", "religious"}},
            {"mosque", {"church", "religious"}}, {"temple", {"church", "religious"}}, {"synagogue", {"church", "religious"}},
            {"school", {"school", "school"}}, {"kindergarten", {"school", "school"}}, {"university", {"university", "school"}},
            {"college", {"school", "school"}}, {"hospital", {"hospital", "hospital"}}, {"civic", {"government", "government"}},
            {"public", {"government", "government"}}, {"government", {"government", "government"}}, {"townhall", {"townhall", "government"}},
            {"fire_station", {"fireStation", "government"}}, {"train_station", {"busStation", "building"}}, {"transportation", {"busStation", "building"}},
            {"hotel", {"hotel", "business"}}, {"retail", {"shop", "business"}}, {"commercial", {"shop", "business"}}, {"supermarket", {"supermarket", "business"}},
            {"kiosk", {"grocery", "business"}}, {"office", {"office", "business"}}, {"industrial", {"factory", "business"}},
            {"warehouse", {"warehouse", "business"}}, {"factory", {"factory", "business"}}, {"manufacture", {"factory", "business"}},
            {"greenhouse", {"greenhouse", "business"}}, {"farm", {"farm", "business"}}, {"barn", {"farm", "business"}},
            {"farm_auxiliary", {"farm", "business"}}, {"stable", {"farm", "business"}}, {"hangar", {"warehouse", "building"}},
            {"stadium", {"stadium", "stadium"}}, {"grandstand", {"stadium", "stadium"}}, {"apartments", {"apartments", "building"}},
            {"residential", {"apartments", "building"}}, {"dormitory", {"apartments", "building"}}, {"mansion", {"mansion", "building"}},
            {"villa", {"mansion", "building"}},
        };
        for (const auto& [k, v] : kBuilding)
            if (building == k)
                return P{v.first, v.second};
        return P{"", "building"};
    }
    return {};
}

OsmExtract extractOsm(const OsmData& d)
{
    LILA_ZONE();

    OsmExtract x;
    std::unordered_map<int64_t, const OsmData::Way*> byId;
    for (const OsmData::Way& w : d.ways)
        byId[w.id] = &w;
    std::vector<std::vector<OsmPoint>> coast;

    auto addArea = [&](const std::string& cls, int64_t id, const std::vector<std::vector<OsmPoint>>& rings) {
        OsmArea a{cls, id, {}};
        for (const auto& r : rings)
            if (r.size() >= 3)
                a.rings.push_back(positions(r, true));
        if (!a.rings.empty())
            x.areas.push_back(std::move(a));
    };
    auto addLake = [&](const std::string& cls, int64_t id, const OsmTags& t, const std::vector<OsmPoint>& ring) {
        if (ring.size() < 3)
            return;
        x.features.push_back({OsmFeatureKind::Lake, id, std::string(tag(t, "name")), cls, 0.0, true, positions(ring, true)});
    };
    auto isBuilding = [](const OsmTags& t) {
        const std::string_view b = tag(t, "building"), p = tag(t, "building:part");
        return (!b.empty() && b != "no") || (!p.empty() && p != "no");
    };
    auto addBuilding = [&](int64_t id, char type, int piece, const OsmTags& t, const std::vector<OsmPoint>& outer,
                           const std::vector<std::vector<OsmPoint>>& holes) {
        if (outer.size() < 4)
            return;
        OsmBuilding b;
        b.id = id;
        b.type = type;
        b.piece = piece;
        const std::string_view partTag = tag(t, "building:part");
        b.part = !partTag.empty() && partTag != "no" && (tag(t, "building").empty() || tag(t, "building") == "no");
        b.building = std::string(b.part ? partTag : tag(t, "building"));
        b.outer = positions(outer, true);
        for (const auto& h : holes)
            if (h.size() >= 4)
                b.holes.push_back(positions(h, true));
        b.height = metresTag(tag(t, "height"));
        b.minHeight = metresTag(tag(t, "min_height"));
        b.roofHeight = metresTag(tag(t, "roof:height"));
        b.levels = levelsTag(tag(t, "building:levels"));
        b.minLevel = levelsTag(tag(t, "building:min_level"));
        b.roofLevels = levelsTag(tag(t, "roof:levels"));
        b.roofShape = std::string(tag(t, "roof:shape"));
        b.colour = std::string(tag(t, "building:colour"));
        b.roofColour = std::string(tag(t, "roof:colour"));
        b.material = std::string(tag(t, "building:material"));
        b.roofMaterial = std::string(tag(t, "roof:material"));
        b.startDate = std::string(tag(t, "start_date"));
        std::tie(b.program, b.kind) = osmProgram(t);
        if (b.kind.empty())
            b.kind = "building";
        b.realNames = realNamesOf(t);
        b.chain = chainOf(t);
        x.buildings.push_back(std::move(b));
    };
    auto addPlace = [&](int64_t id, char type, const OsmTags& t, LatLon at, const std::vector<std::vector<OsmPoint>>& rings) {
        auto [program, kind] = osmProgram(t);
        if (kind.empty() || kind == "building")
            return;
        OsmPlace p;
        p.id = id;
        p.type = type;
        for (const char* k : {"amenity", "shop", "tourism", "office", "leisure", "harbour", "aeroway", "man_made", "industrial", "craft",
                              "landuse", "place", "natural"})
            if (const std::string_view v = tag(t, k); !v.empty())
            {
                p.key = k;
                p.value = std::string(v);
                break;
            }
        p.program = std::move(program);
        p.kind = std::move(kind);
        p.at = at;
        for (const auto& r : rings)
            if (r.size() >= 4)
                p.rings.push_back(positions(r, true));
        if (!p.rings.empty())
            p.at = ringCentre(p.rings.front());
        p.realNames = realNamesOf(t);
        p.chain = chainOf(t);
        x.places.push_back(std::move(p));
    };
    auto structureKind = [](std::string_view mm) {
        return mm == "breakwater" || mm == "groyne" ? OsmFeatureKind::Breakwater : mm == "quay" ? OsmFeatureKind::Quay : OsmFeatureKind::Pier;
    };

    // Outer ways of building multipolygons: the relation is the building, so a way tagged as one too is not a second.
    std::unordered_set<int64_t> buildingOuters;
    for (const OsmData::Relation& r : d.relations)
        if (tag(r.tags, "type") == "multipolygon" && isBuilding(r.tags))
            for (const OsmData::Member& m : r.members)
                if (m.type == 'w' && m.role != "inner")
                    buildingOuters.insert(m.ref);

    for (const OsmData::Way& w : d.ways)
    {
        if (w.points.size() < 2)
            continue;
        const OsmTags& t = w.tags;
        const std::string_view hw = tag(t, "highway"), ww = tag(t, "waterway"), nat = tag(t, "natural"), mm = tag(t, "man_made");
        const std::string name(tag(t, "name"));
        if (!hw.empty() && tag(t, "area") != "yes")
        {
            double m = widthTag(tag(t, "width"));
            if (m <= 0.0)
                if (const double lanes = widthTag(tag(t, "lanes")); lanes > 0.0)
                    m = lanes * 3.5;
            const bool path = isPath(hw);
            if (m <= 0.0)
                m = path ? (hw == "track" ? 3.5 : 2.0) : defaultRoadWidth(hw);
            x.features.push_back({path ? OsmFeatureKind::Path : OsmFeatureKind::Road, w.id, name, std::string(hw), m * 100.0, isClosed(w.points), positions(w.points, false)});
        }
        else if (ww == "river" || ww == "canal" || ww == "stream" || ww == "brook" || ww == "ditch" || ww == "drain" || ww == "tidal_channel")
        {
            const bool river = ww == "river" || ww == "canal" || ww == "tidal_channel";
            double m = widthTag(tag(t, "width"));
            if (m <= 0.0)
                m = ww == "river" ? 20.0 : ww == "canal" ? 15.0 : ww == "stream" || ww == "brook" ? 3.0 : 1.5;
            x.features.push_back({river ? OsmFeatureKind::River : OsmFeatureKind::Stream, w.id, name, std::string(ww), m * 100.0, false, positions(w.points, false)});
        }
        else if (ww == "dam" || ww == "weir")
        {
            const double m = widthTag(tag(t, "width"));
            x.features.push_back({OsmFeatureKind::Dam, w.id, name, std::string(ww), (m > 0.0 ? m : ww == "dam" ? 10.0 : 2.0) * 100.0,
                                  isClosed(w.points), positions(w.points, isClosed(w.points))});
        }
        else if (isStructure(mm))
        {
            const double m = widthTag(tag(t, "width"));
            x.features.push_back({structureKind(mm), w.id, name, std::string(mm), (m > 0.0 ? m : mm == "quay" ? 0.0 : 6.0) * 100.0,
                                  isClosed(w.points), positions(w.points, isClosed(w.points))});
        }
        if (nat == "coastline")
            coast.push_back(w.points);
        if (isClosed(w.points))
        {
            if (const std::string lake = lakeOf(t); !lake.empty())
                addLake(lake, w.id, t, w.points);
            if (const std::string cover = landcoverOf(t); !cover.empty())
                addArea(cover, w.id, {w.points});
            if (isBuilding(t))
            {
                if (!buildingOuters.count(w.id))
                    addBuilding(w.id, 'w', 0, t, w.points, {});
            }
            else if (hw.empty() && !isStructure(mm))
                addPlace(w.id, 'w', t, {}, {w.points});
        }
    }

    for (const OsmData::Node& n : d.tagged)
        addPlace(n.id, 'n', n.tags, n.at, {});

    for (const OsmData::Relation& r : d.relations)
    {
        const std::string_view type = tag(r.tags, "type");
        if (type != "multipolygon" && type != "boundary")
            continue;
        const std::string lake = lakeOf(r.tags), cover = landcoverOf(r.tags);
        const bool building = type == "multipolygon" && isBuilding(r.tags);
        const bool structure = type == "multipolygon" && isStructure(tag(r.tags, "man_made"));
        const bool place = type == "multipolygon" && !building && !structure && !osmProgram(r.tags).second.empty();
        if (lake.empty() && cover.empty() && !building && !structure && !place)
            continue;
        std::vector<std::vector<OsmPoint>> outer, inner;
        for (const OsmData::Member& m : r.members)
        {
            if (m.type != 'w')
                continue;
            const std::vector<OsmPoint>* pts = nullptr;
            if (auto it = byId.find(m.ref); it != byId.end() && it->second->points.size() >= 2)
                pts = &it->second->points;
            else if (m.points.size() >= 2)
                pts = &m.points;
            if (!pts)
                continue;
            (m.role == "inner" ? inner : outer).push_back(*pts);
        }
        std::vector<std::vector<OsmPoint>> rings = joinWays(outer);
        std::erase_if(rings, [](const std::vector<OsmPoint>& c) { return !isClosed(c); });
        std::vector<std::vector<OsmPoint>> holes = joinWays(inner);
        std::erase_if(holes, [](const std::vector<OsmPoint>& c) { return !isClosed(c); });
        if (!lake.empty())
            for (const auto& ring : rings)
                addLake(lake, r.id, r.tags, ring);
        if (building)
        {
            // Each hole goes with the outer ring around it.
            for (size_t k = 0; k < rings.size(); ++k)
            {
                const std::vector<LatLon> o = positions(rings[k], true);
                std::vector<std::vector<OsmPoint>> mine;
                for (const auto& h : holes)
                    if (!h.empty() && inRing(o, h.front().at))
                        mine.push_back(h);
                addBuilding(r.id, 'r', int(k), r.tags, rings[k], mine);
            }
        }
        if (structure)
            for (const auto& ring : rings)
                x.features.push_back({structureKind(tag(r.tags, "man_made")), r.id, std::string(tag(r.tags, "name")),
                                      std::string(tag(r.tags, "man_made")), 0.0, true, positions(ring, true)});
        std::vector<std::vector<OsmPoint>> all = rings;
        all.insert(all.end(), holes.begin(), holes.end());
        if (place && !rings.empty())
            addPlace(r.id, 'r', r.tags, {}, all);
        if (!cover.empty())
            addArea(cover, r.id, all);
    }

    for (const std::vector<OsmPoint>& c : joinWays(coast))
        x.features.push_back({OsmFeatureKind::Coastline, 0, {}, "coastline", 0.0, isClosed(c), positions(c, isClosed(c))});
    return x;
}

void forEachPoint(OsmExtract& x, const std::function<void(LatLon&)>& f)
{
    for (OsmFeature& v : x.features)
        for (LatLon& p : v.points)
            f(p);
    for (OsmArea& a : x.areas)
        for (auto& r : a.rings)
            for (LatLon& p : r)
                f(p);
    for (OsmBuilding& b : x.buildings)
    {
        for (LatLon& p : b.outer)
            f(p);
        for (auto& r : b.holes)
            for (LatLon& p : r)
                f(p);
    }
    for (OsmPlace& pl : x.places)
    {
        f(pl.at);
        for (auto& r : pl.rings)
            for (LatLon& p : r)
                f(p);
    }
}

std::vector<std::vector<glm::dvec2>> clipToHeightfield(const std::vector<glm::dvec2>& in, const Heightfield& h, bool closed)
{
    std::vector<std::vector<glm::dvec2>> out;
    if (in.size() < 2)
        return out;
    std::vector<glm::dvec2> line = in;
    if (closed && line.front() != line.back())
        line.push_back(line.front());
    const glm::dvec2 lo = h.origin, hi = h.origin + h.extent();
    auto inside = [&](glm::dvec2 p) { return p.x >= lo.x && p.y >= lo.y && p.x <= hi.x && p.y <= hi.y; };
    if (std::all_of(line.begin(), line.end(), inside))
    {
        out.push_back(line);
        return out;
    }
    std::vector<glm::dvec2> run;
    for (size_t i = 1; i < line.size(); ++i)
    {
        // Liang-Barsky against the heightfield's rectangle.
        const glm::dvec2 a = line[i - 1], b = line[i], d = b - a;
        double t0 = 0.0, t1 = 1.0;
        bool visible = true;
        const double p[4] = {-d.x, d.x, -d.y, d.y}, q[4] = {a.x - lo.x, hi.x - a.x, a.y - lo.y, hi.y - a.y};
        for (int k = 0; k < 4 && visible; ++k)
        {
            if (p[k] == 0.0)
                visible = q[k] >= 0.0;
            else
            {
                const double r = q[k] / p[k];
                if (p[k] < 0.0)
                    t0 = std::max(t0, r);
                else
                    t1 = std::min(t1, r);
                visible = t0 <= t1;
            }
        }
        if (!visible)
        {
            if (run.size() >= 2)
                out.push_back(std::move(run));
            run.clear();
            continue;
        }
        const glm::dvec2 s = a + d * t0, e = a + d * t1;
        if (run.empty() || glm::length(run.back() - s) > 1e-6)
        {
            if (run.size() >= 2)
                out.push_back(std::move(run));
            run.assign(1, s);
        }
        run.push_back(e);
        if (t1 < 1.0)
        {
            if (run.size() >= 2)
                out.push_back(std::move(run));
            run.clear();
        }
    }
    if (run.size() >= 2)
        out.push_back(std::move(run));
    return out;
}

std::map<std::string, double> rasterizeLandcover(Heightfield& h, const std::vector<OsmArea>& areas, const Georef& g)
{
    LILA_ZONE();

    std::map<std::string, double> cover;
    std::map<std::string, std::vector<const OsmArea*>> byClass;
    for (const OsmArea& a : areas)
        byClass[a.cls].push_back(&a);
    for (const auto& [cls, list] : byClass)
    {
        std::vector<float>& m = h.mask("landcover_" + cls);
        for (const OsmArea* a : list)
        {
            std::vector<std::vector<glm::dvec2>> rings;
            double zlo = std::numeric_limits<double>::infinity(), zhi = -zlo;
            for (const auto& r : a->rings)
            {
                std::vector<glm::dvec2> ring;
                for (const LatLon& p : r)
                {
                    const glm::dvec2 q = g.toLocal(p);
                    ring.push_back(q);
                    zlo = std::min(zlo, q.y);
                    zhi = std::max(zhi, q.y);
                }
                rings.push_back(std::move(ring));
            }
            const int z0 = std::max(0, int(std::ceil((zlo - h.origin.y) / h.cell))), z1 = std::min(h.nz - 1, int(std::floor((zhi - h.origin.y) / h.cell)));
            if (z0 > z1)
                continue;
            tbb::parallel_for(z0, z1 + 1, [&](int z) {
                const double wz = h.origin.y + z * h.cell;
                std::vector<double> xs;
                for (const auto& ring : rings)
                    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
                    {
                        const glm::dvec2 a = ring[j], b = ring[i];
                        if ((a.y > wz) != (b.y > wz))
                            xs.push_back(a.x + (wz - a.y) * (b.x - a.x) / (b.y - a.y));
                    }
                std::sort(xs.begin(), xs.end());
                for (size_t k = 0; k + 1 < xs.size(); k += 2)
                {
                    const int x0 = std::max(0, int(std::ceil((xs[k] - h.origin.x) / h.cell)));
                    const int x1 = std::min(h.nx - 1, int(std::floor((xs[k + 1] - h.origin.x) / h.cell)));
                    for (int x = x0; x <= x1; ++x)
                        m[h.index(x, z)] = 1.0f;
                }
            });
        }
        double sum = 0.0;
        for (float v : m)
            sum += v;
        cover[cls] = sum / double(h.size());
    }
    return cover;
}

const char* osmAttribution()
{
    return "Map data from OpenStreetMap (c) OpenStreetMap contributors, available under the Open Database License (ODbL) "
           "https://www.openstreetmap.org/copyright";
}
} // namespace terrain
