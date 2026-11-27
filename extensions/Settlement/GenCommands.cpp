// Settlement generator commands: settlementCreate's generator flags (resolved here into the generators input, reading
// curves and running Terrain commands by name), and settlementGenerators, settlementEras, settlementWalls (curves and
// Masonry walls) and settlementLevels.

#include "GenCommands.h"
#include "Settlement.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Names.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

using namespace lila;

namespace settlement
{
namespace
{
using scene::Graph;
using scene::Node;
using GenArg = ArgType;

/** The land inside a box as a settlement outline, and the water inside that land as exclusions. */
struct LandShape
{
    json outline;                        // empty when the box has no water: keep the box
    json exclusions = json::array();
};

/**
 * Shapes a settlement's outline to the land in a box: the box sampled on a grid (about 200 cells across, 20 to 100 m),
 * water cells (the water mask, or below sea level) found, and each row's land span (first to last dry cell) joined into
 * one outline, so the sea is outside it rather than one exclusion per strip (each exclusion makes Settlement route on
 * a finer grid). Water inside a row's span (a lake, a river) becomes exclusions, strips of equal span stacked.
 */
LandShape landShape(const std::string& terrain, Vec2 lo, Vec2 hi, const std::string& within = {})
{
    LandShape out;
    if (!ops::find("terrainSample"))
        return out;
    const Vec2 ext = hi - lo;
    const double cell = std::clamp(std::max(ext.x, ext.y) / 200.0, 2000.0, 10000.0);
    const int nx = std::max(1, int(std::ceil(ext.x / cell))), nz = std::max(1, int(std::ceil(ext.y / cell)));
    json pts = json::array();
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
            pts.push_back({lo.x + (x + 0.5) * cell, lo.y + (z + 0.5) * cell});
    double sea = -std::numeric_limits<double>::max();
    json samples;
    try
    {
        const json w = ops::execute("terrainWater", {{"args", {terrain}}});
        if (w.contains("features") && w["features"].contains("settings"))
            sea = w["features"]["settings"].value("seaLevel", sea);
        json masks = {"water"};
        if (!within.empty())
            masks.push_back(within);
        samples = ops::execute("terrainSample", {{"args", {terrain}}, {"points", pts}, {"masks", masks}});
    }
    catch (const std::exception&)
    {
        return out;
    }
    std::vector<char> wet(size_t(nx) * nz, 0), outside(size_t(nx) * nz, 0);
    bool any = false;
    for (size_t i = 0; i < samples.size() && i < wet.size(); ++i)
    {
        any = (wet[i] = samples[i].value("water", 0.0) > 0.5 || samples[i].value("height", 0.0) < sea) || any;
        if (!within.empty())
            any = (outside[i] = samples[i].value(within, 0.0) < 0.5) || any;
    }
    if (!any)
        return out;
    // A cell beside water counts as water: the shore cell is half wet and the shore itself too steep for streets.
    {
        const std::vector<char> was = wet;
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
                for (int dz = -1; dz <= 1 && !wet[size_t(z) * nx + x]; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = x + dx, zz = z + dz;
                        if (xx >= 0 && zz >= 0 && xx < nx && zz < nz && was[size_t(zz) * nx + xx])
                        {
                            wet[size_t(z) * nx + x] = 1;
                            break;
                        }
                    }
        for (size_t i = 0; i < wet.size(); ++i)
            wet[i] = wet[i] || outside[i];
    }
    // Each row's land span; rows with no land are left out of the outline.
    std::vector<std::pair<int, int>> span(size_t(nz), {-1, -1});
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
            if (!wet[size_t(z) * nx + x])
            {
                if (span[z].first < 0)
                    span[z].first = x;
                span[z].second = x;
            }
    json left = json::array(), right = json::array();
    for (int z = 0; z < nz; ++z)
        if (span[z].first >= 0)
        {
            const double z0 = lo.y + z * cell, z1 = lo.y + (z + 1) * cell;
            const double xl = lo.x + span[z].first * cell, xr = lo.x + (span[z].second + 1) * cell;
            left.push_back({xl, z0}), left.push_back({xl, z1});
            right.push_back({xr, z0}), right.push_back({xr, z1});
        }
    if (left.empty())
        return out;
    out.outline = left;
    for (auto it = right.rbegin(); it != right.rend(); ++it)
        out.outline.push_back(*it);
    // Water inside the land spans: runs per row, stacked when consecutive rows share a span.
    struct Run
    {
        int x0, x1, z0, z1;
    };
    std::vector<Run> open, done;
    for (int z = 0; z < nz; ++z)
    {
        std::vector<Run> row;
        for (int x = std::max(0, span[z].first); span[z].first >= 0 && x <= span[z].second;)
        {
            if (!wet[size_t(z) * nx + x])
            {
                ++x;
                continue;
            }
            int e = x;
            while (e + 1 <= span[z].second && wet[size_t(z) * nx + e + 1])
                ++e;
            row.push_back({x, e, z, z});
            x = e + 1;
        }
        std::vector<Run> next;
        for (Run r : row)
        {
            auto it = std::find_if(open.begin(), open.end(), [&](const Run& o) { return o.x0 == r.x0 && o.x1 == r.x1; });
            if (it != open.end())
            {
                r.z0 = it->z0;
                open.erase(it);
            }
            next.push_back(r);
        }
        done.insert(done.end(), open.begin(), open.end());
        open = std::move(next);
    }
    done.insert(done.end(), open.begin(), open.end());
    for (const Run& r : done)
    {
        const double x0 = lo.x + r.x0 * cell, x1 = lo.x + (r.x1 + 1) * cell, z0 = lo.y + r.z0 * cell, z1 = lo.y + (r.z1 + 1) * cell;
        out.exclusions.push_back({{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}});
    }
    return out;
}

/** A flag's value as JSON: text that parses as a JSON object or list is read as one. */
json jsonFlag(const Args& a, const char* name)
{
    json v = a.flags[name];
    if (v.is_string())
    {
        const std::string s = v.get<std::string>();
        if (!s.empty() && (s.front() == '{' || s.front() == '['))
        {
            json parsed = json::parse(s, nullptr, false);
            if (parsed.is_discarded())
                throw CommandError(std::string("settlement: ") + name + " isn't valid JSON");
            return parsed;
        }
    }
    return v;
}

std::string pathOf(const json& made) { return made.is_array() ? made.at(0).get<std::string>() : made.get<std::string>(); }

/** The splines of a curve object in world space, flattened to XZ. */
std::vector<std::vector<Vec2>> curveLines(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": '" + name + "' is not a curve");
    g.evaluate();
    std::vector<std::vector<Vec2>> out;
    for (const curve::Spline& s : curve::splinesOf(n->value(curve::CurveShape::kWorldSpace)))
    {
        std::vector<Vec2> pts;
        if (curve::isValid(s) && s.size() >= 2)
        {
            const curve::Polyline line = curve::tessellate(s);
            for (const glm::dvec3& p : line.points)
                pts.push_back({p.x, p.z});
            if (line.closed && !pts.empty())
                pts.push_back(pts.front());
        }
        out.push_back(std::move(pts));
    }
    return out;
}

/** Streets from a curve terrainImportOsm made: each spline's OSM class from its osmFeatures attribute. */
json osmCurveStreets(Graph& g, const std::string& name, const char* cmd)
{
    const std::vector<std::vector<Vec2>> lines = curveLines(g, name, cmd);
    json info = json::array();
    std::string kind;
    try
    {
        const json v = ops::execute("getAttr", {{"args", {g.find(name)->path() + ".osmFeatures"}}});
        if (v.is_string())
            info = json::parse(v.get<std::string>(), nullptr, false);
    }
    catch (const std::exception&)
    {
    }
    try
    {
        const json v = ops::execute("getAttr", {{"args", {g.find(name)->path() + ".terrainFeature"}}});
        if (v.is_string())
            kind = v.get<std::string>();
    }
    catch (const std::exception&)
    {
    }
    json out = json::array();
    for (size_t k = 0; k < lines.size(); ++k)
    {
        if (lines[k].size() < 2)
            continue;
        std::string cls = kind == "path" ? "path" : "local";
        std::string label, id;
        if (info.is_array() && k < info.size() && info[k].is_object())
        {
            cls = info[k].value("class", cls);
            label = info[k].value("name", std::string());
            id = info[k].contains("id") ? (info[k]["id"].is_string() ? info[k]["id"].get<std::string>() : info[k]["id"].dump()) : std::string();
        }
        json pts = json::array();
        for (const Vec2& p : lines[k])
            pts.push_back({p.x, p.y});
        out.push_back({{"points", pts}, {"class", cls}, {"name", label}, {"id", id}});
    }
    return out;
}

std::string curveAttr(Graph& g, const std::string& name, const char* attr)
{
    try
    {
        const json v = ops::execute("getAttr", {{"args", {g.find(name)->path() + "." + attr}}});
        if (v.is_string())
            return v.get<std::string>();
    }
    catch (const std::exception&)
    {
    }
    return {};
}

/** The data's own centre: its city point inside the outline, else a town's, else none. */
bool osmCentre(Graph& g, const std::string& placeCurve, const json& outline, Vec2* at)
{
    std::vector<Vec2> ring;
    for (const json& p : outline)
        ring.push_back({p[0].get<double>(), p[1].get<double>()});
    const json points = json::parse(curveAttr(g, placeCurve, "osmPlaces"), nullptr, false);
    if (!points.is_array() || ring.size() < 3)
        return false;
    int best = 0;
    for (const json& i : points)
    {
        if (!i.is_object() || i.value("kind", std::string()) != "town" || !i.contains("at") || !i["at"].is_array() || i["at"].size() < 2)
            continue;
        const json& p = i["at"];
        const Vec2 q(p[0].get<double>(), p[p.size() >= 3 ? 2 : 1].get<double>());
        const int rank = i.value("value", std::string()) == "city" ? 2 : 1;
        if (rank > best && contains(ring, q))
        {
            best = rank;
            *at = q;
        }
    }
    return best > 0;
}

json xzRing(const std::vector<Vec2>& pts)
{
    json out = json::array();
    const bool repeated = pts.size() > 2 && glm::length(pts.back() - pts.front()) < 1e-6;
    for (size_t k = 0; k + (repeated ? 1 : 0) < pts.size(); ++k)
        out.push_back({pts[k].x, pts[k].y});
    return out;
}

/** Buildings from a building curve terrainImportOsm made: outer rings with their holes and what osmFeatures says of each. */
json osmCurveBuildings(Graph& g, const std::string& name, const char* cmd)
{
    const std::vector<std::vector<Vec2>> lines = curveLines(g, name, cmd);
    const json info = json::parse(curveAttr(g, name, "osmFeatures"), nullptr, false);
    json out = json::array();
    if (!info.is_array())
        return out;
    std::map<size_t, size_t> outerAt; // spline -> index in out
    for (size_t k = 0; k < lines.size() && k < info.size(); ++k)
    {
        const json& i = info[k];
        if (!i.is_object() || lines[k].size() < 3)
            continue;
        if (i.value("ring", std::string("outer")) == "inner")
        {
            if (auto it = outerAt.find(i.value("outer", size_t(0))); it != outerAt.end())
                out[it->second]["holes"].push_back(xzRing(lines[k]));
            continue;
        }
        json b = {{"id", i.value("id", std::to_string(k))}, {"outer", xzRing(lines[k])}, {"holes", json::array()}};
        for (const char* key : {"part", "building", "program", "kind", "name", "height", "minHeight", "roofHeight", "levels", "minLevel",
                                "roofLevels", "roofShape", "colour", "roofColour", "material", "roofMaterial", "startDate"})
            if (i.contains(key) && !i[key].is_null())
                b[key] = i[key];
        outerAt[k] = out.size();
        out.push_back(std::move(b));
    }
    return out;
}

/** Places from a place curve terrainImportOsm made: areas from its splines, points from its osmPlaces. */
json osmCurvePlaces(Graph& g, const std::string& name, const char* cmd)
{
    json out = json::array();
    const json points = json::parse(curveAttr(g, name, "osmPlaces"), nullptr, false);
    auto place = [](const json& i) {
        json p = json::object();
        for (const char* key : {"id", "key", "value", "program", "kind", "name"})
            if (i.contains(key) && i[key].is_string())
                p[key] = i[key];
        return p;
    };
    if (points.is_array())
        for (const json& i : points)
            if (i.is_object() && i.contains("at"))
            {
                json p = place(i);
                p["at"] = i["at"];
                out.push_back(std::move(p));
            }
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        return out;
    const std::vector<std::vector<Vec2>> lines = curveLines(g, name, cmd);
    const json info = json::parse(curveAttr(g, name, "osmFeatures"), nullptr, false);
    if (!info.is_array())
        return out;
    std::map<std::string, size_t> byId;
    for (size_t k = 0; k < lines.size() && k < info.size(); ++k)
    {
        if (!info[k].is_object() || lines[k].size() < 3)
            continue;
        const std::string id = info[k].value("id", std::to_string(k));
        auto it = byId.find(id);
        if (it == byId.end())
        {
            json p = place(info[k]);
            p["rings"] = json::array();
            it = byId.emplace(id, out.size()).first;
            out.push_back(std::move(p));
        }
        out[it->second]["rings"].push_back(xzRing(lines[k]));
    }
    return out;
}

/** The real names in the scene as hashes, for the generators' avoid list. */
json avoidHashes(Graph& g) { return json::parse(lila::names::sceneRealNames(g).hashes().toJson()); }

const CommandDef* needCommand(const char* name, const char* extension, const char* cmd)
{
    const CommandDef* def = ops::find(name);
    if (!def)
        throw CommandError(std::string(cmd) + ": " + name + " is not available; load the " + extension + " extension");
    return def;
}

json xyz(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

json pointsJson(std::span<const glm::dvec3> pts)
{
    json out = json::array();
    for (const glm::dvec3& p : pts)
        out.push_back(xyz(p));
    return out;
}

/** The closed wall line draped on the ground, densified so it follows the slope. */
std::vector<glm::dvec3> drapedRing(const std::vector<Vec2>& ring, const Ground& g, double step)
{
    std::vector<glm::dvec3> out;
    for (size_t k = 0; k < ring.size(); ++k)
    {
        const Vec2 a = ring[k], b = ring[(k + 1) % ring.size()];
        const int n = std::max(1, int(std::ceil(glm::length(b - a) / step)));
        for (int s = 0; s < n; ++s)
        {
            const Vec2 q = a + (b - a) * (double(s) / n);
            out.push_back({q.x, g.height(q), q.y});
        }
    }
    return out;
}

json eraJson(const Rebuilt& r)
{
    const Layout& l = r.layout;
    json eras = json::array();
    if (!l.gen)
        return eras;
    const GenLayout& gen = *l.gen;
    for (size_t k = 0; k < gen.eras.size(); ++k)
    {
        double len = 0.0;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
            if (l.graph.isStreet(int(e)) && l.graph.edges[e].era == int(k))
                len += l.graph.length(int(e));
        const auto lots = std::count_if(l.lots.begin(), l.lots.end(), [&](const Lot& x) { return x.era == int(k); });
        const auto envs = std::count_if(l.envelopes.begin(), l.envelopes.end(), [&](const Envelope& x) { return x.era == int(k); });
        int walls = 0;
        for (const WallLine& w : gen.walls)
            walls += w.era == int(k) ? 1 : 0;
        eras.push_back({{"era", int(k)},
                        {"name", gen.eras[k].name},
                        {"profile", gen.eras[k].profile.name},
                        {"pattern", patternName(gen.eras[k].profile.pattern)},
                        {"roof", gen.eras[k].profile.roof},
                        {"streetKm", len / 1.0e5},
                        {"lots", int64_t(lots)},
                        {"envelopes", int64_t(envs)},
                        {"walls", walls}});
    }
    return eras;
}

json genSummary(const Rebuilt& r)
{
    const Layout& l = r.layout;
    json out = {{"generators", r.options.generators}, {"targets", targetReport(l)}};
    if (!l.gen)
        return out;
    const GenLayout& gen = *l.gen;
    int standing = 0;
    for (const WallLine& w : gen.walls)
        standing += w.demolished < 0 ? 1 : 0;
    out["eras"] = eraJson(r);
    out["districts"] = gen.districts;
    out["walls"] = {{"standing", standing}, {"demolished", int(gen.walls.size()) - standing}, {"gates", gen.gates.size()}, {"towers", gen.towers.size()}};
    out["imported"] = gen.imported;
    out["demolishedStreets"] = gen.demolishedStreets;
    out["levels"] = gen.levels.size();
    out["ramps"] = gen.ramps.size();
    out["airLanes"] = gen.air.links.size();
    out["megablocks"] = gen.megablocks.size();
    out["warnings"] = gen.warnings;
    out["statistics"] = toJson(streetStats(l.graph, l.blocks));
    if (hasOsmBuildings(l))
        out["osm"] = {{"buildingLots", gen.osmLots}, {"fillLots", gen.osmFillLots}, {"denseBlocks", gen.osmDenseBlocks},
                      {"sparseBlocks", gen.osmSparseBlocks}, {"outside", gen.osmOutside}, {"overlapping", gen.osmOverlapping}, {"parts", gen.osmParts}, {"sites", osmSitesJson(l)}};
    return out;
}
} // namespace

std::vector<FlagDef> generatorFlags()
{
    return {
        {"tensor", "tn", GenArg::Any, nullptr, "Tensor field for pattern tensor: {fields: [{kind: grid|radial|boundary|height, angle, center, "
                                          "weight, decay, lines, follow: outline|water}], paint: [{points, radius, angle, weight}], "
                                          "mask, majorSpacing, minorSpacing}."},
        {"eras", "er", GenArg::Any, nullptr, "Eras in order, each {name, profile, pattern, overrides, growth, inner, outer, polygon, walls, "
                                        "demolishWalls, ring, axes, highwayRing, highwaySpurs}; or a preset: walledTown, "
                                        "medievalToModern, gridToSprawl."},
        {"walls", "wl", GenArg::Any, nullptr, "Walls round the core: True, or {radius, polygon, contour, thickness, height, towerSpacing, "
                                         "starFort, bastions, moat, moatGap}."},
        {"districts", "di", GenArg::List, nullptr, "Districts, each {name, polygon, profile, pattern, overrides, growth, era, seed, eras}: "
                                              "their own generator inside the polygon."},
        {"osm", "os", GenArg::List, nullptr, "Imported streets: curves made by terrainImportOsm (by name), or {points, class} with an OSM "
                                        "highway value or a street class."},
        {"osmFile", "of", GenArg::String, nullptr, "OSM file (.osm, .json, .osm.pbf) read through terrainImportOsm onto the ground."},
        {"osmOrigin", "oo", GenArg::List, nullptr, "[latitude, longitude] of the ground's centre, for osmFile on a terrain not from real data."},
        {"osmBuildings", "obl", GenArg::Bool, true, "With osmFile: buildings and places come in too, as lots, envelopes, businesses and landmarks."},
        {"osmFill", "ofl", GenArg::String, "sparse", "Lots Settlement cuts beside imported buildings: sparse (in blocks they cover less than "
                                                     "osmCoverage of), all, or none.", {"sparse", "all", "none"}},
        {"osmCoverage", "ocv", GenArg::Float, 0.25, "Share of a block imported buildings must cover for it to count as dense (no lots of Settlement's own).", {}, {}, 0.0, 1.0},
        {"osmCulture", "ocu", GenArg::String, "", "Naming culture for every imported name (namingCultures); empty chooses one from where the data lies."},
        {"example", "eg", GenArg::Any, nullptr, "Grow like another street network: a settlement by name, or {orientation, degree, "
                                           "blockAreaMedianM2, meanSegment} (settlementGenerators statistics), or {streets}."},
        {"contourStreets", "cst", GenArg::Bool, false, "Streets along the ground's contours from terrainContourStreets (the ground must be a Terrain)."},
        {"contourSpacing", "csp", GenArg::Float, 3000.0, "Between contour streets across the slope, cm.", {}, {}, 100.0},
        {"levels", "lv", GenArg::Any, nullptr, "Levels above the ground: {decks: [{name, polygon, height, pattern, ramps}], elevated: "
                                          "[{points, height, class, ramps}], skybridgeSpan, skybridgeHeight, airHeights, airSpacing, megablock}."},
    };
}

void resolveGeneratorFlags(Graph& g, const Args& a, json& values, std::span<const Vec2> region, const std::string& ground, const char* cmd)
{
    LILA_ZONE();

    json gen = json::object();
    if (values.contains("generators") && values["generators"].is_string() && !values["generators"].get<std::string>().empty())
    {
        gen = json::parse(values["generators"].get<std::string>(), nullptr, false);
        if (gen.is_discarded() || !gen.is_object())
            throw CommandError(std::string(cmd) + ": generators isn't a JSON object");
    }
    std::string pattern;
    for (const char* key : {"tensor", "eras", "walls", "districts", "levels"})
        if (a.has(key))
            gen[key] = jsonFlag(a, key);
    if (a.has("tensor"))
        pattern = "tensor";
    if (a.has("osm") || a.has("osmFile"))
    {
        json streets = json::array(), buildings = json::array(), places = json::array();
        auto takeCurve = [&](const std::string& name) {
            const std::string kind = curveAttr(g, name, "terrainFeature");
            if (kind == "building")
                for (json& b : osmCurveBuildings(g, name, cmd))
                    buildings.push_back(std::move(b));
            else if (kind == "place")
                for (json& p : osmCurvePlaces(g, name, cmd))
                    places.push_back(std::move(p));
            else
                for (const json& s : osmCurveStreets(g, name, cmd))
                    streets.push_back(s);
        };
        for (const json& item : a.has("osm") ? a.flags["osm"] : json::array())
        {
            if (item.is_string())
                takeCurve(item.get<std::string>());
            else
                streets.push_back(item);
        }
        std::string culture = a.get<std::string>("osmCulture", "");
        if (a.has("osmFile"))
        {
            needCommand("terrainImportOsm", "Terrain", cmd);
            if (ground.empty())
                throw CommandError(std::string(cmd) + ": osmFile needs ground= (the terrain the OSM data lies on)");
            json kinds = {"road", "path"};
            if (a.get<bool>("osmBuildings", true))
                kinds = {"road", "path", "building", "place"};
            json call = {{"args", {a.get<std::string>("osmFile")}}, {"terrain", ground}, {"kinds", kinds}, {"landcover", false}};
            if (a.has("osmOrigin"))
                call["origin"] = a.flags["osmOrigin"];
            if (!culture.empty())
                call["culture"] = culture;
            const json made = ops::execute("terrainImportOsm", call);
            culture = made.value("culture", culture);
            const json curves = made.value("curves", json::object());
            for (const char* kind : {"road", "path", "building", "place"})
                if (curves.contains(kind) && curves[kind].is_string())
                    takeCurve(curves[kind].get<std::string>());
            if (streets.empty())
                throw CommandError(std::string(cmd) + ": " + a.get<std::string>("osmFile") + " has no roads or paths on the ground");
        }
        gen["osm"] = streets;
        if (!buildings.empty())
            gen["osmBuildings"] = buildings;
        else
            gen.erase("osmBuildings");
        if (!places.empty())
            gen["osmPlaces"] = places;
        else
            gen.erase("osmPlaces");
        gen["osmFill"] = {{"mode", a.get<std::string>("osmFill", "sparse")}, {"coverage", a.get<double>("osmCoverage", 0.25)}};
        // Settlement's own street and district names steer clear of the real ones the import replaced.
        gen["avoid"] = avoidHashes(g);
        // The settlement names its streets in the import's culture unless it has one of its own.
        if (!culture.empty() && values.contains("edits"))
        {
            json edits = json::object();
            if (values["edits"].is_string() && !values["edits"].get<std::string>().empty())
                edits = json::parse(values["edits"].get<std::string>(), nullptr, false);
            if (edits.is_object() && edits.value("naming", json::object()).value("culture", std::string()).empty())
            {
                edits["naming"]["culture"] = culture;
                values["edits"] = edits.dump();
            }
        }
        if (pattern.empty())
            pattern = "osm";
    }
    else if (a.has("osmFill") || a.has("osmCoverage"))
        gen["osmFill"] = {{"mode", a.get<std::string>("osmFill", "sparse")}, {"coverage", a.get<double>("osmCoverage", 0.25)}};
    if (a.has("example"))
    {
        const json ex = jsonFlag(a, "example");
        if (ex.is_string())
        {
            const Rebuilt other = rebuildSettlement(g, ex.get<std::string>(), cmd);
            gen["example"] = toJson(streetStats(other.layout.graph, other.layout.blocks));
        }
        else
            gen["example"] = ex;
        if (pattern.empty())
            pattern = "example";
    }
    if (a.get<bool>("contourStreets", false))
    {
        needCommand("terrainContourStreets", "Terrain", cmd);
        if (ground.empty())
            throw CommandError(std::string(cmd) + ": contourStreets needs ground= (a Terrain)");
        json ring = json::array();
        for (const Vec2& q : region)
            ring.push_back({q.x, 0.0, q.y});
        const json made = ops::execute("terrainContourStreets", {{"args", {ground}},
                                                                 {"region", ring},
                                                                 {"spacing", a.get<double>("contourSpacing", 3000.0)},
                                                                 {"maxGrade", 100.0 * std::max(0.01, 0.12)}});
        json streets = json::array();
        for (const json& s : made.value("streets", json::array()))
            streets.push_back({{"points", s.at("points")}, {"kind", s.value("kind", std::string("contour"))}});
        gen["contour"] = streets;
        if (pattern.empty())
            pattern = "contour";
    }
    try
    {
        parseGenerators(gen);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    values["generators"] = gen.empty() ? std::string() : gen.dump();
    if (!pattern.empty() && (!values.contains("pattern") || !values["pattern"].is_string() || values["pattern"].get<std::string>().empty()))
        values["pattern"] = pattern;
}

void registerGeneratorCommands(ext::Context& ctx)
{
    std::vector<FlagDef> genFlags = generatorFlags();
    genFlags.push_back({"clear", "cl", GenArg::Bool, false, "Remove every generator setting first."});
    genFlags.push_back({"pattern", "pt", GenArg::String, nullptr, "Set the pattern too (organic, grid, radial, linear, organicGrid, none, tensor, contour, osm, example)."});

    ctx.registerCommand({
        .name = "settlementGenerators",
        .doc = "Reads or changes a settlement's generator settings: tensor fields, eras, districts, walls, imported OSM streets, "
               "example statistics, contour streets and levels (the same flags as settlementCreate), stored in its generators "
               "input. Only the flags given change; clear=True starts over. Returns the generator settings, eras (name, profile, "
               "pattern, street km, lots, envelopes, walls), districts, walls (standing, demolished, gates, towers), levels, "
               "ramps, air lanes, megablocks, the street statistics (orientation histogram, degrees, block size: what example= "
               "reads), the profile's metric targets and whether they are met, and warnings for fallbacks taken.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = genFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementGenerators";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            Rebuilt r = rebuildSettlement(g, names[0], cmd);
            bool changed = a.get<bool>("clear", false) || a.has("pattern");
            for (const FlagDef& f : generatorFlags())
                changed = changed || (a.has(f.name) && f.name != "contourSpacing" && f.name != "osmOrigin" && !(f.name == "contourStreets" && !a.get<bool>(f.name)));
            if (changed)
            {
                json values = {{"generators", a.get<bool>("clear", false) ? json(std::string()) : r.values.value("generators", json(std::string()))},
                               {"pattern", a.has("pattern") ? a.flags["pattern"] : r.values.value("pattern", json(std::string()))}};
                resolveGeneratorFlags(g, a, values, r.layout.outline.pts, r.values.value("ground", std::string()), cmd);
                ops::execute("setAttr", {{"args", {r.modifier + ".generators", values["generators"]}}});
                ops::execute("setAttr", {{"args", {r.modifier + ".pattern", values["pattern"]}}});
                r = rebuildSettlement(g, names[0], cmd);
            }
            json out = genSummary(r);
            out["mesh"] = r.mesh;
            out["modifier"] = r.modifier;
            return out;
        },
        .examples = {"settlementGenerators('Settlement', eras='medievalToModern')['eras']",
                     "settlementGenerators('Settlement', pattern='tensor', tensor={'fields': [{'kind': 'grid', 'angle': 30}, "
                     "{'kind': 'radial', 'center': [0, 0], 'decay': 30000}]})",
                     "settlementGenerators('Settlement')['targets']['ok']"},
    });

    ctx.registerCommand({
        .name = "settlementEras",
        .doc = "A settlement's eras in order: name, profile, pattern, roof, street km, lots and envelopes laid in each era, walls "
               "built in it; and its districts. Every street, lot and envelope carries its era (settlementExport writes it on "
               "streets, lots and envelopes). Changes nothing.",
        .positionalDoc = "settlement mesh or its modifier",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "settlementEras";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(Graph::current(), names[0], cmd);
            return {{"eras", eraJson(r)}, {"districts", r.layout.gen ? json(r.layout.gen->districts) : json::array()}};
        },
        .examples = {"[e['name'] for e in settlementEras('Settlement')['eras']]"},
    });

    ctx.registerCommand({
        .name = "settlementWalls",
        .doc = "A settlement's fortifications: each wall line (points [x, y, z] on the ground, era, the era it was demolished in or "
               "-1, thickness, height, star fort), its gates (point, distance along the wall, road width and class) and towers "
               "(point, gate or bastion). curves=True makes a closed curve per standing wall; masonry=True builds each standing "
               "wall with Masonry's masonryWall (gates cut as arched openings, towers as round walls with towers=True). Returns "
               "the walls and what was made.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"curves", "cv", GenArg::Bool, false, "Make a closed curve per standing wall."},
                  {"masonry", "ms", GenArg::Bool, false, "Build the standing walls with masonryWall (needs the Masonry extension)."},
                  {"towers", "tw", GenArg::Bool, false, "With masonry: a round wall per tower."},
                  {"height", "h", GenArg::Float, nullptr, "Wall height for Masonry, cm; default the wall's.", {}, {}, 10.0},
                  {"thickness", "th", GenArg::Float, nullptr, "Wall thickness for Masonry, cm; default the wall's.", {}, {}, 10.0},
                  {"gateHeight", "gh", GenArg::Float, 500.0, "Height of the gate openings, cm.", {}, {}, 50.0},
                  {"masonryFlags", "mf", GenArg::Any, nullptr, "More masonryWall flags for every wall and tower, such as {'stoneLength': 60, 'bond': 'flemish'}."},
                  {"name", "n", GenArg::String, "Wall", "Name of the curves and walls made."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementWalls";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(g, names[0], cmd);
            const Layout& l = r.layout;
            const bool masonry = a.get<bool>("masonry", false);
            if (masonry)
                needCommand("masonryWall", "Masonry", cmd);
            const std::string base = a.get<std::string>("name", "Wall");
            json walls = json::array(), made = json::array();
            if (!l.gen)
                return {{"walls", walls}};
            const GenLayout& gen = *l.gen;
            for (size_t wi = 0; wi < gen.walls.size(); ++wi)
            {
                const WallLine& w = gen.walls[wi];
                const std::vector<glm::dvec3> ring = drapedRing(w.pts, r.ground, 500.0);
                json gates = json::array(), towers = json::array();
                for (const Gate& gt : gen.gates)
                    if (gt.wall == int(wi))
                        gates.push_back({{"point", xyz({gt.p.x, r.ground.height(gt.p), gt.p.y})},
                                         {"at", gt.at},
                                         {"width", gt.width},
                                         {"class", className(gt.cls)}});
                for (const Tower& t : gen.towers)
                    if (t.wall == int(wi))
                        towers.push_back({{"point", xyz({t.p.x, r.ground.height(t.p), t.p.y})}, {"gate", t.gate}, {"bastion", t.bastion}});
                json entry = {{"points", pointsJson(ring)}, {"era", w.era},   {"demolished", w.demolished}, {"thickness", w.thickness},
                              {"height", w.height},         {"starFort", w.starFort}, {"gates", gates},     {"towers", towers}};
                if (w.demolished < 0 && a.get<bool>("curves", false))
                {
                    const json c = ops::execute("curve", {{"splines", json::array({{{"type", "poly"}, {"closed", true}, {"points", pointsJson(ring)}}})},
                                                          {"name", base + std::to_string(wi)}});
                    entry["curve"] = pathOf(c);
                    made.push_back(entry["curve"]);
                }
                if (w.demolished < 0 && masonry)
                {
                    const double height = a.get<double>("height", w.height), thickness = a.get<double>("thickness", w.thickness);
                    json openings = json::array();
                    for (const json& gt : gates)
                        openings.push_back({{"at", gt["at"]}, {"width", gt["width"].get<double>() + 100.0},
                                            {"height", std::min(a.get<double>("gateHeight", 500.0), 0.9 * height)}, {"arch", "round"}});
                    json call = {{"points", pointsJson(ring)}, {"closed", true}, {"height", height}, {"thickness", thickness},
                                 {"name", base + "Masonry" + std::to_string(wi)}};
                    if (!openings.empty())
                        call["openings"] = openings.dump();
                    const json extra = a.has("masonryFlags") ? jsonFlag(a, "masonryFlags") : json();
                    if (extra.is_object())
                        for (auto& [k, v] : extra.items())
                            call[k] = v;
                    const json m = ops::execute("masonryWall", call);
                    entry["masonry"] = m.value("mesh", json());
                    made.push_back(entry["masonry"]);
                    if (a.get<bool>("towers", false))
                        for (const Tower& t : gen.towers)
                        {
                            if (t.wall != int(wi))
                                continue;
                            const double rad = std::max(w.thickness * 1.5, 300.0);
                            json pts = json::array();
                            for (int s = 0; s < 16; ++s)
                            {
                                const Vec2 q = t.p + Vec2(std::cos(2.0 * kPi * s / 16), std::sin(2.0 * kPi * s / 16)) * rad;
                                pts.push_back({q.x, r.ground.height(q), q.y});
                            }
                            json tc = {{"points", pts}, {"closed", true}, {"height", height * 1.3}, {"thickness", thickness}, {"name", base + "Tower"}};
                            if (extra.is_object())
                                for (auto& [k, v] : extra.items())
                                    tc[k] = v;
                            const json tm = ops::execute("masonryWall", tc);
                            made.push_back(tm.value("mesh", json()));
                        }
                }
                walls.push_back(std::move(entry));
            }
            return {{"walls", walls}, {"made", made}};
        },
        .examples = {"settlementCreate(profile='medievalTown', size=80000, walls={'moat': 1200, 'towerSpacing': 3000})\n"
                     "w = settlementWalls('Settlement', curves=True)['walls'][0]\nlen(w['gates'])",
                     "settlementWalls('Settlement', masonry=True, height=1000, thickness=250)"},
    });

    ctx.registerCommand({
        .name = "settlementLevels",
        .doc = "A settlement's networks above the ground: each level (deck, elevated road, skybridges) with its layer, height, "
               "street count and junctions from its own road network; ramps between levels and the ground; air-traffic lanes "
               "(altitudes, nodes, one-way links, whether every node reaches every other); megablock footprints. Streets on "
               "different layers never meet except through ramps. curves=True makes a curve per kind (deck, elevated, ramp, "
               "skybridge, air) with every line at its height.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"curves", "cv", GenArg::Bool, false, "Make curves for the lines above the ground."},
                  {"name", "n", GenArg::String, "Levels", "Prefix of the curves made."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementLevels";
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(std::string(cmd) + ": give one settlement");
            const Rebuilt r = rebuildSettlement(g, names[0], cmd);
            const Layout& l = r.layout;
            json levels = json::array(), ramps = json::array(), megas = json::array(), curves = json::object();
            if (!l.gen)
                return {{"levels", levels}};
            const GenLayout& gen = *l.gen;
            for (size_t k = 0; k < gen.levels.size(); ++k)
            {
                const Level& lv = gen.levels[k];
                int streets = 0;
                for (size_t e = 0; e < lv.graph.edges.size(); ++e)
                    streets += lv.graph.isStreet(int(e)) ? 1 : 0;
                const RoadNetwork net = levelNetwork(l, int(k));
                levels.push_back({{"name", lv.name}, {"kind", lv.kind}, {"layer", int(k) + 1}, {"height", lv.height},
                                  {"streets", streets}, {"junctions", net.junctions.size()}, {"lines", lv.lines3.size()}});
            }
            for (const Ramp& rp : gen.ramps)
                ramps.push_back({{"points", pointsJson(rp.pts)}, {"from", rp.from}, {"to", rp.to}, {"class", className(rp.cls)}});
            for (const Megablock& m : gen.megablocks)
                megas.push_back({{"block", m.block}, {"area", area(m.footprint) / kM2}, {"base", m.base}, {"top", m.top}});
            // Every air node reaching every other: forward and backward search from the first.
            bool connected = true;
            if (!gen.air.nodes.empty())
            {
                for (int dir = 0; dir < 2 && connected; ++dir)
                {
                    std::vector<std::vector<int>> adj(gen.air.nodes.size());
                    for (const auto& [x, y] : gen.air.links)
                        adj[size_t(dir ? y : x)].push_back(dir ? x : y);
                    std::vector<char> seen(adj.size(), 0);
                    std::vector<int> stack = {0};
                    seen[0] = 1;
                    while (!stack.empty())
                    {
                        const int n = stack.back();
                        stack.pop_back();
                        for (int m : adj[size_t(n)])
                            if (!seen[size_t(m)])
                            {
                                seen[size_t(m)] = 1;
                                stack.push_back(m);
                            }
                    }
                    connected = std::all_of(seen.begin(), seen.end(), [](char c) { return c != 0; });
                }
            }
            if (a.get<bool>("curves", false))
            {
                std::map<std::string, json> byKind;
                for (const LevelLine& line : levelLines(l, r.ground))
                    byKind[line.kind].push_back({{"type", "poly"}, {"closed", false}, {"points", pointsJson(line.pts)}});
                for (auto& [kind, splines] : byKind)
                    curves[kind] = pathOf(ops::execute("curve", {{"splines", splines}, {"name", a.get<std::string>("name", "Levels") + "_" + kind}}));
            }
            return {{"levels", levels},
                    {"ramps", ramps},
                    {"air", {{"heights", gen.air.heights}, {"nodes", gen.air.nodes.size()}, {"links", gen.air.links.size()}, {"connected", connected}}},
                    {"megablocks", megas},
                    {"curves", curves}};
        },
        .examples = {"settlementCreate(profile='scifiMegacity', size=200000, levels={'decks': [{'polygon': [[-30000, -30000], [30000, -30000], "
                     "[30000, 30000], [-30000, 30000]], 'height': 4000}], 'airHeights': [20000, 40000], 'megablock': 20000})\n"
                     "settlementLevels('Settlement')['air']['connected']"},
    });
    std::vector<std::string> cultureValues = {""};
    for (const std::string& c : lila::names::cultures())
        cultureValues.push_back(c);
    std::vector<std::string> profiles;
    for (const std::string& p : profileNames())
        profiles.push_back(p);
    ctx.registerCommand({
        .name = "settlementFromOsm",
        .doc = "A settlement from an OpenStreetMap file on a terrain: terrainImportOsm brings its streets, buildings and places "
               "onto the ground with fictional names, the settlement's outline is fitted round them, and settlementCreate lays "
               "it out with pattern osm: the streets as they are, every building a lot and envelope (heights, levels, parts, "
               "courtyards, roof shape, colours), shops and offices businesses on the building they stand in, areas (cemeteries, "
               "schools, ports, farmland) the use and landmark of the lots inside, and Settlement's own lots in blocks the data "
               "leaves sparse. Returns what settlementCreate returns, with the import's counts, culture and curves.",
        .positionalDoc = "OSM file (.osm, Overpass .json, .osm.pbf)",
        .flags = {nodeFlag({"ground", "gd", GenArg::String, nullptr, "The terrain the data lies on."}, "mesh"),
                  {"file", "fi", GenArg::String, "", "The OSM file, when not given as the first argument."},
                  {"origin", "or", GenArg::List, nullptr, "[latitude, longitude] of the terrain's centre, for terrains not from real data."},
                  {"profile", "pr", GenArg::String, "usTown", "Style for heights, roofs and the lots Settlement cuts itself.", profiles},
                  {"overrides", "ov", GenArg::Any, nullptr, "Profile fields to change, as a JSON object or its text, such as {\"maxFloors\": 6}."},
                  {"culture", "cu", GenArg::String, "", "Naming culture for every name (namingCultures); empty chooses one from where the data lies.", cultureValues},
                  {"fill", "fl", GenArg::String, "sparse", "Settlement's own lots beside the imported buildings: sparse (in blocks they "
                                                           "cover less than coverage of), all, or none.", {"sparse", "all", "none"}},
                  {"coverage", "cv", GenArg::Float, 0.25, "Share of a block the imported buildings must cover for it to count as dense.", {}, {}, 0.0, 1.0},
                  {"margin", "mg", GenArg::Float, 2000.0, "Room left round the imported streets and buildings, cm.", {}, {}, 0.0},
                  {"name", "n", GenArg::String, "Town", "Name of the settlement's mesh."},
                  {"buildingRoute", "br", GenArg::String, "outline", "How buildings are made once laid out; buildings makes procedural ones.",
                   {"outline", "massing", "library", "masonry", "unreal", "buildings"}},
                  {"buildingStyle", "bs", GenArg::String, nullptr, "buildings route: one Buildings style for every building; empty picks per envelope."},
                  {"water", "wa", GenArg::Bool, true, "Keep Settlement's own streets and lots off the terrain's sea, lakes and rivers."},
                  {"exclusions", "ex", GenArg::List, nullptr, "More regions kept clear: closed polygons [[x, z], ...] (other generators' sites)."},
                  {"within", "wi", GenArg::String, "", "A terrain mask the settlement keeps to (where it is at least 0.5), such as a remix source's "
                                                       "remixPresent_<id>; with water=False it is ignored."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "settlementFromOsm";
            Graph& g = Graph::current();
            const std::string file = !a.positional.empty() ? a.positional[0].get<std::string>() : a.get<std::string>("file", "");
            if (file.empty())
                throw CommandError(std::string(cmd) + ": give the OSM file");
            const std::string ground = a.get<std::string>("ground", "");
            if (ground.empty())
                throw CommandError(std::string(cmd) + ": give ground= (the terrain the data lies on)");
            needCommand("terrainImportOsm", "Terrain", cmd);
            json call = {{"args", {file}}, {"terrain", ground}, {"kinds", {"road", "path", "building", "place"}}, {"landcover", false}};
            if (a.has("origin"))
                call["origin"] = a.flags["origin"];
            if (!a.get<std::string>("culture", "").empty())
                call["culture"] = a.get<std::string>("culture", "");
            const json made = ops::execute("terrainImportOsm", call);
            const json curves = made.value("curves", json::object());
            json osm = json::array();
            Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
            for (const char* kind : {"road", "path", "building", "place"})
                if (curves.contains(kind) && curves[kind].is_string())
                {
                    const std::string name = curves[kind].get<std::string>();
                    osm.push_back(name);
                    if (std::string_view(kind) == "place")
                        continue;
                    for (const std::vector<Vec2>& line : curveLines(g, name, cmd))
                        for (const Vec2& q : line)
                            lo = glm::min(lo, q), hi = glm::max(hi, q);
                }
            if (!curves.contains("road") && !curves.contains("path"))
                throw CommandError(std::string(cmd) + ": " + file + " has no roads or paths on the ground");
            const double m = std::max(0.0, a.get<double>("margin", 2000.0));
            lo -= Vec2(m), hi += Vec2(m);
            json outline = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
            json exclusions = json::array();
            bool shaped = false;
            if (a.get<bool>("water", true))
            {
                LandShape land = landShape(ground, lo, hi, a.get<std::string>("within", ""));
                if (!land.outline.empty())
                {
                    outline = land.outline;
                    shaped = true;
                }
                exclusions = land.exclusions;
            }
            const size_t waterCount = exclusions.size();
            if (a.has("exclusions"))
                for (const json& poly : a.flags["exclusions"])
                    exclusions.push_back(poly);
            json create = {{"profile", a.get<std::string>("profile", "usTown")},
                           {"exclusions", exclusions},
                           {"ground", ground},
                           {"outline", outline},
                           {"osm", osm},
                           {"osmFill", a.get<std::string>("fill", "sparse")},
                           {"osmCoverage", a.get<double>("coverage", 0.25)},
                           {"osmCulture", made.value("culture", std::string())},
                           {"terrainWater", false},
                           {"buildingRoute", a.get<std::string>("buildingRoute", "outline")},
                           {"buildingStyle", a.get<std::string>("buildingStyle", "")},
                           {"name", a.get<std::string>("name", "Town")}};
            if (a.has("overrides"))
                create["overrides"] = a.flags["overrides"].is_string() ? a.flags["overrides"].get<std::string>() : a.flags["overrides"].dump();
            // Heights and land use rank from the centre: the data's own city point, where it has one.
            Vec2 centre(0.0);
            const bool centred = curves.contains("place") && curves["place"].is_string() && osmCentre(g, curves["place"].get<std::string>(), outline, &centre);
            if (centred)
                create["attractors"] = json::array({json::array({centre.x, centre.y})});
            json out = ops::execute("settlementCreate", create);
            out["centreFrom"] = centred ? "the data's city or town point" : "the outline's middle (the data has no city or town point inside it)";
            if (shaped)
                out["outlineFrom"] = "land (the water inside the box is outside the outline)";
            if (waterCount > 0)
                out["waterExclusions"] = waterCount;
            out["import"] = {{"features", made.value("features", json::object())},
                             {"culture", made.value("culture", std::string())},
                             {"cultureFrom", made.value("cultureFrom", json())},
                             {"named", made.value("named", 0)},
                             {"curves", curves},
                             {"attributionFile", made.value("attributionFile", std::string())}};
            return out;
        },
        .examples = {"settlementFromOsm('osm/town.osm', ground='Terrain')",
                     "s = settlementFromOsm('city.osm.pbf', ground='Coast', origin=[23.2, -106.42], fill='none')\ns['import']['culture']"},
    });
}
} // namespace settlement
