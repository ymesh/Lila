// The Terrain Roads node and the road commands: lay roads into a terrain's stack, list and remove them, build their
// meshes (Sweep, arrayAlongCurve), export them as curves with attributes, and send them to Unreal as landscape splines.

#include "LandformCommands.h"
#include "Roads.h"
#include "Stamps.h"
#include "Terrain.h"
#include "TerrainEdit.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Menus.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <set>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
using json = nlohmann::json;

namespace roadcmd
{
namespace
{
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;

constexpr const char* kRoadSourcePrefix = "RoadSource";

using namespace terrain::edit;
using Terrain = TerrainTarget;
using Layers = GroupEdit;

json finite(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }
/** Lila space to Unreal's (forward, right, up), cm. */
glm::dvec3 unrealOf(const glm::dvec3& v)
{
    const math::AxisBasis& b = math::axisBasis();
    return {glm::dot(v, b.forward), glm::dot(v, b.right), glm::dot(v, b.up)};
}

std::string layerName(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Roads" + out;
    return out;
}

/** Lengths, heights, grades and angles from world to the heightfield's own units (a across, v up per local unit). */
void scaleToLocal(Road& r, double a, double v)
{
    if (std::abs(a - 1.0) < 1e-12 && std::abs(v - 1.0) < 1e-12)
        return;
    RoadProfile& p = r.profile;
    for (double* x : {&p.laneWidth, &p.shoulder, &p.kerbWidth, &p.sidewalk, &p.verge, &p.ditchWidth, &p.markerSpacing, &p.poleSpacing, &p.kerbReturn, &p.dirtEdge})
        *x /= a;
    for (double* x : {&p.kerbHeight, &p.ditchDepth, &p.guardrailFill})
        *x /= v;
    RoadDesign& d = r.design;
    for (double* x : {&d.minVerticalRadius, &d.minCurveRadius, &d.tunnelRadius, &d.pillarSpacing})
        *x /= a;
    for (double* x : {&d.wallHeight, &d.bridgeHeight, &d.tunnelDepth, &d.deckDepth, &d.fitTolerance})
        *x /= v;
    d.maxGrade *= a / v;
    d.maxBank *= a / v;
    for (double* x : {&d.cutAngle, &d.fillAngle})
        *x = std::atan(std::tan(*x * 3.14159265358979323846 / 180.0) * a / v) * 180.0 / 3.14159265358979323846;
}

/** A road's result in world terms: points in world space, lengths and heights in cm, grades and banks in percent. */
json roadJson(const Terrain& f, const Road& r, const RoadResult& res, const std::string& layer, bool detail)
{
    const Terrain& p = f;
    const double a = p.across(), v = p.vertical();
    json out = {{"id", r.id},
                {"layer", layer},
                {"kind", r.kind},
                {"profile", r.profileName},
                {"surface", r.profile.surface},
                {"applied", !res.empty()},
                {"length", res.length * a},
                {"width", res.formationHalf * 2.0 * a},
                {"pavedWidth", res.pavedHalf * 2.0 * a},
                {"maxGrade", res.maxGrade * v / a},
                {"maxBank", res.maxBank * v / a},
                {"minVerticalRadius", finite(res.minVerticalRadius * a)},
                {"minCurveRadius", finite(res.minCurveRadius * a)},
                {"cut", res.cut * a * a * v},
                {"fill", res.fill * a * a * v},
                {"cutM3", res.cut * a * a * v * 1e-6},
                {"fillM3", res.fill * a * a * v * 1e-6},
                {"area", res.area * a * a},
                {"warnings", res.warnings},
                {"design", {{"maxGrade", r.design.maxGrade}, {"maxBank", r.design.maxBank}}}};
    const size_t n = res.centreline.size();
    const size_t stride = detail || n <= 400 ? 1 : (n + 399) / 400;
    json line = json::array(), along = json::array(), grade = json::array(), bank = json::array();
    for (size_t i = 0; i < n; i += stride)
    {
        line.push_back(vec3(p.world(res.centreline[i])));
        along.push_back(res.along[i] * a);
        grade.push_back(res.grade[i] * 100.0 * v / a);
        bank.push_back(res.bank[i] * 100.0 * v / a);
    }
    if (n > 0 && (n - 1) % stride != 0)
    {
        line.push_back(vec3(p.world(res.centreline.back())));
        along.push_back(res.along.back() * a);
        grade.push_back(res.grade.back() * 100.0 * v / a);
        bank.push_back(res.bank.back() * 100.0 * v / a);
    }
    out["centreline"] = line;
    out["along"] = along;
    out["grade"] = grade;
    out["bank"] = bank;
    json spans = json::array();
    for (const RoadResult::Span& s : res.spans)
        spans.push_back({{"kind", s.kind}, {"from", res.along[s.from] * a}, {"to", res.along[s.to] * a}, {"length", (res.along[s.to] - res.along[s.from]) * a}});
    out["spans"] = spans;
    json bridges = json::array();
    for (const RoadResult::Bridge& b : res.bridges)
    {
        json deck = json::array(), pillars = json::array();
        for (const glm::dvec3& q : b.deck)
            deck.push_back(vec3(p.world(q)));
        for (const RoadResult::Pillar& q : b.pillars)
            pillars.push_back({{"top", vec3(p.world(q.top))}, {"height", (q.top.y - q.ground) * v}});
        bridges.push_back({{"deck", deck}, {"pillars", pillars}, {"length", b.length * a}, {"maxHeight", b.maxHeight * v}});
    }
    out["bridges"] = bridges;
    json tunnels = json::array();
    for (const RoadResult::Tunnel& t : res.tunnels)
    {
        json axis = json::array(), floor = json::array(), portals = json::array();
        for (const glm::dvec3& q : t.axis)
            axis.push_back(vec3(p.world(q)));
        for (const glm::dvec3& q : t.floor)
            floor.push_back(vec3(p.world(q)));
        for (const RoadResult::Portal& q : t.portals)
            portals.push_back({{"position", vec3(p.world(q.at))}, {"direction", vec3(p.direction(q.direction))}});
        tunnels.push_back({{"axis", axis}, {"floor", floor}, {"radius", t.radius * a}, {"length", t.length * a}, {"portals", portals}});
    }
    out["tunnels"] = tunnels;
    json walls = json::array();
    for (const RoadResult::Wall& w : res.walls)
    {
        json pts = json::array(), heights = json::array();
        for (const RoadResult::WallPoint& q : w.points)
        {
            pts.push_back(vec3(p.world(q.at.x, q.top, q.at.y)));
            heights.push_back((q.top - q.bottom) * v);
        }
        walls.push_back({{"side", w.side > 0 ? "left" : "right"}, {"kind", w.cut ? "cut" : "fill"}, {"points", pts}, {"heights", heights}});
    }
    out["walls"] = walls;
    json culverts = json::array();
    for (const glm::dvec2& c : res.culverts)
        culverts.push_back(vec3(p.world(c.x, f.local->heightAt(c.x, c.y), c.y)));
    out["culverts"] = culverts;
    json rails = json::array();
    for (const RoadResult::PropRun& run : res.guardrails)
    {
        json pts = json::array();
        for (const glm::dvec3& q : run.points)
            pts.push_back(vec3(p.world(q)));
        rails.push_back({{"side", run.side > 0 ? "left" : "right"}, {"points", pts}});
    }
    out["guardrails"] = rails;
    json props = json::array();
    for (const RoadResult::Prop& q : res.props)
        props.push_back({{"kind", q.kind}, {"side", q.side > 0 ? "left" : "right"}, {"position", vec3(p.world(q.at))}, {"direction", vec3(p.direction(q.direction))}});
    out["props"] = props;
    json junctions = json::array();
    for (const RoadResult::Junction& j : res.junctions)
        junctions.push_back({{"other", j.other}, {"type", j.type}, {"position", vec3(p.world(j.at))}});
    out["junctions"] = junctions;
    json kerbs = json::array();
    for (const RoadResult::KerbReturn& k : res.kerbReturns)
    {
        json arc = json::array();
        for (const glm::dvec3& q : k.arc)
            arc.push_back(vec3(p.world(q)));
        kerbs.push_back({{"other", k.other}, {"corner", vec3(p.world(k.corner))}, {"arc", arc}});
    }
    out["kerbReturns"] = kerbs;
    json islands = json::array();
    for (const RoadResult::Island& i : res.islands)
    {
        json outline = json::array();
        for (const glm::dvec2& q : i.outline)
            outline.push_back(vec3(p.world(q.x, i.height, q.y)));
        json is = {{"kind", i.kind}, {"outline", outline}, {"height", upOf(p.world(i.outline[0].x, i.height, i.outline[0].y))}};
        if (!i.other.empty())
            is["other"] = i.other;
        islands.push_back(is);
    }
    out["islands"] = islands;
    if (r.ring)
        out["ring"] = true;
    return out;
}

struct RoadEntry
{
    Road road;
    RoadResult result;
    std::string layer;
};

std::vector<RoadEntry> allRoads(Graph& g, const Terrain& f, const std::string& cmd)
{
    std::vector<RoadEntry> out;
    Layers t = readGroup(g, f, cmd);
    for (const gn::GNode& n : t.group.nodes)
    {
        if (n.type != "TerrainRoads")
            continue;
        const auto results = roadResults(f.modifier->id(), n.name);
        for (const json& j : jsonProp(n, "roads", json::array()))
        {
            RoadEntry e;
            e.road = Road::fromJson(j);
            e.layer = n.name;
            if (auto it = results.find(e.road.id); it != results.end())
                e.result = it->second;
            out.push_back(std::move(e));
        }
    }
    return out;
}

Terrain firstTerrain(const Args& a, const std::string& cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.empty())
        throw CommandError(cmd + ": give a terrain");
    return findTerrain(Graph::current(), names[0], cmd);
}

std::set<std::string> idsFlag(const Args& a)
{
    std::set<std::string> ids;
    for (const json& i : a.flags.value("ids", json::array()))
        ids.insert(i.get<std::string>());
    return ids;
}

/** The profile and design fields a command takes as flags, by name. */
const std::vector<std::string> kProfileKeys = {"lanes", "laneWidth", "shoulder", "kerbHeight", "kerbWidth", "sidewalk", "verge", "ditchWidth", "ditchDepth",
                                               "surface", "guardrailFill", "markerSpacing", "poleSpacing", "kerbReturn", "dirtEdge"};
const std::vector<std::string> kDesignKeys = {"designSpeed", "maxGrade", "minVerticalRadius", "maxBank", "bankRunoff", "minCurveRadius", "cutAngle", "fillAngle",
                                              "wallHeight", "bridgeHeight", "tunnelDepth", "tunnelRadius", "deckDepth", "pillarSpacing", "fitTolerance"};

/** The parts of a polyline outside a circle, each cut where it crosses the circle so it ends on it. */
std::vector<std::vector<glm::dvec2>> outsideCircle(const std::vector<glm::dvec2>& line, glm::dvec2 c, double r)
{
    std::vector<std::vector<glm::dvec2>> out(1);
    auto inside = [&](glm::dvec2 q) { return glm::length(q - c) < r; };
    if (!line.empty() && !inside(line[0]))
        out.back().push_back(line[0]);
    for (size_t k = 1; k < line.size(); ++k)
    {
        const glm::dvec2 a = line[k - 1], d = line[k] - a, f = a - c;
        const double A = std::max(1e-12, glm::dot(d, d)), B = 2.0 * glm::dot(f, d), C = glm::dot(f, f) - r * r;
        const double disc = B * B - 4.0 * A * C;
        if (disc > 0.0)
        {
            const double root = std::sqrt(disc);
            for (const double t : {(-B - root) / (2.0 * A), (-B + root) / (2.0 * A)})
            {
                if (t <= 0.0 || t >= 1.0)
                    continue;
                out.back().push_back(a + d * t);
                // Leaving the outside closes this part; coming back out starts the next.
                if (inside(a + d * std::min(1.0, t + 1e-9)))
                    out.emplace_back();
            }
        }
        if (!inside(line[k]))
            out.back().push_back(line[k]);
    }
    std::erase_if(out, [](const std::vector<glm::dvec2>& part) { return part.size() < 2; });
    return out;
}

json runAddRoundabout(const Args& a, const std::string& cmd)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    const Terrain f = findTerrain(g, names[0], cmd);
    if (!a.has("at"))
        throw CommandError(cmd + ": give at= (the centre)");
    const glm::dvec2 c = f.local2(worldPoint(a.flags["at"], cmd));
    const double radius = a.get<double>("radius", 2500.0) / f.across();
    const std::string layer = layerName(a.get<std::string>("layer", "Roads"));

    Road ring;
    try
    {
        ring = Road::fromJson({{"kind", "road"}, {"profileName", a.get<std::string>("profile", "urban")}});
        json po = {{"lanes", a.get<int>("lanes", 1)}, {"laneWidth", a.get<double>("laneWidth", 500.0)}};
        if (a.has("kerbHeight"))
            po["kerbHeight"] = a.flags["kerbHeight"];
        ring.profile.read(po);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
    ring.ring = true;
    ring.smooth = false;
    ring.ringCrossFall = a.get<double>("crossFall", 2.0);
    scaleToLocal(ring, f.across(), f.vertical());
    if (radius <= ring.profile.kerbHalf() + 100.0 / f.across())
        throw CommandError(cmd + ": radius must leave room for a central island: more than " +
                           std::to_string(int(std::ceil((ring.profile.kerbHalf() + 100.0) * f.across()))) + " cm");
    constexpr double kTau = 6.283185307179586;
    const int segments = std::clamp(int(std::ceil(kTau * radius / std::max(f.local->cell, 1.0))), 32, 512);
    for (int k = 0; k < segments; ++k)
    {
        const double t = kTau * double(k) / double(segments);
        ring.path.push_back(c + glm::dvec2(std::cos(t), std::sin(t)) * radius);
    }
    if (a.has("height"))
        ring.keys.push_back({c, f.localHeight(a.get<double>("height", 0.0), c)});

    Layers t = readGroup(g, f, cmd);
    static const std::set<std::string> skip = {"TerrainStamps", "TerrainVolume", "TerrainBiomes", "TerrainLayers"};
    ListEdit roads(*t.node, chainNode(t.group, layer, "TerrainRoads", skip, -160.0, cmd), "roads");
    const json& list = roads.list();
    std::string id = a.get<std::string>("id", "");
    if (id.empty())
        for (int k = 1; id.empty(); ++k)
        {
            const std::string candidate = "roundabout" + std::to_string(k);
            if (std::none_of(list.begin(), list.end(), [&](const json& e) { return e.value("id", std::string()) == candidate; }))
                id = candidate;
        }
    ring.id = id;

    std::set<std::string> only;
    if (a.has("roads"))
        for (const json& r : a.flags["roads"])
            only.insert(r.get<std::string>());
    for (const std::string& rid : only)
        if (std::none_of(list.begin(), list.end(), [&](const json& e) { return e.value("id", std::string()) == rid; }))
            throw CommandError(cmd + ": no road " + rid + " in layer " + layer);

    // Roads reaching the ring are cut back to its centreline; one running through becomes an approach per side.
    json kept = json::array(), approaches = json::array(), warnings = json::array();
    std::optional<size_t> firstApproach;
    for (const json& e : list)
    {
        const std::string rid = e.value("id", std::string());
        if (rid == id || (!only.empty() && !only.count(rid)))
        {
            if (rid != id)
                kept.push_back(e);
            continue;
        }
        const Road r = Road::fromJson(e);
        if (r.ring)
        {
            kept.push_back(e);
            continue;
        }
        if (!r.object.empty())
        {
            if (only.count(rid))
                warnings.push_back(rid + " follows a curve, so it was left as it is: end the curve on the ring's centreline");
            kept.push_back(e);
            continue;
        }
        const std::vector<std::vector<glm::dvec2>> parts = outsideCircle(r.path, c, radius);
        const bool reaches = parts.size() != 1 || parts[0].size() != r.path.size() ||
                             std::any_of(r.path.begin(), r.path.end(), [&](glm::dvec2 q) { return glm::length(q - c) < radius; });
        if (!reaches)
        {
            if (only.count(rid))
                warnings.push_back(rid + " does not reach the ring, so it was left as it is");
            kept.push_back(e);
            continue;
        }
        if (!firstApproach)
            firstApproach = kept.size();
        for (size_t k = 0; k < parts.size(); ++k)
        {
            Road part = r;
            part.path = parts[k];
            if (k > 0)
                part.id = rid + "_" + std::string(1, char('a' + k));
            kept.push_back(part.toJson());
            approaches.push_back(part.id);
        }
    }
    // The ring goes before its approaches so they join it.
    kept.insert(kept.begin() + std::ptrdiff_t(firstApproach.value_or(kept.size())), ring.toJson());
    roads.list() = std::move(kept);
    roads.store(*nodeNamed(t.group, layer));
    commitGroup(t, cmd);
    if (EditBatch::active())
        return {{"id", id}, {"layer", layer}, {"approaches", approaches}, {"warnings", warnings}, {"batched", true}};

    const Terrain after = findTerrain(g, f.path(names[0]), cmd);
    const auto results = roadResults(after.modifier->id(), layer);
    auto it = results.find(id);
    const RoadResult res = it != results.end() ? it->second : RoadResult{};
    json out = roadJson(after, ring, res, layer, a.get<bool>("detail", false));
    for (const std::string& w : res.warnings)
        log::warn("{}: {}: {}", cmd, id, w);
    for (const json& w : warnings)
        log::warn("{}: {}", cmd, w.get<std::string>());
    out["approaches"] = approaches;
    out["warnings"] = warnings;
    out["revision"] = revision(*after.local);
    return out;
}

std::vector<FlagDef> addRoadFlags()
{
    return {
        {"path", "pa", T::List, nullptr, "The line [[x, z] or [x, y, z], ...] in world space (x, y on a Z-up scene)."},
        nodeFlag({"object", "obj", T::String, nullptr, "A curve the road follows live (or give it as the second positional)."}, "curve"),
        {"id", "id", T::String, "", "Stable id; the same id again replaces that road. Empty makes one."},
        {"kind", "k", T::String, "road", "road, path, track, rail or runway: sets the design defaults.", roadKindNames()},
        {"profile", "pr", T::String, "", "Cross-section preset; empty takes the kind's (road: rural).", [] {
             std::vector<std::string> o{""};
             for (const std::string& n : roadProfileNames())
                 o.push_back(n);
             return o;
         }()},
        {"layer", "ly", T::String, "Roads", "Roads layer (a Terrain Roads node)."},
        {"stage", "sg", T::String, "final", "final: after erosion, before the stamps; base: before erosion, which then leaves the road alone (erosionProtect).",
         {"final", "base"}},
        {"keys", "ky", T::List, nullptr, "Heights the road must pass through: [[x, y, z], ...] world points (z up on a Z-up scene)."},
        {"width", "w", T::Float, nullptr, "Carriageway width, cm (split over the lanes).", {}, {}, 1.0},
        {"lanes", "ln", T::Int, nullptr, "Lanes.", {}, {}, 1.0, 16.0},
        {"laneWidth", "lw", T::Float, nullptr, "Lane width, cm.", {}, {}, 1.0},
        {"shoulder", "sh", T::Float, nullptr, "Paved shoulder each side, cm.", {}, {}, 0.0},
        {"kerbHeight", "kh", T::Float, nullptr, "Kerb height, cm; 0 is none.", {}, {}, 0.0},
        {"sidewalk", "sw", T::Float, nullptr, "Sidewalk each side behind the kerb, cm.", {}, {}, 0.0},
        {"verge", "vg", T::Float, nullptr, "Level verge each side, cm.", {}, {}, 0.0},
        {"ditchWidth", "dw", T::Float, nullptr, "Ditch in cuttings, cm; 0 is none.", {}, {}, 0.0},
        {"ditchDepth", "dd", T::Float, nullptr, "Ditch depth, cm.", {}, {}, 0.0},
        {"surface", "su", T::String, nullptr, "asphalt, concrete, gravel, dirt or ballast."},
        {"designSpeed", "ds", T::Float, nullptr, "km/h: sets curve radius and banking.", {}, {}, 1.0},
        {"maxGrade", "mg", T::Float, nullptr, "Steepest grade, percent.", {}, {}, 0.0},
        {"minVerticalRadius", "vr", T::Float, nullptr, "Smallest crest or sag radius, cm.", {}, {}, 0.0},
        {"maxBank", "mb", T::Float, nullptr, "Largest superelevation, percent.", {}, {}, 0.0},
        {"minCurveRadius", "cr", T::Float, nullptr, "Smallest horizontal curve radius, cm; default from designSpeed.", {}, {}, 0.0},
        {"cutAngle", "ca", T::Float, nullptr, "Cutting slope, degrees.", {}, {}, 5.0, 89.0},
        {"fillAngle", "fa", T::Float, nullptr, "Embankment slope, degrees.", {}, {}, 5.0, 89.0},
        {"wallHeight", "wh", T::Float, nullptr, "Slopes taller than this become retaining walls, cm; 0 never.", {}, {}, 0.0},
        {"bridgeHeight", "bh", T::Float, nullptr, "Fill deeper than this becomes a bridge, cm; 0 never.", {}, {}, 0.0},
        {"tunnelDepth", "td", T::Float, nullptr, "Cut deeper than this becomes a tunnel, cm; 0 never.", {}, {}, 0.0},
        {"smooth", "sm", T::Bool, true, "Round the path's corners into curves."},
        {"masks", "mk", T::List, nullptr, "Extra masks over the paved surface; default paved for roads and runways."},
        {"settings", "set", T::Any, nullptr, "Any other profile or design field: {kerbReturn: 600, poleSpacing: 3000, ...}."},
        {"build", "b", T::Bool, false, "Also build its meshes (terrainRoadBuild)."},
        {"detail", "dt", T::Bool, false, "Return every station, not a sample of them."},
    };
}

json runAddRoad(const Args& a, const std::string& cmd)
{
    LILA_ZONE();

    std::vector<std::string> names = a.objects();
    if (names.empty() || names.size() > 2)
        throw CommandError(cmd + ": give a terrain (and optionally the curve to follow)");
    Graph& g = Graph::current();
    if (names.size() == 2)
        if (Node* first = g.find(names[0]); first && first->type().isA("curve"))
            std::swap(names[0], names[1]);
    const Terrain f = findTerrain(g, names[0], cmd);
    const Terrain& fr = f;
    const std::string layer = layerName(a.get<std::string>("layer", "Roads"));

    Road r;
    try
    {
        r = Road::fromJson({{"kind", a.get<std::string>("kind", "road")}, {"profileName", a.get<std::string>("profile", "")}});
        json po = json::object(), de = json::object();
        if (a.has("settings"))
        {
            const json s = a.flags["settings"];
            if (!s.is_object())
                throw std::invalid_argument("settings is an object of profile and design fields");
            for (const auto& [k, v] : s.items())
            {
                if (std::find(kProfileKeys.begin(), kProfileKeys.end(), k) != kProfileKeys.end())
                    po[k] = v;
                else if (std::find(kDesignKeys.begin(), kDesignKeys.end(), k) != kDesignKeys.end())
                    de[k] = v;
                else
                    throw std::invalid_argument("settings has no field " + k);
            }
        }
        for (const std::string& k : kProfileKeys)
            if (a.has(k))
                po[k] = a.flags[k];
        for (const std::string& k : kDesignKeys)
            if (a.has(k))
                de[k] = a.flags[k];
        r.profile.read(po);
        r.design.read(de);
        if (a.has("width"))
            r.profile.laneWidth = std::max(1.0, a.get<double>("width", 700.0) / double(std::max(1, r.profile.lanes)));
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
    r.smooth = a.get<bool>("smooth", true);
    if (a.has("masks"))
    {
        r.masks.clear();
        const json m = a.flags["masks"];
        if (m.is_string())
            r.masks.push_back(m.get<std::string>());
        else
            for (const json& x : m)
                r.masks.push_back(x.get<std::string>());
    }
    scaleToLocal(r, fr.across(), fr.vertical());

    Layers t = readGroup(g, f, cmd);
    const bool base = a.get<std::string>("stage", "final") == "base";
    const std::set<std::string> skip = base ? withLandformTypes({"ErodeTerrain", "TerrainStamps", "TerrainSculpt", "TerrainRoads", "TerrainWaterFeatures", "TerrainVolume", "TerrainBiomes", "TerrainLayers"})
                                                : std::set<std::string>{"TerrainStamps", "TerrainVolume", "TerrainBiomes", "TerrainLayers"};
    ListEdit roads(*t.node, chainNode(t.group, layer, "TerrainRoads", skip, -160.0, cmd), "roads");
    json& list = roads.list();
    std::string id = a.get<std::string>("id", "");
    if (id.empty())
        for (int k = 1; id.empty(); ++k)
        {
            const std::string candidate = r.kind + std::to_string(k);
            if (std::none_of(list.begin(), list.end(), [&](const json& e) { return e.value("id", std::string()) == candidate; }))
                id = candidate;
        }
    r.id = id;

    std::string object = a.get<std::string>("object", "");
    if (object.empty() && names.size() == 2)
        object = names[1];
    if (a.has("path"))
    {
        for (const json& q : a.flags["path"])
            r.path.push_back(fr.local2(worldPoint(q, cmd)));
        if (r.path.size() < 2)
            throw CommandError(cmd + ": path needs at least two points");
    }
    else if (!object.empty())
    {
        Node* src = g.find(object);
        if (!src || !src->type().isA("curve"))
            throw CommandError(cmd + ": object must name a curve");
        r.object = sourceNode(t.group, kRoadSourcePrefix, src->path());
    }
    else
        throw CommandError(cmd + ": give path= or object= (a curve)");
    if (a.has("keys"))
        for (const json& q : a.flags["keys"])
        {
            if (!q.is_array() || q.size() != 3)
                throw CommandError(cmd + ": keys are [x, y, z] world points");
            const glm::dvec3 w = worldPoint(q, cmd);
            const glm::dvec2 at = fr.local2(w);
            r.keys.push_back({at, fr.localHeight(upOf(w), at)});
        }

    bool replaced = false;
    for (json& e : list)
        if (e.value("id", std::string()) == id)
        {
            e = r.toJson();
            replaced = true;
        }
    if (!replaced)
        list.push_back(r.toJson());
    roads.store(*nodeNamed(t.group, layer));
    // A batch leaves unused sources for the next edit to drop: its lists are only written back when it commits.
    if (!EditBatch::active())
        dropUnusedSources(t.group, kRoadSourcePrefix, "TerrainRoads", {"roads"});
    commitGroup(t, cmd);
    if (EditBatch::active())
        return {{"id", id}, {"layer", layer}, {"replaced", replaced}, {"batched", true}};

    const Terrain after = findTerrain(g, f.path(names[0]), cmd);
    const auto results = roadResults(after.modifier->id(), layer);
    auto it = results.find(id);
    const RoadResult res = it != results.end() ? it->second : RoadResult{};
    json out = roadJson(after, r, res, layer, a.get<bool>("detail", false));
    for (const std::string& w : res.warnings)
        log::warn("{}: road {}: {}", cmd, id, w);
    out["replaced"] = replaced;
    out["revision"] = revision(*after.local);
    if (a.get<bool>("build", false))
        out["build"] = ops::execute("terrainRoadBuild", {{"args", {after.path(names[0])}}, {"ids", {id}}, {"layer", layer}});
    return out;
}

// ---------------------------------------------------------------------------------------------------- building geometry

std::string made(const json& r) { return r.is_string() ? r.get<std::string>() : r.at(0).get<std::string>(); }

std::string makeMesh(const std::vector<glm::dvec3>& pts, const std::vector<std::vector<int>>& faces, const std::string& name)
{
    json p = json::array(), f = json::array();
    for (const glm::dvec3& q : pts)
        p.push_back(vec3(q));
    for (const auto& face : faces)
        f.push_back(face);
    return made(ops::execute("createMesh", {{"points", p}, {"faces", f}, {"name", name}}));
}

/** A box in its own space: x from x0 to x1, y from y0 to y1, z from z0 to z1, faces out. */
void addBox(std::vector<glm::dvec3>& pts, std::vector<std::vector<int>>& faces, glm::dvec3 lo, glm::dvec3 hi)
{
    const int b = int(pts.size());
    for (int k = 0; k < 8; ++k)
        pts.push_back({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
    for (const auto& f : std::vector<std::vector<int>>{{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}})
        faces.push_back({b + f[0], b + f[1], b + f[2], b + f[3]});
}

/** A box piece with the scene's up: on a Z-up scene the piece's Y becomes Z. */
glm::dvec3 upright(glm::dvec3 p) { return zUp() ? glm::dvec3(p.x, -p.z, p.y) : p; }

std::string makePiece(const std::vector<std::pair<glm::dvec3, glm::dvec3>>& boxes, const std::string& name)
{
    std::vector<glm::dvec3> pts;
    std::vector<std::vector<int>> faces;
    for (const auto& [lo, hi] : boxes)
        addBox(pts, faces, lo, hi);
    for (glm::dvec3& p : pts)
        p = upright(p);
    return makeMesh(pts, faces, name);
}

/** World frame of a station for a sweep: tangent along the 3D centreline, normal toward up, binormal (left) = t x n. */
struct SweepFrame
{
    glm::dvec3 t, n, b;
};

SweepFrame frameAt(const std::vector<glm::dvec3>& w, size_t i)
{
    const size_t a = i > 0 ? i - 1 : i, z = std::min(w.size() - 1, i + 1);
    SweepFrame f;
    f.t = glm::normalize(w[z] - w[a]);
    const glm::dvec3 up = upVector();
    f.n = glm::normalize(up - f.t * glm::dot(up, f.t));
    f.b = glm::cross(f.t, f.n);
    return f;
}

/** Stations kept for meshes: every one when close, else about every step cm, ends and span edges included. */
std::vector<size_t> meshStations(const RoadResult& r, size_t from, size_t to, double step)
{
    std::vector<size_t> out;
    double last = -1e30;
    for (size_t i = from; i <= to; ++i)
        if (i == from || i == to || r.along[i] - last >= step)
        {
            out.push_back(i);
            last = r.along[i];
        }
    return out;
}

/**
 * A sweep along stations of the road: the path curve (tilted by the bank) and a profile laid across the first station in
 * world space, (lateral left +, up) pairs, swept with extrude. Returns the mesh.
 */
std::string sweepAlong(const Terrain& f, const RoadResult& r, const std::vector<size_t>& at, const std::vector<std::pair<double, double>>& profile,
                       double drop, const std::string& name, std::vector<std::string>& made_)
{
    const Terrain& p = f;
    std::vector<glm::dvec3> w;
    json pts = json::array(), tilt = json::array();
    for (size_t i : at)
    {
        glm::dvec3 q = r.centreline[i];
        q.y -= drop;
        w.push_back(p.world(q));
        pts.push_back(vec3(w.back()));
        tilt.push_back(std::atan(r.bank[i] * p.vertical() / p.across()));
    }
    const std::string path = made(ops::execute("curve", {{"points", pts}, {"type", "poly"}, {"tilt", tilt}, {"name", name + "_path"}}));
    const SweepFrame fr = frameAt(w, 0);
    const double tau = tilt[0].get<double>();
    const glm::dvec3 bt = fr.b * std::cos(tau) - fr.n * std::sin(tau), nt = fr.n * std::cos(tau) + fr.b * std::sin(tau);
    json prof = json::array();
    for (const auto& [d, up] : profile)
        prof.push_back(vec3(w[0] + bt * (d * p.across()) + nt * (up * p.vertical())));
    const std::string profileCurve = made(ops::execute("curve", {{"points", prof}, {"type", "poly"}, {"name", name + "_profile"}}));
    const json ex = ops::execute("extrude", {{"args", {path, profileCurve}}, {"alignProfile", false}, {"frameMode", "fixedUp"}, {"upVector", vec3(upVector())}, {"name", name}});
    made_.push_back(path);
    made_.push_back(profileCurve);
    const std::string mesh = made(ex);
    made_.push_back(mesh);
    return mesh;
}

std::vector<std::pair<double, double>> surfaceProfile(const Road& rd, double skirt)
{
    const RoadProfile& p = rd.profile;
    const double fh = p.formationHalf(), ph = p.pavedHalf();
    const double top = p.kerbHeight > 0.0 ? p.kerbHeight : 0.0;
    std::vector<std::pair<double, double>> left{{fh, -skirt}, {fh, top}};
    if (p.kerbHeight > 0.0)
        left.push_back({ph, top});
    left.push_back({ph, 0.0});
    std::vector<std::pair<double, double>> out = left;
    for (auto it = left.rbegin(); it != left.rend(); ++it)
        out.push_back({-it->first, it->second});
    return out;
}

/** A run of placements spread evenly along a curve through them with arrayAlongCurve (merged into the piece mesh). */
void arrayRun(const std::vector<glm::dvec3>& at, const std::string& piece, const std::string& name, bool align, std::vector<std::string>& out)
{
    json pts = json::array();
    for (const glm::dvec3& q : at)
        pts.push_back(vec3(q));
    const std::string curve = made(ops::execute("curve", {{"points", pts}, {"type", "poly"}, {"name", name + "_path"}}));
    out.push_back(curve);
    ops::execute("arrayAlongCurve", {{"args", {piece}},
                                     {"curve", curve},
                                     {"fill", "count"},
                                     {"count", int(at.size())},
                                     {"align", align},
                                     {"upMode", "autoNormals"},
                                     {"upVector", vec3(upVector())},
                                     {"output", "merged"}});
    out.push_back(piece);
}

/** The Set Dressing library asset standing for each road prop kind, by default. */
const json& defaultPropAssets()
{
    static const json d = {{"marker", "Road/Delineator"}, {"pole", "Road/StreetLight"}, {"edgeLight", "Road/EdgeLight"}, {"mast", "Road/CatenaryMast"}};
    return d;
}

/** Degrees to turn an asset about the scene's up so its +X runs along dir. */
double headingOf(const glm::dvec3& dir)
{
    return glm::degrees(zUp() ? std::atan2(dir.y, dir.x) : std::atan2(-dir.z, dir.x));
}

json buildRoad(const Terrain& f, const RoadEntry& e, const json& propAssets, json& warnings)
{
    LILA_ZONE();

    const Road& rd = e.road;
    const RoadResult& r = e.result;
    const Terrain& p = f;
    std::vector<std::string> nodes;
    const std::string base = layerName(rd.id);
    const double step = std::max(200.0, r.along.size() > 1 ? r.along[1] - r.along[0] : 200.0);
    json out = {{"id", rd.id}};

    // The surface: one sweep per stretch on the ground or over a bridge; tunnels keep their road too.
    const std::string surface = sweepAlong(f, r, meshStations(r, 0, r.centreline.size() - 1, step), surfaceProfile(rd, 50.0 / p.vertical()), 0.0,
                                           base + "_surface", nodes);
    out["surface"] = surface;

    json bridges = json::array();
    for (size_t b = 0; b < r.bridges.size(); ++b)
    {
        const RoadResult::Bridge& br = r.bridges[b];
        const RoadResult::Span* span = nullptr;
        size_t count = 0;
        for (const RoadResult::Span& s : r.spans)
            if (s.kind == "bridge" && count++ == b)
                span = &s;
        if (!span)
            continue;
        const size_t a = span->from > 0 ? span->from - 1 : span->from, z = std::min(r.centreline.size() - 1, span->to + 1);
        const double half = r.formationHalf + 30.0 / p.across(), dd = rd.design.deckDepth;
        const std::string deck = sweepAlong(f, r, meshStations(r, a, z, step), {{half, 0.0}, {half, -dd}, {-half, -dd}, {-half, 0.0}}, 1.0 / p.vertical(),
                                            base + "_bridge" + std::to_string(b + 1), nodes);
        json entry = {{"deck", deck}};
        if (!br.pillars.empty())
        {
            double tallest = 0.0;
            for (const RoadResult::Pillar& q : br.pillars)
                tallest = std::max(tallest, (q.top.y - q.ground) * p.vertical());
            const double w = std::clamp(r.formationHalf * p.across() * 0.4, 60.0, 250.0);
            const std::string piece = makePiece({{{-w * 0.5, -(tallest + 300.0), -60.0}, {w * 0.5, 0.0, 60.0}}}, base + "_pillars" + std::to_string(b + 1));
            std::vector<glm::dvec3> tops;
            for (const RoadResult::Pillar& q : br.pillars)
                tops.push_back(p.world(q.top));
            if (tops.size() == 1)
            {
                ops::execute("setAttr", {{"args", {piece + ".translate", vec3(tops[0])}}});
                nodes.push_back(piece);
            }
            else
                arrayRun(tops, piece, base + "_pillars" + std::to_string(b + 1), false, nodes);
            entry["pillars"] = piece;
        }
        bridges.push_back(entry);
    }
    out["bridges"] = bridges;

    // Tunnel portals: an arch frame at each end, facing out.
    json portals = json::array();
    for (size_t k = 0; k < r.tunnels.size(); ++k)
        for (const RoadResult::Portal& q : r.tunnels[k].portals)
        {
            const double rad = r.tunnels[k].radius * p.across();
            std::vector<glm::dvec3> pts;
            std::vector<std::vector<int>> faces;
            const glm::dvec3 at = p.world(q.at);
            const glm::dvec3 fwd = p.direction(q.direction), up = upVector(), side = glm::normalize(glm::cross(fwd, up));
            const int segs = 12;
            for (int s = 0; s <= segs; ++s)
            {
                const double ang = 3.14159265358979323846 * double(s) / double(segs);
                const glm::dvec3 dir = side * std::cos(ang) + up * std::sin(ang);
                pts.push_back(at + dir * rad);
                pts.push_back(at + dir * (rad + 80.0));
            }
            for (int s = 0; s < segs; ++s)
                faces.push_back({2 * s, 2 * s + 2, 2 * s + 3, 2 * s + 1});
            const std::string m = makeMesh(pts, faces, base + "_portal");
            nodes.push_back(m);
            portals.push_back(m);
        }
    out["portals"] = portals;

    // Retaining walls: a face from top to bottom along the formation edge with a capping strip.
    json walls = json::array();
    for (const RoadResult::Wall& w : r.walls)
    {
        std::vector<glm::dvec3> pts;
        std::vector<std::vector<int>> faces;
        for (size_t i = 0; i < w.points.size(); ++i)
        {
            const RoadResult::WallPoint& q = w.points[i];
            pts.push_back(p.world(q.at.x, q.top, q.at.y));
            pts.push_back(p.world(q.at.x, q.bottom - 50.0 / p.vertical(), q.at.y));
            if (i > 0)
            {
                const int a = int(2 * (i - 1)), b = int(2 * i);
                faces.push_back(w.side > 0 ? std::vector<int>{a, a + 1, b + 1, b} : std::vector<int>{a, b, b + 1, a + 1});
            }
        }
        const std::string m = makeMesh(pts, faces, base + "_wall");
        nodes.push_back(m);
        walls.push_back(m);
    }
    out["walls"] = walls;

    // Kerb returns: fan patches over the corners.
    json patches = json::array();
    for (const RoadResult::KerbReturn& k : r.kerbReturns)
    {
        std::vector<glm::dvec3> pts{p.world(k.corner + glm::dvec3(0.0, 1.0, 0.0))};
        std::vector<std::vector<int>> faces;
        for (size_t i = 0; i < k.arc.size(); ++i)
        {
            pts.push_back(p.world(k.arc[i] + glm::dvec3(0.0, 1.0, 0.0)));
            if (i > 0)
                faces.push_back({0, int(i), int(i + 1)});
        }
        const glm::dvec3 n = glm::cross(pts[1] - pts[0], pts[2] - pts[0]);
        if (glm::dot(n, upVector()) < 0.0)
            for (auto& face : faces)
                std::reverse(face.begin(), face.end());
        const std::string m = makeMesh(pts, faces, base + "_junction");
        nodes.push_back(m);
        patches.push_back(m);
    }
    out["junctions"] = patches;

    // Islands: a raised cap over the outline with kerb faces down into the ground.
    json islandMeshes = json::array();
    for (const RoadResult::Island& i : r.islands)
    {
        const size_t m = i.outline.size();
        if (m < 3)
            continue;
        const double drop = std::max(30.0, rd.profile.kerbHeight + 20.0);
        std::vector<glm::dvec3> pts;
        for (const glm::dvec2& q : i.outline)
            pts.push_back(p.world(q.x, f.local->heightAt(q.x, q.y), q.y));
        for (const glm::dvec2& q : i.outline)
            pts.push_back(p.world(q.x, f.local->heightAt(q.x, q.y) - drop, q.y));
        std::vector<std::vector<int>> faces;
        std::vector<int> cap;
        for (size_t k = 0; k < m; ++k)
            cap.push_back(int(k));
        faces.push_back(cap);
        for (size_t k = 0; k < m; ++k)
        {
            const int a = int(k), b = int((k + 1) % m);
            faces.push_back({b, a, a + int(m), b + int(m)});
        }
        const glm::dvec3 n = glm::cross(pts[1] - pts[0], pts[2] - pts[0]);
        if (glm::dot(n, upVector()) < 0.0)
            for (auto& face : faces)
                std::reverse(face.begin(), face.end());
        const std::string mesh = makeMesh(pts, faces, base + "_" + i.kind + "Island");
        nodes.push_back(mesh);
        islandMeshes.push_back(mesh);
    }
    out["islands"] = islandMeshes;

    // Props: guardrails bend along their runs; markers, lights and poles stand upright along theirs.
    json props = json::array();
    for (size_t k = 0; k < r.guardrails.size(); ++k)
    {
        const std::string piece = makePiece({{{0.0, 0.0, -8.0}, {12.0, 100.0, 8.0}}, {{0.0, 55.0, -12.0}, {400.0, 85.0, -4.0}}},
                                            base + "_guardrail" + std::to_string(k + 1));
        std::vector<glm::dvec3> pts;
        for (const glm::dvec3& q : r.guardrails[k].points)
            pts.push_back(p.world(q));
        json c = json::array();
        for (const glm::dvec3& q : pts)
            c.push_back(vec3(q));
        const std::string curve = made(ops::execute("curve", {{"points", c}, {"type", "poly"}, {"name", base + "_guardrail" + std::to_string(k + 1) + "_path"}}));
        ops::execute("arrayAlongCurve", {{"args", {piece}}, {"curve", curve}, {"fill", "spacing"}, {"spacing", 400.0}, {"deform", true}, {"forwardAxis", "+x"},
                                         {"upMode", "autoNormals"}, {"upVector", vec3(upVector())}, {"output", "merged"}});
        nodes.push_back(curve);
        nodes.push_back(piece);
        props.push_back(piece);
    }
    std::map<std::pair<std::string, int>, std::vector<const RoadResult::Prop*>> groups;
    for (const RoadResult::Prop& q : r.props)
        groups[{q.kind, q.side}].push_back(&q);
    std::set<std::string> boxed;
    for (const auto& [key, list] : groups)
    {
        const std::string& kind = key.first;
        // A library asset stands at each place, turned along the road; boxes stand in when there is none.
        if (const std::string asset = propAssets.is_object() ? propAssets.value(kind, std::string()) : std::string(); !asset.empty() && !boxed.count(kind))
        {
            try
            {
                for (const RoadResult::Prop* q : list)
                {
                    const json placed = ops::execute("place", {{"args", {asset}}, {"at", vec3(p.world(q->at))}, {"snap", false}, {"rotate", headingOf(p.direction(q->direction))}});
                    for (const json& one : placed)
                    {
                        nodes.push_back(one.at("node").get<std::string>());
                        props.push_back(one.at("node"));
                    }
                }
                continue;
            }
            catch (const std::exception& ex)
            {
                boxed.insert(kind);
                warnings.push_back(std::format("{}: {} props are boxes: library asset {} could not be placed ({})", rd.id, kind, asset, ex.what()));
            }
        }
        std::vector<std::pair<glm::dvec3, glm::dvec3>> boxes;
        if (kind == "pole" || kind == "mast")
            boxes = {{{-12.0, 0.0, -12.0}, {12.0, kind == "mast" ? 650.0 : 800.0, 12.0}}};
        else if (kind == "edgeLight")
            boxes = {{{-15.0, 0.0, -15.0}, {15.0, 35.0, 15.0}}};
        else
            boxes = {{{-6.0, 0.0, -6.0}, {6.0, 100.0, 6.0}}};
        // Runs break where props were skipped (tunnels, guardrails), so copies spread evenly only where they were evenly placed.
        std::vector<glm::dvec3> run;
        double spacing = 0.0;
        if (list.size() >= 2)
            spacing = glm::length(list[1]->at - list[0]->at);
        auto flush = [&] {
            const std::string name = base + "_" + kind + (key.second > 0 ? "L" : "R");
            if (run.size() >= 2)
                arrayRun(run, makePiece(boxes, name), name, false, nodes), props.push_back(nodes.back());
            else if (run.size() == 1)
            {
                const std::string piece = makePiece(boxes, name);
                ops::execute("setAttr", {{"args", {piece + ".translate", vec3(run[0])}}});
                nodes.push_back(piece);
                props.push_back(piece);
            }
            run.clear();
        };
        for (const RoadResult::Prop* q : list)
        {
            const glm::dvec3 w = p.world(q->at);
            if (!run.empty() && spacing > 0.0 && glm::length(w - run.back()) > spacing * 1.5 * p.across())
                flush();
            run.push_back(w);
        }
        flush();
    }
    out["props"] = props;

    std::vector<std::string> unique;
    for (const std::string& n : nodes)
        if (std::find(unique.begin(), unique.end(), n) == unique.end())
            unique.push_back(n);
    json args = json::array();
    for (const std::string& n : unique)
        args.push_back(n);
    out["group"] = ops::execute("group", {{"args", args}, {"name", base + "_Road"}});
    out["nodes"] = unique.size();
    return out;
}

// ---------------------------------------------------------------------------------------------------- export

/** A road as a curve with attributes: per point the world position, Unreal position, distance, grade, bank and span. */
json exportRoad(const Terrain& f, const RoadEntry& e, double step)
{
    const Terrain& p = f;
    const double a = p.across(), v = p.vertical();
    const RoadResult& r = e.result;
    json pts = json::array();
    for (size_t i : meshStations(r, 0, r.centreline.size() - 1, step))
    {
        const glm::dvec3 w = p.world(r.centreline[i]);
        const char* span = r.span[i] == 1 ? "bridge" : r.span[i] == 2 ? "tunnel" : r.span[i] == 3 ? "junction" : "ground";
        pts.push_back({{"position", vec3(w)},
                       {"unreal", vec3(unrealOf(w))},
                       {"along", r.along[i] * a},
                       {"grade", r.grade[i] * 100.0 * v / a},
                       {"bank", r.bank[i] * 100.0 * v / a},
                       {"curvature", r.curvature[i] / a},
                       {"width", r.formationHalf * 2.0 * a},
                       {"span", span}});
    }
    json out = roadJson(f, e.road, r, e.layer, false);
    out.erase("centreline");
    out.erase("along");
    out.erase("grade");
    out.erase("bank");
    out["points"] = pts;
    out["profileSettings"] = e.road.profile.toJson();
    out["design"] = e.road.design.toJson();
    return out;
}

// ---------------------------------------------------------------------------------------------------- Unreal

/** setTerrainRoads' road entries: Bezier splines through stations every step cm, in Unreal space relative to origin. */
json unrealRoads(const Terrain& f, const std::vector<RoadEntry>& roads, const glm::dvec3& origin, const Args& a)
{
    const Terrain& p = f;
    json out = json::array();
    for (const RoadEntry& e : roads)
    {
        const RoadResult& r = e.result;
        if (r.empty())
            continue;
        const double step = std::max(a.get<double>("spacing", 2000.0) / p.across(), r.formationHalf * 2.0);
        curve::Spline s;
        s.kind = curve::Kind::Bezier;
        std::vector<size_t> at = meshStations(r, 0, r.centreline.size() - 1, step);
        // A ring's last station is its first; Unreal closes the loop with a segment back to it.
        if (r.ring && at.size() > 3)
            at.pop_back();
        for (size_t i : at)
        {
            s.points.push_back(p.world(r.centreline[i]));
            s.tilt.push_back(std::atan(r.bank[i] * p.vertical() / p.across()));
        }
        // Catmull-Rom handles, so the spline passes smoothly through every station it keeps.
        const size_t m = s.points.size();
        for (size_t k = 0; k < m; ++k)
        {
            const bool inner = r.ring || (k > 0 && k + 1 < m);
            const glm::dvec3 prev = s.points[k > 0 ? k - 1 : (r.ring ? m - 1 : k)], next = s.points[k + 1 < m ? k + 1 : (r.ring ? 0 : k)];
            const glm::dvec3 d = (next - prev) / (inner ? 6.0 : 3.0);
            s.handleL.push_back(s.points[k] - d);
            s.handleR.push_back(s.points[k] + d);
            s.typeL.push_back(curve::HandleType::Free);
            s.typeR.push_back(curve::HandleType::Free);
        }
        const json splines = link::splinePoints({s}, origin);
        json road = {{"id", e.road.id},
                     {"label", e.road.id},
                     {"kind", e.road.kind},
                     {"halfWidth", r.formationHalf * p.across()},
                     {"pavedHalfWidth", r.pavedHalf * p.across()},
                     {"sideFalloff", a.get<double>("falloff", 500.0)},
                     {"endFalloff", a.get<double>("falloff", 500.0)},
                     {"raiseTerrain", a.get<bool>("raise", false)},
                     {"lowerTerrain", a.get<bool>("lower", false)},
                     {"layer", a.get<std::string>("paintLayer", "")},
                     {"points", splines.at(0).at("points")}};
        if (r.ring)
            road["closed"] = true;
        if (const std::string mesh = a.get<std::string>("segmentMesh", ""); !mesh.empty())
            road["mesh"] = {{"asset", mesh}, {"forwardAxis", a.get<std::string>("meshForwardAxis", "x")}, {"scaleToWidth", a.get<bool>("scaleToWidth", true)}};
        json spans = json::array();
        for (const RoadResult::Span& sp : r.spans)
            spans.push_back({{"kind", sp.kind}, {"from", r.along[sp.from] * p.across()}, {"to", r.along[sp.to] * p.across()}});
        road["spans"] = spans;
        out.push_back(road);
    }
    return out;
}

void registerRoadNode(ext::Context& ctx);
} // namespace
} // namespace roadcmd

using namespace roadcmd;

bool registerRoads(ext::Context& ctx)
{
    registerRoadNode(ctx);

    ctx.registerCommand({
        .name = "terrainAddRoad",
        .doc = "Lay a road, path, track, rail line or runway into a terrain along path= or a curve (object=, live: editing the "
               "curve re-lays it).\n\n"
               "The plan is rounded into curves of the design radius; the longitudinal profile is a chain of grades no steeper "
               "than maxGrade joined by vertical curves of at least minVerticalRadius, fitted to the smoothed ground and passing "
               "through keys=; curves bank toward their inside as designSpeed asks, capped at maxBank and ramped in. Across its "
               "width the surface is one banked plane. Cuttings and embankments slope at cutAngle and fillAngle; slopes taller "
               "than wallHeight become retaining walls; fill deeper than bridgeHeight becomes a bridge span and cut deeper than "
               "tunnelDepth a tunnel span (left to the volume; returned as tunnels with axis, radius and portals). A road meeting "
               "an earlier one in its layer joins it at grade, matching its cross fall, with kerb returns at the corners. Writes "
               "masks roadSurface, roadVerge, roadDirtEdge, roadWear and erosionProtect.\n\n"
               "Profiles: rural, highway, urban, lane, path, track, rail, runway; any field can be set by flag or settings=. "
               "Returns length, maxGrade and maxBank (percent), the radii built, cut and fill, the centreline with grade and bank, "
               "spans, bridges (deck, pillars), tunnels, walls, guardrails, props, junctions, kerb returns, warnings and the "
               "revision.",
        .positionalDoc = "terrain mesh or its modifier [curve]",
        .flags = addRoadFlags(),
        .run = [](const Args& a) -> json { return runAddRoad(a, "terrainAddRoad"); },
        .examples = {"r = terrainAddRoad('Terrain', id='passRoad', path=[[-40000, 0], [0, 8000], [40000, 2000]], maxGrade=7)\nr['maxGrade'], r['spans']",
                     "terrainAddRoad('Terrain', 'TrackCurve', kind='track', id='farmTrack')",
                     "terrainAddRoad('Terrain', kind='runway', id='strip', path=[[0, 0], [250000, 0]], build=True)"},
    });

    ctx.registerCommand({
        .name = "terrainAddRoundabout",
        .doc = "Lay a roundabout into a terrain's roads layer: a level ring road around a raised central island, its cross "
               "fall running away from the island. Roads in the layer that reach the ring are cut back to its centreline (one "
               "running through becomes two approaches) and laid after it, so each joins at grade with kerb returns and a "
               "splitter island between its entry and exit lanes. Writes the roadIsland mask besides the road masks. Returns "
               "the ring as terrainAddRoad does, with its islands and the approach ids.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"at", "at", T::List, nullptr, "Centre [x, z] or [x, y, z] in world space (x, y on a Z-up scene)."},
                  {"radius", "r", T::Float, 2500.0, "Radius of the ring's centreline, cm.", {}, {}, 100.0},
                  {"id", "id", T::String, "", "Stable id; the same id again replaces that roundabout. Empty makes one."},
                  {"layer", "ly", T::String, "Roads", "Roads layer (a Terrain Roads node)."},
                  {"roads", "rd", T::List, nullptr, "Ids of the roads to join to it; default every road with a path reaching the ring."},
                  {"profile", "pr", T::String, "urban", "Cross-section preset of the ring.", roadProfileNames()},
                  {"lanes", "ln", T::Int, 1, "Circulating lanes.", {}, {}, 1.0, 4.0},
                  {"laneWidth", "lw", T::Float, 500.0, "Circulating lane width, cm.", {}, {}, 100.0},
                  {"kerbHeight", "kh", T::Float, nullptr, "Kerb height, cm; also how high the islands stand.", {}, {}, 0.0},
                  {"crossFall", "cf", T::Float, 2.0, "Cross fall away from the island, percent.", {}, {}, 0.0, 10.0},
                  {"height", "ht", T::Float, nullptr, "Ring height along the up axis, cm; default the mean ground under it."},
                  {"detail", "dt", T::Bool, false, "Return every station, not a sample of them."}},
        .run = [](const Args& a) -> json { return runAddRoundabout(a, "terrainAddRoundabout"); },
        .examples = {"terrainAddRoad('Terrain', id='main', path=[[-40000, 0], [40000, 0]])\n"
                     "terrainAddRoad('Terrain', id='side', path=[[0, -30000], [0, 0]])\n"
                     "r = terrainAddRoundabout('Terrain', at=[0, 0], radius=2500)\nr['approaches'], r['islands']"},
    });

    ctx.registerCommand({
        .name = "terrainRoads",
        .doc = "The roads on a terrain, by layer: each road's kind and what it built on the last evaluation (as terrainAddRoad "
               "returns), and the terrain revision.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"detail", "dt", T::Bool, false, "Every station, not a sample of them."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Terrain f = firstTerrain(a, "terrainRoads");
            json layers = json::object();
            for (const RoadEntry& e : allRoads(g, f, "terrainRoads"))
            {
                if (!layers.contains(e.layer))
                    layers[e.layer] = json::array();
                layers[e.layer].push_back(roadJson(f, e.road, e.result, e.layer, a.get<bool>("detail", false)));
            }
            return {{"layers", layers}, {"revision", revision(*f.local)}};
        },
        .examples = {"terrainRoads('Terrain')['layers']['Roads'][0]['maxGrade']"},
    });

    ctx.registerCommand({
        .name = "terrainRoadRemove",
        .doc = "Remove roads from a terrain by id, or every road in a layer with all=True. Returns the ids removed.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"ids", "i", T::List, json::array(), "Road ids."}, {"layer", "ly", T::String, "Roads", "Roads layer."}, {"all", "a", T::Bool, false, "Every road in the layer."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Terrain f = firstTerrain(a, "terrainRoadRemove");
            Layers t = readGroup(g, f, "terrainRoadRemove");
            const std::string layer = layerName(a.get<std::string>("layer", "Roads"));
            gn::GNode* node = nodeNamed(t.group, layer);
            if (!node || node->type != "TerrainRoads")
                throw CommandError("terrainRoadRemove: no roads layer " + layer);
            const std::set<std::string> ids = idsFlag(a);
            json kept = json::array(), removed = json::array();
            const json roads = jsonProp(*node, "roads", json::array());
            for (const json& r : roads)
            {
                const std::string id = r.value("id", std::string());
                if (a.get<bool>("all", false) || ids.count(id))
                    removed.push_back(id);
                else
                    kept.push_back(r);
            }
            node->values["roads"] = kept.dump();
            dropUnusedSources(t.group, kRoadSourcePrefix, "TerrainRoads", {"roads"});
            commitGroup(t, "terrainRoadRemove");
            return removed;
        },
        .examples = {"terrainRoadRemove('Terrain', ids=['passRoad'])"},
    });

    ctx.registerCommand({
        .name = "terrainRoadBuild",
        .doc = "Build a terrain's roads as meshes, each under a group <id>_Road made again on every call: the surface swept along "
               "the banked centreline with the road's cross-section (extrude), bridge decks swept likewise with pillars by "
               "arrayAlongCurve, tunnel portal arches, retaining walls, junction corner patches, guardrails bent along their "
               "runs, raised islands, and markers, edge lights, poles and masts from the Set Dressing library (props=). The meshes are made from the road as it is "
               "now: call again after the terrain or the road changes. Returns the nodes made per road.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"ids", "i", T::List, json::array(), "Road ids; empty builds every road."},
                  {"layer", "ly", T::String, "", "Only this roads layer."},
                  {"props", "pp", T::Any, "library",
                   "library: Set Dressing assets Road/Delineator, Road/StreetLight, Road/EdgeLight and Road/CatenaryMast; boxes: plain "
                   "stand-ins; or {kind: asset id} for marker, pole, edgeLight and mast (the rest from the library)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Terrain f = firstTerrain(a, "terrainRoadBuild");
            const std::set<std::string> ids = idsFlag(a);
            const std::string layer = a.get<std::string>("layer", "");
            json assets = defaultPropAssets();
            if (a.has("props"))
            {
                const json& pp = a.flags["props"];
                if (pp.is_string() && pp.get<std::string>() == "boxes")
                    assets = json::object();
                else if (pp.is_object())
                    assets.update(pp);
                else if (!pp.is_string() || pp.get<std::string>() != "library")
                    throw CommandError("terrainRoadBuild: props is library, boxes or {kind: asset id}");
            }
            if (!assets.empty() && !ops::find("place"))
            {
                log::warn("terrainRoadBuild: road props are boxes: the Set Dressing (Stagehand) extension is not loaded");
                assets = json::object();
            }
            json warnings = json::array();
            json out = json::array();
            for (const RoadEntry& e : allRoads(g, f, "terrainRoadBuild"))
            {
                if ((!ids.empty() && !ids.count(e.road.id)) || (!layer.empty() && layerName(layer) != e.layer) || e.result.empty())
                    continue;
                if (Node* old = g.find(layerName(e.road.id) + "_Road"))
                    ops::execute("delete", {{"args", {old->path()}}});
                out.push_back(buildRoad(f, e, assets, warnings));
            }
            for (const json& w : warnings)
                log::warn("terrainRoadBuild: {}", w.get<std::string>());
            return out;
        },
        .examples = {"terrainRoadBuild('Terrain', ids=['passRoad'])[0]['surface']"},
    });

    ctx.registerCommand({
        .name = "terrainRoadExport",
        .doc = "Write a terrain's roads to a JSON file: per road its settings, stats, spans, bridges, tunnels, walls, junctions "
               "and props, and its curve as points with world position, Unreal position (cm, Unreal axes), distance along, grade "
               "and bank (percent), curvature, width and span (ground, bridge, tunnel, junction). terrainExport writes it next to "
               "the heights. Returns the file and the road count.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"file", "f", T::String, "", "Output .json; default <folder>/<name>_roads.json."},
                  {"folder", "fd", T::String, "", "Folder for the default file name."},
                  {"name", "n", T::String, "Terrain", "Stem for the default file name."},
                  {"spacing", "sp", T::Float, 500.0, "Distance between curve points, cm.", {}, {}, 1.0},
                  {"ifAny", "ia", T::Bool, false, "Write nothing (and return roads 0) when the terrain has no roads."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Terrain f = firstTerrain(a, "terrainRoadExport");
            const std::vector<RoadEntry> roads = allRoads(g, f, "terrainRoadExport");
            if (roads.empty() && a.get<bool>("ifAny", false))
                return {{"roads", 0}};
            fs::path file = a.get<std::string>("file", "");
            if (file.empty())
                file = fs::path(a.get<std::string>("folder", "")) / (a.get<std::string>("name", "Terrain") + "_roads.json");
            json list = json::array();
            for (const RoadEntry& e : roads)
                if (!e.result.empty())
                    list.push_back(exportRoad(f, e, std::max(1.0, a.get<double>("spacing", 500.0)) / f.across()));
            const json doc = {{"terrain", f.path("")}, {"units", "cm"}, {"upAxis", zUp() ? "z" : "y"}, {"revision", revision(*f.local)}, {"roads", list}};
            std::error_code ec;
            if (file.has_parent_path())
                fs::create_directories(file.parent_path(), ec);
            std::ofstream o(file, std::ios::binary);
            if (!o)
                throw CommandError("terrainRoadExport: can't write " + file.string());
            o << doc.dump(1);
            return {{"file", file.string()}, {"roads", list.size()}};
        },
        .examples = {"terrainRoadExport('Terrain', file='C:/out/roads.json')['roads']"},
    });

    ctx.registerCommand({
        .name = "terrainSendRoadsToUnreal",
        .doc = "Send a terrain's roads to the connected Unreal editor as landscape splines on its landscape (the one "
               "terrainSendToUnreal made, or landscape=): a control point every spacing cm with the road's width, bank (roll) "
               "and side falloff, segments carrying segmentMesh as spline meshes, paintLayer as the layer they paint, and "
               "raise/lower for whether they deform the landscape (off by default: Lila's heights already hold the road). "
               "kind='meshTerrain' sends them to the Mesh Terrain terrainSendToUnreal(kind='meshTerrain') made instead, as Mesh "
               "Partition spline modifiers (needs the LilaLinkMeshTerrain plugin): a plateau of the road's half width, the "
               "falloff beyond, paintLayer as the channel they write. Sending again replaces them. Returns what LilaLink made.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"kind", "k", T::String, "landscape", "landscape: landscape splines; meshTerrain: Mesh Partition spline modifiers.", {"landscape", "meshTerrain"}},
                  {"landscape", "ls", T::String, "", "Landscape label in Unreal; default the terrain's name, as terrainSendToUnreal names it."},
                  {"segmentMesh", "sm", T::String, "", "Static mesh asset each segment carries, such as /Game/Roads/SM_Road."},
                  {"meshForwardAxis", "mfa", T::String, "x", "The segment mesh's forward axis.", {"x", "y", "z"}},
                  {"scaleToWidth", "stw", T::Bool, true, "Scale the segment mesh to the road's width."},
                  {"paintLayer", "pl", T::String, "", "Landscape paint layer the splines paint; empty paints none."},
                  {"raise", "r", T::Bool, false, "Splines raise the landscape to the road."},
                  {"lower", "l", T::Bool, false, "Splines lower the landscape to the road."},
                  {"falloff", "fo", T::Float, 500.0, "Side and end falloff, cm.", {}, {}, 0.0},
                  {"spacing", "sp", T::Float, 2000.0, "Distance between control points, cm.", {}, {}, 100.0},
                  {"ifAny", "ia", T::Bool, false, "Do nothing (and return roads 0) when the terrain has no roads."},
                  {"timeout", "to", T::Float, 120.0, "Seconds to wait for Unreal.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Terrain f = firstTerrain(a, "terrainSendRoadsToUnreal");
            const std::vector<RoadEntry> roads = allRoads(g, f, "terrainSendRoadsToUnreal");
            if (roads.empty())
            {
                if (a.get<bool>("ifAny", false))
                    return {{"roads", 0}};
                throw CommandError("terrainSendRoadsToUnreal: the terrain has no roads (terrainAddRoad)");
            }
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainSendRoadsToUnreal: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            const bool meshTerrain = a.get<std::string>("kind", "landscape") == "meshTerrain";
            const std::string feature = meshTerrain ? "meshTerrainRoads" : "terrainRoads";
            if (!client || !features.is_array() || std::find(features.begin(), features.end(), json(feature)) == features.end())
                throw CommandError(meshTerrain ? "terrainSendRoadsToUnreal: Mesh Terrain roads need the LilaLinkMeshTerrain plugin in Unreal, built from "
                                                 "Lila's ue/LilaLinkMeshTerrain"
                                               : "terrainSendRoadsToUnreal: the LilaLink plugin in Unreal predates terrain roads; rebuild it from Lila's ue/LilaLink");
            std::string landscape = a.get<std::string>("landscape", "");
            if (landscape.empty())
                landscape = layerName(f.mesh ? f.mesh->name() : std::string("Terrain"));
            const glm::dvec3 origin = f.world(f.local->origin.x + f.local->extent().x * 0.5, 0.0, f.local->origin.y + f.local->extent().y * 0.5);
            const json params = {{"id", f.path("")}, {"landscape", landscape}, {"location", vec3(unrealOf(origin))}, {"roads", unrealRoads(f, roads, origin, a)}};
            const int timeoutMs = int(std::clamp(a.get<double>("timeout", 120.0), 1.0, 3600.0) * 1000.0);
            link::RpcResult r = client->call(meshTerrain ? "setMeshTerrainRoads" : "setTerrainRoads", params, timeoutMs);
            if (!r.ok)
                throw CommandError("terrainSendRoadsToUnreal: " + r.error);
            json out = r.result;
            out["roads"] = params["roads"].size();
            const json warnings = out.value("warnings", json::array());
            for (const json& w : warnings)
                log::warn("terrainSendRoadsToUnreal: {}", w.get<std::string>());
            return out;
        },
        .examples = {"terrainSendRoadsToUnreal('Terrain', segmentMesh='/Game/Roads/SM_RoadSegment', paintLayer='Road')"},
    });

    actions::add(ctx, {.id = "terrain.addRoad",
                       .label = "Road from Curve",
                       .category = "Terrain",
                       .doc = "Lay a road along the selected curve into the selected terrain.",
                       .python = "terrainAddRoad('Terrain', 'RoadCurve', kind='road')",
                       .command = "terrainAddRoad",
                       .call = json::object()});
    actions::add(ctx, {.id = "terrain.buildRoads",
                       .label = "Build Road Meshes",
                       .category = "Terrain",
                       .doc = "Make the selected terrain's road surfaces, bridges, walls and props as meshes.",
                       .python = "terrainRoadBuild('Terrain')",
                       .command = "terrainRoadBuild",
                       .call = json::object()});
    menus::addItem(ctx, {.id = "terrain.menu.roadsSep", .menu = "Level/Terrain", .kind = menus::Kind::Separator});
    menus::addItem(ctx, {.id = "terrain.menu.terrain.addRoad", .menu = "Level/Terrain", .action = "terrain.addRoad"});
    menus::addItem(ctx, {.id = "terrain.menu.terrain.buildRoads", .menu = "Level/Terrain", .action = "terrain.buildRoads"});
    return true;
}

namespace roadcmd
{
namespace
{
void registerRoadNode(ext::Context& ctx)
{
    gn::NodeDecl d{
        .type = "TerrainRoads",
        .label = "Terrain Roads",
        .category = "Terrain",
        .doc = "Roads, paths, tracks, rail lines and runways laid into the heightfield in order: graded with vertical curves, banked "
               "into curves, cut and filled at their talus angles with retaining walls, bridge and tunnel spans left open, joined "
               "at junctions, and painted into the masks roadSurface, roadVerge, roadDirtEdge, roadWear and erosionProtect. A road "
               "follows a path or a curve through an Object Info node in this group. terrainAddRoad writes the list.",
        .props = {{.id = "roads", .defaultValue = "[]", .doc = "The roads, a JSON list in the heightfield's own space (see terrainAddRoad)."}},
        .sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}},
        .exec =
            [](gn::ExecContext& c) {
                LILA_ZONE();

                gn::GeometryPtr g = c.geometry("heightfield");
                const Heightfield* src = heightfieldOf(*g);
                if (!src)
                    throw gn::ExecError(c.node.name + ": the geometry has no heightfield (start from Heightfield Grid or Mesh to Heightfield)");
                Heightfield h = *src;
                json list = json::array();
                if (const std::string text = c.prop("roads"); !text.empty())
                {
                    list = json::parse(text, nullptr, false);
                    if (!list.is_array())
                        throw gn::ExecError(c.node.name + ": roads is not a JSON list");
                }
                std::vector<RoadInput> inputs;
                std::map<std::string, RoadResult> lost;
                for (const json& j : list)
                {
                    RoadInput in;
                    try
                    {
                        in.road = Road::fromJson(j);
                    }
                    catch (const std::exception& e)
                    {
                        throw gn::ExecError(c.node.name + ": " + e.what());
                    }
                    if (!in.road.object.empty())
                    {
                        auto it = c.env.objects.find(in.road.object);
                        const Footprint fp = it != c.env.objects.end() && it->second.curves
                                                 ? footprintFromCurves(*it->second.curves, glm::inverse(c.env.ownerMatrix) * it->second.matrix)
                                                 : Footprint{};
                        if (fp.lines.empty())
                        {
                            lost[in.road.id].warnings.push_back("its curve is gone or empty: the road was not laid");
                            continue;
                        }
                        in.line = fp.lines.front();
                    }
                    else
                        in.line = in.road.path;
                    inputs.push_back(std::move(in));
                }
                std::map<std::string, RoadResult> results = applyRoads(h, inputs);
                results.merge(lost);
                publishRoadResults(c.env.owner, c.node.name, std::move(results));
                gn::Geometry out = *g;
                out.heightfield = std::make_shared<const Heightfield>(std::move(h));
                c.out("heightfield", gn::makeGeometry(std::move(out)));
            },
    };
    gn::registerDecl(ctx, std::move(d));
}
} // namespace
} // namespace roadcmd
} // namespace terrain
