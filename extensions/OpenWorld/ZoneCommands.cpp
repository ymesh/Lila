// Zone commands: the zone set node, layers and zones (drawn, painted, or derived from Settlement districts, terrain
// masks and water routes), point queries, the check, the spec, and exports (engine JSON with PNG masks, Unreal). The spec
// lives on the zone set's zones attribute, so every edit is a setAttr and undoes, and it saves with the scene.

#include "OpenWorld.h"
#include "ZoneScene.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mcp.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

using namespace lila;
using namespace lila::ops;
namespace fs = std::filesystem;

namespace openworld::zones
{
namespace
{
using T = ArgType;
using scene::Graph;
using scene::Node;

// ----------------------------------------------------------------------------------------------------------------------
// Scene access

std::string textAttr(const Node& n, const char* attr)
{
    const int i = n.type().attrIndex(attr);
    if (i < 0)
        return {};
    const std::string* s = std::get_if<std::string>(&n.value(i));
    return s ? *s : std::string();
}

struct SetRef
{
    std::string path;
    scene::NodeId id = scene::kNoNode;
    ZoneSpec spec;
    /** Set when this call made the set. */
    bool created = false;
    std::string groundFrom;
};

bool isSet(const Node& n) { return n.type().isA(kSetType); }

/** The only terrain in the scene, for a new set's ground; empty when there are none or several. */
std::string onlyTerrain()
{
    Graph& g = Graph::current();
    std::string found;
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && groundOf(n->path()).valid())
        {
            if (!found.empty())
                return {};
            found = n->path();
        }
    return found;
}

SetRef readSet(Node& n, const std::string& cmd)
{
    SetRef r;
    r.path = n.path();
    r.id = n.id();
    std::string err;
    std::shared_ptr<const ZoneSpec> s = specOf(n, &err);
    if (!s)
        throw CommandError(cmd + ": the zone set's spec is broken: " + err);
    r.spec = *s;
    return r;
}

/** The zone set a name means; empty takes the selection, then the only set; create makes one when there is none. */
SetRef findSet(const std::string& name, const std::string& cmd, bool create, const Args* a = nullptr)
{
    Graph& g = Graph::current();
    if (!name.empty())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(cmd + ": no node named '" + name + "'");
        if (!isSet(*n))
            throw CommandError(cmd + ": " + n->path() + " is not a zone set (make one with zoneCreate or zoneLayer)");
        return readSet(*n, cmd);
    }
    for (scene::NodeId id : g.selection())
        if (Node* n = g.node(id); n && isSet(*n))
            return readSet(*n, cmd);
    const std::vector<scene::NodeId> all = zoneSets();
    if (all.size() == 1)
        return readSet(*g.node(all[0]), cmd);
    if (all.size() > 1)
        throw CommandError(cmd + ": several zone sets; name one");
    if (!create)
        throw CommandError(cmd + ": no zone set in the scene (make one with zoneCreate or zoneLayer)");
    Node* n = g.createNode(*scene::types::find(kSetType), a ? a->get<std::string>("setName", "Zones") : "Zones");
    SetRef r;
    r.path = n->path();
    r.id = n->id();
    r.created = true;
    std::string ground = a ? a->get<std::string>("ground", "") : std::string();
    if (!ground.empty())
    {
        if (!groundOf(ground).valid())
            throw CommandError(cmd + ": " + ground + " is not a terrain");
        r.groundFrom = "given";
    }
    else if ((ground = onlyTerrain()); !ground.empty())
        r.groundFrom = "the only terrain in the scene";
    else
        r.groundFrom = "none: no terrain in the scene, zones lie at height 0";
    if (!ground.empty())
        ops::execute("setAttr", {{"args", {r.path + "." + kGroundAttr, groundOf(ground).mesh}}});
    if (const Ground gr = groundOf(ground); gr.valid())
        r.spec.maskCell = std::clamp(std::round(std::max(gr.h->extent().x, gr.h->extent().y) / 2048.0 / 100.0) * 100.0, 100.0, 5000.0);
    return r;
}

void store(const SetRef& r, const ZoneSpec& s) { ops::execute("setAttr", {{"args", {r.path + "." + kSpecAttr, toJson(s).dump()}}}); }

json setHeader(const SetRef& r)
{
    json out = {{"set", r.path}};
    if (r.created)
    {
        out["created"] = true;
        out["groundFrom"] = r.groundFrom;
    }
    return out;
}

// ----------------------------------------------------------------------------------------------------------------------
// Names

std::string capitalised(std::string s)
{
    if (!s.empty())
        s[0] = char(std::toupper(uint8_t(s[0])));
    return s;
}

/** "goFast" as "go fast": craft ids read as words (and never as the brand a joined name can match). */
std::string spacedWords(const std::string& id)
{
    std::string out;
    for (char ch : id)
    {
        if (std::isupper(uint8_t(ch)) && !out.empty())
            out += ' ';
        out += char(std::tolower(uint8_t(ch)));
    }
    return out;
}

std::string singular(const std::string& label)
{
    if (label.size() > 3 && label.ends_with("ies"))
        return label.substr(0, label.size() - 3) + "y";
    if (label.size() > 1 && label.ends_with("s") && !label.ends_with("ss"))
        return label.substr(0, label.size() - 1);
    return label;
}

/**
 * A fictional name for a new zone: fictionalName in the set's culture when Lila has it (stable by set, layer and zone),
 * else the layer's label and a number. Never repeats a name already in the layer.
 */
std::string newName(const SetRef& r, const ZoneSpec& s, const Layer& l, const std::string& zoneId, std::string* source, std::string* note)
{
    std::set<std::string> taken;
    for (const Zone& z : l.zones)
        if (z.id != zoneId)
            taken.insert(z.name);
    const LayerType* t = s.type(l.type);
    if (ops::find("fictionalName"))
    {
        for (int attempt = 0; attempt < 8; ++attempt)
            try
            {
                const std::string key = r.path + "/" + l.id + "/" + zoneId + (attempt ? "#" + std::to_string(attempt) : "");
                const json n = ops::execute("fictionalName", {{"culture", s.culture}, {"kind", t ? t->nameKind : "district"}, {"key", key}});
                if (const std::string name = n.value("name", std::string()); !name.empty() && !taken.count(name))
                {
                    *source = "fictionalName";
                    return name;
                }
            }
            catch (const CommandError& e)
            {
                *note = std::string("fictionalName failed (") + e.what() + "); numbered names used";
                break;
            }
    }
    else
        *note = "fictionalName is not in this Lila (naming cultures come with OSM places); numbered names used";
    *source = "numbered";
    for (int k = int(l.zones.size()) + (l.zone(zoneId) ? 0 : 1);; ++k)
        if (std::string n = singular(l.label) + " " + std::to_string(k); !taken.count(n))
            return n;
}

// ----------------------------------------------------------------------------------------------------------------------
// Layers and zones

std::string sanitizeId(std::string s)
{
    for (char& c : s)
        if (!std::isalnum(uint8_t(c)) && c != '_' && c != '-')
            c = '_';
    return s;
}

/** The layer a call means: layer= by id, made from type= (or a layer id that names a type) when missing. */
Layer& layerFor(ZoneSpec& s, const Args& a, const std::string& cmd, bool* created, const std::string& fallbackType = {})
{
    std::string id = a.get<std::string>("layer", "");
    const std::string type = a.get<std::string>("type", id.empty() ? fallbackType : "");
    if (id.empty())
        id = type;
    if (id.empty())
        throw CommandError(cmd + ": give layer= (a layer id) or type= (zoneTypes lists them)");
    if (Layer* l = s.layer(id))
    {
        if (!type.empty() && type != l->type)
            throw CommandError(cmd + ": layer '" + id + "' is of type " + l->type + ", not " + type);
        return *l;
    }
    const std::string ty = type.empty() ? id : type;
    const LayerType* t = s.type(ty);
    if (!t)
        throw CommandError(cmd + ": no layer '" + id + "' and '" + ty + "' is not a layer type (zoneTypes lists them; give type=)");
    Layer l = makeLayer(*t, sanitizeId(id));
    l.priority = int(s.layers.size());
    s.layers.push_back(std::move(l));
    *created = true;
    return s.layers.back();
}

std::string nextZoneId(const Layer& l)
{
    for (int k = 1;; ++k)
        if (std::string id = l.id + "_" + std::to_string(k); !l.zone(id))
            return id;
}

const char* jsonKind(const json& v)
{
    if (v.is_boolean())
        return "a bool";
    if (v.is_number())
        return "a number";
    if (v.is_string())
        return "text";
    if (v.is_array())
        return "a list";
    if (v.is_object())
        return "an object";
    return "null";
}

/** Merges attrs into a zone, typed by the layer's defaults; a null value removes the key. */
void mergeAttrs(const Layer& l, Zone& z, const json& attrs, const std::string& cmd)
{
    if (!attrs.is_object())
        throw CommandError(cmd + ": attrs is a dict");
    for (auto it = attrs.begin(); it != attrs.end(); ++it)
    {
        if (it->is_null())
        {
            z.attrs.erase(it.key());
            continue;
        }
        if (l.defaults.contains(it.key()))
        {
            const json& d = l.defaults[it.key()];
            if (std::string_view(jsonKind(d)) != jsonKind(*it) && !d.is_null())
                throw CommandError(cmd + ": " + l.id + " attribute '" + it.key() + "' is " + jsonKind(d) + ", not " + jsonKind(*it));
        }
        z.attrs[it.key()] = *it;
    }
}

Polygon circle(Vec2 c, double r)
{
    Polygon p;
    for (int i = 0; i < 32; ++i)
    {
        const double t = i * 6.283185307179586 / 32.0;
        p.push_back(c + Vec2(std::cos(t), std::sin(t)) * r);
    }
    return p;
}

Polygon ringOf(const json& pts, const std::string& what, const std::string& cmd)
{
    if (!pts.is_array())
        throw CommandError(cmd + ": " + what + " is a list of [x, z] points");
    Polygon r;
    for (const json& p : pts)
        r.push_back(planePoint(p, what, cmd));
    if (r.size() > 3 && glm::length(r.front() - r.back()) < 1e-6)
        r.pop_back();
    if (r.size() < 3)
        throw CommandError(cmd + ": " + what + " needs at least 3 points");
    return r;
}

/** Every closed spline of the curves, world space, as rings on the ground plane. */
std::vector<Polygon> curveRings(const json& curves, const std::string& cmd)
{
    std::vector<Polygon> out;
    std::vector<std::string> names;
    if (curves.is_string())
        names.push_back(curves.get<std::string>());
    else if (curves.is_array())
        for (const json& c : curves)
            names.push_back(c.get<std::string>());
    for (const std::string& name : names)
    {
        const json data = ops::execute("curveData", {{"args", {name}}, {"worldSpace", true}});
        if (!data.is_array() || data.empty())
            throw CommandError(cmd + ": " + name + " has no splines");
        for (const json& sp : data)
        {
            Polygon r;
            for (const json& p : sp.value("points", json::array()))
                r.push_back(planeOf({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}));
            if (r.size() > 3 && glm::length(r.front() - r.back()) < 1e-6)
                r.pop_back();
            if (r.size() >= 3)
                out.push_back(std::move(r));
        }
    }
    if (out.empty())
        throw CommandError(cmd + ": the curves have no spline of 3 points or more");
    return out;
}

/** Geometry flags (polygon, polygons, holes, curve, circle) into the zone; true when any were given. */
bool takeGeometry(const Args& a, Zone& z, const std::string& cmd)
{
    std::vector<Polygon> polys, holes;
    if (a.has("polygon"))
        polys.push_back(ringOf(a.flags["polygon"], "polygon", cmd));
    if (a.has("polygons"))
        for (const json& r : a.flags["polygons"])
            polys.push_back(ringOf(r, "polygons", cmd));
    if (a.has("curve"))
        for (Polygon& r : curveRings(a.flags["curve"], cmd))
            polys.push_back(std::move(r));
    if (a.has("circle"))
    {
        const json c = a.flags["circle"];
        if (!c.is_array() || c.size() < 3)
            throw CommandError(cmd + ": circle is [x, z, radius]");
        polys.push_back(circle(Vec2(c[0].get<double>(), c[c.size() == 4 ? 2 : 1].get<double>()), c.back().get<double>()));
    }
    if (a.has("holes"))
        for (const json& r : a.flags["holes"])
            holes.push_back(ringOf(r, "holes", cmd));
    if (polys.empty() && holes.empty())
        return false;
    if (!polys.empty())
        z.polygons = std::move(polys);
    if (a.has("holes"))
        z.holes = std::move(holes);
    return true;
}

json zoneJson(const Layer& l, const Zone& z)
{
    Vec2 lo, hi;
    json j = {{"layer", l.id}, {"id", z.id}, {"name", z.name}, {"priority", z.priority}, {"attrs", z.attrs}, {"polygons", z.polygons.size()},
              {"holes", z.holes.size()}, {"painted", z.hasMask()}, {"areaM2", areaM2(z, l)}};
    if (!z.nameSource.empty())
        j["nameSource"] = z.nameSource;
    if (bounds(z, lo, hi))
        j["bounds"] = {{lo.x, lo.y}, {hi.x, hi.y}};
    if (!z.source.is_null())
        j["source"] = z.source;
    return j;
}

json layerJson(const Layer& l, bool zones)
{
    double area = 0.0;
    json zs = json::array();
    for (const Zone& z : l.zones)
    {
        area += areaM2(z, l);
        if (zones)
            zs.push_back(zoneJson(l, z));
    }
    json j = {{"id", l.id},
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
              {"count", l.zones.size()},
              {"areaM2", area}};
    if (zones)
        j["zones"] = zs;
    return j;
}

/** Gives a new zone its name from the call or the culture; records why. */
void nameZone(const SetRef& r, const ZoneSpec& s, const Layer& l, Zone& z, const Args& a, json& out)
{
    if (a.has("name") && !a.get<std::string>("name").empty())
    {
        z.name = a.get<std::string>("name");
        z.nameSource = "given";
        return;
    }
    if (!z.name.empty())
        return;
    std::string note;
    z.name = newName(r, s, l, z.id, &z.nameSource, &note);
    if (!note.empty())
        out["notes"].push_back(note);
}

/** The zone a paint or edit names, made (with defaults) when create and missing. */
Zone& zoneFor(Layer& l, const Args& a, const std::string& cmd, bool create, bool* made)
{
    std::string id = a.get<std::string>("zone", a.get<std::string>("id", ""));
    if (!id.empty())
        if (Zone* z = l.zone(id))
            return *z;
    if (!create)
        throw CommandError(cmd + ": layer '" + l.id + "' has no zone '" + id + "'");
    Zone z;
    z.id = id.empty() ? nextZoneId(l) : sanitizeId(id);
    l.zones.push_back(std::move(z));
    *made = true;
    return l.zones.back();
}

std::vector<Vec2> pathPoints(const json& pts, const std::string& cmd)
{
    std::vector<Vec2> out;
    if (!pts.is_array())
        throw CommandError(cmd + ": points is a list of [x, z] or [x, y, z]");
    for (const json& p : pts)
        out.push_back(planePoint(p, "points", cmd));
    return out;
}

/** Dabs along a path, spaced a quarter of the radius. */
size_t paintPath(Mask& m, const std::vector<Vec2>& pts, Dab d, std::span<const double> radii = {})
{
    size_t changed = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const double ri = radii.empty() ? d.radius : radii[std::min(i, radii.size() - 1)];
        if (i == 0)
        {
            Dab one = d;
            one.center = pts[0];
            one.radius = ri;
            changed += paint(m, one);
            continue;
        }
        const double r0 = radii.empty() ? d.radius : radii[std::min(i - 1, radii.size() - 1)];
        const double len = glm::length(pts[i] - pts[i - 1]);
        const int n = std::max(1, int(std::ceil(len / std::max(std::min(r0, ri) * 0.25, m.cell * 0.5))));
        for (int k = 1; k <= n; ++k)
        {
            Dab one = d;
            const double t = double(k) / n;
            one.center = pts[i - 1] + (pts[i] - pts[i - 1]) * t;
            one.radius = r0 + (ri - r0) * t;
            changed += paint(m, one);
        }
    }
    return changed;
}

/** The play area a check uses, and where it came from. */
std::vector<Polygon> playAreaOf(const SetRef& r, std::string* from)
{
    if (!r.spec.playArea.empty())
    {
        *from = "spec";
        return r.spec.playArea;
    }
    Graph& g = Graph::current();
    if (Node* n = g.node(r.id))
        if (const Ground gr = groundOf(groundPathOf(*n)); gr.valid())
        {
            Vec2 lo, hi;
            gr.extent(lo, hi);
            *from = "ground " + gr.mesh;
            return {{lo, {hi.x, lo.y}, hi, {lo.x, hi.y}}};
        }
    *from = "zone bounds (no play area and no ground)";
    return {};
}

fs::path outputFolder(const std::string& name, json& out)
{
    const fs::path& root = Workspace::get().root();
    if (!root.empty())
        return root / "openworld" / "zones" / name;
    const fs::path tmp = fs::temp_directory_path() / "lila_zones" / name;
    out["notes"].push_back("no workspace open; files went to " + paths::toUtf8(tmp));
    return tmp;
}

std::string leafName(const std::string& path)
{
    const size_t bar = path.find_last_of('|');
    return bar == std::string::npos ? path : path.substr(bar + 1);
}

// ----------------------------------------------------------------------------------------------------------------------
// Unreal

std::string linkKey(Node& n)
{
    Graph& g = Graph::current();
    const std::string* s = std::get_if<std::string>(&n.value(scene::Transform::kLinkId));
    if (s && !s->empty())
        return *s;
    const std::string key = std::format("{:016x}", std::hash<std::string>{}(n.path() + std::to_string(n.id())));
    g.setValue({n.id(), scene::Transform::kLinkId, -1}, key);
    return key;
}

json ue(const glm::dvec3& p)
{
    const math::Vector v = math::toUnreal(math::Vector(p));
    return {v.x, v.y, v.z};
}

json unrealZones(const ZoneSpec& s, const Ground& gr, double height, double depth)
{
    json layers = json::array();
    for (const Layer& l : s.layers)
    {
        json zones = json::array();
        for (const Zone& z : l.zones)
        {
            std::vector<Polygon> rings = z.polygons, holes = z.holes;
            if (z.hasMask())
                maskOutline(z.mask, l.density ? 1 : 128, z.mask.cell * 0.5, rings, holes);
            if (rings.empty())
                continue;
            double lo = 1e300, hi = -1e300;
            for (const Polygon& ring : rings)
                for (Vec2 p : ring)
                {
                    const double h = gr.heightAt(p);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
            if (gr.valid())
            {
                // The ground inside can be higher or lower than at the outline: take the whole heightfield's range under the bounds.
                Vec2 a, b;
                bounds(z, a, b);
                const double step = std::max(gr.h->cell, (b.x - a.x) / 64.0);
                for (double x = a.x; x <= b.x; x += step)
                    for (double y = a.y; y <= b.y; y += std::max(gr.h->cell, (b.y - a.y) / 64.0))
                    {
                        const double h = gr.heightAt({x, y});
                        lo = std::min(lo, h);
                        hi = std::max(hi, h);
                    }
            }
            const double bottom = lo - depth, top = hi + height;
            auto ring3 = [&](const Polygon& ring) {
                json pts = json::array();
                for (Vec2 p : ring)
                    pts.push_back(ue(worldOf(p, 0.0)));
                return pts;
            };
            json outline = json::array(), holeJ = json::array(), pieces = json::array();
            for (const Polygon& ring : rings)
            {
                outline.push_back(ring3(ring));
                for (const Polygon& piece : convexPieces(ring, holes))
                    pieces.push_back(ring3(piece));
            }
            for (const Polygon& h : holes)
                holeJ.push_back(ring3(h));
            json attrs = l.defaults;
            attrs.update(z.attrs);
            json zj = {{"id", z.id},
                       {"name", z.name},
                       {"priority", l.priority * 1000 + z.priority},
                       {"attrs", attrs},
                       {"outline", outline},
                       {"holes", holeJ},
                       {"pieces", pieces},
                       {"bottom", ue(worldOf({0.0, 0.0}, bottom))[2]},
                       {"top", ue(worldOf({0.0, 0.0}, top))[2]}};
            glm::ivec2 a, b;
            if (z.mask.cellBounds(a, b))
            {
                const glm::ivec2 size = b - a + 1;
                std::vector<uint8_t> v(size_t(size.x) * size_t(size.y));
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                        v[size_t(y) * size_t(size.x) + size_t(x)] = z.mask.cellValue(a.x + x, a.y + y);
                const json o = ue(worldOf(Vec2(a) * z.mask.cell, 0.0));
                const json ex = ue(worldOf({z.mask.cell, 0.0}, 0.0)), ez = ue(worldOf({0.0, z.mask.cell}, 0.0));
                zj["mask"] = {{"origin", o}, {"axisU", ex}, {"axisV", ez}, {"size", {size.x, size.y}}, {"values", base64(v)}};
            }
            zones.push_back(std::move(zj));
        }
        layers.push_back({{"id", l.id},
                          {"type", l.type},
                          {"label", l.label},
                          {"priority", l.priority},
                          {"density", l.density},
                          {"exclusive", l.exclusive},
                          {"dataLayer", l.dataLayer.empty() ? l.label : l.dataLayer},
                          {"color", {l.color.r, l.color.g, l.color.b}},
                          {"zones", zones}});
    }
    return layers;
}

// ----------------------------------------------------------------------------------------------------------------------
// Check

json checkJson(const SetRef& r, double cell, int minCells)
{
    std::string from;
    const std::vector<Polygon> play = playAreaOf(r, &from);
    const std::vector<Issue> issues = check(r.spec, play, cell, minCells);
    json list = json::array();
    std::map<std::string, int> counts{{"gap", 0}, {"overlap", 0}, {"empty", 0}, {"unnamed", 0}};
    for (const Issue& i : issues)
    {
        list.push_back(toJson(i));
        ++counts[i.kind];
    }
    return {{"ok", issues.empty()}, {"issues", list}, {"counts", counts}, {"playAreaFrom", from}, {"set", r.path}};
}

const std::vector<std::string>& typeIds()
{
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (const LayerType& t : builtInTypes())
            v.push_back(t.id);
        return v;
    }();
    return ids;
}

FlagDef setFlag() { return nodeFlag({"set", "zs", T::String, nullptr, "The zone set (default the selected one, else the only one)."}, "zoneSet"); }

std::string setName(const Args& a)
{
    if (a.has("set"))
        return a.get<std::string>("set");
    return a.positional.empty() ? std::string() : a.positional[0].get<std::string>();
}
} // namespace

// ----------------------------------------------------------------------------------------------------------------------

bool zUp() { return math::upAxis() == math::UpAxis::Z; }

glm::dvec3 worldOf(Vec2 p, double up) { return zUp() ? glm::dvec3(p.x, p.y, up) : glm::dvec3(p.x, up, p.y); }
Vec2 planeOf(const glm::dvec3& w) { return zUp() ? Vec2(w.x, w.y) : Vec2(w.x, w.z); }
double upOf(const glm::dvec3& w) { return zUp() ? w.z : w.y; }

double Ground::heightAt(Vec2 p) const
{
    if (!valid())
        return 0.0;
    const glm::dvec3 l = glm::dvec3(inv * glm::dvec4(worldOf(p, 0.0), 1.0));
    const double y = h->heightAt(l.x, l.z);
    return upOf(glm::dvec3(world * glm::dvec4(l.x, y, l.z, 1.0)));
}

bool Ground::extent(Vec2& lo, Vec2& hi) const
{
    if (!valid())
        return false;
    lo = Vec2(1e300);
    hi = Vec2(-1e300);
    const glm::dvec2 o = h->origin, e = h->origin + h->extent();
    for (glm::dvec2 c : {o, glm::dvec2(e.x, o.y), e, glm::dvec2(o.x, e.y)})
    {
        const glm::dvec3 w = glm::dvec3(world * glm::dvec4(c.x, 0.0, c.y, 1.0));
        lo = glm::min(lo, planeOf(w));
        hi = glm::max(hi, planeOf(w));
    }
    return true;
}

Ground groundOf(const std::string& mesh)
{
    Ground out;
    if (mesh.empty())
        return out;
    Graph& g = Graph::current();
    Node* n = g.find(mesh);
    if (!n)
        return out;
    if (gn::isGnModifier(*n))
        if (scene::NodeId m = scene::modifiers::meshOf(g, n->id()); m != scene::kNoNode)
            n = g.node(m);
    if (!n || !n->type().isA("mesh"))
        return out;
    const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
            if (auto h = gn::modifierHeightfield(m->id()); h && !h->empty())
            {
                out.mesh = n->path();
                out.h = std::move(h);
                out.world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                out.inv = glm::inverse(out.world);
                return out;
            }
    return out;
}

std::vector<scene::NodeId> zoneSets()
{
    // The overlay asks every frame; the sets change only when nodes come or go.
    static const Graph* cachedGraph = nullptr;
    static uint64_t cachedStructure = 0;
    static std::vector<scene::NodeId> cached;
    Graph& g = Graph::current();
    if (cachedGraph == &g && cachedStructure == g.structure())
        return cached;
    std::vector<scene::NodeId> out;
    for (scene::NodeId id : g.nodes())
        if (Node* n = g.node(id); n && isSet(*n))
            out.push_back(id);
    cachedGraph = &g;
    cachedStructure = g.structure();
    cached = out;
    return out;
}

std::shared_ptr<const ZoneSpec> specOf(const Node& set, std::string* error)
{
    static std::mutex m;
    static std::map<uint64_t, std::pair<std::string, std::shared_ptr<const ZoneSpec>>> cache;
    static const std::string none;
    const int ai = set.type().attrIndex(kSpecAttr);
    const std::string* held = ai < 0 ? nullptr : std::get_if<std::string>(&set.value(ai));
    const std::string& text = held ? *held : none;
    std::lock_guard lock(m);
    auto it = cache.find(set.id());
    if (it != cache.end() && it->second.first == text)
        return it->second.second;
    std::shared_ptr<const ZoneSpec> s;
    try
    {
        s = std::make_shared<const ZoneSpec>(text.empty() ? ZoneSpec{} : zonesFromJson(json::parse(text)));
    }
    catch (const std::exception& e)
    {
        if (error)
            *error = e.what();
        return nullptr;
    }
    cache[set.id()] = {text, s};
    return s;
}

std::string groundPathOf(const Node& set) { return textAttr(set, kGroundAttr); }

Vec2 planePoint(const json& v, const std::string& what, const std::string& cmd)
{
    if (!v.is_array() || v.size() < 2 || v.size() > 3)
        throw CommandError(cmd + ": " + what + " takes [x, z] or [x, y, z] points");
    return v.size() == 3 ? planeOf({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}) : Vec2(v[0].get<double>(), v[1].get<double>());
}

void registerZoneCommands(ext::Context& ctx)
{
    {
        const scene::NodeType* transform = scene::types::find("transform");
        scene::NodeType t{.name = kSetType,
                          .base = transform,
                          .dag = true,
                          .doc = "A zone set: the gameplay layer of an open-world map. Typed layers (districts, jurisdictions, territory, spawn "
                                 "sets, densities, wanted response, interiors, missions, user types) of zones over the world XZ plane, "
                                 "drawn over the ground. Edit with the zone commands; the node's own transform does not move the zones.",
                          .icon = "map-2",
                          .create = transform->create};
        t.ownAttrs = {
            {.name = kSpecAttr, .type = scene::PlugType::String, .defaultValue = std::string(), .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden,
             .doc = "The zones as JSON (lila://schema/zones); zoneSpec reads and writes it."},
            {.name = kGroundAttr, .type = scene::PlugType::String, .defaultValue = std::string(), .flags = scene::AttrInput | scene::AttrStorable,
             .doc = "Terrain the zones drape over and whose extent is the default play area; empty lays them at height 0."},
            {.name = kOverlayAttr, .type = scene::PlugType::Bool, .defaultValue = true, .flags = scene::AttrInput | scene::AttrStorable,
             .doc = "Draw the zones over the viewport."},
            {.name = kOpacityAttr, .type = scene::PlugType::Float, .defaultValue = kDefaultFillOpacity, .flags = scene::AttrInput | scene::AttrStorable,
             .doc = "Opacity of the shown layer's fills, drawn while the set is selected, the Zones panel is open or the Zones tool is on.", .minValue = 0.0, .maxValue = 1.0},
        };
        scene::types::registerType(ctx, std::move(t));
    }

    // Zones lie on the world plane, not in the set's space, and their masks are world grids: relocating a scene can't carry them yet.
    scene::relocate::registerHandler(kSetType, {.plan = [](const scene::relocate::Move& move, scene::NodeId node) -> scene::relocate::Edit {
        const Node* n = move.graph->node(node);
        std::shared_ptr<const ZoneSpec> spec = n ? specOf(*n) : nullptr;
        if (!spec || !spec->playArea.empty() || std::any_of(spec->layers.begin(), spec->layers.end(), [](const Layer& l) { return !l.zones.empty(); }))
            throw CommandError(std::string(n ? n->path() : "a zone set") + ": a zone set with zones can't be relocated, its zones are laid in world space");
        return {};
    }});

    ctx.registerCommand({
        .name = "zoneTypes",
        .doc = "The zone layer types: built in (district, police, military, territory, pedSpawn, vehicleSpawn, boatSpawn, aircraftSpawn, "
               "traffic, pedestrians, wanted, interiors, missions) and the set's own. define= adds or replaces a type of the set "
               "({id, label, density, exclusive, mustCover, defaults, nameKind, color, doc}); remove= drops one no layer uses. "
               "Returns every type with its defaults.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"define", "df", T::Any, nullptr, "A type to add to the set: {id, label, density, exclusive, mustCover, defaults, nameKind, color, doc}."},
                  {"remove", "rm", T::String, nullptr, "Id of a set type to remove."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneTypes";
            json out = json::object();
            ZoneSpec spec;
            if (a.has("define") || a.has("remove"))
            {
                SetRef r = findSet(setName(a), cmd, true, &a);
                out = setHeader(r);
                if (a.has("define"))
                {
                    LayerType t;
                    try
                    {
                        t = typeFromJson(a.flags["define"]);
                    }
                    catch (const std::exception& e)
                    {
                        throw CommandError(std::string(cmd) + ": " + e.what());
                    }
                    if (std::ranges::find(typeIds(), t.id) != typeIds().end())
                        throw CommandError(std::string(cmd) + ": '" + t.id + "' is a built-in type");
                    std::erase_if(r.spec.types, [&](const LayerType& x) { return x.id == t.id; });
                    r.spec.types.push_back(t);
                }
                if (a.has("remove"))
                {
                    const std::string id = a.get<std::string>("remove");
                    for (const Layer& l : r.spec.layers)
                        if (l.type == id)
                            throw CommandError(std::string(cmd) + ": layer '" + l.id + "' uses type '" + id + "'");
                    if (!std::erase_if(r.spec.types, [&](const LayerType& x) { return x.id == id; }))
                        throw CommandError(std::string(cmd) + ": the set has no type '" + id + "'");
                }
                store(r, r.spec);
                spec = r.spec;
            }
            else if (!zoneSets().empty())
            {
                try
                {
                    spec = findSet(setName(a), cmd, false).spec;
                }
                catch (const CommandError&)
                {
                }
            }
            json types = json::array();
            for (const LayerType& t : builtInTypes())
                types.push_back(toJson(t));
            for (const LayerType& t : spec.types)
                types.push_back(toJson(t));
            out["types"] = types;
            return out;
        },
        .examples = {"[t['id'] for t in zoneTypes()['types']]",
                     "zoneTypes(define={'id': 'gangTurf', 'label': 'Gang Turf', 'defaults': {'gang': '', 'respect': 0}})"},
    });

    ctx.registerCommand({
        .name = "zoneLayer",
        .doc = "Add or edit a zone layer: id, type, label, priority (higher draws on top and wins Unreal volume priority), exclusive "
               "(zones may not overlap), mustCover (zones must cover the play area), visible, density, color, defaults (attributes "
               "every zone starts with) and the Unreal data layer. Makes the zone set when there is none. remove=True deletes the "
               "layer and its zones. Returns the layer.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"layer", "l", T::String, nullptr, "Layer id; default the type's id."},
                  {"type", "t", T::String, nullptr, "Layer type for a new layer (zoneTypes).", typeIds()},
                  {"label", "lb", T::String, nullptr, "Display label."},
                  {"priority", "p", T::Int, nullptr, "Layer priority."},
                  {"exclusive", "ex", T::Bool, nullptr, "Zones may not overlap."},
                  {"mustCover", "mc", T::Bool, nullptr, "Zones must cover the play area."},
                  {"visible", "v", T::Bool, nullptr, "Draw the layer in the viewport."},
                  {"density", "dn", T::Bool, nullptr, "Painted weight scales the values instead of counting from half up."},
                  {"color", "c", T::List, nullptr, "Layer colour [r, g, b] 0 to 1 (density layers draw in it)."},
                  {"defaults", "df", T::Any, nullptr, "Attributes every zone of the layer starts with (replaces the layer's)."},
                  {"dataLayer", "dl", T::String, nullptr, "Unreal data layer name; empty for the label."},
                  {"remove", "rm", T::Bool, false, "Delete the layer and its zones."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over (default the only terrain)."}, "mesh")},
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneLayer";
            SetRef r = findSet(setName(a), cmd, !a.get<bool>("remove", false), &a);
            json out = setHeader(r);
            if (a.get<bool>("remove", false))
            {
                const std::string id = a.get<std::string>("layer", a.get<std::string>("type", ""));
                if (!std::erase_if(r.spec.layers, [&](const Layer& l) { return l.id == id; }))
                    throw CommandError(std::string(cmd) + ": no layer '" + id + "'");
                store(r, r.spec);
                out["removed"] = id;
                return out;
            }
            bool created = false;
            Layer& l = layerFor(r.spec, a, cmd, &created);
            if (a.has("label"))
                l.label = a.get<std::string>("label");
            if (a.has("priority"))
                l.priority = a.get<int>("priority");
            if (a.has("exclusive"))
                l.exclusive = a.get<bool>("exclusive");
            if (a.has("mustCover"))
                l.mustCover = a.get<bool>("mustCover");
            if (a.has("visible"))
                l.visible = a.get<bool>("visible");
            if (a.has("density"))
                l.density = a.get<bool>("density");
            if (a.has("color"))
            {
                const json c = a.flags["color"];
                if (!c.is_array() || c.size() < 3)
                    throw CommandError(std::string(cmd) + ": color is [r, g, b]");
                l.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
            }
            if (a.has("defaults"))
            {
                if (!a.flags["defaults"].is_object())
                    throw CommandError(std::string(cmd) + ": defaults is a dict");
                l.defaults = a.flags["defaults"];
            }
            if (a.has("dataLayer"))
                l.dataLayer = a.get<std::string>("dataLayer");
            const json lj = layerJson(l, false);
            store(r, r.spec);
            out["layer"] = lj;
            out["layerCreated"] = created;
            return out;
        },
        .examples = {"zoneLayer(type='police', mustCover=True)", "zoneLayer(layer='cartels', type='territory', label='Cartels', priority=5)"},
    });

    ctx.registerCommand({
        .name = "zoneCreate",
        .doc = "Add a zone, or update one by id: its region from polygon=, polygons= (with holes=), closed curves, or a circle, its "
               "name (generated from the set's naming culture when not given), priority within the layer and typed attributes "
               "(checked against the layer's defaults; null removes one). The layer is made from type= when missing, the zone set "
               "when there is none. Returns the zone.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"layer", "l", T::String, nullptr, "Layer id; made from type= when missing."},
                  {"type", "t", T::String, nullptr, "Layer type when the layer is new (zoneTypes).", typeIds()},
                  {"id", "i", T::String, nullptr, "Zone id; the same id updates the zone. Default <layer>_<n>."},
                  {"name", "n", T::String, nullptr, "Zone name; default a fictional one from the naming culture."},
                  {"polygon", "pg", T::List, nullptr, "Outline [[x, z], ...] or [[x, y, z], ...], cm, world space."},
                  {"polygons", "pgs", T::List, nullptr, "Several outlines."},
                  {"holes", "hl", T::List, nullptr, "Holes cut from the outlines."},
                  nodeFlag({"curve", "cv", T::Any, nullptr, "Closed curve(s) to outline it: a name or a list."}, "curve"),
                  {"circle", "ci", T::List, nullptr, "A circle [x, z, radius] (mission markers, small spawns)."},
                  {"priority", "p", T::Int, nullptr, "Priority within the layer; higher wins."},
                  {"attrs", "at", T::Any, nullptr, "Attributes, merged; null removes one."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over (default the only terrain)."}, "mesh")},
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneCreate";
            SetRef r = findSet(setName(a), cmd, true, &a);
            json out = setHeader(r);
            out["notes"] = json::array();
            bool layerMade = false, made = false;
            Layer& l = layerFor(r.spec, a, cmd, &layerMade);
            Zone& z = zoneFor(l, a, cmd, true, &made);
            const bool shaped = takeGeometry(a, z, cmd);
            if (made && !shaped)
                throw CommandError(std::string(cmd) + ": give the zone a region: polygon=, polygons=, curve= or circle= (or paint it with zonePaint)");
            if (made && shaped)
                z.source = {{"kind", "drawn"}};
            if (a.has("priority"))
                z.priority = a.get<int>("priority");
            if (a.has("attrs"))
                mergeAttrs(l, z, a.flags["attrs"], cmd);
            nameZone(r, r.spec, l, z, a, out);
            out["zone"] = zoneJson(l, z);
            out["zoneCreated"] = made;
            out["layerCreated"] = layerMade;
            store(r, r.spec);
            return out;
        },
        .examples = {"zoneCreate(type='district', polygon=[[0, 0], [50000, 0], [50000, 50000], [0, 50000]])",
                     "zoneCreate(layer='missions', type='missions', circle=[12000, -4000, 1500], attrs={'mission': 'm01_intro'})",
                     "zoneCreate(layer='police', id='police_1', attrs={'agency': 'state', 'responseTime': 45})"},
    });

    ctx.registerCommand({
        .name = "zonePaint",
        .doc = "Paint a zone's mask along points: dabs of radius cm a quarter radius apart, each moving the mask toward value by "
               "strength, softened toward the edge. erase=True lowers it to nothing. Region layers count a cell from half up; "
               "density layers scale the zone's density by the painted value. The zone (and layer, and set) is made when missing. "
               "One call is one undo step. Returns the zone and how many cells changed.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"layer", "l", T::String, nullptr, "Layer id; made from type= when missing."},
                  {"type", "t", T::String, nullptr, "Layer type when the layer is new.", typeIds()},
                  {"zone", "z", T::String, nullptr, "Zone id; made when missing (default <layer>_<n>)."},
                  {"name", "n", T::String, nullptr, "Name of a new zone; default a fictional one."},
                  {"points", "pt", T::List, nullptr, "Stroke points [[x, z], ...] or [[x, y, z], ...], world cm."},
                  {"radius", "r", T::Float, 2000.0, "Brush radius, cm.", {}, {}, 1.0},
                  {"value", "v", T::Float, 1.0, "Value painted toward, 0 to 1 (density layers: the weight).", {}, {}, 0.0, 1.0},
                  {"strength", "s", T::Float, 1.0, "How far each dab goes toward the value.", {}, {}, 0.0, 1.0},
                  {"softness", "sf", T::Float, 0.3, "0 a hard edge, 1 fading from the centre.", {}, {}, 0.0, 1.0},
                  {"erase", "e", T::Bool, false, "Lower the mask instead."},
                  {"attrs", "at", T::Any, nullptr, "Attributes of a new zone."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over."}, "mesh")},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zonePaint";
            if (!a.has("points"))
                throw CommandError(std::string(cmd) + ": give points=");
            SetRef r = findSet(setName(a), cmd, true, &a);
            json out = setHeader(r);
            out["notes"] = json::array();
            bool layerMade = false, made = false;
            Layer& l = layerFor(r.spec, a, cmd, &layerMade);
            Zone& z = zoneFor(l, a, cmd, !a.get<bool>("erase", false), &made);
            z.mask.cell = r.spec.maskCell;
            if (made)
            {
                z.source = {{"kind", "painted"}};
                if (a.has("attrs"))
                    mergeAttrs(l, z, a.flags["attrs"], cmd);
            }
            Dab d;
            d.radius = a.get<double>("radius", 2000.0);
            d.value = a.get<double>("value", 1.0);
            d.strength = a.get<double>("strength", 1.0);
            d.softness = a.get<double>("softness", 0.3);
            d.erase = a.get<bool>("erase", false);
            const size_t changed = paintPath(z.mask, pathPoints(a.flags["points"], cmd), d);
            nameZone(r, r.spec, l, z, a, out);
            out["zone"] = zoneJson(l, z);
            out["changed"] = changed;
            out["zoneCreated"] = made;
            out["layerCreated"] = layerMade;
            out["cells"] = z.mask.setCells(l.density ? 1 : 128);
            store(r, r.spec);
            return out;
        },
        .examples = {"zonePaint(layer='traffic', type='traffic', zone='downtown', points=[[0, 0], [20000, 5000]], radius=4000, value=0.8)",
                     "zonePaint(layer='territory', zone='territory_1', points=[[1000, 0]], radius=3000, erase=True)"},
    });

    ctx.registerCommand({
        .name = "zoneEdit",
        .doc = "Change a zone: name, priority, attributes (merged, null removes; replaceAttrs=True starts from none), or move it to "
               "another layer with layerTo=. Returns the zone.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"layer", "l", T::String, nullptr, "The zone's layer."},
                  {"zone", "z", T::String, nullptr, "The zone's id."},
                  {"name", "n", T::String, nullptr, "New name; empty generates one."},
                  {"priority", "p", T::Int, nullptr, "Priority within the layer."},
                  {"attrs", "at", T::Any, nullptr, "Attributes to merge; null removes one."},
                  {"replaceAttrs", "ra", T::Bool, false, "Replace the attributes instead of merging."},
                  {"layerTo", "lt", T::String, nullptr, "Move the zone to this layer."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneEdit";
            SetRef r = findSet(setName(a), cmd, false);
            json out = setHeader(r);
            out["notes"] = json::array();
            Layer* l = r.spec.layer(a.get<std::string>("layer", ""));
            if (!l)
                throw CommandError(std::string(cmd) + ": no layer '" + a.get<std::string>("layer", "") + "'");
            bool made = false;
            Zone* z = &zoneFor(*l, a, cmd, false, &made);
            if (a.has("name"))
            {
                z->name = a.get<std::string>("name");
                z->nameSource = z->name.empty() ? "" : "given";
                if (z->name.empty())
                {
                    std::string note;
                    z->name = newName(r, r.spec, *l, z->id, &z->nameSource, &note);
                    if (!note.empty())
                        out["notes"].push_back(note);
                }
            }
            if (a.has("priority"))
                z->priority = a.get<int>("priority");
            if (a.get<bool>("replaceAttrs", false))
                z->attrs = json::object();
            if (a.has("attrs"))
                mergeAttrs(*l, *z, a.flags["attrs"], cmd);
            if (a.has("layerTo"))
            {
                Layer* to = r.spec.layer(a.get<std::string>("layerTo"));
                if (!to)
                    throw CommandError(std::string(cmd) + ": no layer '" + a.get<std::string>("layerTo") + "'");
                if (to != l)
                {
                    Zone moved = *z;
                    if (to->zone(moved.id))
                        moved.id = nextZoneId(*to);
                    std::erase_if(l->zones, [&](const Zone& x) { return x.id == z->id; });
                    to->zones.push_back(std::move(moved));
                    l = to;
                    z = &to->zones.back();
                }
            }
            out["zone"] = zoneJson(*l, *z);
            store(r, r.spec);
            return out;
        },
        .examples = {"zoneEdit(layer='district', zone='district_1', name='Colonia Vista', attrs={'era': 'colonial'})"},
    });

    ctx.registerCommand({
        .name = "zoneDelete",
        .doc = "Delete a zone (layer= and zone=), or a whole layer with its zones (layer= alone). Returns what went.",
        .positionalDoc = "zone set",
        .flags = {setFlag(), {"layer", "l", T::String, nullptr, "Layer id."}, {"zone", "z", T::String, nullptr, "Zone id; omit to delete the layer."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneDelete";
            SetRef r = findSet(setName(a), cmd, false);
            json out = setHeader(r);
            const std::string lid = a.get<std::string>("layer", "");
            Layer* l = r.spec.layer(lid);
            if (!l)
                throw CommandError(std::string(cmd) + ": no layer '" + lid + "'");
            if (a.has("zone"))
            {
                const std::string zid = a.get<std::string>("zone");
                if (!std::erase_if(l->zones, [&](const Zone& z) { return z.id == zid; }))
                    throw CommandError(std::string(cmd) + ": layer '" + lid + "' has no zone '" + zid + "'");
                out["deleted"] = {{"layer", lid}, {"zone", zid}};
            }
            else
            {
                out["deleted"] = {{"layer", lid}, {"zones", l->zones.size()}};
                std::erase_if(r.spec.layers, [&](const Layer& x) { return x.id == lid; });
            }
            store(r, r.spec);
            return out;
        },
        .examples = {"zoneDelete(layer='missions', zone='missions_2')", "zoneDelete(layer='traffic')"},
    });

    ctx.registerCommand({
        .name = "zoneQuery",
        .doc = "Every layer's value at a point: the winning zone (highest priority, then the later one), its name, attributes with the "
               "layer's defaults filled in, its painted weight, the density for density layers, and every zone holding the point. "
               "Changes nothing.",
        .positionalDoc = "zone set",
        .flags = {setFlag(), {"point", "pt", T::List, nullptr, "World point [x, z] or [x, y, z], cm."}, {"layer", "l", T::String, nullptr, "Only this layer."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneQuery";
            if (!a.has("point"))
                throw CommandError(std::string(cmd) + ": give point=");
            const SetRef r = findSet(setName(a), cmd, false);
            const Vec2 p = planePoint(a.flags["point"], "point", cmd);
            json out = {{"set", r.path}, {"point", {p.x, p.y}}};
            if (a.has("layer"))
            {
                const Layer* l = r.spec.layer(a.get<std::string>("layer"));
                if (!l)
                    throw CommandError(std::string(cmd) + ": no layer '" + a.get<std::string>("layer") + "'");
                out["layers"] = {{l->id, queryLayer(*l, p)}};
            }
            else
                out["layers"] = query(r.spec, p);
            return out;
        },
        .examples = {"zoneQuery(point=[12000, 0, -3500])['layers']['police']['attrs']['agency']"},
    });

    ctx.registerCommand({
        .name = "zoneList",
        .doc = "The zone set: its ground, play area, naming culture, mask cell, and every layer with its counts, area and (unless "
               "zones=False) its zones. Changes nothing.",
        .positionalDoc = "zone set",
        .flags = {setFlag(), {"layer", "l", T::String, nullptr, "Only this layer."}, {"zones", "zn", T::Bool, true, "List each layer's zones."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const SetRef r = findSet(setName(a), "zoneList", false);
            Graph& g = Graph::current();
            json layers = json::array();
            for (const Layer& l : r.spec.layers)
                if (!a.has("layer") || l.id == a.get<std::string>("layer"))
                    layers.push_back(layerJson(l, a.get<bool>("zones", true)));
            std::string from;
            playAreaOf(r, &from);
            json play = json::array();
            for (const Polygon& ring : r.spec.playArea)
            {
                json pts = json::array();
                for (Vec2 p : ring)
                    pts.push_back({p.x, p.y});
                play.push_back(pts);
            }
            const std::string ground = groundPathOf(*g.node(r.id));
            return {{"set", r.path}, {"ground", ground}, {"groundValid", groundOf(ground).valid()}, {"culture", r.spec.culture},
                    {"maskCell", r.spec.maskCell}, {"playArea", play}, {"playAreaFrom", from}, {"layers", layers}};
        },
        .examples = {"[(l['id'], l['count']) for l in zoneList()['layers']]"},
    });

    ctx.registerCommand({
        .name = "zoneSpec",
        .doc = "Read a zone set's spec (the world spec's zones section, lila://schema/zones), or replace it with spec=. culture= and "
               "maskCell= change those alone. Returns the spec.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"spec", "sp", T::Any, nullptr, "A whole spec to replace the set's (a dict or JSON text)."},
                  {"culture", "cu", T::String, nullptr, "Naming culture of generated names."},
                  {"maskCell", "mc", T::Float, nullptr, "Mask cell size, cm; only while nothing is painted.", {}, {}, 1.0},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over."}, "mesh")},
        .run = [](const Args& a) -> json {
            const char* cmd = "zoneSpec";
            const bool writes = a.has("spec") || a.has("culture") || a.has("maskCell");
            SetRef r = findSet(setName(a), cmd, writes, &a);
            if (a.has("spec"))
            {
                json j = a.flags["spec"];
                try
                {
                    if (j.is_string())
                        j = json::parse(j.get<std::string>());
                    r.spec = zonesFromJson(j);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string(cmd) + ": " + e.what());
                }
            }
            if (a.has("culture"))
                r.spec.culture = a.get<std::string>("culture");
            if (a.has("maskCell"))
            {
                for (const Layer& l : r.spec.layers)
                    for (const Zone& z : l.zones)
                        if (z.hasMask())
                            throw CommandError(std::string(cmd) + ": zone '" + z.id + "' is painted; maskCell changes only while nothing is");
                r.spec.maskCell = a.get<double>("maskCell");
            }
            if (writes)
                store(r, r.spec);
            return toJson(r.spec);
        },
        .examples = {"spec = zoneSpec()\nspec['layers'][0]['priority'] = 3\nzoneSpec(spec=spec)"},
    });

    ctx.registerCommand({
        .name = "zonePlayArea",
        .doc = "Set the play area that must-cover layers are checked against: polygon=, closed curve(s), or clear=True for the "
               "ground's extent (else the zones' bounds). Returns the play area.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"polygon", "pg", T::List, nullptr, "Outline [[x, z], ...]."},
                  nodeFlag({"curve", "cv", T::Any, nullptr, "Closed curve(s)."}, "curve"),
                  {"clear", "cl", T::Bool, false, "Back to the ground's extent."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "zonePlayArea";
            SetRef r = findSet(setName(a), cmd, false);
            if (a.get<bool>("clear", false))
                r.spec.playArea.clear();
            else if (a.has("polygon"))
                r.spec.playArea = {ringOf(a.flags["polygon"], "polygon", cmd)};
            else if (a.has("curve"))
                r.spec.playArea = curveRings(a.flags["curve"], cmd);
            else
                throw CommandError(std::string(cmd) + ": give polygon=, curve= or clear=True");
            store(r, r.spec);
            std::string from;
            playAreaOf(r, &from);
            return {{"set", r.path}, {"rings", r.spec.playArea.size()}, {"playAreaFrom", from}};
        },
        .examples = {"zonePlayArea(curve='IslandOutline')"},
    });

    ctx.registerCommand({
        .name = "zoneFromDistricts",
        .doc = "Zones from a Settlement's districts (the quarters between main roads, with their culture names): one zone per "
               "district, id d<district id>, named as the settlement names it, into layer= (default districts, type district). "
               "Zones from an earlier run of the same settlement are replaced; others stay. Returns the zones.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  nodeFlag({"settlement", "st", T::String, nullptr, "The settlement mesh (default the selected one)."}, "mesh"),
                  {"layer", "l", T::String, "districts", "Layer id."},
                  {"type", "t", T::String, "district", "Layer type when the layer is new.", typeIds()},
                  {"attrs", "at", T::Any, nullptr, "Attributes every new zone gets."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over."}, "mesh")},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneFromDistricts";
            if (!ops::find("settlementNames"))
                throw CommandError(std::string(cmd) + ": the Settlement extension is not loaded");
            Graph& g = Graph::current();
            std::string settlement = a.get<std::string>("settlement", "");
            if (settlement.empty())
                for (scene::NodeId id : g.selection())
                    if (Node* n = g.node(id); n && !isSet(*n))
                    {
                        settlement = n->path();
                        break;
                    }
            if (settlement.empty())
                throw CommandError(std::string(cmd) + ": give settlement= (or select one)");
            json names;
            try
            {
                names = ops::execute("settlementNames", {{"args", {settlement}}});
            }
            catch (const CommandError& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }
            glm::dmat4 world(1.0);
            if (Node* n = settlement.empty() ? nullptr : g.find(settlement))
            {
                if (gn::isGnModifier(*n))
                    if (scene::NodeId m = scene::modifiers::meshOf(g, n->id()); m != scene::kNoNode)
                        n = g.node(m);
                if (n && n->type().isA("transform"))
                {
                    world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                    settlement = n->path();
                }
            }
            if (!names.contains("districts") || names["districts"].empty())
                throw CommandError(std::string(cmd) + ": the settlement has no districts");

            SetRef r = findSet(setName(a), cmd, true, &a);
            json out = setHeader(r);
            out["notes"] = json::array();
            bool layerMade = false;
            Layer& l = layerFor(r.spec, a, cmd, &layerMade, "district");
            std::erase_if(l.zones, [&](const Zone& z) {
                return z.source.is_object() && z.source.value("kind", std::string()) == "district" && z.source.value("settlement", std::string()) == settlement;
            });
            json made = json::array();
            for (const json& d : names["districts"])
            {
                if (!d.contains("outline") || d["outline"].size() < 3)
                {
                    out["notes"].push_back("district " + d["id"].dump() + " has no outline (an older Settlement); skipped");
                    continue;
                }
                Zone z;
                z.id = "d" + std::to_string(d["id"].get<uint64_t>());
                if (l.zone(z.id))
                    z.id = settlement.empty() ? z.id : sanitizeId(leafName(settlement)) + "_" + z.id;
                Polygon ring;
                for (const json& p : d["outline"])
                {
                    const glm::dvec3 w = glm::dvec3(world * glm::dvec4(p[0].get<double>(), 0.0, p[1].get<double>(), 1.0));
                    ring.push_back(planeOf(w));
                }
                z.polygons = {ring};
                z.name = d.value("name", std::string());
                z.nameSource = "settlement";
                z.source = {{"kind", "district"}, {"settlement", settlement}, {"district", d["id"]}, {"culture", names.value("culture", std::string())}};
                if (a.has("attrs"))
                    mergeAttrs(l, z, a.flags["attrs"], cmd);
                if (z.name.empty())
                    nameZone(r, r.spec, l, z, a, out);
                l.zones.push_back(std::move(z));
                made.push_back(zoneJson(l, l.zones.back()));
            }
            store(r, r.spec);
            out["layer"] = l.id;
            out["layerCreated"] = layerMade;
            out["zones"] = made;
            out["settlement"] = settlement;
            out["culture"] = names.value("culture", std::string());
            return out;
        },
        .examples = {"zoneFromDistricts(settlement='Mazatlan')", "zoneFromDistricts(layer='police', type='police', attrs={'agency': 'municipal'})"},
    });

    ctx.registerCommand({
        .name = "zoneFromTerrain",
        .doc = "Zones from a terrain's masks: one painted zone per mask (biomes=True takes every biome_<name> mask), id the mask's "
               "name, copied onto the zone grid. Region layers keep cells at or over threshold; density layers keep the mask's "
               "values as weights. Zones from an earlier run on the same masks are replaced. Returns the zones.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  nodeFlag({"terrain", "tr", T::String, nullptr, "The terrain (default the set's ground)."}, "mesh"),
                  {"masks", "m", T::List, nullptr, "Mask names (terrainInfo lists them)."},
                  {"biomes", "b", T::Bool, false, "Every biome_ mask (terrainBiomes makes them)."},
                  {"layer", "l", T::String, nullptr, "Layer id; made from type= when missing."},
                  {"type", "t", T::String, nullptr, "Layer type when the layer is new.", typeIds()},
                  {"threshold", "th", T::Float, 0.5, "Region layers: mask value from which a cell is in.", {}, {}, 0.0, 1.0},
                  {"attrs", "at", T::Any, nullptr, "Attributes every new zone gets."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over."}, "mesh")},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneFromTerrain";
            SetRef r = findSet(setName(a), cmd, true, &a);
            Graph& g = Graph::current();
            const std::string terrain = a.get<std::string>("terrain", groundPathOf(*g.node(r.id)));
            const Ground gr = groundOf(terrain);
            if (!gr.valid())
                throw CommandError(std::string(cmd) + ": " + (terrain.empty() ? std::string("give terrain= (the set has no ground)") : terrain + " is not a terrain"));
            std::vector<std::string> masks;
            if (a.has("masks"))
                for (const json& m : a.flags["masks"])
                    masks.push_back(m.get<std::string>());
            if (a.get<bool>("biomes", false))
                for (const auto& [name, v] : gr.h->masks)
                    if (name.starts_with("biome_"))
                        masks.push_back(name);
            if (masks.empty())
                throw CommandError(std::string(cmd) + ": give masks= or biomes=True (the terrain has " + std::to_string(gr.h->masks.size()) + " masks)");
            json out = setHeader(r);
            out["notes"] = json::array();
            bool layerMade = false;
            Layer& l = layerFor(r.spec, a, cmd, &layerMade);
            const double th = a.get<double>("threshold", 0.5);
            Vec2 lo, hi;
            gr.extent(lo, hi);
            Mask frame;
            frame.cell = r.spec.maskCell;
            const glm::ivec2 c0 = frame.cellOf(lo), c1 = frame.cellOf(hi);
            json made = json::array();
            for (const std::string& name : masks)
            {
                if (!gr.h->findMask(name))
                    throw CommandError(std::string(cmd) + ": " + gr.mesh + " has no mask '" + name + "'");
                std::erase_if(l.zones, [&](const Zone& z) {
                    return z.source.is_object() && z.source.value("kind", std::string()) == "terrain" && z.source.value("mask", std::string()) == name &&
                           z.source.value("terrain", std::string()) == gr.mesh;
                });
                Zone z;
                z.id = sanitizeId(name.starts_with("biome_") ? name.substr(6) : name);
                if (l.zone(z.id))
                    z.id = nextZoneId(l);
                z.mask.cell = r.spec.maskCell;
                for (int cz = c0.y; cz <= c1.y; ++cz)
                    for (int cx = c0.x; cx <= c1.x; ++cx)
                    {
                        const Vec2 p = (Vec2(cx, cz) + 0.5) * z.mask.cell;
                        const glm::dvec3 lp = glm::dvec3(gr.inv * glm::dvec4(worldOf(p, 0.0), 1.0));
                        if (!gr.h->contains(lp.x, lp.z))
                            continue;
                        const double v = gr.h->maskAt(name, lp.x, lp.z);
                        const uint8_t c = l.density ? uint8_t(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5) : (v >= th ? 255 : 0);
                        if (c)
                            z.mask.setCell(cx, cz, c);
                    }
                if (z.mask.empty())
                {
                    out["notes"].push_back("mask '" + name + "' is empty at this threshold; no zone made");
                    continue;
                }
                z.source = {{"kind", "terrain"}, {"terrain", gr.mesh}, {"mask", name}};
                z.attrs = name.starts_with("biome_") ? json{{"biome", name.substr(6)}} : json{{"mask", name}};
                if (a.has("attrs"))
                    mergeAttrs(l, z, a.flags["attrs"], cmd);
                l.zones.push_back(std::move(z));
                Zone& added = l.zones.back();
                nameZone(r, r.spec, l, added, a, out);
                made.push_back(zoneJson(l, added));
            }
            store(r, r.spec);
            out["layer"] = l.id;
            out["layerCreated"] = layerMade;
            out["zones"] = made;
            out["terrain"] = gr.mesh;
            return out;
        },
        .examples = {"zoneFromTerrain(biomes=True, layer='pedSpawn', type='pedSpawn')", "zoneFromTerrain(masks=['mangrove'], layer='boatSpawn', type='boatSpawn')"},
    });

    ctx.registerCommand({
        .name = "zoneFromWater",
        .doc = "Zones along a terrain's water routes (terrainWaterRoutes): one painted zone per lane, as wide as the lane's clear "
               "water (or the craft's lane profile) times widthScale, with the lane's craft and kind as attributes. Boat lanes go "
               "into a boatSpawn layer, air lanes (air=True) into aircraftSpawn. kinds= and crafts= filter. Zones from an earlier "
               "run on the same terrain are replaced. Returns the zones.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  nodeFlag({"terrain", "tr", T::String, nullptr, "The terrain (default the set's ground)."}, "mesh"),
                  {"air", "ai", T::Bool, false, "Air lanes instead of boat lanes."},
                  {"kinds", "k", T::List, nullptr, "Only these lane kinds (coast, channel, river, harbour, link, approach, departure)."},
                  {"crafts", "cr", T::List, nullptr, "Only these craft classes."},
                  {"widthScale", "ws", T::Float, 1.0, "Zone width as a multiple of the lane's width.", {}, {}, 0.05, 20.0},
                  {"airWidth", "aw", T::Float, 30000.0, "Air lanes: corridor width, cm.", {}, {}, 100.0},
                  {"layer", "l", T::String, nullptr, "Layer id (default boatSpawn or aircraftSpawn)."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over."}, "mesh")},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneFromWater";
            if (!ops::find("terrainWaterRoutes"))
                throw CommandError(std::string(cmd) + ": the Terrain extension is not loaded");
            SetRef r = findSet(setName(a), cmd, true, &a);
            Graph& g = Graph::current();
            const std::string terrain = a.get<std::string>("terrain", groundPathOf(*g.node(r.id)));
            if (terrain.empty())
                throw CommandError(std::string(cmd) + ": give terrain= (the set has no ground)");
            json routes;
            try
            {
                routes = ops::execute("terrainWaterRoutes", {{"args", {terrain}}});
            }
            catch (const CommandError& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }
            const bool air = a.get<bool>("air", false);
            const std::string type = air ? "aircraftSpawn" : "boatSpawn";
            Args la = a;
            if (!la.has("layer"))
                la.flags["layer"] = type;
            la.flags["type"] = type;
            json out = setHeader(r);
            out["notes"] = json::array();
            bool layerMade = false;
            Layer& l = layerFor(r.spec, la, cmd, &layerMade);
            std::erase_if(l.zones, [&](const Zone& z) {
                return z.source.is_object() && z.source.value("kind", std::string()) == "water" && z.source.value("terrain", std::string()) == terrain;
            });
            std::map<std::string, double> profile;
            for (const json& p : routes.value("zoneGraph", json::object()).value("laneProfiles", json::array()))
                if (p.contains("lanes") && !p["lanes"].empty())
                    profile[p.value("name", std::string())] = p["lanes"][0].value("width", 0.0);
            auto wanted = [&](const char* flag, const std::string& v) {
                if (!a.has(flag))
                    return true;
                for (const json& x : a.flags[flag])
                    if (x.get<std::string>() == v)
                        return true;
                return false;
            };
            const double scale = a.get<double>("widthScale", 1.0);
            json made = json::array();
            int skipped = 0;
            for (const json& lane : routes.value("lanes", json::array()))
            {
                if (lane.value("air", false) != air || !wanted("kinds", lane.value("kind", std::string())) || !wanted("crafts", lane.value("craft", std::string())))
                {
                    ++skipped;
                    continue;
                }
                std::vector<Vec2> pts;
                std::vector<double> radii;
                const double fallbackWidth = air ? a.get<double>("airWidth", 30000.0) : profile[lane.value("craft", std::string())];
                for (const json& p : lane.value("points", json::array()))
                {
                    pts.push_back(planePoint(p["position"], "lane point", cmd));
                    const double w = p.value("width", 0.0) > 0.0 ? p.value("width", 0.0) : fallbackWidth;
                    radii.push_back(std::max(w * 0.5 * scale, r.spec.maskCell));
                }
                if (pts.size() < 2)
                    continue;
                Zone z;
                z.id = sanitizeId("lane_" + lane.value("id", std::to_string(made.size() + 1)));
                if (l.zone(z.id))
                    z.id = nextZoneId(l);
                z.mask.cell = r.spec.maskCell;
                Dab d;
                d.softness = 0.0;
                paintPath(z.mask, pts, d, radii);
                const std::string craft = lane.value("craft", std::string()), kind = lane.value("kind", std::string());
                z.attrs = {{"craft", craft}, {"kind", kind}, {"lane", lane.value("id", std::string())}};
                z.name = capitalised(spacedWords(craft)) + " " + kind + " lane " + std::to_string(made.size() + 1);
                z.nameSource = "lane";
                z.source = {{"kind", "water"}, {"terrain", terrain}, {"lane", lane.value("id", std::string())}};
                l.zones.push_back(std::move(z));
                made.push_back(zoneJson(l, l.zones.back()));
            }
            if (made.empty())
                out["notes"].push_back(std::string("no ") + (air ? "air" : "boat") + " lanes matched (" + std::to_string(skipped) + " skipped by air/kinds/crafts)");
            store(r, r.spec);
            out["layer"] = l.id;
            out["layerCreated"] = layerMade;
            out["zones"] = made;
            return out;
        },
        .examples = {"zoneFromWater(terrain='Coast', crafts=['skiff', 'goFast'])", "zoneFromWater(terrain='Coast', air=True)"},
    });

    ctx.registerCommand({
        .name = "zoneCheck",
        .doc = "Check the zones: gaps where a must-cover layer (districts, police by default) leaves the play area uncovered, overlaps "
               "where an exclusive layer's zones share ground, zones with no region, zones with no name. Sampled on a grid of cell "
               "cm (default about 512 across); regions smaller than minCells samples are ignored. report=True lists the issues in "
               "the Validation panel on the zone set. Returns ok, the issues (kind, layer, zones, where, area) and counts.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"cell", "c", T::Float, 0.0, "Sample spacing, cm; 0 picks one.", {}, {}, 0.0},
                  {"minCells", "mn", T::Int, 2, "Smallest gap or overlap reported, in samples.", {}, {}, 1.0},
                  {"report", "rp", T::Bool, false, "Also list the issues in the Validation panel."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneCheck";
            const SetRef r = findSet(setName(a), cmd, false);
            json out = checkJson(r, a.get<double>("cell", 0.0), a.get<int>("minCells", 2));
            if (a.get<bool>("report", false))
            {
                if (!ops::find("reportIssue") || !ops::find("clearIssues"))
                    throw CommandError(std::string(cmd) + ": the Validation commands are not available");
                ops::execute("clearIssues", {{"args", {r.path}}, {"source", "zoneCheck"}});
                int n = 0;
                for (const json& i : out["issues"])
                    if (n++ < 200)
                        ops::execute("reportIssue", {{"args", {r.path}}, {"message", i["what"]}, {"severity", i["kind"] == "unnamed" ? "info" : "warning"}, {"source", "zoneCheck"}});
            }
            return out;
        },
        .examples = {"c = zoneCheck()\nc['ok'], c['counts']"},
    });

    ctx.registerCommand({
        .name = "zoneExport",
        .doc = "Write the zones for any engine: <name>_zones.json (format lila.zones: layers, zones with polygons, holes, attributes "
               "with defaults filled in, mask outlines and mask image references) and one 8-bit PNG per painted zone "
               "(<name>_<layer>_<zone>.png, row 0 at the lowest Z, value 255 full). zoneImport reads it back. Default folder: the "
               "workspace's openworld/zones/<name>. Returns the files.",
        .positionalDoc = "zone set",
        .flags = {setFlag(), {"folder", "f", T::String, nullptr, "Output folder."}, {"name", "n", T::String, nullptr, "File name stem; default the set's name."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneExport";
            const SetRef r = findSet(setName(a), cmd, false);
            const std::string stem = a.get<std::string>("name", leafName(r.path));
            json out = {{"set", r.path}, {"notes", json::array()}};
            const fs::path dir = a.has("folder") ? paths::fromUtf8(a.get<std::string>("folder")) : outputFolder(stem, out);
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::vector<ExportedMask> masks;
            const json doc = exportJson(r.spec, stem + "_", &masks);
            json files = json::array();
            for (const ExportedMask& m : masks)
            {
                std::vector<float> rgba(size_t(m.size.x) * size_t(m.size.y) * 4);
                for (size_t i = 0; i < m.values.size(); ++i)
                {
                    const float v = m.values[i] / 255.0f;
                    rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v;
                    rgba[i * 4 + 3] = 1.0f;
                }
                const fs::path file = dir / (stem + "_" + m.layer + "_" + m.zone + ".png");
                try
                {
                    image::writePng(file, m.size.x, m.size.y, rgba, 8);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string(cmd) + ": " + e.what());
                }
                files.push_back(paths::toUtf8(file));
            }
            const fs::path jf = dir / (stem + "_zones.json");
            std::ofstream f(jf, std::ios::binary);
            if (!f)
                throw CommandError(std::string(cmd) + ": can't write " + paths::toUtf8(jf));
            f << doc.dump(1);
            out["json"] = paths::toUtf8(jf);
            out["masks"] = files;
            out["folder"] = paths::toUtf8(dir);
            out["layers"] = doc["layers"].size();
            return out;
        },
        .examples = {"zoneExport(folder='C:/game/data/zones')['json']"},
    });

    ctx.registerCommand({
        .name = "zoneImport",
        .doc = "Read zones from a lila.zones JSON (zoneExport) and its PNG masks into a zone set, replacing its spec (merge=True "
               "adds the file's layers, replacing layers of the same id). Makes the set when there is none. Returns the layers.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"path", "p", T::String, nullptr, "The _zones.json file."},
                  {"merge", "mg", T::Bool, false, "Add to the set's layers instead of replacing them."},
                  {"setName", "sn", T::String, "Zones", "Name of a new zone set."},
                  nodeFlag({"ground", "g", T::String, nullptr, "Terrain a new zone set drapes over."}, "mesh")},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneImport";
            const fs::path file = paths::fromUtf8(a.get<std::string>("path", ""));
            std::ifstream f(file, std::ios::binary);
            if (!f)
                throw CommandError(std::string(cmd) + ": can't read " + a.get<std::string>("path", ""));
            ZoneSpec in;
            try
            {
                const json doc = json::parse(f);
                in = importJson(doc, [&](const std::string& name, glm::ivec2 size) {
                    std::string err;
                    const image::Image img = image::readPng(file.parent_path() / paths::fromUtf8(name), &err);
                    if (img.width != size.x || img.height != size.y)
                        throw std::runtime_error("mask image " + name + (err.empty() ? " has the wrong size" : ": " + err));
                    std::vector<uint8_t> v(size_t(size.x) * size_t(size.y));
                    for (size_t i = 0; i < v.size(); ++i)
                        v[i] = uint8_t(std::clamp(img.rgba[i * 4], 0.0f, 1.0f) * 255.0f + 0.5f);
                    return v;
                });
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }
            SetRef r = findSet(setName(a), cmd, true, &a);
            if (a.get<bool>("merge", false))
            {
                if (std::abs(in.maskCell - r.spec.maskCell) > 1e-6)
                    for (const Layer& l : r.spec.layers)
                        for (const Zone& z : l.zones)
                            if (z.hasMask())
                                throw CommandError(std::string(cmd) + ": the file's mask cell differs from the set's painted zones'");
                r.spec.maskCell = in.maskCell;
                for (LayerType& t : in.types)
                {
                    std::erase_if(r.spec.types, [&](const LayerType& x) { return x.id == t.id; });
                    r.spec.types.push_back(t);
                }
                for (Layer& l : in.layers)
                {
                    std::erase_if(r.spec.layers, [&](const Layer& x) { return x.id == l.id; });
                    r.spec.layers.push_back(std::move(l));
                }
            }
            else
                r.spec = std::move(in);
            store(r, r.spec);
            json out = setHeader(r);
            json layers = json::array();
            for (const Layer& l : r.spec.layers)
                layers.push_back(layerJson(l, false));
            out["layers"] = layers;
            return out;
        },
        .examples = {"zoneImport(path='C:/game/data/zones/Zones_zones.json')"},
    });

    ctx.registerCommand({
        .name = "zoneSendToUnreal",
        .doc = "Send the zones to Unreal through LilaLink: a ALilaZoneVolume per zone (brush from the zone's outline, extruded from "
               "below the ground to height above it, with its layer, name, priority and attributes), one ULilaZoneData asset with "
               "every layer, zone and painted mask for point queries at runtime, and a World Partition data layer per layer "
               "(dataLayer name). Zones of an earlier send that are gone are removed. Returns what Unreal made.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"height", "h", T::Float, 50000.0, "Volume top above the highest ground under the zone, cm.", {}, {}, 0.0},
                  {"depth", "d", T::Float, 5000.0, "Volume bottom below the lowest ground under the zone, cm.", {}, {}, 0.0},
                  {"dataLayers", "dl", T::Bool, true, "Put each layer's volumes in a World Partition data layer."},
                  {"asset", "as", T::String, "/Game/Lila/Zones", "Content folder for the zone data asset and data layer assets."},
                  {"timeout", "to", T::Float, 120.0, "Seconds to wait for Unreal.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneSendToUnreal";
            const SetRef r = findSet(setName(a), cmd, false);
            Graph& g = Graph::current();
            Node* n = g.node(r.id);
            const Ground gr = groundOf(groundPathOf(*n));
            json params = {{"id", linkKey(*n)},
                           {"label", leafName(r.path)},
                           {"folder", "Lila/Zones/" + leafName(r.path)},
                           {"asset", a.get<std::string>("asset", "/Game/Lila/Zones")},
                           {"dataLayers", a.get<bool>("dataLayers", true)},
                           {"layers", unrealZones(r.spec, gr, a.get<double>("height", 50000.0), a.get<double>("depth", 5000.0))}};
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            if (!client || !features.is_array() || std::find(features.begin(), features.end(), json("zones")) == features.end())
                throw CommandError(std::string(cmd) + ": the LilaLink plugin in Unreal predates zones; rebuild it from Lila's ue/LilaLink");
            link::RpcResult res = client->call("setZones", params, int(std::clamp(a.get<double>("timeout", 120.0), 1.0, 3600.0) * 1000.0));
            if (!res.ok)
                throw CommandError(std::string(cmd) + ": " + res.error);
            json out = {{"set", r.path}, {"id", params["id"]}, {"label", params["label"]}, {"asset", params["asset"]}, {"unreal", res.result}};
            if (!gr.valid())
                out["notes"] = {"the zone set has no ground; volumes span heights around 0"};
            return out;
        },
        .examples = {"zoneSendToUnreal(height=80000)"},
    });

    ctx.registerCommand({
        .name = "zoneRemoveFromUnreal",
        .doc = "Removes a zone set's send from the open Unreal level: its volumes, its zone data asset, and the data layers "
               "(with their assets) no other actor uses. Give the set, or id= and label= for one no longer in the scene "
               "(zoneSendToUnreal returns both). Returns what Unreal removed.",
        .positionalDoc = "zone set",
        .flags = {setFlag(),
                  {"id", "id", T::String, nullptr, "Unreal id of a zone set no longer in the scene (zoneSendToUnreal's id)."},
                  {"label", "lb", T::String, nullptr, "With id: the set's label (names its ZD_ asset)."},
                  {"asset", "as", T::String, "/Game/Lila/Zones", "Content folder its sends used."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "zoneRemoveFromUnreal";
            std::string id = a.get<std::string>("id", ""), label = a.get<std::string>("label", "");
            if (id.empty())
            {
                const SetRef r = findSet(setName(a), cmd, false);
                id = linkKey(*Graph::current().node(r.id));
                label = leafName(r.path);
            }
            else if (label.empty())
                throw CommandError(std::string(cmd) + ": id= needs label= (the set's name), which names its zone data asset");
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string(cmd) + ": " + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            if (!client)
                throw CommandError(std::string(cmd) + ": not connected");
            link::RpcResult res = client->call("removeZones", {{"id", id}, {"label", label}, {"asset", a.get<std::string>("asset", "/Game/Lila/Zones")}}, 120000);
            if (!res.ok)
                throw CommandError(std::string(cmd) + ": " + res.error);
            return {{"id", id}, {"label", label}, {"unreal", res.result}};
        },
        .examples = {"zoneRemoveFromUnreal('Zones')", "zoneRemoveFromUnreal(id='w1a2b3c4d5e6f708192a3', label='Zones')"},
    });

    mcp::Server& server = mcp::Server::get();
    server.addResource(ctx, {.uri = "lila://schema/zones",
                             .name = "Zones schema",
                             .description = "JSON schema of a zone set's spec (the world spec's zones section): layers of typed zones with polygons, "
                                            "painted masks and attributes. Read before writing one for zoneSpec.",
                             .mimeType = "application/json",
                             .read = [] { return zonesSchema().dump(2); }});
}
} // namespace openworld::zones
