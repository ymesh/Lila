// Starting terrains from the real world and from drawings: fetching elevation by area or place name, OpenStreetMap
// features and land cover, drawn constraints erosion keeps, bare earth and amplification nodes, and the live link that
// re-runs a terrain when its source file is saved.

#include "DemRefine.h"
#include "Fetch.h"
#include "Http.h"
#include "Inputs.h"
#include "LandCover.h"
#include "Osm.h"
#include "Painted.h"
#include "Projection.h"
#include "Stamps.h"
#include "Terrain.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Names.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Transform.h>
#include <lila/Workspace.h>

#include <QApplication>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace inputcmd
{
using scene::Graph;
using scene::Node;
using T = ArgType;
namespace fs = std::filesystem;
using gn::ExecContext;
using gn::GeometryPtr;
using Socket = gn::SocketType;

constexpr const char* kPrefsFile = "terrain";
constexpr const char* kKeyPref = "/openTopography/apiKey";

// ------------------------------------------------------------------------------------------------- node helpers

gn::SocketDecl sock(std::string id, Socket t, json def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    gn::SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    s.max = hi;
    return s;
}

gn::SocketDecl hfSocket() { return {.id = "heightfield", .type = Socket::Geometry}; }

const Heightfield& heightfieldIn(const ExecContext& c, const GeometryPtr& g)
{
    if (const Heightfield* h = heightfieldOf(*g))
        return *h;
    throw gn::ExecError(c.node.name + ": the geometry has no heightfield (start from Heightfield Grid or Mesh to Heightfield)");
}

GeometryPtr replaceHeightfield(const GeometryPtr& g, Heightfield h)
{
    gn::Geometry out = *g;
    out.heightfield = std::make_shared<const Heightfield>(std::move(h));
    return gn::makeGeometry(std::move(out));
}

json listValue(const std::string& text)
{
    if (text.empty())
        return json::array();
    json j = json::parse(text, nullptr, false);
    return j.is_array() ? j : json::array();
}

/** The extract of an OSM file, read once per modification time. */
std::shared_ptr<const OsmExtract> cachedOsm(const fs::path& file)
{
    static std::mutex lock;
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const OsmExtract>>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec)
        throw std::runtime_error("no file " + file.string());
    std::lock_guard guard(lock);
    auto it = cache.find(file.string());
    if (it != cache.end() && it->second.first == stamp)
        return it->second.second;
    auto x = std::make_shared<const OsmExtract>(extractOsm(readOsm(file)));
    cache[file.string()] = {stamp, x};
    return x;
}

gn::NodeDecl constraintsDecl()
{
    return {
        .type = "TerrainConstraints",
        .label = "Terrain Constraints",
        .category = "Terrain",
        .doc = "Drawn ridges, valleys, cliffs, rivers and flat ground shape the heightfield and are held while Erode Terrain runs, "
               "so erosion carves around them instead of through them. Each follows a path or a curve through an Object Info "
               "node in this group (moving the curve moves it). Rivers are drawn downstream: their bed only falls along the "
               "curve. terrainConstrain writes the list; put this before Erode Terrain.",
        .props = {{.id = "constraints", .defaultValue = "[]", .doc = "The constraints, a JSON list in the heightfield's own space (see terrainConstrain)."}},
        .sockets = {.inputs = {hfSocket()}, .outputs = {hfSocket()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = heightfieldIn(c, g);
                for (const json& j : listValue(c.prop("constraints")))
                {
                    Constraint k;
                    try
                    {
                        k = Constraint::fromJson(j);
                    }
                    catch (const std::exception& e)
                    {
                        throw gn::ExecError(c.node.name + ": " + e.what());
                    }
                    std::vector<std::vector<glm::dvec2>> lines;
                    if (!k.object.empty())
                    {
                        auto it = c.env.objects.find(k.object);
                        if (it == c.env.objects.end() || !it->second.curves)
                            continue;
                        lines = footprintFromCurves(*it->second.curves, glm::inverse(c.env.ownerMatrix) * it->second.matrix).lines;
                    }
                    else
                        lines.push_back(k.path);
                    applyConstraint(h, k, lines);
                }
                c.out("heightfield", replaceHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl landcoverDecl()
{
    return {
        .type = "OsmLandcover",
        .label = "OSM Land Cover",
        .category = "Terrain",
        .doc = "OpenStreetMap land cover (.osm, Overpass .json or .osm.pbf) as masks landcover_forest, landcover_grassland, "
               "landcover_farmland, landcover_scrub, landcover_rock, landcover_sand, landcover_urban, landcover_wetland, "
               "landcover_water and landcover_glacier, placed by the georeference terrainImportOsm stored. Re-read when the "
               "file changes.",
        .sockets = {.inputs = {hfSocket(), sock("file", Socket::String, "", "OSM file; relative paths are under the workspace."),
                               sock("georef", Socket::String, "", "Where the heightfield is on the earth (JSON, from terrainGeoref).")},
                    .outputs = {hfSocket()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = heightfieldIn(c, g);
                const std::string file = c.s("file");
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no file");
                try
                {
                    const json geo = json::parse(c.s("georef"));
                    rasterizeLandcover(h, cachedOsm(paths::resolvePath(file))->areas, Georef::fromJson(geo));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                c.out("heightfield", replaceHeightfield(g, std::move(h)));
            },
    };
}

/** A land cover raster's part over a heightfield, read once per file time and window. */
std::shared_ptr<const Dem> cachedLandCover(const fs::path& file, const Heightfield& h, const Georef& g)
{
    static std::mutex lock;
    struct Entry
    {
        fs::file_time_type stamp;
        std::array<int, 4> window;
        std::shared_ptr<const Dem> codes;
    };
    static std::map<std::string, Entry> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec)
        throw std::runtime_error("no file " + file.string());
    const GeoTiffWindowReader r(fileByteRange(file), file.filename().string());
    const std::array<int, 4> w = rasterWindow(r.info(), h, g);
    if (w[0] == w[2])
        throw std::runtime_error(file.filename().string() + " doesn't cover the terrain");
    std::lock_guard guard(lock);
    auto it = cache.find(file.string());
    if (it != cache.end() && it->second.stamp == stamp && it->second.window == w)
        return it->second.codes;
    auto codes = std::make_shared<const Dem>(r.window(w[0], w[1], w[2], w[3]));
    cache[file.string()] = {stamp, w, codes};
    return codes;
}

gn::NodeDecl readLandCoverDecl()
{
    return {
        .type = "ReadLandCover",
        .label = "Read Land Cover",
        .category = "Terrain",
        .doc = "ESA WorldCover class codes from a GeoTIFF (degrees or UTM) onto the heightfield by nearest sample: a mask of the "
               "codes (code / 255, what terrainBiomes' landcover reads) and the landcover_<class> masks OSM Land Cover writes. "
               "Only the part of the file over the terrain is read. terrainImportLandCover sets it up.",
        .sockets = {.inputs = {hfSocket(), sock("file", Socket::String, "", "Land cover GeoTIFF; relative paths are under the workspace."),
                               sock("georef", Socket::String, "", "Where the heightfield is on the earth (JSON, from terrainGeoref)."),
                               sock("mask", Socket::String, "worldcover", "Mask for the class codes."),
                               sock("classes", Socket::Bool, true, "Also write the landcover_<class> masks.")},
                    .outputs = {hfSocket()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = heightfieldIn(c, g);
                const std::string file = c.s("file"), mask = c.s("mask");
                if (file.empty())
                    throw gn::ExecError(c.node.name + ": no file");
                if (mask.empty())
                    throw gn::ExecError(c.node.name + ": no mask name");
                try
                {
                    const Georef geo = Georef::fromJson(json::parse(c.s("georef")));
                    rasterizeWorldCover(h, *cachedLandCover(paths::resolvePath(file), h, geo), geo, mask, c.b("classes"));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                c.out("heightfield", replaceHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl bareEarthDecl()
{
    const BareEarthParams p;
    return {
        .type = "BareEarth",
        .label = "Bare Earth",
        .category = "Terrain",
        .doc = "Surface models (Copernicus GLO-30, SRTM) measure the tops of trees and roofs. This takes down what stands up "
               "from the ground around it, up to radius wide and maxHeight tall, and smooths it into the ground; taller or wider "
               "shapes stay. Limit it to a mask such as landcover_forest. Writes mask bareEarth.",
        .sockets = {.inputs = {hfSocket(), sock("radius", Socket::Float, p.radius, "Widest object removed, cm.", 0.0),
                               sock("maxHeight", Socket::Float, p.maxHeight, "Tallest object removed, cm.", 0.0),
                               sock("mask", Socket::String, "", "Only where this mask is; empty is everywhere.")},
                    .outputs = {hfSocket()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = heightfieldIn(c, g);
                BareEarthParams p;
                p.radius = std::max(0.0, c.f("radius"));
                p.maxHeight = std::max(0.0, c.f("maxHeight"));
                p.mask = c.s("mask");
                bareEarth(h, p);
                c.out("heightfield", replaceHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl amplifyDecl()
{
    const AmplifyParams p;
    return {
        .type = "AmplifyTerrain",
        .label = "Amplify Terrain",
        .category = "Terrain",
        .doc = "Refines coarse data (30 m DEMs) to game scale: each doubling of resolution adds slope-scaled detail and erodes at "
               "the finer cell, keeping the original shape, so real places gain gullies and ridgelets. Factor 4 turns 30 m cells "
               "into 7.5 m. Seconds per million samples.",
        .sockets = {.inputs = {hfSocket(), sock("factor", Socket::Int, int64_t(p.factor), "Samples per original sample along a side: 2, 4 or 8.", 1.0, 16.0),
                               sock("detail", Socket::Float, p.detail, "Strength of the new detail.", 0.0),
                               sock("iterations", Socket::Int, int64_t(p.iterations), "Erosion steps per doubling.", 0.0, 500.0),
                               sock("seed", Socket::Int, int64_t(0), "Seed for the detail.")},
                    .outputs = {hfSocket()}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                AmplifyParams p;
                p.factor = int(std::clamp<int64_t>(c.i("factor"), 1, 16));
                p.detail = std::max(0.0, c.f("detail"));
                p.iterations = int(std::clamp<int64_t>(c.i("iterations"), 0, 500));
                p.seed = uint32_t(c.i("seed"));
                try
                {
                    c.out("heightfield", replaceHeightfield(g, amplify(heightfieldIn(c, g), p)));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
            },
    };
}

// ------------------------------------------------------------------------------------------------- scene helpers

bool zUpScene() { return math::upAxis() == math::UpAxis::Z; }

/** A terrain as these commands need it: its mesh, modifier, heightfield (own space) and world matrix. */
struct TerrainRef
{
    Node* mesh = nullptr;
    Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> local;
    glm::dmat4 world{1.0};

    glm::dvec3 toWorld(double x, double y, double z) const { return glm::dvec3(world * glm::dvec4(x, y, z, 1.0)); }
    glm::dvec2 toLocal(const glm::dvec3& w) const
    {
        const glm::dvec4 q = glm::inverse(world) * glm::dvec4(w, 1.0);
        return {q.x, q.z};
    }
};

TerrainRef findTerrainRef(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    g.evaluate();
    TerrainRef t;
    if (gn::isGnModifier(*n))
    {
        t.modifier = n;
        t.local = gn::modifierHeightfield(n->id());
        const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
        t.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
    }
    else
    {
        t.mesh = n;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend() && !t.local; ++it)
            if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()))
                {
                    t.modifier = m;
                    t.local = std::move(h);
                }
    }
    if (!t.local || t.local->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    t.world = t.mesh ? t.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    return t;
}

std::string terrainArg(const Args& a, const std::string& cmd, const char* flag = nullptr)
{
    if (flag && a.has(flag))
        return a.get<std::string>(flag, "");
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    return names[0];
}

/** Files passed positionally; never the selection, which objects() falls back to. */
std::vector<std::string> givenFiles(const Args& a)
{
    std::vector<std::string> out;
    for (const json& v : a.positional)
        if (v.is_string())
            out.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(x.get<std::string>());
    return out;
}

/** Where terrain data that belongs with the work goes: the workspace's terrain folder, else beside the scene, else temp. */
fs::path terrainDataDir()
{
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain";
    const fs::path& sceneFile = io::currentPath();
    if (!sceneFile.empty())
        return sceneFile.parent_path() / (sceneFile.stem().string() + "_terrain");
    return fs::temp_directory_path() / "lila_terrain";
}

fs::path resolvedFile(const std::string& f)
{
    fs::path p = paths::fromUtf8(f);
    if (p.is_relative())
        if (const fs::path& root = Workspace::get().root(); !root.empty())
            p = root / p;
    return p;
}

std::string safeName(std::string s)
{
    for (char& ch : s)
        if (!std::isalnum((unsigned char)ch) && ch != '-' && ch != '_')
            ch = '_';
    return s.empty() ? std::string("terrain") : s;
}

/** The heightfield chain's link into node.socket (a node's heightfield input, or GroupOutput's geometry). */
gn::GLink* linkInto(gn::Group& grp, const std::string& node, const std::string& socket)
{
    for (gn::GLink& l : grp.links)
        if (l.toNode == node && l.toSocket == socket)
            return &l;
    return nullptr;
}

gn::GNode* nodeOfType(gn::Group& grp, const std::string& type)
{
    for (gn::GNode& n : grp.nodes)
        if (n.type == type)
            return &n;
    return nullptr;
}

gn::GNode* nodeNamed(gn::Group& grp, const std::string& name)
{
    for (gn::GNode& n : grp.nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

/** Adds a heightfield node on the link into before.socket (before = GroupOutput, socket geometry, for the end). */
gn::GNode& insertOnChain(gn::Group& grp, const std::string& type, const std::string& name, const std::string& before, const std::string& socket, const std::string& cmd)
{
    gn::GLink* l = linkInto(grp, before, socket);
    if (!l)
        throw CommandError(cmd + ": the terrain's group has no heightfield running into " + before);
    const gn::GNode* to = nodeNamed(grp, before);
    const gn::GNode* from = nodeNamed(grp, l->fromNode);
    const glm::dvec2 at = to && from ? (to->position + from->position) * 0.5 + glm::dvec2(0.0, 160.0) : glm::dvec2(0.0);
    const std::string fromName = l->fromNode, fromSocket = l->fromSocket;
    l->fromNode = name;
    l->fromSocket = "heightfield";
    grp.links.push_back({fromName, fromSocket, name, "heightfield"});
    gn::GNode n;
    n.name = name;
    n.type = type;
    n.position = at;
    grp.nodes.push_back(std::move(n));
    return grp.nodes.back();
}

/** The node after which the source stage ends: the first heightfield node after the group's source, before erosion. */
std::string afterSource(gn::Group& grp, const std::string& source)
{
    // Follow the chain from the source while the next node is one of these refining steps.
    std::string cur = source;
    for (int guard = 0; guard < 64; ++guard)
    {
        const gn::GLink* next = nullptr;
        for (const gn::GLink& l : grp.links)
            if (l.fromNode == cur && l.fromSocket == "heightfield")
                next = &l;
        const gn::GNode* n = next ? nodeNamed(grp, next->toNode) : nullptr;
        if (!n || (n->type != "BareEarth" && n->type != "AmplifyTerrain"))
            break;
        cur = n->name;
    }
    return cur;
}

/** Where the next node goes when it should run right after cur: the target of cur's heightfield link. */
std::pair<std::string, std::string> linkFrom(gn::Group& grp, const std::string& cur)
{
    for (const gn::GLink& l : grp.links)
        if (l.fromNode == cur && l.fromSocket == "heightfield")
            return {l.toNode, l.toSocket};
    return {"GroupOutput", "geometry"};
}

struct GroupEdit
{
    Node* node = nullptr;
    gn::Group group;
};

GroupEdit editGroup(Graph& g, const TerrainRef& t, const std::string& cmd)
{
    Node* node = t.modifier ? gn::groupOfModifier(g, *t.modifier) : nullptr;
    if (!node)
        throw CommandError(cmd + ": the terrain has no node group");
    try
    {
        return {node, gn::readGroup(*node)};
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void commit(GroupEdit& e, const std::string& cmd)
{
    try
    {
        gn::commitGroup(Graph::current(), *e.node, std::move(e.group));
    }
    catch (const std::exception& ex)
    {
        throw CommandError(cmd + ": " + ex.what());
    }
}

json terrainInfo(const std::string& mesh) { return ops::execute("terrainInfo", {{"args", {mesh}}}); }

json georefJson(const Georef& g)
{
    json j = g.toJson();
    if (g.geographicOk())
    {
        const LatLon c = g.toGeographic(g.origin);
        j["originLatLon"] = {c.lat, c.lon};
    }
    return j;
}

/** The terrain's georeference: what its Read Elevation Data node published, or one around origin= [lat, lon]. */
Georef georefFor(const TerrainRef& t, const Args& a, const std::string& cmd)
{
    if (a.has("origin"))
    {
        const json o = a.flags["origin"];
        if (!o.is_array() || o.size() != 2)
            throw CommandError(cmd + ": origin is [latitude, longitude] of the terrain's centre");
        // An option box's list field hands the numbers over as text.
        auto degrees = [&](const json& v) {
            if (v.is_number())
                return v.get<double>();
            char* end = nullptr;
            const std::string text = v.is_string() ? v.get<std::string>() : std::string();
            const double d = std::strtod(text.c_str(), &end);
            if (text.empty() || end == text.c_str())
                throw CommandError(cmd + ": origin is [latitude, longitude] of the terrain's centre");
            return d;
        };
        Georef g;
        g.kind = ProjectionKind::Local;
        g.latMid = degrees(o[0]);
        g.x0 = degrees(o[1]);
        g.y0 = g.latMid;
        const glm::dvec2 centre = t.local->origin + t.local->extent() * 0.5;
        // One degree in metres at that latitude, as Local data is flattened.
        const double p = g.latMid * 0.017453292519943295;
        g.mx = 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p);
        g.my = 111132.92 - 559.82 * std::cos(2 * p) + 1.175 * std::cos(4 * p);
        g.origin = centre;
        return g;
    }
    Georef g;
    if (!t.modifier || !publishedGeoref(t.modifier->id(), &g))
        throw CommandError(cmd + ": " + (t.mesh ? t.mesh->path() : std::string("the terrain")) +
                           " isn't from real elevation data; give origin=[latitude, longitude] of its centre");
    if (!g.geographicOk())
        throw CommandError(cmd + ": the terrain's data is in a projection Lila doesn't convert (EPSG " + std::to_string(g.foreignEpsg) +
                           "); import the data in degrees, or give origin=");
    return g;
}

GeoBox boxOf(const Heightfield& h, const Georef& g)
{
    GeoBox b{1e9, 1e9, -1e9, -1e9};
    const glm::dvec2 lo = h.origin, hi = h.origin + h.extent();
    for (int i = 0; i <= 8; ++i)
        for (int j = 0; j <= 8; ++j)
        {
            const LatLon p = g.toGeographic(lo + (hi - lo) * glm::dvec2(i / 8.0, j / 8.0));
            b.west = std::min(b.west, p.lon);
            b.east = std::max(b.east, p.lon);
            b.south = std::min(b.south, p.lat);
            b.north = std::max(b.north, p.lat);
        }
    return b;
}

Downloader networkDownloader()
{
    return [](const std::string& url, const fs::path& dest, std::string* why) { return httpGet(url, dest, why); };
}

/** One place-name search, at most one a second, cached, as the Nominatim usage policy asks. */
Place lookUpPlace(const std::string& query, const fs::path& cacheDir, bool offline, const std::string& cmd)
{
    const fs::path cacheFile = cacheDir / "places.json";
    json cache = json::object();
    if (std::ifstream f(cacheFile); f)
        cache = json::parse(f, nullptr, false);
    if (!cache.is_object())
        cache = json::object();
    std::string text;
    if (cache.contains(query) && cache[query].is_string())
        text = cache[query].get<std::string>();
    else
    {
        if (offline)
            throw CommandError(cmd + ": offline and '" + query + "' was never looked up; give bbox=");
        static std::mutex lock;
        static std::chrono::steady_clock::time_point last{};
        std::lock_guard guard(lock);
        const auto wait = last + std::chrono::milliseconds(1100) - std::chrono::steady_clock::now();
        if (wait.count() > 0)
            std::this_thread::sleep_for(wait);
        std::string why;
        const int status = httpGetText(nominatimUrl(query), &text, &why);
        last = std::chrono::steady_clock::now();
        if (status != 200)
            throw CommandError(cmd + ": the place search failed (" + (status ? std::to_string(status) : std::string("no connection")) + (why.empty() ? "" : ": " + why) + ")");
        cache[query] = text;
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        std::ofstream(cacheFile) << cache.dump(1);
    }
    std::vector<Place> found;
    try
    {
        found = parseNominatim(text);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
    if (found.empty())
        throw CommandError(cmd + ": no place called '" + query + "'");
    return found.front();
}

/** The world point of a [x, z] (or [x, y] on a Z-up scene) ground pair, or of an [x, y, z] point. */
glm::dvec3 worldOf(const json& p)
{
    if (!p.is_array() || p.size() < 2)
        throw CommandError("points are [x, z] or [x, y, z]");
    if (p.size() >= 3)
        return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
    return zUpScene() ? glm::dvec3(p[0].get<double>(), p[1].get<double>(), 0.0) : glm::dvec3(p[0].get<double>(), 0.0, p[1].get<double>());
}

/** Source nodes that read a file, in a terrain's group. */
bool readsFile(const gn::GNode& n)
{
    return n.type == "ImportTerrainImage" || n.type == "ReadElevationData" || n.type == "OsmLandcover" || n.type == "ReadLandCover" || n.type == "ReadHeightfield";
}

/** A source node's file: its own value, or the modifier's exposed input feeding it. */
fs::path fileOf(const gn::Group& grp, const gn::GNode& n, const Node* modifier)
{
    for (const gn::GLink& l : grp.links)
        if (l.toNode == n.name && l.toSocket == "file" && l.fromNode == "GroupInput" && modifier)
        {
            const json v = ops::execute("getAttr", {{"args", {modifier->path() + "." + l.fromSocket}}});
            return v.is_string() ? paths::resolvePath(v.get<std::string>()) : fs::path();
        }
    auto it = n.values.find("file");
    return it != n.values.end() && it->is_string() && !it->get<std::string>().empty() ? paths::resolvePath(it->get<std::string>()) : fs::path();
}

int64_t stampOf(const fs::path& f)
{
    std::error_code ec;
    const auto t = fs::last_write_time(f, ec);
    return ec ? 0 : int64_t(t.time_since_epoch().count());
}

/** Writes each file-reading node's current file time into its values, so the group re-runs it. Returns the files. */
json restamp(GroupEdit& e, const Node* modifier, std::optional<bool> live)
{
    json files = json::array();
    for (gn::GNode& n : e.group.nodes)
    {
        if (!readsFile(n))
            continue;
        const fs::path f = fileOf(e.group, n, modifier);
        n.values["fileStamp"] = stampOf(f);
        if (live)
        {
            if (*live)
                n.values["liveLink"] = true;
            else
                n.values.erase("liveLink");
        }
        files.push_back({{"node", n.name}, {"file", paths::toUtf8(f)}, {"live", n.values.value("liveLink", false)}});
    }
    return files;
}

// ---------------------------------------------------------------------------------------------------- live link

/** Polls live-linked source files once a second and runs terrainReload when one was saved. Polling, unlike a file
 * watcher, survives painting apps that save by writing a new file and renaming it over the old one. */
void pollLiveLinks()
{
    static bool busy = false;
    if (busy)
        return;
    busy = true;
    try
    {
        Graph& g = Graph::current();
        std::set<std::string> reload;
        for (const std::string& name : gn::groupNames(g))
        {
            Node* gnode = gn::findGroupNode(g, name);
            if (!gnode)
                continue;
            gn::Group grp;
            try
            {
                grp = gn::readGroup(*gnode);
            }
            catch (...)
            {
                continue;
            }
            for (const gn::GNode& n : grp.nodes)
            {
                if (!readsFile(n) || !n.values.value("liveLink", false))
                    continue;
                for (scene::NodeId id : gn::instancesOf(g, gnode->id()))
                {
                    const Node* mod = g.node(id);
                    const fs::path f = fileOf(grp, n, mod);
                    if (!f.empty() && stampOf(f) != n.values.value("fileStamp", int64_t(0)) && stampOf(f) != 0)
                        reload.insert(mod->path());
                }
            }
        }
        for (const std::string& mod : reload)
        {
            log::info("terrain live link: {} changed on disk, re-running", mod);
            ops::execute("terrainReload", {{"args", {mod}}});
        }
    }
    catch (const std::exception& e)
    {
        log::warn("terrain live link: {}", e.what());
    }
    busy = false;
}

// ---------------------------------------------------------------------------------------------------- commands

json runImportDem(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainImportDem";
    ProjectionKind projection = ProjectionKind::Local;
    if (!parseProjection(a.get<std::string>("projection", "utm"), &projection))
        throw CommandError(cmd + ": projection is local, utm or enu");
    const fs::path dataDir = terrainDataDir();
    const fs::path cacheDir = a.has("cache") ? resolvedFile(a.get<std::string>("cache", "")) : dataDir / "dem_cache";
    json fetched = nullptr;
    fs::path file;
    std::string sourceName = "file";
    std::vector<std::string> notes;
    std::string attribution;
    fs::path attributionFile;

    const std::vector<std::string> pos = givenFiles(a);
    if (!pos.empty())
    {
        file = resolvedFile(pos[0]);
        if (!fs::exists(file))
            throw CommandError(cmd + ": no file " + file.string());
    }
    else
    {
        DemSource source = DemSource::Copernicus;
        sourceName = a.get<std::string>("source", "copernicus");
        if (!parseDemSource(sourceName, &source))
            throw CommandError(cmd + ": source is copernicus, terrarium or opentopography");
        const bool offline = a.get<bool>("offline", false);
        GeoBox box;
        json place = nullptr;
        if (a.has("bbox"))
        {
            const json b = a.flags["bbox"];
            if (!b.is_array() || b.size() != 4)
                throw CommandError(cmd + ": bbox is [west, south, east, north] in degrees");
            box = {b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
        }
        else if (a.has("place"))
        {
            const Place p = lookUpPlace(a.get<std::string>("place", ""), cacheDir, offline, cmd);
            const json size = a.flags.value("size", json::array({1000000.0, 1000000.0}));
            const double w = size.is_array() && size.size() == 2 ? size[0].get<double>() : 1000000.0;
            const double d = size.is_array() && size.size() == 2 ? size[1].get<double>() : 1000000.0;
            box = w > 0.0 && d > 0.0 ? GeoBox::around(p.at, w * 0.01, d * 0.01) : p.box;
            place = {{"name", p.name}, {"latLon", {p.at.lat, p.at.lon}}, {"boundingBox", {p.box.west, p.box.south, p.box.east, p.box.north}}};
        }
        else
            throw CommandError(cmd + ": give a file, bbox=[west, south, east, north] or place='name'");
        if (!box.valid())
            throw CommandError(cmd + ": the area is not a valid box in degrees");

        FetchRequest r;
        r.source = source;
        r.box = box;
        r.spacing = a.get<double>("spacing", 3000.0) * 0.01;
        r.dataset = a.get<std::string>("dataset", "COP30");
        r.cache = cacheDir;
        r.offline = offline;
        std::string keyNote;
        try
        {
            r.apiKey = chooseApiKey(a.get<std::string>("apiKey", ""), PrefStore::get().value<std::string>(kPrefsFile, kKeyPref, std::string()), &keyNote);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what());
        }
        if (a.has("apiKey") && !r.apiKey.empty() && a.get<bool>("saveKey", false))
            PrefStore::get().set(kPrefsFile, kKeyPref, r.apiKey);
        FetchResult res;
        try
        {
            res = fetchDem(r, networkDownloader());
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what() + (r.apiKey.empty() && !keyNote.empty() ? "; " + keyNote : std::string()));
        }
        notes = res.notes;
        for (const std::string& n : notes)
            log::warn("{}: {}", cmd, n);
        attribution = res.attribution;
        char tag[96];
        std::snprintf(tag, sizeof tag, "%s_%.4f_%.4f_%.4f_%.4f", sourceName.c_str(), box.west, box.south, box.east, box.north);
        file = dataDir / "dem" / (safeName(tag) + ".tif");
        try
        {
            writeGeoTiff(file, res.dem);
            const std::string title = sourceName == "copernicus" ? "Copernicus DEM GLO-30" : sourceName == "terrarium" ? "AWS Terrain Tiles" : "OpenTopography " + r.dataset;
            attributionFile = writeAttribution(dataDir, title, attribution);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what());
        }
        fetched = {{"source", sourceName}, {"bbox", {box.west, box.south, box.east, box.north}}, {"tiles", res.tiles}, {"downloaded", res.downloaded},
                   {"cached", res.cached}, {"emptyTiles", res.empty}, {"samples", {res.dem.width, res.dem.height}}, {"file", paths::toUtf8(file)}};
        if (!place.is_null())
            fetched["place"] = place;
    }

    json call = {{"args", {paths::toUtf8(file)}}, {"mode", "elevation"}, {"name", a.get<std::string>("name", "Terrain")}};
    for (const char* f : {"cellSize", "exaggeration", "fromLowest", "center"})
        if (a.has(f))
            call[f] = a.flags[f];
    json out = ops::execute("terrainImport", call);

    Graph& g = Graph::current();
    const std::string mesh = out["mesh"].get<std::string>();
    TerrainRef t = findTerrainRef(g, mesh, cmd);
    GroupEdit e = editGroup(g, t, cmd);
    gn::GNode* source = nodeOfType(e.group, "ReadElevationData");
    if (!source)
        throw CommandError(cmd + ": terrainImport made no Read Elevation Data node");
    source->values["projection"] = projectionNames()[size_t(projection)];
    source->values["zone"] = a.get<int64_t>("zone", 0);
    const std::string sourceNodeName = source->name;
    if (a.get<bool>("bareEarth", false))
    {
        const auto [to, socket] = linkFrom(e.group, afterSource(e.group, sourceNodeName));
        gn::GNode& n = insertOnChain(e.group, "BareEarth", e.group.uniqueNodeName("BareEarth"), to, socket, cmd);
        n.values["radius"] = a.get<double>("bareEarthRadius", BareEarthParams().radius);
        n.values["maxHeight"] = a.get<double>("bareEarthHeight", BareEarthParams().maxHeight);
        if (a.has("bareEarthMask"))
            n.values["mask"] = a.get<std::string>("bareEarthMask", "");
    }
    if (const int64_t factor = a.get<int64_t>("amplify", 1); factor > 1)
    {
        const auto [to, socket] = linkFrom(e.group, afterSource(e.group, sourceNodeName));
        gn::GNode& n = insertOnChain(e.group, "AmplifyTerrain", e.group.uniqueNodeName("Amplify"), to, socket, cmd);
        n.values["factor"] = factor;
        n.values["detail"] = a.get<double>("amplifyDetail", AmplifyParams().detail);
    }
    restamp(e, t.modifier, std::nullopt);
    commit(e, cmd);

    t = findTerrainRef(g, mesh, cmd);
    out = terrainInfo(mesh);
    out["mesh"] = mesh;
    out["modifier"] = t.modifier->path();
    out["file"] = paths::toUtf8(file);
    Georef geo;
    if (publishedGeoref(t.modifier->id(), &geo))
        out["georef"] = georefJson(geo);
    if (!fetched.is_null())
        out["fetched"] = fetched;
    if (!attributionFile.empty())
    {
        out["attribution"] = attribution;
        out["attributionFile"] = paths::toUtf8(attributionFile);
    }
    out["notes"] = notes;
    return out;
}

json runImportOsm(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainImportOsm";
    Graph& g = Graph::current();
    if (!a.has("terrain"))
        throw CommandError(cmd + ": give terrain= (the terrain the features lie on)");
    TerrainRef t = findTerrainRef(g, a.get<std::string>("terrain", ""), cmd);
    const Georef geo = georefFor(t, a, cmd);
    const Heightfield& hf = *t.local;
    const fs::path dataDir = terrainDataDir();

    fs::path file;
    json fetched = nullptr;
    const std::vector<std::string> pos = givenFiles(a);
    if (!pos.empty())
    {
        file = resolvedFile(pos[0]);
        if (!fs::exists(file))
            throw CommandError(cmd + ": no file " + file.string());
    }
    else if (a.get<bool>("fetch", false))
    {
        const GeoBox b = boxOf(hf, geo);
        file = dataDir / "osm" / (safeName(t.mesh ? t.mesh->name() : std::string("terrain")) + ".json");
        fs::create_directories(file.parent_path());
        std::string why;
        const int status = httpGet(overpassUrl(b), file, &why);
        if (status != 200)
        {
            std::error_code ec;
            fs::remove(file, ec);
            throw CommandError(cmd + ": the Overpass API answered " + (status ? std::to_string(status) : std::string("nothing")) + (why.empty() ? "" : ": " + why));
        }
        fetched = {{"source", "overpass"}, {"bbox", {b.west, b.south, b.east, b.north}}, {"file", paths::toUtf8(file)}};
    }
    else
        throw CommandError(cmd + ": give an OSM file (.osm, Overpass .json, .osm.pbf) or fetch=True");

    std::shared_ptr<const OsmExtract> x;
    try
    {
        x = cachedOsm(file);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }

    std::set<std::string> kinds;
    if (a.has("kinds"))
        for (const json& k : a.flags["kinds"])
        {
            const std::string name = k.get<std::string>();
            const auto& known = osmFeatureKindNames();
            if (name != "building" && name != "place" && std::ranges::find(known, name) == known.end())
                throw CommandError(cmd + ": unknown kind '" + name + "' (road, path, river, stream, lake, coastline, pier, breakwater, quay, dam, building, place)");
            kinds.insert(name);
        }
    auto wanted = [&](const std::string& kind) { return kinds.empty() || kinds.count(kind); };
    const double lift = a.get<double>("lift", 0.0);
    const double step = a.get<double>("step", 0.0) > 0.0 ? a.get<double>("step", 0.0) : hf.cell * 2.0;
    const bool perFeature = a.get<bool>("perFeature", false);
    const std::string prefix = a.get<std::string>("name", safeName(t.mesh ? t.mesh->name() : std::string("Terrain")) + "_OSM");

    // Every name comes out fictional: real ones only reach the hidden sourceNames attribute of the curve that holds them.
    std::string culture = a.get<std::string>("culture", "");
    json cultureFrom = "given";
    if (culture.empty())
    {
        std::string why;
        const LatLon mid = geo.toGeographic(hf.origin + hf.extent() * 0.5);
        culture = names::cultureForLocation(mid.lat, mid.lon, &why);
        cultureFrom = why;
    }
    else if (!names::hasCulture(culture))
    {
        std::string known;
        for (const std::string& c : names::cultures())
            known += (known.empty() ? "" : ", ") + c;
        throw CommandError(cmd + ": unknown culture '" + culture + "' (" + known + ")");
    }
    auto real = std::make_shared<names::RealNames>(names::sceneRealNames(g));
    for (const OsmFeature& f : x->features)
        real->add(f.name, "osm");
    for (const OsmBuilding& b : x->buildings)
        for (const std::string& n : b.realNames)
            real->add(n, "osm");
    for (const OsmPlace& p : x->places)
        for (const std::string& n : p.realNames)
            real->add(n, "osm");
    names::NameBook book(culture, real);
    size_t named = 0;
    auto streetClassOf = [](const OsmFeature& f) {
        const std::string& c = f.cls;
        return f.kind == OsmFeatureKind::Path                                                         ? 4
               : c.starts_with("motorway") || c.starts_with("trunk") || c.starts_with("primary") ? 0
               : c.starts_with("secondary")                                                  ? 1
               : c == "service" || c == "living_street"                                       ? 3
                                                                                              : 2;
    };
    auto featureKey = [](const OsmFeature& f) { return names::textKey(std::string(osmFeatureKindNames()[size_t(f.kind)]) + "|" + names::normalize(f.name)); };
    {
        // One name per real street: its ways can carry several classes, so it takes its highest, and streets are named in
        // key order, so the name doesn't depend on which way the data lists first. Settlement reads these names back.
        std::map<uint64_t, int> streets;
        for (const OsmFeature& f : x->features)
            if (!f.name.empty() && (f.kind == OsmFeatureKind::Road || f.kind == OsmFeatureKind::Path))
            {
                const auto [it, fresh] = streets.try_emplace(featureKey(f), streetClassOf(f));
                it->second = std::min(it->second, streetClassOf(f));
            }
        for (const auto& [key, cls] : streets)
            book.street(cls, key);
    }
    auto osmKey = [](char type, int64_t id) { return std::string(1, type) + std::to_string(id); };
    auto featureName = [&](const OsmFeature& f) -> std::string {
        if (f.name.empty())
            return {};
        ++named;
        const uint64_t key = featureKey(f);
        switch (f.kind)
        {
        case OsmFeatureKind::Road:
        case OsmFeatureKind::Path: return book.street(streetClassOf(f), key);
        case OsmFeatureKind::River: return book.place(names::Kind::River, "", key);
        case OsmFeatureKind::Stream: return book.place(names::Kind::Stream, "", key);
        case OsmFeatureKind::Lake: return book.place(f.cls == "reservoir" ? names::Kind::Reservoir : names::Kind::Lake, "", key);
        case OsmFeatureKind::Dam: return book.place(names::Kind::Dam, "", key);
        case OsmFeatureKind::Pier:
        case OsmFeatureKind::Breakwater:
        case OsmFeatureKind::Quay: return book.place(names::Kind::Port, "port", key);
        default: return book.place(names::Kind::Landmark, "", key);
        }
    };
    /** Named things, and businesses and institutions without a name in the data, get a fictional name; plain buildings none. */
    auto thingName = [&](const std::string& kind, const std::string& program, char type, int64_t id, const std::vector<std::string>& realNames,
                         const std::string& chain) -> std::string {
        names::Kind k = names::Kind::Building;
        names::parseKind(kind, &k);
        const bool institution = k != names::Kind::Building && k != names::Kind::Park;
        if (realNames.empty() && !(institution && !program.empty()) && !(k == names::Kind::Building && !program.empty() && program != "apartments"))
            return {};
        ++named;
        // Branches of one chain share a fictional chain name; named places of one real name share one too.
        const uint64_t key = !chain.empty()                                                                  ? names::textKey("chain|" + chain)
                             : (k == names::Kind::Town || k == names::Kind::District || k == names::Kind::Island) && !realNames.empty()
                                 ? names::textKey(kind + "|" + names::normalize(realNames.front()))
                                 : names::textKey(osmKey(type, id));
        return book.place(k, program, key);
    };
    auto drape = [&](const glm::dvec2& p) {
        const glm::dvec3 w = t.toWorld(p.x, hf.heightAt(p.x, p.y) + lift, p.y);
        return json::array({w.x, w.y, w.z});
    };
    auto inside = [&](const glm::dvec2& p) {
        const glm::dvec2 hi = hf.origin + hf.extent();
        return p.x >= hf.origin.x && p.y >= hf.origin.y && p.x <= hi.x && p.y <= hi.y;
    };
    auto ringJson = [&](const std::vector<LatLon>& ring, bool* allInside) {
        json pts = json::array();
        for (const LatLon& q : ring)
        {
            const glm::dvec2 p = geo.toLocal(q);
            *allInside = *allInside && inside(p);
            pts.push_back(drape(p));
        }
        return pts;
    };
    auto metres = [](double m) { return std::isfinite(m) ? json(m * 100.0) : json(); };

    struct Made
    {
        json splines = json::array();
        json info = json::array();
        json source = json::object();
        json points = json::array();
        size_t count = 0;
        double length = 0.0;
    };
    std::map<std::string, Made> byKind;
    std::vector<std::pair<std::string, json>> singles;
    size_t outside = 0;
    for (const OsmFeature& f : x->features)
    {
        const std::string kind = osmFeatureKindNames()[size_t(f.kind)];
        if (!wanted(kind))
            continue;
        std::vector<glm::dvec2> local;
        for (const LatLon& p : f.points)
            local.push_back(geo.toLocal(p));
        const auto runs = clipToHeightfield(local, hf, f.closed);
        if (runs.empty())
        {
            ++outside;
            continue;
        }
        Made& m = byKind[kind];
        ++m.count;
        const std::string name = featureName(f);
        for (const std::vector<glm::dvec2>& run : runs)
        {
            // Dense enough to follow the ground between OSM's nodes.
            std::vector<glm::dvec2> dense{run.front()};
            for (size_t i = 1; i < run.size(); ++i)
            {
                const double len = glm::length(run[i] - run[i - 1]);
                m.length += len;
                const int n = std::max(1, int(std::ceil(len / step)));
                for (int s = 1; s <= n; ++s)
                    dense.push_back(run[i - 1] + (run[i] - run[i - 1]) * (double(s) / n));
            }
            const bool closed = f.closed && runs.size() == 1;
            if (closed && dense.size() > 2 && glm::length(dense.front() - dense.back()) < 1e-6)
                dense.pop_back();
            json pts = json::array();
            for (const glm::dvec2& p : dense)
                pts.push_back(drape(p));
            const json spline = {{"type", "poly"}, {"closed", closed}, {"points", pts}};
            const json info = {{"id", std::to_string(f.id)}, {"name", name}, {"class", f.cls}, {"width", f.width}};
            const json source = f.name.empty() ? json::object() : json{{std::to_string(f.id), f.name}};
            if (perFeature)
                singles.push_back({kind, {{"spline", spline}, {"info", info}, {"source", source}}});
            else
            {
                m.splines.push_back(spline);
                m.info.push_back(info);
                m.source.update(source);
            }
        }
    }

    if (wanted("building"))
        for (const OsmBuilding& b : x->buildings)
        {
            bool in = true;
            const json outer = ringJson(b.outer, &in);
            std::vector<json> holes;
            for (const auto& h : b.holes)
                holes.push_back(ringJson(h, &in));
            // A footprint is kept whole or not at all: half a building is no building.
            if (!in)
            {
                ++outside;
                continue;
            }
            Made& m = byKind["building"];
            ++m.count;
            const std::string id = osmKey(b.type, b.id) + (b.piece ? "." + std::to_string(b.piece) : std::string());
            json info = {{"id", id},
                         {"osmId", b.id},
                         {"ring", "outer"},
                         {"part", b.part},
                         {"building", b.building},
                         {"program", b.program},
                         {"kind", b.kind},
                         {"name", thingName(b.kind, b.program, b.type, b.id, b.realNames, b.chain)},
                         {"height", metres(b.height)},
                         {"minHeight", metres(b.minHeight)},
                         {"roofHeight", metres(b.roofHeight)},
                         {"levels", b.levels},
                         {"minLevel", b.minLevel},
                         {"roofLevels", b.roofLevels},
                         {"roofShape", b.roofShape},
                         {"colour", b.colour},
                         {"roofColour", b.roofColour},
                         {"material", b.material},
                         {"roofMaterial", b.roofMaterial},
                         {"startDate", b.startDate}};
            if (!b.realNames.empty())
                m.source[id] = b.realNames;
            const size_t outerIndex = m.splines.size();
            m.splines.push_back({{"type", "poly"}, {"closed", true}, {"points", outer}});
            m.info.push_back(info);
            for (const json& h : holes)
            {
                m.splines.push_back({{"type", "poly"}, {"closed", true}, {"points", h}});
                m.info.push_back({{"id", id}, {"ring", "inner"}, {"outer", outerIndex}});
            }
        }

    if (wanted("place"))
        for (const OsmPlace& p : x->places)
        {
            const std::string id = osmKey(p.type, p.id);
            const json name = thingName(p.kind, p.program, p.type, p.id, p.realNames, p.chain);
            const json info = {{"id", id}, {"osmId", p.id}, {"key", p.key}, {"value", p.value}, {"program", p.program}, {"kind", p.kind}, {"name", name}};
            if (p.rings.empty())
            {
                const glm::dvec2 q = geo.toLocal(p.at);
                if (!inside(q))
                {
                    ++outside;
                    continue;
                }
                Made& m = byKind["place"];
                ++m.count;
                json pt = info;
                pt["at"] = drape(q);
                m.points.push_back(pt);
                if (!p.realNames.empty())
                    m.source[id] = p.realNames;
                continue;
            }
            bool in = true;
            std::vector<json> rings;
            for (const auto& r : p.rings)
                rings.push_back(ringJson(r, &in));
            // Areas larger than the terrain (a whole town's boundary) are cut by nothing: keep their centre if it is on it.
            if (!in && !inside(geo.toLocal(p.at)))
            {
                ++outside;
                continue;
            }
            Made& m = byKind["place"];
            ++m.count;
            if (!p.realNames.empty())
                m.source[id] = p.realNames;
            for (size_t k = 0; k < rings.size(); ++k)
            {
                m.splines.push_back({{"type", "poly"}, {"closed", true}, {"points", rings[k]}});
                json ri = info;
                ri["ring"] = k == 0 ? "outer" : "other";
                m.info.push_back(ri);
            }
        }

    json curves = json::object();
    if (a.get<bool>("curves", true) && !byKind.empty())
    {
        const json grp = ops::execute("group", {{"empty", true}, {"name", prefix}});
        const std::string groupPath = grp.is_array() ? grp[0].get<std::string>() : grp.get<std::string>();
        Node* groupNode = g.find(groupPath);
        auto make = [&](const std::string& kind, const json& splines, const json& info, const json& source, const json& points, const std::string& name) {
            json made;
            if (!splines.empty())
                made = ops::execute("curve", {{"splines", splines}, {"name", name}});
            else
                made = ops::execute("group", {{"empty", true}, {"name", name}});
            Node* n = g.find(made.is_array() ? made[0].get<std::string>() : made.get<std::string>());
            ops::execute("parent", {{"args", {n->path(), groupNode->path()}}});
            n = g.node(n->id());
            ops::execute("addAttr", {{"args", {n->path()}}, {"longName", "terrainFeature"}, {"dataType", "string"}, {"doc", "Terrain feature kind."}});
            ops::execute("setAttr", {{"args", {n->path() + ".terrainFeature", kind}}});
            ops::execute("addAttr", {{"args", {n->path()}}, {"longName", "osmFeatures"}, {"dataType", "string"},
                                     {"doc", "Per spline: OSM id, fictional name, class and width (cm), and for buildings and places what they "
                                             "are, heights and roofs, as JSON."}});
            ops::execute("setAttr", {{"args", {n->path() + ".osmFeatures", info.dump()}}});
            if (!points.empty())
            {
                ops::execute("addAttr", {{"args", {n->path()}}, {"longName", "osmPlaces"}, {"dataType", "string"},
                                         {"doc", "Places that are points (shops, offices, towns, peaks): OSM id, what they are, fictional name and where (world cm), as JSON."}});
                ops::execute("setAttr", {{"args", {n->path() + ".osmPlaces", points.dump()}}});
            }
            if (!source.empty())
            {
                ops::execute("addAttr", {{"args", {n->path()}}, {"longName", names::kSourceAttr}, {"dataType", "string"}, {"hidden", true},
                                         {"doc", "Real names the import replaced, by OSM id: for the real-name check only. Never exported."}});
                ops::execute("setAttr", {{"args", {n->path() + "." + names::kSourceAttr, source.dump()}}});
            }
            return n->path();
        };
        for (auto& [kind, m] : byKind)
            if (!perFeature || kind == "building" || kind == "place")
                if (!m.splines.empty() || !m.points.empty())
                    curves[kind] = make(kind, m.splines, m.info, m.source, m.points, kind);
        for (auto& [kind, s] : singles)
        {
            const json& info = s["info"];
            std::string nm = info["name"].get<std::string>();
            const std::string path = make(kind, json::array({s["spline"]}), json::array({info}), s["source"], json::array(),
                                          safeName(kind + "_" + (nm.empty() ? info["id"].get<std::string>() : nm)));
            if (!curves.contains(kind))
                curves[kind] = json::array();
            curves[kind].push_back(path);
        }
        curves["group"] = g.node(groupNode->id())->path();
    }

    json landcover = json::object();
    if (a.get<bool>("landcover", true) && !x->areas.empty())
    {
        GroupEdit e = editGroup(g, t, cmd);
        gn::GNode* n = nodeOfType(e.group, "OsmLandcover");
        if (!n)
            n = &insertOnChain(e.group, "OsmLandcover", e.group.uniqueNodeName("OsmLandcover"), "GroupOutput", "geometry", cmd);
        n->values["file"] = paths::storedPath(file);
        n->values["georef"] = geo.toJson().dump();
        n->values["fileStamp"] = stampOf(file);
        commit(e, cmd);
        Heightfield probe = hf;
        landcover = rasterizeLandcover(probe, x->areas, geo);
    }

    fs::path attributionFile;
    try
    {
        attributionFile = writeAttribution(dataDir, "OpenStreetMap", osmAttribution());
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }

    if (book.coined())
        log::info("{}: {} names coined from the {} words, every listed one matching a real name in the data", cmd, book.coined(), culture);
    json counts = json::object(), lengths = json::object();
    for (const auto& [kind, m] : byKind)
    {
        counts[kind] = m.count;
        lengths[kind] = m.length;
    }
    json out = {{"terrain", t.mesh ? t.mesh->path() : t.modifier->path()},
                {"file", paths::toUtf8(file)},
                {"features", counts},
                {"lengths", lengths},
                {"outside", outside},
                {"curves", curves},
                {"culture", culture},
                {"cultureFrom", cultureFrom},
                {"named", named},
                {"coinedNames", book.coined()},
                {"landcover", landcover},
                {"georef", georefJson(geo)},
                {"attribution", osmAttribution()},
                {"attributionFile", paths::toUtf8(attributionFile)}};
    if (!fetched.is_null())
        out["fetched"] = fetched;
    return out;
}

json runImportLandCover(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainImportLandCover";
    Graph& g = Graph::current();
    if (!a.has("terrain"))
        throw CommandError(cmd + ": give terrain= (the terrain the land cover goes on)");
    TerrainRef t = findTerrainRef(g, a.get<std::string>("terrain", ""), cmd);
    const Georef geo = georefFor(t, a, cmd);
    const Heightfield& hf = *t.local;
    const fs::path dataDir = terrainDataDir();
    const std::string mask = a.get<std::string>("mask", "worldcover");
    if (mask.empty())
        throw CommandError(cmd + ": mask is empty");
    const bool classes = a.get<bool>("classes", true);

    fs::path file;
    json fetched = nullptr;
    const std::vector<std::string> pos = givenFiles(a);
    if (!pos.empty())
    {
        file = resolvedFile(pos[0]);
        if (!fs::exists(file))
            throw CommandError(cmd + ": no file " + file.string());
    }
    else
    {
        if (a.get<std::string>("source", "worldcover") != "worldcover")
            throw CommandError(cmd + ": source is worldcover");
        const GeoBox b = boxOf(hf, geo);
        char tag[96];
        std::snprintf(tag, sizeof tag, "worldcover_%.5f_%.5f_%.5f_%.5f", b.west, b.south, b.east, b.north);
        file = dataDir / "landcover" / (safeName(tag) + ".tif");
        fetched = {{"source", "worldcover"}, {"bbox", {b.west, b.south, b.east, b.north}}, {"file", paths::toUtf8(file)}, {"cached", fs::exists(file)}};
        if (!fs::exists(file))
        {
            if (a.get<bool>("offline", false))
                throw CommandError(cmd + ": offline and this area was never fetched (" + file.string() + ")");
            try
            {
                const LandCoverFetch res = fetchWorldCover(b, [](const std::string& url, uint64_t at, uint64_t n, std::vector<uint8_t>* out, std::string* why) {
                    return httpGetRange(url, at, n, out, why);
                });
                writeGeoTiff(file, res.codes, true);
                fetched["tiles"] = res.tiles;
                fetched["emptyTiles"] = res.empty;
                fetched["downloadedBytes"] = res.bytes;
                fetched["samples"] = {res.codes.width, res.codes.height};
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
        }
    }

    std::map<std::string, double> share;
    try
    {
        Heightfield probe = hf;
        share = rasterizeWorldCover(probe, *cachedLandCover(file, probe, geo), geo, mask, classes);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
    const std::string terrainPath = t.mesh ? t.mesh->path() : t.modifier->path();
    GroupEdit e = editGroup(g, t, cmd);
    gn::GNode* n = nodeOfType(e.group, "ReadLandCover");
    if (!n)
        n = &insertOnChain(e.group, "ReadLandCover", e.group.uniqueNodeName("ReadLandCover"), "GroupOutput", "geometry", cmd);
    n->values["file"] = paths::storedPath(file);
    n->values["georef"] = geo.toJson().dump();
    n->values["mask"] = mask;
    n->values["classes"] = classes;
    n->values["fileStamp"] = stampOf(file);
    commit(e, cmd);

    fs::path attributionFile;
    try
    {
        attributionFile = writeAttribution(dataDir, "ESA WorldCover", worldCoverAttribution());
    }
    catch (const std::exception& ex)
    {
        throw CommandError(cmd + ": " + ex.what());
    }
    const double nodata = share.contains("nodata") ? share["nodata"] : 0.0;
    share.erase("nodata");
    json out = {{"terrain", terrainPath},
                {"file", paths::toUtf8(file)},
                {"mask", mask},
                {"landcover", share},
                {"nodata", nodata},
                {"georef", georefJson(geo)},
                {"attribution", worldCoverAttribution()},
                {"attributionFile", paths::toUtf8(attributionFile)}};
    if (!fetched.is_null())
        out["fetched"] = fetched;
    if (nodata > 0.5)
        out["warning"] = "most of the terrain has no land cover data; check the file covers the terrain's place";
    return out;
}

json runConstrain(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainConstrain";
    Graph& g = Graph::current();
    TerrainRef t = findTerrainRef(g, terrainArg(a, cmd), cmd);
    GroupEdit e = editGroup(g, t, cmd);
    gn::GNode* node = nodeOfType(e.group, "TerrainConstraints");
    if (!node)
    {
        // Before erosion, so erosion holds them; at the end of the chain when there is no Erode Terrain node.
        const gn::GNode* erode = nodeOfType(e.group, "ErodeTerrain");
        node = erode ? &insertOnChain(e.group, "TerrainConstraints", "Constraints", erode->name, "heightfield", cmd)
                     : &insertOnChain(e.group, "TerrainConstraints", "Constraints", "GroupOutput", "geometry", cmd);
    }
    json list = listValue(node->values.value("constraints", std::string("[]")));
    const std::string nodeName = node->name;

    std::string removed;
    if (a.get<bool>("clear", false))
        list = json::array();
    else if (a.has("remove"))
    {
        removed = a.get<std::string>("remove", "");
        json kept = json::array();
        for (const json& c : list)
            if (c.value("id", std::string()) != removed)
                kept.push_back(c);
        if (kept.size() == list.size())
            throw CommandError(cmd + ": no constraint '" + removed + "'");
        list = kept;
    }
    else
    {
        Constraint c;
        if (!parseConstraintKind(a.get<std::string>("kind", "ridge"), &c.kind))
            throw CommandError(cmd + ": kind is ridge, valley, cliff, river or flat");
        c.height = a.get<double>("height", c.height);
        c.width = a.get<double>("width", c.width);
        c.strength = std::clamp(a.get<double>("strength", 1.0), 0.0, 1.0);
        if (a.has("curve"))
        {
            const std::string curve = a.get<std::string>("curve", "");
            Node* cn = g.find(curve);
            if (!cn || !cn->type().isA("curve"))
                throw CommandError(cmd + ": no curve named '" + curve + "'");
            std::string source;
            for (const gn::GNode& n : e.group.nodes)
                if (n.type == "ObjectInfo" && n.values.value("object", std::string()) == cn->path())
                    source = n.name;
            if (source.empty())
            {
                gn::GNode n;
                n.name = source = e.group.uniqueNodeName("ConstraintSource");
                n.type = "ObjectInfo";
                n.position = {-600.0, 400.0 + 120.0 * double(e.group.nodes.size())};
                n.values["object"] = cn->path();
                e.group.nodes.push_back(n);
            }
            c.object = source;
        }
        else if (a.has("path"))
        {
            for (const json& p : a.flags["path"])
                c.path.push_back(t.toLocal(worldOf(p)));
            if (c.path.size() < 2)
                throw CommandError(cmd + ": a path needs two points");
        }
        else
            throw CommandError(cmd + ": give curve= or path=");
        c.id = a.get<std::string>("id", "");
        if (c.id.empty())
        {
            std::set<std::string> taken;
            for (const json& j : list)
                taken.insert(j.value("id", std::string()));
            for (int i = 1; c.id.empty() || taken.count(c.id); ++i)
                c.id = constraintKindNames()[size_t(c.kind)] + std::to_string(i);
        }
        json kept = json::array();
        for (const json& j : list)
            if (j.value("id", std::string()) != c.id)
                kept.push_back(j);
        kept.push_back(c.toJson());
        list = kept;
    }
    nodeNamed(e.group, nodeName)->values["constraints"] = list.dump();
    // Drop curve sources no constraint reads any more.
    std::set<std::string> used;
    for (const json& c : list)
        if (c.contains("object"))
            used.insert(c["object"].get<std::string>());
    std::erase_if(e.group.nodes, [&](const gn::GNode& n) { return n.type == "ObjectInfo" && n.name.starts_with("ConstraintSource") && !used.count(n.name); });
    commit(e, cmd);

    t = findTerrainRef(g, t.mesh ? t.mesh->path() : t.modifier->path(), cmd);
    json out = t.mesh ? terrainInfo(t.mesh->path()) : json::object();
    out["constraints"] = list;
    if (!removed.empty())
        out["removed"] = removed;
    json coverage = json::object();
    for (const std::string& k : constraintKindNames())
    {
        const std::string mask = "constraint" + std::string(1, char(std::toupper(k[0]))) + k.substr(1);
        if (const std::vector<float>* m = t.local->findMask(mask))
        {
            size_t n = 0;
            for (float v : *m)
                n += v > 0.5f ? 1 : 0;
            coverage[k] = double(n) / double(m->size());
        }
    }
    out["coverage"] = coverage;
    if (!nodeOfType(e.group, "ErodeTerrain"))
        out["warning"] = "the terrain has no Erode Terrain node, so nothing holds the constraints yet";
    return out;
}

json runLiveLink(const Args& a, bool reloadOnly)
{
    const std::string cmd = reloadOnly ? "terrainReload" : "terrainLiveLink";
    Graph& g = Graph::current();
    TerrainRef t = findTerrainRef(g, terrainArg(a, cmd), cmd);
    GroupEdit e = editGroup(g, t, cmd);
    const json files = restamp(e, t.modifier, reloadOnly ? std::nullopt : std::optional<bool>(a.get<bool>("on", true)));
    if (files.empty())
        throw CommandError(cmd + ": the terrain reads no file (Import Terrain Image, Read Elevation Data, OSM Land Cover, Read Land Cover or Read Heightfield)");
    commit(e, cmd);
    json out = t.mesh ? terrainInfo(t.mesh->path()) : json::object();
    out["files"] = files;
    if (!reloadOnly && a.get<bool>("on", true) && !qobject_cast<QApplication*>(QCoreApplication::instance()))
        out["warning"] = "live link watches files only in the app; here run terrainReload after saving";
    return out;
}

json runGeoref(const Args& a)
{
    const std::string cmd = "terrainGeoref";
    Graph& g = Graph::current();
    TerrainRef t = findTerrainRef(g, terrainArg(a, cmd), cmd);
    const Georef geo = georefFor(t, a, cmd);
    json out = georefJson(geo);
    const GeoBox b = boxOf(*t.local, geo);
    out["bbox"] = {b.west, b.south, b.east, b.north};
    if (a.has("latLon"))
    {
        json pts = json::array();
        for (const json& p : a.flags["latLon"])
        {
            const glm::dvec2 l = geo.toLocal({p.at(0).get<double>(), p.at(1).get<double>()});
            const glm::dvec3 w = t.toWorld(l.x, t.local->heightAt(l.x, l.y), l.y);
            pts.push_back({w.x, w.y, w.z});
        }
        out["points"] = pts;
    }
    if (a.has("points"))
    {
        json ll = json::array();
        for (const json& p : a.flags["points"])
        {
            const LatLon q = geo.toGeographic(t.toLocal(worldOf(p)));
            ll.push_back({q.lat, q.lon});
        }
        out["latLon"] = ll;
    }
    return out;
}
} // namespace inputcmd

using namespace inputcmd;

bool registerInputs(ext::Context& ctx)
{
    for (gn::NodeDecl d : {constraintsDecl(), landcoverDecl(), readLandCoverDecl(), bareEarthDecl(), amplifyDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;

    ctx.registerCommand({
        .name = "terrainImportDem",
        .doc = "Make a terrain from real elevation: fetched by area (bbox in degrees) or place name, or a local DEM file. Sources: "
               "copernicus (GLO-30 DSM, 30 m, AWS open data), terrarium (AWS Terrain Tiles, any spacing) and opentopography "
               "(COP30, SRTMGL1, NASADEM, AW3D30 ... with your API key). Tiles are cached in the workspace's terrain/dem_cache; "
               "the merged area is written to terrain/dem as a GeoTIFF and the source's attribution to terrain/ATTRIBUTION.txt. "
               "Place names are looked up once on OpenStreetMap Nominatim and cached.\n\n"
               "projection utm (default) or enu reprojects to metres; local flattens degrees. Copernicus measures tree and roof "
               "tops: bareEarth smooths them toward the ground. amplify refines coarse data with erosion. Returns terrainInfo's "
               "stats, the georeference (for terrainImportOsm and terrainGeoref), what was fetched and any notes.",
        .positionalDoc = "a local elevation file (optional)",
        .flags = {{"bbox", "bb", T::List, nullptr, "[west, south, east, north], degrees."},
                  {"place", "pl", T::String, nullptr, "A place name, looked up on Nominatim."},
                  {"size", "sz", T::List, json::array({1000000.0, 1000000.0}), "With place: [width, depth] around it, cm; [0, 0] takes the place's own bounds."},
                  {"source", "src", T::String, "copernicus", "Where elevation comes from.", {"copernicus", "terrarium", "opentopography"}},
                  {"dataset", "ds", T::String, "COP30", "OpenTopography dataset (COP30, COP90, SRTMGL1, NASADEM, AW3D30)."},
                  {"apiKey", "key", T::String, nullptr, "OpenTopography API key (free at opentopography.org)."},
                  {"saveKey", "sk", T::Bool, false, "Remember apiKey in the preferences."},
                  {"spacing", "sp", T::Float, 3000.0, "Terrarium: finest sample spacing wanted, cm (picks the tile zoom).", {}, {}, 100.0},
                  {"projection", "pr", T::String, "utm", "utm, enu (tangent plane, earth's curvature kept) or local.", {"utm", "enu", "local"}},
                  {"zone", "z", T::Int, int64_t(0), "UTM zone; 0 picks the area's.", {}, {}, 0.0, 60.0},
                  {"cellSize", "cs", T::Float, 0.0, "Grid spacing, cm; 0 keeps the data's own.", {}, {}, 0.0},
                  {"exaggeration", "ex", T::Float, 1.0, "Multiplies heights.", {}, {}, 0.0},
                  {"fromLowest", "fl", T::Bool, false, "Lowest point at 0 instead of sea level."},
                  {"center", "c", T::Vec3, json::array({0.0, 0.0, 0.0}), "Centre."},
                  {"bareEarth", "be", T::Bool, false, "Smooth trees and buildings out of a surface model."},
                  {"bareEarthRadius", "ber", T::Float, BareEarthParams().radius, "Widest object removed, cm.", {}, {}, 0.0},
                  {"bareEarthHeight", "beh", T::Float, BareEarthParams().maxHeight, "Tallest object removed, cm.", {}, {}, 0.0},
                  {"bareEarthMask", "bem", T::String, nullptr, "Only where this mask is."},
                  {"amplify", "am", T::Int, int64_t(1), "Refine with erosion: 2, 4 or 8 samples per data sample; 1 is off.", {}, {}, 1.0, 16.0},
                  {"amplifyDetail", "ad", T::Float, AmplifyParams().detail, "Strength of the added detail.", {}, {}, 0.0},
                  {"cache", "ca", T::String, nullptr, "Tile cache folder; default the workspace's terrain/dem_cache."},
                  {"offline", "off", T::Bool, false, "Use cached tiles only."},
                  {"name", "n", T::String, "Terrain", "Name of the mesh."}},
        .run = runImportDem,
        .examples = {"terrainImportDem(bbox=[7.60, 45.94, 7.72, 46.02], cellSize=3000)",
                     "terrainImportDem(place='Matterhorn', size=[1200000, 1200000], bareEarth=True, amplify=4)",
                     "terrainImportDem(bbox=[-122.2, 46.15, -122.1, 46.25], source='opentopography', dataset='SRTMGL1', apiKey='...', saveKey=True)",
                     "terrainImportDem('dem/N46E007.hgt', projection='utm')"},
    });

    ctx.registerCommand({
        .name = "terrainImportOsm",
        .doc = "Put OpenStreetMap data on a terrain: roads, paths, rivers, streams, lakes, coastline, piers, breakwaters, quays "
               "and dams become curves draped on the ground (one curve per kind under a group, or one per feature with "
               "perFeature=True), each with attributes terrainFeature and osmFeatures (OSM id, name, class and width in cm per "
               "spline). Buildings and building parts become closed footprints (holes as inner rings) on a building curve whose "
               "osmFeatures carry what each is (program), heights, levels, roof shape, colours and material; places (shops, "
               "offices, schools, cemeteries, ports, farmland, towns, beaches, islands) become a place curve of their areas, "
               "with point places in its osmPlaces. Every name is fictional, from the naming culture (default from where the "
               "data lies), stable by OSM id or by real name; real names are kept only in a hidden sourceNames attribute for "
               "realNameCheck. Land cover becomes masks "
               "landcover_<class> through an OSM Land Cover node in the terrain's group. Reads .osm XML, Overpass .json and "
               ".osm.pbf, or fetch=True asks the Overpass API for the terrain's area. Writes the ODbL attribution to the "
               "workspace's terrain/ATTRIBUTION.txt.\n\n"
               "The terrain must come from real elevation (terrainImportDem, or terrainImport of a DEM file), or give origin= "
               "[latitude, longitude] of its centre. Returns feature counts and lengths (cm) by kind, curves made, land cover "
               "fractions, how many features lay outside, the culture names came from and how many things were named.",
        .positionalDoc = "OSM file (.osm, .json, .osm.pbf)",
        .flags = {nodeFlag({"terrain", "t", T::String, nullptr, "The terrain."}, "mesh"),
                  {"origin", "o", T::List, nullptr, "[latitude, longitude] of the terrain's centre, for terrains not from real data."},
                  {"fetch", "f", T::Bool, false, "Download the terrain's area from the Overpass API."},
                  {"kinds", "k", T::List, nullptr, "Feature kinds to bring in (road, path, river, stream, lake, coastline, pier, breakwater, "
                                                   "quay, dam, building, place); default all."},
                  {"culture", "cu", T::String, "", "Naming culture for the fictional names (namingCultures); empty chooses one from where the data lies.",
                   [] {
                       std::vector<std::string> c = {""};
                       for (const std::string& n : names::cultures())
                           c.push_back(n);
                       return c;
                   }()},
                  {"curves", "cv", T::Bool, true, "Make curves for the features."},
                  {"perFeature", "pf", T::Bool, false, "One curve per feature instead of one per kind."},
                  {"landcover", "lc", T::Bool, true, "Land cover into masks."},
                  {"lift", "lf", T::Float, 0.0, "Raise the curves above the ground, cm."},
                  {"step", "st", T::Float, 0.0, "Point spacing along the draped curves, cm; 0 is two terrain cells.", {}, {}, 0.0},
                  {"name", "n", T::String, nullptr, "Name of the group; default <terrain>_OSM."}},
        .run = runImportOsm,
        .examples = {"terrainImportOsm('osm/zermatt.osm', terrain='Terrain')", "terrainImportOsm(terrain='Terrain', fetch=True, kinds=['road', 'river'])",
                     "terrainImportOsm('city.osm.pbf', terrain='Island', origin=[60.39, 5.32])"},
    });

    ctx.registerCommand({
        .name = "terrainImportLandCover",
        .doc = "Put ESA WorldCover land cover (10 m, 2021) on a terrain: the class codes into a mask (worldcover by default; code / "
               "255, what terrainBiomes(landcover={'mask': 'worldcover'}) reads) and the same landcover_<class> masks "
               "terrainImportOsm writes, through a Read Land Cover node in the terrain's group. Fetches the terrain's area from "
               "WorldCover's cloud-optimised tiles on AWS, downloading only the parts over it, and keeps it in the workspace's "
               "terrain/landcover; or reads a local WorldCover GeoTIFF (degrees or UTM). Writes the CC BY 4.0 attribution to "
               "terrain/ATTRIBUTION.txt.\n\n"
               "Classes: 10 forest, 20 scrub, 30 grassland, 40 farmland, 50 urban, 60 rock (bare or sparse), 70 glacier (snow "
               "and ice), 80 water, 90 and 95 wetland (95 mangroves), 100 grassland (moss and lichen). The terrain must come from "
               "real elevation, or give origin= [latitude, longitude] of its centre. Returns the share of the terrain per class "
               "and the share without data.",
        .positionalDoc = "a local WorldCover GeoTIFF (optional)",
        .flags = {nodeFlag({"terrain", "t", T::String, nullptr, "The terrain."}, "mesh"),
                  {"origin", "o", T::List, nullptr, "[latitude, longitude] of the terrain's centre, for terrains not from real data."},
                  {"source", "src", T::String, "worldcover", "Where land cover comes from.", {"worldcover"}},
                  {"mask", "m", T::String, "worldcover", "Mask for the class codes."},
                  {"classes", "cl", T::Bool, true, "Also write the landcover_<class> masks."},
                  {"offline", "off", T::Bool, false, "Use an area fetched before only."}},
        .run = runImportLandCover,
        .examples = {"terrainImportLandCover(terrain='Terrain')", "terrainImportLandCover('ESA_WorldCover_10m_2021_v200_N45E006_Map.tif', terrain='Terrain')"},
    });

    ctx.registerCommand({
        .name = "terrainConstrain",
        .doc = "Draw a constraint erosion keeps: a ridge raised by height and held, a valley lowered and held, a cliff stepping "
               "height down from the left to the right side of the curve, a river height deep whose bed only falls along the "
               "curve (draw it downstream), or flat ground inside a closed curve. width is the full width across, cm. Follows a "
               "curve live or a path; the same id replaces. Added to a Terrain Constraints node before Erode Terrain. Returns "
               "terrainInfo's stats, the constraint list and the share of the terrain each kind covers.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"kind", "k", T::String, "ridge", "What it holds.", {"ridge", "valley", "cliff", "river", "flat"}},
                  nodeFlag({"curve", "cv", T::String, nullptr, "A curve it follows live."}, "curve"),
                  {"path", "pa", T::List, nullptr, "A line [[x, z] or [x, y, z], ...] in world space (x, y on a Z-up scene)."},
                  {"height", "h", T::Float, 5000.0, "Rise, depth or drop, cm.", {}, {}, 0.0},
                  {"width", "w", T::Float, 6000.0, "Full width across, cm.", {}, {}, 1.0},
                  {"strength", "s", T::Float, 1.0, "0 to 1: how firmly erosion is held.", {}, {}, 0.0, 1.0},
                  {"id", "id", T::String, "", "Stable id; the same id replaces."},
                  {"remove", "rm", T::String, nullptr, "Remove the constraint with this id instead."},
                  {"clear", "cl", T::Bool, false, "Remove every constraint."}},
        .run = runConstrain,
        .examples = {"terrainConstrain('Terrain', kind='ridge', curve='NorthRidge', height=8000, width=12000)",
                     "terrainConstrain('Terrain', kind='river', path=[[-40000, -30000], [0, 0], [45000, 20000]], height=600, width=3000)"},
    });

    ctx.registerCommand({
        .name = "terrainLiveLink",
        .doc = "Re-run a terrain whenever its source file (painted image, DEM, OSM) is saved, so you can keep painting in Krita "
               "or Photoshop and see the terrain follow. The app checks the files once a second. Returns the files watched.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"on", "on", T::Bool, true, "Watch (True) or stop (False)."}},
        .run = [](const Args& a) { return runLiveLink(a, false); },
        .examples = {"terrainLiveLink('Terrain')", "terrainLiveLink('Terrain', on=False)"},
    });

    ctx.registerCommand({
        .name = "terrainReload",
        .doc = "Re-read a terrain's source files (painted image, DEM, OSM, .lilahf) now. Returns terrainInfo's stats and the files.",
        .positionalDoc = "terrain mesh or its modifier",
        .run = [](const Args& a) { return runLiveLink(a, true); },
        .examples = {"terrainReload('Terrain')"},
    });

    ctx.registerCommand({
        .name = "terrainGeoref",
        .doc = "Where a terrain from real data is on the earth: its projection, the latitude and longitude of its origin, the "
               "area it covers [west, south, east, north], and conversions: latLon= gives world points on the ground, points= "
               "gives latitude and longitude.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"latLon", "ll", T::List, nullptr, "[[latitude, longitude], ...] to world points."},
                  {"points", "p", T::List, nullptr, "World points [[x, z] or [x, y, z], ...] to latitude and longitude."},
                  {"origin", "o", T::List, nullptr, "[latitude, longitude] of the centre, for terrains not from real data."}},
        .undoable = false,
        .run = runGeoref,
        .examples = {"terrainGeoref('Terrain', latLon=[[45.9763, 7.6586]])['points']"},
    });

    // Extensions load before the application object exists, so the poll timer starts on the first idle of the UI loop.
    // QPointer: the application deletes its child timer before extensions unload at shutdown.
    auto timer = std::make_shared<QPointer<QTimer>>();
    ctx.subscribe(events::Event::AppIdle, [timer](const events::EventData&) {
        if (*timer || !qobject_cast<QApplication*>(QCoreApplication::instance()))
            return;
        *timer = new QTimer(QCoreApplication::instance());
        (*timer)->setInterval(1000);
        QObject::connect(*timer, &QTimer::timeout, *timer, [] { pollLiveLinks(); });
        (*timer)->start();
    });
    ctx.track("timer:terrainLiveLink", [timer] { delete *timer; });
    return true;
}
} // namespace terrain
