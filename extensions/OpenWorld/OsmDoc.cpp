// OSM data read whole (nodes with tags, ways, relations) and written back as Overpass JSON.

#include "OsmDoc.h"

#include <lila/Profile.h>

#include <nlohmann/json.hpp>

#include <QXmlStreamReader>

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openworld
{
using json = nlohmann::json;

void OsmDoc::index()
{
    nodeAt.clear();
    nodeAt.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        nodeAt[nodes[i].id] = i;
}

const OsmDoc::Node* OsmDoc::node(int64_t id) const
{
    const auto it = nodeAt.find(id);
    return it == nodeAt.end() ? nullptr : &nodes[it->second];
}

std::string_view tagOf(const OsmTags& t, std::string_view key)
{
    const auto it = t.find(key);
    return it == t.end() ? std::string_view() : std::string_view(it->second);
}

namespace
{
/** A node id for a position given without one: negative, so it never meets a real OSM id. */
int64_t positionKey(LatLon p)
{
    const int64_t a = int64_t(std::llround(p.lat * 1e7)), b = int64_t(std::llround(p.lon * 1e7));
    return -int64_t((uint64_t(a) * 0x9E3779B97F4A7C15ull ^ uint64_t(b) * 0xC2B2AE3D27D4EB4Full) >> 2) - 1;
}

/** Adds positions given inline (Overpass out geom) as nodes, unless a node with that id is already there. */
void addInline(OsmDoc& d, const json& geometry, const json* ids, std::vector<int64_t>* refs)
{
    for (size_t i = 0; i < geometry.size(); ++i)
    {
        const json& g = geometry[i];
        if (!g.is_object() || !g.contains("lat"))
            continue;
        const LatLon p{g["lat"].get<double>(), g["lon"].get<double>()};
        const int64_t id = ids && i < ids->size() ? (*ids)[i].get<int64_t>() : positionKey(p);
        if (!d.nodeAt.count(id))
        {
            d.nodeAt[id] = d.nodes.size();
            d.nodes.push_back({id, p, {}});
        }
        if (refs)
            refs->push_back(id);
    }
}

OsmTags tagsOf(const json& e)
{
    OsmTags t;
    if (e.contains("tags") && e["tags"].is_object())
        for (const auto& [k, v] : e["tags"].items())
            t[k] = v.is_string() ? v.get<std::string>() : v.dump();
    return t;
}
} // namespace

OsmDoc parseOsmDocJson(std::string_view text)
{
    LILA_ZONE();

    const json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("elements") || !j["elements"].is_array())
        throw std::runtime_error("not Overpass JSON (no elements list)");
    OsmDoc d;
    for (const json& e : j["elements"])
        if (e.value("type", std::string()) == "node" && e.contains("lat"))
        {
            const int64_t id = e.value("id", int64_t(0));
            d.nodeAt[id] = d.nodes.size();
            d.nodes.push_back({id, {e["lat"].get<double>(), e["lon"].get<double>()}, tagsOf(e)});
        }
    for (const json& e : j["elements"])
    {
        const std::string type = e.value("type", std::string());
        if (type == "way")
        {
            OsmDoc::Way w;
            w.id = e.value("id", int64_t(0));
            w.tags = tagsOf(e);
            const json* ids = e.contains("nodes") && e["nodes"].is_array() ? &e["nodes"] : nullptr;
            if (e.contains("geometry") && e["geometry"].is_array())
                addInline(d, e["geometry"], ids, &w.nodes);
            else if (ids)
                w.nodes = ids->get<std::vector<int64_t>>();
            d.ways.push_back(std::move(w));
        }
        else if (type == "relation")
        {
            OsmDoc::Relation r;
            r.id = e.value("id", int64_t(0));
            r.tags = tagsOf(e);
            if (e.contains("members") && e["members"].is_array())
                for (const json& m : e["members"])
                {
                    const std::string mt = m.value("type", std::string("way"));
                    OsmDoc::Member mem{mt.empty() ? 'w' : mt[0], m.value("ref", int64_t(0)), m.value("role", std::string())};
                    // A member way known only by its inline geometry becomes a way of its own.
                    if (mem.type == 'w' && m.contains("geometry") && m["geometry"].is_array())
                    {
                        OsmDoc::Way w;
                        w.id = mem.ref;
                        addInline(d, m["geometry"], nullptr, &w.nodes);
                        d.ways.push_back(std::move(w));
                    }
                    r.members.push_back(std::move(mem));
                }
            d.relations.push_back(std::move(r));
        }
    }
    // Member ways given inline may repeat a way the data also has; the first (the full way) is kept.
    std::unordered_map<int64_t, size_t> seen;
    std::vector<OsmDoc::Way> ways;
    for (OsmDoc::Way& w : d.ways)
        if (seen.emplace(w.id, ways.size()).second)
            ways.push_back(std::move(w));
    d.ways = std::move(ways);
    d.index();
    return d;
}

OsmDoc parseOsmDocXml(std::string_view text)
{
    LILA_ZONE();

    QXmlStreamReader x(QByteArray(text.data(), qsizetype(text.size())));
    OsmDoc d;
    enum class In
    {
        None,
        Node,
        Way,
        Relation
    } in = In::None;
    while (!x.atEnd())
    {
        const QXmlStreamReader::TokenType t = x.readNext();
        if (t == QXmlStreamReader::StartElement)
        {
            const QStringView name = x.name();
            const QXmlStreamAttributes a = x.attributes();
            if (name == u"node")
            {
                in = In::Node;
                d.nodes.push_back({a.value(u"id").toLongLong(), {a.value(u"lat").toDouble(), a.value(u"lon").toDouble()}, {}});
            }
            else if (name == u"way")
            {
                in = In::Way;
                d.ways.push_back({a.value(u"id").toLongLong(), {}, {}});
            }
            else if (name == u"relation")
            {
                in = In::Relation;
                d.relations.push_back({a.value(u"id").toLongLong(), {}, {}});
            }
            else if (name == u"nd" && in == In::Way)
                d.ways.back().nodes.push_back(a.value(u"ref").toLongLong());
            else if (name == u"member" && in == In::Relation)
            {
                const QString type = a.value(u"type").toString();
                d.relations.back().members.push_back({type.isEmpty() ? 'w' : char(type[0].toLatin1()), a.value(u"ref").toLongLong(), a.value(u"role").toString().toStdString()});
            }
            else if (name == u"tag")
            {
                const std::string k = a.value(u"k").toString().toStdString(), v = a.value(u"v").toString().toStdString();
                if (in == In::Node)
                    d.nodes.back().tags[k] = v;
                else if (in == In::Way)
                    d.ways.back().tags[k] = v;
                else if (in == In::Relation)
                    d.relations.back().tags[k] = v;
            }
        }
        else if (t == QXmlStreamReader::EndElement)
        {
            const QStringView name = x.name();
            if (name == u"node" || name == u"way" || name == u"relation")
                in = In::None;
        }
    }
    if (x.hasError())
        throw std::runtime_error("not OSM XML: " + x.errorString().toStdString());
    d.index();
    return d;
}

OsmDoc readOsmDoc(const std::filesystem::path& path, [[maybe_unused]] std::vector<std::string>* notes)
{
    LILA_ZONE();

    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    // Overpass answers are JSON or XML even when saved under a .pbf name, so text content wins over the extension.
    const size_t first = bytes.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    const bool text = first != std::string::npos && (bytes[first] == '{' || bytes[first] == '<');
    try
    {
        if (ext == ".pbf" && !text)
        {
            const terrain::OsmData od = terrain::parseOsmPbf(bytes);
            OsmDoc d;
            for (const terrain::OsmData::Node& n : od.tagged)
            {
                d.nodeAt[n.id] = d.nodes.size();
                d.nodes.push_back({n.id, n.at, OsmTags(n.tags.begin(), n.tags.end())});
            }
            for (const terrain::OsmData::Way& w : od.ways)
            {
                OsmDoc::Way way{w.id, {}, OsmTags(w.tags.begin(), w.tags.end())};
                for (const terrain::OsmPoint& p : w.points)
                {
                    if (!d.nodeAt.count(p.key))
                    {
                        d.nodeAt[p.key] = d.nodes.size();
                        d.nodes.push_back({p.key, p.at, {}});
                    }
                    way.nodes.push_back(p.key);
                }
                d.ways.push_back(std::move(way));
            }
            for (const terrain::OsmData::Relation& r : od.relations)
            {
                OsmDoc::Relation rel{r.id, {}, OsmTags(r.tags.begin(), r.tags.end())};
                for (const terrain::OsmData::Member& m : r.members)
                    rel.members.push_back({m.type, m.ref, m.role});
                d.relations.push_back(std::move(rel));
            }
            d.index();
            return d;
        }
        if (ext == ".json" || (first != std::string::npos && bytes[first] == '{'))
            return parseOsmDocJson(bytes);
        return parseOsmDocXml(bytes);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(path.filename().string() + ": " + e.what());
    }
}

std::string writeOsmDocJson(const OsmDoc& d)
{
    LILA_ZONE();

    auto tags = [](const OsmTags& t) {
        json o = json::object();
        for (const auto& [k, v] : t)
            o[k] = v;
        return o;
    };
    json elements = json::array();
    for (const OsmDoc::Node& n : d.nodes)
    {
        json e = {{"type", "node"}, {"id", n.id}, {"lat", n.at.lat}, {"lon", n.at.lon}};
        if (!n.tags.empty())
            e["tags"] = tags(n.tags);
        elements.push_back(std::move(e));
    }
    for (const OsmDoc::Way& w : d.ways)
    {
        json e = {{"type", "way"}, {"id", w.id}, {"nodes", w.nodes}};
        if (!w.tags.empty())
            e["tags"] = tags(w.tags);
        elements.push_back(std::move(e));
    }
    for (const OsmDoc::Relation& r : d.relations)
    {
        json members = json::array();
        for (const OsmDoc::Member& m : r.members)
            members.push_back({{"type", m.type == 'n' ? "node" : m.type == 'r' ? "relation" : "way"}, {"ref", m.ref}, {"role", m.role}});
        json e = {{"type", "relation"}, {"id", r.id}, {"members", members}};
        if (!r.tags.empty())
            e["tags"] = tags(r.tags);
        elements.push_back(std::move(e));
    }
    return json{{"version", 0.6}, {"generator", "Lila World Remix"}, {"elements", elements}}.dump();
}

terrain::OsmData toOsmData(const OsmDoc& d)
{
    terrain::OsmData out;
    out.nodes = d.nodes.size();
    for (const OsmDoc::Node& n : d.nodes)
        if (!n.tags.empty())
            out.tagged.push_back({n.id, n.at, terrain::OsmTags(n.tags.begin(), n.tags.end())});
    for (const OsmDoc::Way& w : d.ways)
    {
        terrain::OsmData::Way way;
        way.id = w.id;
        way.tags = terrain::OsmTags(w.tags.begin(), w.tags.end());
        for (int64_t id : w.nodes)
            if (const OsmDoc::Node* n = d.node(id))
                way.points.push_back({id, n->at});
            else
                ++out.missingNodes;
        out.ways.push_back(std::move(way));
    }
    for (const OsmDoc::Relation& r : d.relations)
    {
        terrain::OsmData::Relation rel;
        rel.id = r.id;
        rel.tags = terrain::OsmTags(r.tags.begin(), r.tags.end());
        for (const OsmDoc::Member& m : r.members)
            rel.members.push_back({m.type, m.ref, m.role, {}});
        out.relations.push_back(std::move(rel));
    }
    return out;
}

WayKind wayKind(const OsmDoc::Way& w)
{
    const bool closed = w.nodes.size() > 2 && w.nodes.front() == w.nodes.back();
    if (tagOf(w.tags, "natural") == "coastline")
        return WayKind::Coastline;
    if (closed && (tagOf(w.tags, "area") == "yes" || !tagOf(w.tags, "building").empty() || !tagOf(w.tags, "landuse").empty() ||
                   (tagOf(w.tags, "highway").empty() && tagOf(w.tags, "waterway").empty() && tagOf(w.tags, "barrier").empty())))
        return WayKind::Area;
    if (!tagOf(w.tags, "highway").empty())
        return WayKind::Road;
    const std::string_view ww = tagOf(w.tags, "waterway");
    if (ww == "river" || ww == "stream" || ww == "canal" || ww == "ditch" || ww == "drain" || ww == "brook")
        return WayKind::River;
    return closed ? WayKind::Area : WayKind::Other;
}
} // namespace openworld
