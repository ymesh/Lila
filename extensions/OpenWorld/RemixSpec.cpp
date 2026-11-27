// The World Remix spec: JSON in and out, its schema, and the polygon helpers the remix measures with.

#include "RemixSpec.h"

#include "Landform.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openworld
{
namespace
{
constexpr double kSpecDeg = 0.017453292519943295;

Vec2 vec2(const json& v, const std::string& field)
{
    if (!v.is_array() || v.size() < 2 || !v[0].is_number() || !v[1].is_number())
        throw std::runtime_error(field + " is [x, z]");
    // [x, y, z] takes x and z, as points picked in the viewport come.
    if (v.size() >= 3 && v[2].is_number())
        return {v[0].get<double>(), v[2].get<double>()};
    return {v[0].get<double>(), v[1].get<double>()};
}

json vecJson(Vec2 v) { return json::array({v.x, v.y}); }

Polygon polygonOf(const json& v, const std::string& field)
{
    if (!v.is_array() || v.size() < 3)
        throw std::runtime_error(field + " is a list of at least three [x, z] points");
    Polygon p;
    for (size_t i = 0; i < v.size(); ++i)
        p.push_back(vec2(v[i], field + "[" + std::to_string(i) + "]"));
    if (p.size() > 3 && glm::length(p.front() - p.back()) < 1e-6)
        p.pop_back();
    return p;
}

template <class T>
T num(const json& j, const char* key, T def, const std::string& where)
{
    if (!j.contains(key) || j[key].is_null())
        return def;
    if (!j[key].is_number() && !j[key].is_boolean())
        throw std::runtime_error(where + key + " is a number");
    return j[key].get<T>();
}

std::string str(const json& j, const char* key, const std::string& where)
{
    if (!j.contains(key) || j[key].is_null())
        return {};
    if (!j[key].is_string())
        throw std::runtime_error(where + key + " is a string");
    return j[key].get<std::string>();
}

void onlyKeys(const json& j, std::initializer_list<const char*> keys, const std::string& where)
{
    for (const auto& [k, v] : j.items())
        if (std::find_if(keys.begin(), keys.end(), [&](const char* a) { return k == a; }) == keys.end())
            throw std::runtime_error(where + "unknown key '" + k + "' (see lila://schema/remix-spec)");
}
} // namespace

Vec2 rotated(Vec2 v, double degrees)
{
    const double c = std::cos(degrees * kSpecDeg), s = std::sin(degrees * kSpecDeg);
    return {v.x * c + v.y * s, -v.x * s + v.y * c};
}

Vec2 Rigid::apply(Vec2 p) const { return rotated(p - pivot, angle) + pivot + translate; }
Vec2 Rigid::inverse(Vec2 p) const { return rotated(p - translate - pivot, -angle) + pivot; }

const std::vector<std::string>& regionOpNames()
{
    static const std::vector<std::string> n = {"move", "keep", "cut", "duplicate", "delete", "sea"};
    return n;
}

bool parseRegionOp(std::string_view name, RegionOp* out)
{
    const auto& n = regionOpNames();
    for (size_t i = 0; i < n.size(); ++i)
        if (n[i] == name)
        {
            *out = RegionOp(i);
            return true;
        }
    return false;
}

Rigid Region::rigid() const
{
    if (stays(op))
        return {};
    return {angle, pivot ? *pivot : centroid(polygon), translate};
}

Polygon Region::target() const { return transformed(polygon, rigid()); }

const Source* RemixSpec::source(std::string_view id) const
{
    for (const Source& s : sources)
        if (s.id == id)
            return &s;
    return nullptr;
}

const Region* RemixSpec::region(std::string_view id) const
{
    for (const Region& r : regions)
        if (r.id == id)
            return &r;
    return nullptr;
}

double parseScale(const json& v, const char* field)
{
    if (v.is_number())
    {
        const double f = v.get<double>();
        if (!(f > 0.0) || !std::isfinite(f))
            throw std::runtime_error(std::string(field) + " is a factor above 0 or \"1:N\"");
        return f;
    }
    if (v.is_string())
    {
        const std::string s = v.get<std::string>();
        const size_t colon = s.find(':');
        try
        {
            if (colon == std::string::npos)
                return parseScale(std::stod(s), field);
            const double a = std::stod(s.substr(0, colon)), b = std::stod(s.substr(colon + 1));
            if (a > 0.0 && b > 0.0)
                return a / b;
        }
        catch (const std::invalid_argument&)
        {
        }
        catch (const std::out_of_range&)
        {
        }
    }
    throw std::runtime_error(std::string(field) + " is a factor above 0 or \"1:N\"");
}

std::string scaleText(double factor)
{
    char buf[48];
    const double n = 1.0 / factor;
    if (std::abs(n - std::round(n)) < 1e-6)
        std::snprintf(buf, sizeof buf, "1:%g", std::round(n));
    else
        std::snprintf(buf, sizeof buf, "1:%.4g", n);
    return buf;
}

json toJson(const RemixSpec& s)
{
    json sources = json::array(), regions = json::array(), pins = json::array();
    for (const Source& src : s.sources)
    {
        json j = {{"id", src.id}, {"at", vecJson(src.at)}, {"angle", src.angle}};
        for (auto [k, v] : {std::pair{"dem", &src.dem}, {"osm", &src.osm}, {"landcover", &src.landcover}, {"bathymetry", &src.bathymetry}})
            if (!v->empty())
                j[k] = *v;
        if (src.bbox.size() == 4)
            j["bbox"] = src.bbox;
        if (src.scale > 0.0)
            j["scale"] = src.scale;
        if (src.vertical > 0.0)
            j["vertical"] = scaleText(src.vertical);
        if (src.extent != "box")
        {
            j["extent"] = src.extent;
            j["margin"] = src.margin;
        }
        if (src.seam > 0.0)
            j["seam"] = src.seam;
        if (src.seamNoise >= 0.0)
            j["seamNoise"] = src.seamNoise;
        if (src.erosion != 1.0)
            j["erosion"] = src.erosion;
        sources.push_back(j);
    }
    for (const Region& r : s.regions)
    {
        json poly = json::array();
        for (Vec2 p : r.polygon)
            poly.push_back(vecJson(p));
        json j = {{"id", r.id}, {"op", regionOpNames()[size_t(r.op)]}, {"polygon", poly}};
        if (!stays(r.op))
        {
            j["translate"] = vecJson(r.translate);
            j["angle"] = r.angle;
            if (r.pivot)
                j["pivot"] = vecJson(*r.pivot);
        }
        if (r.seam)
            j["seam"] = *r.seam;
        if (!r.matchHeight)
            j["matchHeight"] = false;
        regions.push_back(j);
    }
    for (const Pin& p : s.pins)
        pins.push_back({{"id", p.id}, {"at", vecJson(p.at)}, {"to", vecJson(p.to)}});
    json out = {{"version", 1},
                {"scale", {{"horizontal", scaleText(s.horizontal)}, {"vertical", scaleText(s.vertical)}}},
                {"map", {{"center", vecJson(s.map.center)}, {"size", vecJson(s.map.size)}, {"cell", s.map.cell}, {"seaDepth", s.map.seaDepth},
                         {"maxResolution", s.map.maxResolution}}},
                {"sources", sources},
                {"regions", regions},
                {"pins", pins},
                {"warp", {{"falloff", s.warp.falloff}, {"maxSteps", s.warp.maxSteps}}},
                {"bathymetry", s.bathymetry},
                {"seams", {{"width", s.seams.width}, {"maxSlope", s.seams.maxSlope}, {"erosion", s.seams.erosion}, {"detail", s.seams.detail}, {"roads", s.seams.roads},
                           {"roadReach", s.seams.roadReach}, {"rivers", s.seams.rivers}, {"coast", s.seams.coast}, {"seed", s.seams.seed}}},
                {"view", s.view}};
    if (s.erosion.on())
        out["erosion"] = {{"preset", s.erosion.preset}, {"age", s.erosion.age}, {"form", s.erosion.form}, {"detail", s.erosion.detail}, {"detailScale", s.erosion.detailScale},
                          {"seed", s.erosion.seed}};
    if (s.origin)
        out["origin"] = json::array({s.origin->x, s.origin->y});
    return out;
}

RemixSpec specFromJson(const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("the remix spec is a dict (see lila://schema/remix-spec)");
    onlyKeys(j, {"version", "scale", "origin", "map", "sources", "regions", "pins", "warp", "seams", "erosion", "bathymetry", "view"}, "");
    RemixSpec s;
    if (j.contains("scale"))
    {
        const json& sc = j["scale"];
        if (sc.is_object())
        {
            onlyKeys(sc, {"horizontal", "vertical"}, "scale.");
            if (sc.contains("horizontal"))
                s.horizontal = parseScale(sc["horizontal"], "scale.horizontal");
            if (sc.contains("vertical"))
                s.vertical = parseScale(sc["vertical"], "scale.vertical");
        }
        else
            s.horizontal = parseScale(sc, "scale");
    }
    if (j.contains("origin") && !j["origin"].is_null())
    {
        const json& o = j["origin"];
        if (!o.is_array() || o.size() != 2 || !o[0].is_number() || !o[1].is_number())
            throw std::runtime_error("origin is [latitude, longitude]");
        s.origin = glm::dvec2(o[0].get<double>(), o[1].get<double>());
    }
    if (j.contains("map"))
    {
        const json& m = j["map"];
        onlyKeys(m, {"center", "size", "cell", "seaDepth", "maxResolution"}, "map.");
        if (m.contains("center"))
            s.map.center = vec2(m["center"], "map.center");
        if (m.contains("size"))
            s.map.size = vec2(m["size"], "map.size");
        s.map.cell = num(m, "cell", 0.0, "map.");
        s.map.seaDepth = num(m, "seaDepth", s.map.seaDepth, "map.");
        s.map.maxResolution = std::clamp(num(m, "maxResolution", s.map.maxResolution, "map."), 17, 16385);
    }
    if (j.contains("sources"))
    {
        if (!j["sources"].is_array())
            throw std::runtime_error("sources is a list");
        for (size_t i = 0; i < j["sources"].size(); ++i)
        {
            const json& e = j["sources"][i];
            const std::string where = "sources[" + std::to_string(i) + "].";
            if (!e.is_object())
                throw std::runtime_error(where + " is a dict");
            onlyKeys(e, {"id", "dem", "osm", "landcover", "bathymetry", "bbox", "at", "angle", "scale", "vertical", "extent", "margin", "seam", "seamNoise", "erosion"},
                     where);
            Source src;
            src.id = str(e, "id", where);
            if (src.id.empty())
                src.id = "source" + std::to_string(i + 1);
            src.dem = str(e, "dem", where);
            src.osm = str(e, "osm", where);
            src.landcover = str(e, "landcover", where);
            src.bathymetry = str(e, "bathymetry", where);
            if (e.contains("bbox") && !e["bbox"].is_null())
            {
                if (!e["bbox"].is_array() || e["bbox"].size() != 4)
                    throw std::runtime_error(where + "bbox is [west, south, east, north]");
                src.bbox = e["bbox"].get<std::vector<double>>();
            }
            if (e.contains("at"))
                src.at = vec2(e["at"], where + "at");
            src.angle = num(e, "angle", 0.0, where);
            src.scale = num(e, "scale", 0.0, where);
            if (e.contains("vertical") && !e["vertical"].is_null())
                src.vertical = parseScale(e["vertical"], "vertical");
            if (e.contains("extent") && !e["extent"].is_null())
            {
                src.extent = str(e, "extent", where);
                if (src.extent != "box" && src.extent != "builtUp")
                    throw std::runtime_error(where + "extent is box or builtUp");
            }
            src.margin = std::max(0.0, num(e, "margin", 0.0, where));
            src.seam = std::max(0.0, num(e, "seam", 0.0, where));
            src.seamNoise = num(e, "seamNoise", -1.0, where);
            src.erosion = std::clamp(num(e, "erosion", 1.0, where), 0.0, 1.0);
            if (s.source(src.id))
                throw std::runtime_error(where + "id '" + src.id + "' is used twice");
            s.sources.push_back(std::move(src));
        }
    }
    if (j.contains("regions"))
    {
        if (!j["regions"].is_array())
            throw std::runtime_error("regions is a list");
        for (size_t i = 0; i < j["regions"].size(); ++i)
        {
            const json& e = j["regions"][i];
            const std::string where = "regions[" + std::to_string(i) + "].";
            if (!e.is_object())
                throw std::runtime_error(where + " is a dict");
            onlyKeys(e, {"id", "op", "polygon", "translate", "angle", "pivot", "seam", "matchHeight"}, where);
            Region r;
            r.id = str(e, "id", where);
            if (r.id.empty())
                r.id = "region" + std::to_string(i + 1);
            const std::string op = e.contains("op") ? str(e, "op", where) : std::string("cut");
            if (!parseRegionOp(op, &r.op))
                throw std::runtime_error(where + "op is move, keep, cut, duplicate or delete");
            if (!e.contains("polygon"))
                throw std::runtime_error(where + "polygon is missing");
            r.polygon = polygonOf(e["polygon"], where + "polygon");
            if (e.contains("translate"))
                r.translate = vec2(e["translate"], where + "translate");
            r.angle = num(e, "angle", 0.0, where);
            if (e.contains("pivot") && !e["pivot"].is_null())
                r.pivot = vec2(e["pivot"], where + "pivot");
            if (e.contains("seam") && !e["seam"].is_null())
                r.seam = std::max(0.0, num(e, "seam", 0.0, where));
            if (e.contains("matchHeight"))
            {
                if (!e["matchHeight"].is_boolean())
                    throw std::runtime_error(where + "matchHeight is true or false");
                r.matchHeight = e["matchHeight"].get<bool>();
            }
            if (s.region(r.id))
                throw std::runtime_error(where + "id '" + r.id + "' is used twice");
            s.regions.push_back(std::move(r));
        }
    }
    if (j.contains("pins"))
    {
        if (!j["pins"].is_array())
            throw std::runtime_error("pins is a list");
        for (size_t i = 0; i < j["pins"].size(); ++i)
        {
            const json& e = j["pins"][i];
            const std::string where = "pins[" + std::to_string(i) + "].";
            onlyKeys(e, {"id", "at", "to"}, where);
            Pin p;
            p.id = str(e, "id", where);
            if (p.id.empty())
                p.id = "pin" + std::to_string(i + 1);
            if (!e.contains("at"))
                throw std::runtime_error(where + "at is missing");
            p.at = vec2(e["at"], where + "at");
            p.to = e.contains("to") ? vec2(e["to"], where + "to") : p.at;
            s.pins.push_back(p);
        }
    }
    if (j.contains("warp"))
    {
        const json& w = j["warp"];
        onlyKeys(w, {"falloff", "maxSteps"}, "warp.");
        s.warp.falloff = std::max(0.0, num(w, "falloff", 0.0, "warp."));
        s.warp.maxSteps = std::clamp(num(w, "maxSteps", s.warp.maxSteps, "warp."), 1, 4096);
    }
    if (j.contains("seams"))
    {
        const json& m = j["seams"];
        onlyKeys(m, {"width", "maxSlope", "erosion", "detail", "roads", "roadReach", "rivers", "coast", "seed"}, "seams.");
        s.seams.width = std::max(0.0, num(m, "width", s.seams.width, "seams."));
        s.seams.maxSlope = std::max(0.0, num(m, "maxSlope", s.seams.maxSlope, "seams."));
        s.seams.erosion = std::clamp(num(m, "erosion", s.seams.erosion, "seams."), 0, 2000);
        s.seams.detail = std::max(0.0, num(m, "detail", s.seams.detail, "seams."));
        s.seams.roads = num(m, "roads", s.seams.roads, "seams.");
        s.seams.roadReach = std::max(0.0, num(m, "roadReach", 0.0, "seams."));
        s.seams.rivers = num(m, "rivers", s.seams.rivers, "seams.");
        s.seams.coast = num(m, "coast", s.seams.coast, "seams.");
        s.seams.seed = num(m, "seed", s.seams.seed, "seams.");
    }
    if (j.contains("erosion") && !j["erosion"].is_null())
    {
        const json& m = j["erosion"];
        if (!m.is_object())
            throw std::runtime_error("erosion is a dict");
        onlyKeys(m, {"preset", "age", "form", "detail", "detailScale", "seed"}, "erosion.");
        RemixSpec::Erosion& er = s.erosion;
        if (m.contains("preset"))
            er.preset = str(m, "preset", "erosion.");
        if (!terrain::isErodePreset(er.preset))
            throw std::runtime_error("erosion.preset is natural, alpine, desert or hills");
        er.age = std::clamp(num(m, "age", er.age, "erosion."), 0.0, 2.0);
        er.form = std::max(100.0, num(m, "form", er.form, "erosion."));
        er.detail = std::max(0.0, num(m, "detail", er.detail, "erosion."));
        er.detailScale = std::max(100.0, num(m, "detailScale", er.detailScale, "erosion."));
        er.seed = num(m, "seed", er.seed, "erosion.");
    }
    if (j.contains("bathymetry") && !j["bathymetry"].is_null())
    {
        const json& b = j["bathymetry"];
        if (!b.is_object())
            throw std::runtime_error("bathymetry is a dict");
        onlyKeys(b, {"enabled", "shoreSlope", "blendWidth", "smoothing", "depthScale", "shelfDepth", "shelfWidth", "dropDepth", "slopeWidth", "minDepth", "landTolerance"},
                 "bathymetry.");
        for (const auto& [k, v] : b.items())
            if (k == "enabled" ? !v.is_boolean() : !v.is_number())
                throw std::runtime_error("bathymetry." + k + (k == "enabled" ? " is true or false" : " is a number"));
        s.bathymetry = b;
    }
    if (j.contains("view") && j["view"].is_object())
        s.view = j["view"];
    return s;
}

json remixSpecSchema()
{
    const json point = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 3}, {"description", "[x, z] cm on the map"}};
    const json scale = {{"type", {"number", "string"}}, {"description", "A factor (0.333) or a ratio \"1:3\"."}};
    return {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"$id", "lila://schema/remix-spec"},
        {"title", "Lila World Remix spec"},
        {"description",
         "Real places composed onto one map before they become terrain and settlements. Each source (DEM, OSM, land cover over a box) is "
         "scaled and placed on the base map; regions (polygons on the base map) are moved, kept, cut, duplicated or deleted; pins warp "
         "the empty land between. Heights, OSM features and land cover all go through the same map, so a road on a ridge stays on it. "
         "Lengths cm, +X east, -Z north, angles degrees counter-clockwise seen from above. remixCreate(spec=...) builds it; "
         "remixSpec reads it back."},
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"version", {{"type", "integer"}, {"const", 1}}},
          {"scale",
           {{"type", {"object", "number", "string"}},
            {"description", "Horizontal and vertical scale; a bare value is horizontal. Default 1:3 and 1:1.5 so mountains stay mountains."},
            {"properties", {{"horizontal", scale}, {"vertical", scale}}}}},
          {"origin", {{"type", "array"}, {"description", "[latitude, longitude] the map's centre is written at in the remixed OSM; default the first source's centre."}}},
          {"map",
           {{"type", "object"},
            {"properties",
             {{"center", point},
              {"size", {{"type", "array"}, {"description", "[width, depth] cm; [0, 0] fits everything placed."}}},
              {"cell", {{"type", "number"}, {"description", "Grid spacing cm; 0 is the finest source's cell times the horizontal scale."}}},
              {"seaDepth", {{"type", "number"}, {"description", "Flat sea floor where no source covers, cm, with bathymetry off. Default -3000."}}},
              {"maxResolution", {{"type", "integer"}, {"description", "Most samples along the longer side. Default 2049."}}}}}}},
          {"sources",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id"}},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"dem", {{"type", "string"}, {"description", "Elevation file (GeoTIFF, .hgt, .asc, .xyz, Terrarium PNG)."}}},
                {"osm", {{"type", "string"}, {"description", "OSM file (.osm, Overpass .json, .osm.pbf)."}}},
                {"landcover", {{"type", "string"}, {"description", "ESA WorldCover GeoTIFF."}}},
                {"bathymetry", {{"type", "string"}, {"description", "Bathymetry grid around the place (GEBCO GeoTIFF): the sea floor near it."}}},
                {"bbox", {{"type", "array"}, {"description", "[west, south, east, north] degrees: what remixFetch downloads."}}},
                {"at", point},
                {"angle", {{"type", "number"}}},
                {"scale", {{"type", "number"}, {"description", "This source's own horizontal factor; 0 takes the spec's."}}},
                {"vertical", {{"type", {"number", "string"}}, {"description", "This source's own height factor (0.33 or \"1:3\"); default the spec's."}}},
                {"extent", {{"enum", {"box", "builtUp"}}, {"description", "box: present over its whole bbox (default); builtUp: only over its built-up land, gaps closed, so a city ends where the real one does."}}},
                {"margin", {{"type", "number"}, {"description", "builtUp: how far past the built-up land it reaches, cm."}}},
                {"seam", {{"type", "number"}, {"description", "Width of the band where it blends over the sources under it, cm; default the remix's feather."}}},
                {"seamNoise", {{"type", "number"}, {"description", "How far that band wanders in from its edge, cm, so no straight line shows; default the seam width."}}},
                {"erosion", {{"type", "number"}, {"description", "Share of the erosion pass its ground takes, 0 (held: a city) to 1 (default)."}}}}}}}}},
          {"regions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"polygon"}},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"op", {{"enum", regionOpNames()}, {"description", "move (land around stretches), keep (held rigid), cut (lifted and placed, hole filled), duplicate, delete, sea (land sunk to the sea floor over the seam band)."}}},
                {"polygon", {{"type", "array"}, {"items", point}, {"description", "On the base map."}}},
                {"translate", point},
                {"angle", {{"type", "number"}}},
                {"pivot", point},
                {"seam", {{"type", "number"}, {"description", "Seam band width cm; default seams.width."}}},
                {"matchHeight", {{"type", "boolean"}, {"description", "cut and duplicate: raise or lower the pasted ground by the median step along its edge and widen the seam where what is left would be steeper than 8% (default true)."}}}}}}}}},
          {"pins",
           {{"type", "array"},
            {"items", {{"type", "object"}, {"required", {"at"}}, {"properties", {{"id", {{"type", "string"}}}, {"at", point}, {"to", point}}}}},
            {"description", "Points of the base map and where they go; to omitted holds the point still."}}},
          {"warp",
           {{"type", "object"},
            {"properties",
             {{"falloff", {{"type", "number"}, {"description", "Reach of moves and pins, cm; 0 is a quarter of the map."}}},
              {"maxSteps", {{"type", "integer"}, {"description", "Most integration steps for the fold-free warp."}}}}}}},
          {"seams",
           {{"type", "object"},
            {"properties",
             {{"width", {{"type", "number"}, {"description", "Blend band cm. Default 1500."}}},
              {"maxSlope",
               {{"type", "number"},
                {"description", "Steepest seam slope (rise over run) left where a pasted region meets the ground round it. "
                                "Default 0.08; 0 uses the default."}}},
              {"erosion", {{"type", "integer"}, {"description", "Erosion steps re-run in seam bands."}}},
              {"detail", {{"type", "number"}}},
              {"roads", {{"type", "boolean"}, {"description", "Reconnect cut roads with terrainRoute, keeping their class."}}},
              {"roadReach", {{"type", "number"}}},
              {"rivers", {{"type", "boolean"}, {"description", "Re-solve rivers by flow routing so they drain."}}},
              {"coast", {{"type", "boolean"}, {"description", "Re-merge cut coastline along sea level."}}},
              {"seed", {{"type", "integer"}}}}}}},
          {"erosion",
           {{"type", "object"},
            {"description", "Erosion over the whole map after the seams: gullies down the slopes (erosion filter), then Terrain's Erode preset "
                            "(stream power, debris, thermal, rills, deposition) so ridges and valleys drain. Sources take their erosion share; the sea "
                            "is held. Off by default."},
            {"properties",
             {{"preset", {{"enum", {"natural", "alpine", "desert", "hills"}}, {"description", "Erode preset. Default natural."}}},
              {"age", {{"type", "number"}, {"description", "0 off, 1 very old."}}},
              {"form", {{"type", "number"}, {"description", "Landforms wider than this stay the sources' own, cm. Default 30000."}}},
              {"detail", {{"type", "number"}, {"description", "Gully depth laid before erosion, fraction of their spacing on a 45 degree slope; 0 none."}}},
              {"detailScale", {{"type", "number"}, {"description", "Spacing of the largest gullies, cm. Default 12000."}}},
              {"seed", {{"type", "integer"}}}}}}},
          {"bathymetry",
           {{"type", "object"},
            {"description", "Sea floor where no source covers and where a source's elevation leaves the sea flat: the sources' bathymetry grids "
                            "blended in from a shelf profile (Terrain's bathymetry). Lengths cm on the map."},
            {"properties",
             {{"enabled", {{"type", "boolean"}, {"description", "Default true; false lays a flat floor at map.seaDepth."}}},
              {"shoreSlope", {{"type", "number"}, {"description", "Seabed slope off the shore, degrees. Default 1.5."}}},
              {"blendWidth", {{"type", "number"}, {"description", "Distance from the shore over which the profile hands over to the grids. Default 30000."}}},
              {"smoothing", {{"type", "number"}, {"description", "Blur of the grids, cm; 0 is a grid cell."}}},
              {"depthScale", {{"type", "number"}, {"description", "Multiplies the grids' depths (after the vertical scale)."}}},
              {"shelfDepth", {{"type", "number"}, {"description", "Profile shelf depth. Default 3000."}}},
              {"shelfWidth", {{"type", "number"}, {"description", "Profile shelf width. Default 200000."}}},
              {"dropDepth", {{"type", "number"}, {"description", "Depth past the shelf. Default 20000."}}},
              {"slopeWidth", {{"type", "number"}, {"description", "Width of the drop past the shelf. Default 300000."}}},
              {"minDepth", {{"type", "number"}, {"description", "Shallowest seabed off the shore. Default 30."}}},
              {"landTolerance", {{"type", "number"}, {"description", "Ground this close above sea level and joined to the sea counts as flat sea. Default 50."}}}}}}},
          {"view", {{"type", "object"}, {"description", "UI state: {overlay: true, show: after|before}."}}}}},
    };
}

// ---------------------------------------------------------------------------------------------------- polygons

bool inside(const Polygon& poly, Vec2 p)
{
    bool in = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

double distanceTo(const Polygon& poly, Vec2 p)
{
    double best = 1e300;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        const Vec2 a = poly[j], ab = poly[i] - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        const Vec2 d = a + ab * t - p;
        best = std::min(best, glm::dot(d, d));
    }
    return std::sqrt(best);
}

double signedDistance(const Polygon& poly, Vec2 p)
{
    const double d = distanceTo(poly, p);
    return inside(poly, p) ? d : -d;
}

double area(const Polygon& poly)
{
    double a = 0.0;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
        a += poly[j].x * poly[i].y - poly[i].x * poly[j].y;
    return a * 0.5;
}

Vec2 centroid(const Polygon& poly)
{
    double a = 0.0;
    Vec2 c(0.0);
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        const double cross = poly[j].x * poly[i].y - poly[i].x * poly[j].y;
        a += cross;
        c += (poly[j] + poly[i]) * cross;
    }
    if (std::abs(a) < 1e-12)
    {
        Vec2 m(0.0);
        for (Vec2 p : poly)
            m += p;
        return poly.empty() ? m : m / double(poly.size());
    }
    return c / (3.0 * a);
}

namespace
{
bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    auto orient = [](Vec2 p, Vec2 q, Vec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
    const double o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
    return ((o1 > 0) != (o2 > 0)) && ((o3 > 0) != (o4 > 0));
}
} // namespace

bool overlaps(const Polygon& a, const Polygon& b)
{
    if (a.size() < 3 || b.size() < 3)
        return false;
    for (size_t i = 0, pi = a.size() - 1; i < a.size(); pi = i++)
        for (size_t k = 0, pk = b.size() - 1; k < b.size(); pk = k++)
            if (segmentsCross(a[pi], a[i], b[pk], b[k]))
                return true;
    return inside(a, b[0]) || inside(b, a[0]);
}

Polygon transformed(const Polygon& poly, const Rigid& r)
{
    Polygon out;
    out.reserve(poly.size());
    for (Vec2 p : poly)
        out.push_back(r.apply(p));
    return out;
}
} // namespace openworld
