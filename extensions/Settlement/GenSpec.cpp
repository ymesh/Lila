// Settlement generators: the generators JSON read into a GenSpec, era presets, OSM street classes.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace settlement
{
namespace
{
[[noreturn]] void bad(const std::string& where, const std::string& what) { throw std::runtime_error("generators: " + where + ": " + what); }

void onlyKeys(const json& j, const std::string& where, std::initializer_list<const char*> keys)
{
    if (!j.is_object())
        bad(where, "must be a JSON object");
    for (auto& [k, v] : j.items())
        if (std::none_of(keys.begin(), keys.end(), [&](const char* x) { return k == x; }))
        {
            std::string list;
            for (const char* x : keys)
                list += (list.empty() ? "" : ", ") + std::string(x);
            bad(where, "unknown field '" + k + "' (" + list + ")");
        }
}

double number(const json& j, const char* key, double fallback, const std::string& where)
{
    if (!j.contains(key))
        return fallback;
    if (!j[key].is_number())
        bad(where, std::string(key) + " takes a number");
    return j[key].get<double>();
}

bool boolean(const json& j, const char* key, bool fallback, const std::string& where)
{
    if (!j.contains(key))
        return fallback;
    if (!j[key].is_boolean())
        bad(where, std::string(key) + " takes true or false");
    return j[key].get<bool>();
}

std::string text(const json& j, const char* key, const std::string& where)
{
    if (!j.contains(key))
        return {};
    if (!j[key].is_string())
        bad(where, std::string(key) + " takes a string");
    return j[key].get<std::string>();
}

Vec2 point(const json& p, const std::string& where)
{
    if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number())
        bad(where, "points are [x, z] or [x, y, z]");
    return p.size() >= 3 && p[2].is_number() ? Vec2(p[0].get<double>(), p[2].get<double>()) : Vec2(p[0].get<double>(), p[1].get<double>());
}

std::vector<Vec2> points(const json& j, const char* key, const std::string& where, size_t min = 0)
{
    std::vector<Vec2> out;
    if (!j.contains(key))
        return out;
    if (!j[key].is_array())
        bad(where, std::string(key) + " takes a list of points");
    for (const json& p : j[key])
        out.push_back(point(p, where + "." + key));
    if (out.size() < min)
        bad(where, std::string(key) + " needs at least " + std::to_string(min) + " points");
    return out;
}

std::optional<Pattern> pattern(const json& j, const std::string& where)
{
    const std::string name = text(j, "pattern", where);
    if (name.empty())
        return std::nullopt;
    Pattern p;
    if (!parsePattern(name, &p))
        bad(where, "unknown pattern '" + name + "'");
    return p;
}

TensorSpec tensorFrom(const json& j)
{
    const std::string w = "tensor";
    onlyKeys(j, w, {"fields", "paint", "mask", "majorSpacing", "minorSpacing", "step", "minor"});
    TensorSpec t;
    for (const json& f : j.value("fields", json::array()))
    {
        const std::string fw = w + ".fields";
        onlyKeys(f, fw, {"kind", "angle", "center", "weight", "decay", "lines", "follow"});
        TensorBasis b;
        b.kind = text(f, "kind", fw);
        if (b.kind != "grid" && b.kind != "radial" && b.kind != "boundary" && b.kind != "height")
            bad(fw, "kind is grid, radial, boundary or height");
        b.angle = number(f, "angle", 0.0, fw);
        if (f.contains("center"))
        {
            b.center = point(f["center"], fw + ".center");
            b.hasCenter = true;
        }
        b.weight = number(f, "weight", 1.0, fw);
        b.decay = std::max(0.0, number(f, "decay", 0.0, fw));
        b.follow = text(f, "follow", fw);
        if (!b.follow.empty() && b.follow != "outline" && b.follow != "water")
            bad(fw, "follow is outline or water");
        if (f.contains("lines"))
        {
            if (!f["lines"].is_array())
                bad(fw, "lines takes a list of polylines");
            for (const json& line : f["lines"])
            {
                std::vector<Vec2> pts;
                for (const json& p : line)
                    pts.push_back(point(p, fw + ".lines"));
                if (pts.size() >= 2)
                    b.lines.push_back(std::move(pts));
            }
        }
        if (b.kind == "boundary" && b.lines.empty() && b.follow.empty())
            bad(fw, "a boundary field needs lines or follow");
        t.fields.push_back(std::move(b));
    }
    for (const json& p : j.value("paint", json::array()))
    {
        const std::string pw = w + ".paint";
        onlyKeys(p, pw, {"points", "closed", "radius", "angle", "weight"});
        TensorPaint s;
        s.pts = points(p, "points", pw, 2);
        s.closed = boolean(p, "closed", false, pw);
        s.radius = std::max(1.0, number(p, "radius", s.radius, pw));
        if (p.contains("angle"))
            s.angle = number(p, "angle", 0.0, pw);
        s.weight = number(p, "weight", s.weight, pw);
        t.paint.push_back(std::move(s));
    }
    if (j.contains("mask"))
    {
        const json& m = j["mask"];
        const std::string mw = w + ".mask";
        onlyKeys(m, mw, {"origin", "cell", "width", "height", "values"});
        WeightMask mask;
        if (m.contains("origin"))
            mask.origin = point(m["origin"], mw);
        mask.cell = std::max(1.0, number(m, "cell", 100.0, mw));
        mask.w = int(number(m, "width", 0.0, mw));
        mask.h = int(number(m, "height", 0.0, mw));
        for (const json& v : m.value("values", json::array()))
            mask.values.push_back(v.is_number() ? float(v.get<double>()) : 0.0f);
        if (mask.w <= 0 || mask.h <= 0 || mask.values.size() != size_t(mask.w) * size_t(mask.h))
            bad(mw, "values must hold width * height numbers");
        t.mask = std::move(mask);
    }
    t.majorSpacing = std::max(0.0, number(j, "majorSpacing", 0.0, w));
    t.minorSpacing = std::max(0.0, number(j, "minorSpacing", 0.0, w));
    t.step = std::max(0.0, number(j, "step", 0.0, w));
    t.minor = boolean(j, "minor", true, w);
    return t;
}

WallSpec wallsFrom(const json& j, const std::string& w)
{
    WallSpec s;
    s.enabled = true;
    if (j.is_boolean())
    {
        s.enabled = j.get<bool>();
        return s;
    }
    onlyKeys(j, w, {"radius", "polygon", "contour", "thickness", "height", "towerSpacing", "starFort", "bastions", "moat", "moatGap"});
    s.radius = std::clamp(number(j, "radius", 0.0, w), 0.0, 1.5);
    s.polygon = points(j, "polygon", w);
    if (!s.polygon.empty() && s.polygon.size() < 3)
        bad(w, "polygon needs at least 3 points");
    s.contour = boolean(j, "contour", true, w);
    s.thickness = std::max(10.0, number(j, "thickness", s.thickness, w));
    s.height = std::max(10.0, number(j, "height", s.height, w));
    s.towerSpacing = std::max(0.0, number(j, "towerSpacing", s.towerSpacing, w));
    s.starFort = boolean(j, "starFort", false, w);
    s.bastions = std::clamp(int(number(j, "bastions", 0.0, w)), 0, 24);
    if (s.bastions > 0 && s.bastions < 3)
        bad(w, "bastions takes 3 or more");
    s.moat = std::max(0.0, number(j, "moat", 0.0, w));
    s.moatGap = std::max(0.0, number(j, "moatGap", s.moatGap, w));
    return s;
}

GenRegion regionFrom(const json& j, const std::string& w)
{
    GenRegion r;
    r.polygon = points(j, "polygon", w);
    if (!r.polygon.empty() && r.polygon.size() < 3)
        bad(w, "polygon needs at least 3 points");
    r.inner = std::clamp(number(j, "inner", 0.0, w), 0.0, 1.5);
    r.outer = std::clamp(number(j, "outer", 1.0, w), 0.0, 1.5);
    if (r.outer <= r.inner)
        bad(w, "outer must be past inner");
    return r;
}

std::vector<EraSpec> erasFrom(const json& j, const std::string& w);

EraSpec eraFrom(const json& j, const std::string& w)
{
    onlyKeys(j, w, {"name", "profile", "overrides", "pattern", "growth", "inner", "outer", "polygon", "walls", "demolishWalls", "ring",
                    "axes", "highwayRing", "highwaySpurs"});
    EraSpec e;
    e.name = text(j, "name", w);
    e.profile = text(j, "profile", w);
    if (!e.profile.empty())
    {
        Profile probe;
        if (!builtinProfile(e.profile, &probe))
            bad(w, "unknown profile '" + e.profile + "'");
    }
    e.overrides = j.value("overrides", json::object());
    if (!e.overrides.is_object())
        bad(w, "overrides must be a JSON object");
    if (!e.overrides.empty())
    {
        Profile probe;
        try
        {
            applyJson(probe, e.overrides);
        }
        catch (const std::exception& x)
        {
            bad(w, x.what());
        }
    }
    e.pattern = pattern(j, w);
    e.growth = std::clamp(number(j, "growth", 1.0, w), 0.0, 1.0);
    e.region = regionFrom(j, w);
    if (j.contains("walls"))
        e.walls = wallsFrom(j["walls"], w + ".walls");
    e.demolishWalls = boolean(j, "demolishWalls", false, w);
    e.ring = boolean(j, "ring", false, w);
    if (j.contains("axes"))
    {
        const json& a = j["axes"];
        if (a.is_number())
        {
            AxisSpec x;
            x.count = std::clamp(int(a.get<double>()), 1, 8);
            e.axes.push_back(x);
        }
        else if (a.is_array())
            for (const json& x : a)
            {
                const std::string aw = w + ".axes";
                onlyKeys(x, aw, {"points", "count", "angle", "clearance"});
                AxisSpec s;
                s.pts = points(x, "points", aw);
                if (!s.pts.empty() && s.pts.size() != 2)
                    bad(aw, "points takes two points");
                s.count = std::clamp(int(number(x, "count", 1.0, aw)), 1, 8);
                s.angle = number(x, "angle", 0.0, aw);
                s.clearance = std::max(0.0, number(x, "clearance", 0.0, aw));
                e.axes.push_back(std::move(s));
            }
        else
            bad(w, "axes takes a count or a list of axes");
    }
    e.highwayRing = std::clamp(number(j, "highwayRing", 0.0, w), 0.0, 1.5);
    e.highwaySpurs = std::clamp(int(number(j, "highwaySpurs", 0.0, w)), 0, 16);
    return e;
}

std::vector<EraSpec> erasFrom(const json& j, const std::string& w)
{
    json list = j;
    if (j.is_string())
    {
        list = eraPreset(j.get<std::string>());
        if (list.is_null())
        {
            std::string names;
            for (const std::string& n : eraPresetNames())
                names += (names.empty() ? "" : ", ") + n;
            bad(w, "unknown preset '" + j.get<std::string>() + "' (" + names + ")");
        }
    }
    if (!list.is_array())
        bad(w, "takes a list of eras or a preset name");
    std::vector<EraSpec> out;
    for (size_t k = 0; k < list.size(); ++k)
        out.push_back(eraFrom(list[k], w + "[" + std::to_string(k) + "]"));
    return out;
}

std::vector<ImportedStreet> streetsFrom(const json& j, const std::string& w)
{
    if (!j.is_array())
        bad(w, "takes a list of streets {points, class}");
    std::vector<ImportedStreet> out;
    for (const json& s : j)
    {
        onlyKeys(s, w, {"points", "class", "name", "id"});
        ImportedStreet x;
        x.pts = points(s, "points", w, 2);
        x.cls = classFromOsm(text(s, "class", w));
        x.name = text(s, "name", w);
        x.osmId = text(s, "id", w);
        out.push_back(std::move(x));
    }
    return out;
}

double heightField(const json& j, const char* key, const std::string& where)
{
    if (!j.contains(key) || j[key].is_null())
        return std::numeric_limits<double>::quiet_NaN();
    if (!j[key].is_number())
        bad(where, std::string(key) + " takes a number (cm) or null");
    return j[key].get<double>();
}

std::vector<std::vector<Vec2>> ringsFrom(const json& j, const char* key, const std::string& where)
{
    std::vector<std::vector<Vec2>> out;
    if (!j.contains(key))
        return out;
    if (!j[key].is_array())
        bad(where, std::string(key) + " takes a list of rings");
    for (const json& r : j[key])
    {
        std::vector<Vec2> ring;
        if (!r.is_array())
            bad(where, std::string(key) + " takes a list of rings");
        for (const json& p : r)
            ring.push_back(point(p, where + "." + key));
        if (ring.size() >= 3)
            out.push_back(std::move(ring));
    }
    return out;
}

std::vector<OsmBuildingIn> osmBuildingsFrom(const json& j)
{
    const std::string w = "osmBuildings";
    if (!j.is_array())
        bad(w, "takes a list of buildings {id, outer, holes, ...}");
    std::vector<OsmBuildingIn> out;
    out.reserve(j.size());
    for (const json& b : j)
    {
        onlyKeys(b, w, {"id", "outer", "holes", "part", "building", "program", "kind", "name", "height", "minHeight", "roofHeight", "levels",
                        "minLevel", "roofLevels", "roofShape", "colour", "roofColour", "material", "roofMaterial", "startDate"});
        OsmBuildingIn x;
        x.id = text(b, "id", w);
        if (x.id.empty())
            bad(w, "every building needs an id");
        x.outer = points(b, "outer", w, 3);
        x.holes = ringsFrom(b, "holes", w);
        x.part = boolean(b, "part", false, w);
        x.building = text(b, "building", w);
        x.program = text(b, "program", w);
        x.kind = text(b, "kind", w);
        x.name = text(b, "name", w);
        x.height = heightField(b, "height", w);
        x.minHeight = heightField(b, "minHeight", w);
        x.roofHeight = heightField(b, "roofHeight", w);
        x.levels = int(number(b, "levels", -1.0, w));
        x.minLevel = int(number(b, "minLevel", -1.0, w));
        x.roofLevels = int(number(b, "roofLevels", -1.0, w));
        x.roofShape = text(b, "roofShape", w);
        x.colour = text(b, "colour", w);
        x.roofColour = text(b, "roofColour", w);
        x.material = text(b, "material", w);
        x.roofMaterial = text(b, "roofMaterial", w);
        x.startDate = text(b, "startDate", w);
        out.push_back(std::move(x));
    }
    return out;
}

std::vector<OsmPlaceIn> osmPlacesFrom(const json& j)
{
    const std::string w = "osmPlaces";
    if (!j.is_array())
        bad(w, "takes a list of places {id, program, kind, name, at or rings}");
    std::vector<OsmPlaceIn> out;
    for (const json& p : j)
    {
        onlyKeys(p, w, {"id", "key", "value", "program", "kind", "name", "at", "rings"});
        OsmPlaceIn x;
        x.id = text(p, "id", w);
        x.key = text(p, "key", w);
        x.value = text(p, "value", w);
        x.program = text(p, "program", w);
        x.kind = text(p, "kind", w);
        x.name = text(p, "name", w);
        x.rings = ringsFrom(p, "rings", w);
        if (p.contains("at"))
            x.at = point(p["at"], w + ".at");
        else if (!x.rings.empty())
        {
            Vec2 c(0.0);
            for (const Vec2& q : x.rings.front())
                c += q / double(x.rings.front().size());
            x.at = c;
        }
        else
            bad(w, "a place needs at or rings");
        out.push_back(std::move(x));
    }
    return out;
}

LevelsSpec levelsFrom(const json& j)
{
    const std::string w = "levels";
    onlyKeys(j, w, {"decks", "elevated", "skybridgeSpan", "skybridgeHeight", "airHeights", "airSpacing", "megablock"});
    LevelsSpec s;
    for (const json& d : j.value("decks", json::array()))
    {
        const std::string dw = w + ".decks";
        onlyKeys(d, dw, {"name", "polygon", "height", "pattern", "ramps"});
        DeckSpec x;
        x.name = text(d, "name", dw);
        x.polygon = points(d, "polygon", dw, 3);
        x.height = std::max(100.0, number(d, "height", x.height, dw));
        x.pattern = pattern(d, dw);
        x.ramps = std::clamp(int(number(d, "ramps", 2.0, dw)), 0, 16);
        s.decks.push_back(std::move(x));
    }
    for (const json& e : j.value("elevated", json::array()))
    {
        const std::string ew = w + ".elevated";
        onlyKeys(e, ew, {"points", "height", "class", "ramps"});
        ElevatedSpec x;
        x.pts = points(e, "points", ew, 2);
        x.height = std::max(100.0, number(e, "height", x.height, ew));
        if (const std::string c = text(e, "class", ew); !c.empty() && !parseClass(c, &x.cls))
            bad(ew, "unknown class '" + c + "'");
        x.ramps = boolean(e, "ramps", true, ew);
        s.elevated.push_back(std::move(x));
    }
    s.skybridgeSpan = std::max(0.0, number(j, "skybridgeSpan", 0.0, w));
    s.skybridgeHeight = std::clamp(number(j, "skybridgeHeight", 0.5, w), 0.05, 1.0);
    for (const json& h : j.value("airHeights", json::array()))
    {
        if (!h.is_number())
            bad(w, "airHeights takes numbers");
        s.airHeights.push_back(std::max(100.0, h.get<double>()));
    }
    s.airSpacing = std::max(1000.0, number(j, "airSpacing", s.airSpacing, w));
    s.megablock = std::max(0.0, number(j, "megablock", 0.0, w));
    return s;
}
} // namespace

double WeightMask::at(Vec2 p) const
{
    if (w <= 0 || h <= 0)
        return 1.0;
    const int x = int(std::floor((p.x - origin.x) / cell)), y = int(std::floor((p.y - origin.y) / cell));
    if (x < 0 || y < 0 || x >= w || y >= h)
        return 0.0;
    return std::clamp(double(values[size_t(y) * size_t(w) + size_t(x)]), 0.0, 1.0);
}

StreetClass classFromOsm(std::string_view tag)
{
    StreetClass c;
    if (parseClass(tag, &c))
        return c;
    for (std::string_view t : {"motorway", "trunk", "primary", "motorway_link", "trunk_link", "primary_link"})
        if (tag == t)
            return StreetClass::Arterial;
    for (std::string_view t : {"secondary", "tertiary", "secondary_link", "tertiary_link"})
        if (tag == t)
            return StreetClass::Collector;
    for (std::string_view t : {"service", "alley", "driveway"})
        if (tag == t)
            return StreetClass::Alley;
    for (std::string_view t : {"footway", "path", "pedestrian", "steps", "cycleway", "track", "bridleway", "corridor"})
        if (tag == t)
            return StreetClass::Path;
    return StreetClass::Local;
}

std::vector<std::string> eraPresetNames() { return {"walledTown", "medievalToModern", "gridToSprawl"}; }

json eraPreset(std::string_view name)
{
    if (name == "walledTown")
        return json::array({
            {{"name", "medieval"}, {"profile", "medievalTown"}, {"pattern", "organic"}, {"outer", 0.5}, {"walls", {{"moat", 1200.0}}}},
            {{"name", "faubourgs"}, {"profile", "medievalTown"}, {"pattern", "organic"}, {"inner", 0.5}, {"growth", 0.6}},
        });
    if (name == "medievalToModern")
        return json::array({
            {{"name", "medieval"}, {"profile", "medievalTown"}, {"pattern", "organic"}, {"outer", 0.25}, {"walls", json::object()}},
            {{"name", "baroque"}, {"profile", "medievalTown"}, {"pattern", "none"}, {"outer", 0.25}, {"axes", 2}},
            {{"name", "nineteenthCentury"}, {"profile", "europeanCity"}, {"pattern", "grid"}, {"inner", 0.25}, {"outer", 0.5},
             {"demolishWalls", true}, {"ring", true}},
            {{"name", "modern"}, {"profile", "modernCity"}, {"pattern", "organic"}, {"inner", 0.5}, {"growth", 0.7},
             {"highwayRing", 0.6}, {"highwaySpurs", 4}},
        });
    if (name == "gridToSprawl")
        return json::array({
            {{"name", "townsite"}, {"profile", "usTown"}, {"pattern", "grid"}, {"outer", 0.45}},
            {{"name", "streetcarSuburb"}, {"profile", "usTown"}, {"pattern", "grid"}, {"inner", 0.45}, {"outer", 0.7},
             {"overrides", {{"gridAngle", 8.0}}}},
            {{"name", "postwar"}, {"profile", "usTown"}, {"pattern", "organic"}, {"inner", 0.7}, {"growth", 0.6},
             {"overrides", {{"turn", 30.0}, {"plaza", false}}}, {"highwayRing", 0.9}, {"highwaySpurs", 3}},
        });
    return nullptr;
}

GenSpec parseGenerators(const json& j)
{
    LILA_ZONE();

    GenSpec s;
    if (j.is_null() || (j.is_object() && j.empty()))
        return s;
    onlyKeys(j, "generators", {"tensor", "walls", "eras", "districts", "osm", "osmBuildings", "osmPlaces", "osmFill", "avoid", "example", "contour", "levels"});
    if (j.contains("tensor"))
        s.tensor = tensorFrom(j["tensor"]);
    if (j.contains("walls"))
        if (WallSpec w = wallsFrom(j["walls"], "walls"); w.enabled)
            s.walls = w;
    if (j.contains("eras"))
        s.eras = erasFrom(j["eras"], "eras");
    if (j.contains("districts"))
    {
        if (!j["districts"].is_array())
            bad("districts", "takes a list of districts");
        for (const json& d : j["districts"])
        {
            const std::string w = "districts";
            onlyKeys(d, w, {"name", "polygon", "profile", "overrides", "pattern", "growth", "era", "seed", "eras"});
            DistrictSpec x;
            x.name = text(d, "name", w);
            x.polygon = points(d, "polygon", w, 3);
            x.profile = text(d, "profile", w);
            if (!x.profile.empty())
            {
                Profile probe;
                if (!builtinProfile(x.profile, &probe))
                    bad(w, "unknown profile '" + x.profile + "'");
            }
            x.overrides = d.value("overrides", json::object());
            if (!x.overrides.is_object())
                bad(w, "overrides must be a JSON object");
            x.pattern = pattern(d, w);
            x.growth = std::clamp(number(d, "growth", 1.0, w), 0.0, 1.0);
            x.era = int(number(d, "era", -1.0, w));
            x.seed = uint32_t(std::max(0.0, number(d, "seed", 0.0, w)));
            if (d.contains("eras"))
                x.eras = erasFrom(d["eras"], w + ".eras");
            s.districts.push_back(std::move(x));
        }
    }
    if (j.contains("osm"))
        s.osm = streetsFrom(j["osm"], "osm");
    if (j.contains("osmBuildings"))
        s.osmBuildings = osmBuildingsFrom(j["osmBuildings"]);
    if (j.contains("osmPlaces"))
        s.osmPlaces = osmPlacesFrom(j["osmPlaces"]);
    if (j.contains("osmFill"))
    {
        const json& f = j["osmFill"];
        onlyKeys(f, "osmFill", {"mode", "coverage"});
        s.osmFill = text(f, "mode", "osmFill");
        if (s.osmFill.empty())
            s.osmFill = "sparse";
        if (s.osmFill != "sparse" && s.osmFill != "all" && s.osmFill != "none")
            bad("osmFill", "mode is sparse, all or none");
        s.osmCoverage = std::clamp(number(f, "coverage", 0.25, "osmFill"), 0.0, 1.0);
    }
    if (j.contains("avoid"))
    {
        try
        {
            s.avoid = lila::names::NameHashes::fromJson(j["avoid"].dump());
        }
        catch (const std::exception& e)
        {
            bad("avoid", e.what());
        }
    }
    if (j.contains("example"))
    {
        const json& e = j["example"];
        if (e.is_object() && e.contains("streets"))
            s.example = statsFromStreets(streetsFrom(e["streets"], "example.streets"));
        else
            s.example = statsFromJson(e);
        if (!s.example.valid)
            bad("example", "has no streets or orientation to learn from");
    }
    if (j.contains("contour"))
    {
        if (!j["contour"].is_array())
            bad("contour", "takes a list of streets {points, kind}");
        for (const json& c : j["contour"])
        {
            onlyKeys(c, "contour", {"points", "kind", "grade"});
            ContourStreet x;
            x.kind = text(c, "kind", "contour");
            if (x.kind.empty())
                x.kind = "contour";
            for (const json& p : c.value("points", json::array()))
            {
                if (!p.is_array() || p.size() < 2)
                    bad("contour", "points are [x, z] or [x, y, z]");
                x.pts.push_back(p.size() >= 3 ? glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>())
                                              : glm::dvec3(p[0].get<double>(), std::nan(""), p[1].get<double>()));
            }
            if (x.pts.size() >= 2)
                s.contour.push_back(std::move(x));
        }
    }
    if (j.contains("levels"))
        s.levels = levelsFrom(j["levels"]);
    return s;
}
} // namespace settlement
