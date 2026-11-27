// World Remix commands: create a remix terrain, add sources, regions and pins, apply it (files and OSM features on the
// terrain) and check it. The spec lives on the remix modifier's remixSpec input, so every edit is a setAttr and undoes.

#include "OpenWorld.h"
#include "RemixNode.h"

#include "Fetch.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mcp.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/Workspace.h>

#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::ops;
namespace fs = std::filesystem;

namespace openworld
{
namespace
{
using T = ArgType;
using scene::Graph;
using scene::Node;

constexpr const char* kSpecAttr = "remixSpec";
constexpr const char* kBeforeAttr = "remixBefore";

struct RemixRef
{
    std::string mesh;
    std::string modifier;
    uint64_t modifierId = 0;
    RemixSpec spec;
};

bool isRemixModifier(const Node& n) { return gn::isGnModifier(n) && n.type().attrIndex(kSpecAttr) >= 0; }

/** The remix a name means: a remix mesh or its modifier; empty takes the selection, then the only remix in the scene. */
RemixRef findRemix(const std::string& name, const std::string& cmd)
{
    Graph& g = Graph::current();
    Node* mod = nullptr;
    auto fromNode = [&](Node* n) -> Node* {
        if (!n)
            return nullptr;
        if (isRemixModifier(*n))
            return n;
        for (scene::NodeId id : scene::modifiers::stack(g, n->id()))
            if (Node* m = g.node(id); m && isRemixModifier(*m))
                return m;
        return nullptr;
    };
    if (!name.empty())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(cmd + ": no node named '" + name + "'");
        mod = fromNode(n);
        if (!mod)
            throw CommandError(cmd + ": " + n->path() + " is not a remix (make one with remixCreate)");
    }
    else
    {
        for (scene::NodeId id : g.selection())
            if ((mod = fromNode(g.node(id))))
                break;
        if (!mod)
        {
            std::vector<Node*> all;
            for (scene::NodeId id : g.nodes())
                if (Node* n = g.node(id); n && isRemixModifier(*n))
                    all.push_back(n);
            if (all.size() == 1)
                mod = all[0];
            else
                throw CommandError(cmd + (all.empty() ? ": no remix in the scene (make one with remixCreate)" : ": several remixes; name one"));
        }
    }
    RemixRef r;
    r.modifier = mod->path();
    r.modifierId = mod->id();
    const scene::NodeId mesh = scene::modifiers::meshOf(g, mod->id());
    r.mesh = mesh != scene::kNoNode ? g.node(mesh)->path() : std::string();
    const json text = ops::execute("getAttr", {{"args", {r.modifier + "." + kSpecAttr}}});
    try
    {
        r.spec = specFromJson(json::parse(text.is_string() ? text.get<std::string>() : std::string("{}")));
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": the remix's spec is broken: " + e.what());
    }
    return r;
}

void store(const RemixRef& r, const RemixSpec& s) { ops::execute("setAttr", {{"args", {r.modifier + "." + kSpecAttr, toJson(s).dump()}}}); }

/** The result for a spec, through the same cache the node uses; CommandError when nothing can be built. */
std::shared_ptr<const RemixResult> result(const RemixSpec& s, const std::string& cmd)
{
    try
    {
        return cachedRemix(s);
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

json summary(const RemixResult& r)
{
    json out = r.stats;
    out["notes"] = r.notes;
    out["errors"] = r.errors;
    return out;
}

Vec2 point(const json& v, const std::string& what, const std::string& cmd)
{
    if (!v.is_array() || v.size() < 2)
        throw CommandError(cmd + ": " + what + " is [x, z]");
    return v.size() >= 3 ? Vec2(v[0].get<double>(), v[2].get<double>()) : Vec2(v[0].get<double>(), v[1].get<double>());
}

/** World points to the remix's map plane through its mesh's inverse world matrix. */
Vec2 toMap(const RemixRef& r, const glm::dvec3& world)
{
    Graph& g = Graph::current();
    Node* m = r.mesh.empty() ? nullptr : g.find(r.mesh);
    const glm::dmat4 xf = m ? m->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    const glm::dvec3 l = glm::dvec3(glm::inverse(xf) * glm::dvec4(world, 1.0));
    return {l.x, l.z};
}

Polygon polygonFromFlags(const RemixRef& ref, const Args& a, const std::string& cmd)
{
    Polygon poly;
    if (a.has("polygon"))
    {
        for (const json& p : a.flags["polygon"])
            poly.push_back(point(p, "each polygon point", cmd));
    }
    else if (a.has("curve"))
    {
        const json data = ops::execute("curveData", {{"args", {a.get<std::string>("curve")}}, {"worldSpace", true}});
        if (!data.is_array() || data.empty())
            throw CommandError(cmd + ": " + a.get<std::string>("curve") + " has no splines");
        for (const json& p : data[0]["points"])
            poly.push_back(toMap(ref, {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}));
    }
    else if (a.has("latLon"))
    {
        const Source* s = ref.spec.source(a.get<std::string>("source", ref.spec.sources.empty() ? std::string() : ref.spec.sources[0].id));
        if (!s)
            throw CommandError(cmd + ": latLon needs source= (the source the latitudes are in)");
        std::shared_ptr<const SourceData> d;
        try
        {
            d = loadSource(*s);
        }
        catch (const std::exception& e)
        {
            throw CommandError(cmd + ": " + e.what());
        }
        for (const json& p : a.flags["latLon"])
            poly.push_back(placeOnBase(*s, ref.spec.horizontal, d->geo.toLocal({p[0].get<double>(), p[1].get<double>()})));
    }
    else
        throw CommandError(cmd + ": give polygon=, curve= or latLon= with source=");
    if (poly.size() > 3 && glm::length(poly.front() - poly.back()) < 1e-6)
        poly.pop_back();
    if (poly.size() < 3)
        throw CommandError(cmd + ": a region needs at least three points");
    return poly;
}

std::string uniqueId(const std::string& base, const std::function<bool(const std::string&)>& taken)
{
    for (int k = 1;; ++k)
        if (const std::string id = base + std::to_string(k); !taken(id))
            return id;
}

fs::path outputFolder(const std::string& name, std::vector<std::string>* notes)
{
    const fs::path& root = Workspace::get().root();
    if (!root.empty())
        return root / "openworld" / "remix" / name;
    const fs::path tmp = fs::temp_directory_path() / "lila_remix" / name;
    notes->push_back("no workspace open; files went to " + paths::toUtf8(tmp));
    return tmp;
}

/** The remixed heights as a geographic float GeoTIFF any terrainImportDem reads back where the remixed OSM lies. */
terrain::Dem demOf(const RemixResult& r)
{
    const Heightfield& h = r.after;
    terrain::Dem d;
    d.width = h.nx;
    d.height = h.nz;
    d.geographic = true;
    d.epsg = 4326;
    const LatLon nw = r.geo.toGeographic(h.origin);
    d.x0 = nw.lon;
    d.y0 = nw.lat;
    d.dx = h.cell * 0.01 / r.geo.mx;
    d.dy = h.cell * 0.01 / r.geo.my;
    d.metres.resize(h.size());
    for (size_t i = 0; i < h.size(); ++i)
        d.metres[i] = h.heights[i] * 0.01f;
    return d;
}

} // namespace

void registerRemix(ext::Context& ctx)
{
    registerRemixNode(ctx);

    ctx.registerCommand({
        .name = "remixCreate",
        .doc = "Make a World Remix: real places composed onto one map before they become terrain and settlements. The result is a "
               "terrain (a mesh whose World Remix node makes the heightfield), so every Terrain command and node works on it, and it "
               "stays live: remixSource, remixAddRegion and remixPin edit its spec and the terrain follows.\n\n"
               "Give sources (each {id, dem, osm, landcover, bbox, at, angle}) or a whole spec (lila://schema/remix-spec). Scales are "
               "factors or \"1:N\": horizontal 1:3 and vertical 1:1.5 by default, so distances shrink more than heights. Returns the "
               "mesh, modifier, spec and the remix's stats (grid, warp, seams, features).",
        .flags = {{"name", "n", T::String, "World", "Name of the mesh."},
                  {"sources", "src", T::List, nullptr, "Sources: [{id, dem, osm, landcover, bbox, at: [x, z], angle}]."},
                  {"spec", "sp", T::Any, nullptr, "A whole remix spec; sources and scales given too override it."},
                  {"horizontal", "h", T::Any, nullptr, "Distance scale: 0.333 or \"1:3\"."},
                  {"vertical", "v", T::Any, nullptr, "Height scale: 0.667 or \"1:1.5\"."},
                  {"origin", "o", T::List, nullptr, "[latitude, longitude] of the map's centre in the remixed OSM."},
                  {"cell", "c", T::Float, nullptr, "Grid spacing, cm; default the finest source's cell times the scale.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "remixCreate";
            json spec = a.has("spec") ? a.flags["spec"] : json::object();
            if (a.has("sources"))
                spec["sources"] = a.flags["sources"];
            if (a.has("horizontal") || a.has("vertical"))
            {
                if (!spec.contains("scale") || !spec["scale"].is_object())
                    spec["scale"] = json::object();
                if (a.has("horizontal"))
                    spec["scale"]["horizontal"] = a.flags["horizontal"];
                if (a.has("vertical"))
                    spec["scale"]["vertical"] = a.flags["vertical"];
            }
            if (a.has("origin"))
                spec["origin"] = a.flags["origin"];
            if (a.has("cell"))
                spec["map"]["cell"] = a.flags["cell"];
            RemixSpec s;
            try
            {
                s = specFromJson(spec);
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
            if (!s.view.contains("overlay"))
                s.view["overlay"] = true;
            const std::string text = toJson(s).dump();

            Graph& g = Graph::current();
            const json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", a.get<std::string>("name", "World")}});
            const std::string host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            // Stood on the scene's ground as Terrain stands its terrains: the map's Y (heights) turns to world Z on a Z-up scene.
            if (math::upAxis() == math::UpAxis::Z)
                ops::execute("setAttr", {{"args", {host + ".rotate", json::array({90.0, 0.0, 90.0})}}});
            std::string group = g.find(host)->name() + "_Remix";
            const json groups = ops::execute("gnGroups", json::object());
            for (int k = 2;; ++k)
            {
                bool taken = false;
                for (const json& e : groups)
                    taken = taken || (e.is_string() && e.get<std::string>() == group) || (e.is_object() && e.value("name", std::string()) == group);
                if (!taken)
                    break;
                group = g.find(host)->name() + "_Remix" + std::to_string(k);
            }
            const json gspec = {
                {"name", group},
                {"description", "A World Remix: real places composed onto one map. Add Terrain nodes between Remix and the output."},
                {"inputs", json::array({{{"name", kSpecAttr}, {"type", "string"}, {"default", text}, {"panel", "Remix"}, {"description", "The remix spec, JSON."}},
                                        {{"name", kBeforeAttr}, {"type", "bool"}, {"default", false}, {"panel", "Remix"},
                                         {"description", "Show the base map before any region or pin."}}})},
                {"nodes", json::array({{{"name", "Remix"}, {"type", "WorldRemix"}}})},
                {"links", json::array({std::string("GroupInput.") + kSpecAttr + " -> Remix.spec", std::string("GroupInput.") + kBeforeAttr + " -> Remix.before",
                                       "Remix.heightfield -> GroupOutput.geometry"})}};
            ops::execute("gnBuildGroup", {{"args", {gspec}}, {"replace", false}});
            ops::execute("gnApply", {{"args", {host, group}}});
            const RemixRef ref = findRemix(host, cmd);
            json out = {{"mesh", ref.mesh}, {"modifier", ref.modifier}, {"group", group}, {"spec", toJson(ref.spec)}};
            if (!s.sources.empty())
                out["stats"] = summary(*result(ref.spec, cmd));
            return out;
        },
        .examples = {"r = remixCreate(name='Sinaloa', sources=[{'id': 'coast', 'dem': 'dem/mazatlan.tif', 'osm': 'osm/mazatlan.osm.pbf'},\n"
                     "    {'id': 'capital', 'dem': 'dem/culiacan.tif', 'osm': 'osm/culiacan.json', 'at': [400000, -300000]}])\nr['stats']['grid']",
                     "remixCreate(spec={'scale': {'horizontal': '1:4', 'vertical': '1:2'}, 'sources': [{'id': 'a', 'dem': 'a.tif'}]})"},
    });

    ctx.registerCommand({
        .name = "remixSource",
        .doc = "Add or change a remix source (the same id replaces): a real place's elevation, OSM data and WorldCover land cover, "
               "placed on the base map at at (cm, its centre) and turned by angle. fetch=True downloads what the source lacks for its "
               "bbox through Terrain's own fetch (Copernicus elevation, Overpass OSM, WorldCover), cached under the workspace's terrain "
               "folder; the downloads and their sizes are reported. remove= drops one. Returns the source and the remix's stats.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .flags = {{"id", "id", T::String, "", "Source id; the same id replaces; empty makes one."},
                  {"dem", "d", T::String, nullptr, "Elevation file."},
                  {"osm", "os", T::String, nullptr, "OSM file (.osm, Overpass .json, .osm.pbf)."},
                  {"landcover", "lc", T::String, nullptr, "WorldCover GeoTIFF."},
                  {"bbox", "bb", T::List, nullptr, "[west, south, east, north] degrees."},
                  {"at", "at", T::List, nullptr, "[x, z]: where its centre lands on the base map, cm."},
                  {"angle", "a", T::Float, nullptr, "Turn, degrees counter-clockwise seen from above."},
                  {"scale", "s", T::Float, nullptr, "Its own distance factor; 0 takes the remix's.", {}, {}, 0.0},
                  {"fetch", "f", T::Bool, false, "Download missing elevation, OSM and land cover for bbox."},
                  {"offline", "off", T::Bool, false, "With fetch: cached data only."},
                  {"remove", "rm", T::String, nullptr, "Remove the source with this id instead."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "remixSource";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            RemixSpec s = ref.spec;
            if (a.has("remove"))
            {
                const std::string id = a.get<std::string>("remove");
                const auto it = std::find_if(s.sources.begin(), s.sources.end(), [&](const Source& x) { return x.id == id; });
                if (it == s.sources.end())
                    throw CommandError(cmd + ": no source '" + id + "'");
                s.sources.erase(it);
                store(ref, s);
                return {{"removed", id}, {"sources", s.sources.size()}};
            }
            std::string id = a.get<std::string>("id", "");
            if (id.empty())
                id = uniqueId("source", [&](const std::string& x) { return s.source(x) != nullptr; });
            Source src;
            bool existed = false;
            for (Source& x : s.sources)
                if (x.id == id)
                {
                    src = x;
                    existed = true;
                }
            src.id = id;
            if (a.has("dem"))
                src.dem = a.get<std::string>("dem");
            if (a.has("osm"))
                src.osm = a.get<std::string>("osm");
            if (a.has("landcover"))
                src.landcover = a.get<std::string>("landcover");
            if (a.has("bbox"))
            {
                src.bbox = a.flags["bbox"].get<std::vector<double>>();
                if (src.bbox.size() != 4)
                    throw CommandError(cmd + ": bbox is [west, south, east, north]");
            }
            if (a.has("at"))
                src.at = point(a.flags["at"], "at", cmd);
            if (a.has("angle"))
                src.angle = a.get<double>("angle");
            if (a.has("scale"))
                src.scale = a.get<double>("scale");

            json fetched = json::object();
            if (a.get<bool>("fetch", false))
            {
                if (src.bbox.size() != 4)
                    throw CommandError(cmd + ": fetch needs bbox=[west, south, east, north]");
                const bool offline = a.get<bool>("offline", false);
                // Terrain's fetch, through a scratch terrain over the box removed afterwards.
                const json dem = ops::execute("terrainImportDem", {{"bbox", src.bbox}, {"projection", "local"}, {"offline", offline}, {"name", "remixFetch"}});
                const std::string tmp = dem["mesh"].get<std::string>();
                fetched["dem"] = dem.value("fetched", json());
                if (src.dem.empty())
                    src.dem = dem["file"].get<std::string>();
                try
                {
                    if (src.osm.empty() && !offline)
                    {
                        const json osm = ops::execute("terrainImportOsm", {{"terrain", tmp}, {"fetch", true}, {"curves", false}, {"landcover", false}});
                        src.osm = osm["file"].get<std::string>();
                        fetched["osm"] = osm.value("fetched", json());
                    }
                    if (src.landcover.empty())
                    {
                        const json lc = ops::execute("terrainImportLandCover", {{"terrain", tmp}, {"offline", offline}});
                        src.landcover = lc["file"].get<std::string>();
                        fetched["landcover"] = lc.value("fetched", json());
                    }
                }
                catch (const std::exception& e)
                {
                    fetched["warning"] = e.what();
                    log::warn("{}: {}", cmd, e.what());
                }
                ops::execute("delete", {{"args", {tmp}}});
                for (const char* k : {"dem", "osm", "landcover"})
                    if (fetched.contains(k) && fetched[k].is_object() && fetched[k].contains("file"))
                    {
                        std::error_code ec;
                        const auto bytes = fs::file_size(paths::fromUtf8(fetched[k]["file"].get<std::string>()), ec);
                        if (!ec)
                            fetched[k]["bytes"] = bytes;
                    }
            }
            if (existed)
            {
                for (Source& x : s.sources)
                    if (x.id == id)
                        x = src;
            }
            else
                s.sources.push_back(src);
            store(ref, s);
            json out = {{"id", id}, {"source", toJson(s)["sources"][existed ? 0 : s.sources.size() - 1]}};
            for (const json& j : toJson(s)["sources"])
                if (j["id"] == id)
                    out["source"] = j;
            if (!fetched.empty())
                out["fetched"] = fetched;
            out["stats"] = summary(*result(s, cmd));
            return out;
        },
        .examples = {"remixSource('World', id='capital', dem='dem/culiacan.tif', osm='osm/culiacan.json', at=[350000, -200000])",
                     "remixSource('World', id='coast', bbox=[-106.47, 23.17, -106.36, 23.27], fetch=True)",
                     "remixSource('World', remove='capital')"},
    });

    ctx.registerCommand({
        .name = "remixAddRegion",
        .doc = "Add or change a remix region (the same id replaces): a polygon on the base map and what happens to it. op cut lifts "
               "it out and places it translated and turned (the hole is filled as land, both edges become seams); move carries it "
               "rigidly while the land around stretches and compresses; keep holds it rigid while pins warp the land around; "
               "duplicate copies it; delete removes it (filled as land); sea sinks it to the sea floor, fading over the seam band inside the outline, to shape a coast where a source's box ends on land. The polygon comes from polygon=, a closed curve, or latLon= in a source's "
               "own latitude and longitude. remove= drops a region. Returns the region, where it ends up and the remix's stats.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .flags = {{"op", "op", T::String, "cut", "What happens to it.", regionOpNames()},
                  {"polygon", "p", T::List, nullptr, "[[x, z], ...] on the base map, cm."},
                  nodeFlag({"curve", "cv", T::String, nullptr, "A closed curve outlining it."}, "curve"),
                  {"latLon", "ll", T::List, nullptr, "[[latitude, longitude], ...] in a source."},
                  {"source", "src", T::String, nullptr, "The source latLon is in; default the first."},
                  {"translate", "t", T::List, nullptr, "[x, z] move, cm."},
                  {"angle", "a", T::Float, nullptr, "Turn, degrees counter-clockwise seen from above."},
                  {"pivot", "pv", T::List, nullptr, "[x, z] turn centre; default the polygon's centroid."},
                  {"seam", "sm", T::Float, nullptr, "Seam band width, cm; default the remix's, widened so the step left after levelling stays under seams.maxSlope.", {}, {}, 0.0},
                  {"id", "id", T::String, "", "Region id; the same id replaces; empty makes one."},
                  {"remove", "rm", T::String, nullptr, "Remove the region with this id instead."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "remixAddRegion";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            RemixSpec s = ref.spec;
            if (a.has("remove"))
            {
                const std::string id = a.get<std::string>("remove");
                const auto it = std::find_if(s.regions.begin(), s.regions.end(), [&](const Region& x) { return x.id == id; });
                if (it == s.regions.end())
                    throw CommandError(cmd + ": no region '" + id + "'");
                s.regions.erase(it);
                store(ref, s);
                return {{"removed", id}, {"regions", s.regions.size()}};
            }
            std::string id = a.get<std::string>("id", "");
            Region* existing = nullptr;
            for (Region& r : s.regions)
                if (r.id == id)
                    existing = &r;
            Region reg = existing ? *existing : Region();
            if (id.empty())
                id = uniqueId("region", [&](const std::string& x) { return s.region(x) != nullptr; });
            reg.id = id;
            if (!existing || a.has("op"))
                if (!parseRegionOp(a.get<std::string>("op", "cut"), &reg.op))
                    throw CommandError(cmd + ": op is move, keep, cut, duplicate or delete");
            if (!existing || a.has("polygon") || a.has("curve") || a.has("latLon"))
                reg.polygon = polygonFromFlags(ref, a, cmd);
            if (a.has("translate"))
                reg.translate = point(a.flags["translate"], "translate", cmd);
            if (a.has("angle"))
                reg.angle = a.get<double>("angle");
            if (a.has("pivot"))
                reg.pivot = point(a.flags["pivot"], "pivot", cmd);
            if (a.has("seam"))
                reg.seam = a.get<double>("seam");
            if (existing)
                *existing = reg;
            else
                s.regions.push_back(reg);
            store(ref, s);
            json target = json::array(), polygon = json::array();
            for (Vec2 p : reg.target())
                target.push_back({p.x, p.y});
            for (Vec2 p : reg.polygon)
                polygon.push_back({p.x, p.y});
            json out = {{"id", id}, {"op", regionOpNames()[size_t(reg.op)]}, {"polygon", polygon}, {"target", target}, {"area", std::abs(area(reg.polygon))}};
            if (!s.sources.empty())
                out["stats"] = summary(*result(s, cmd));
            return out;
        },
        .examples = {"remixAddRegion('World', id='capital', op='cut', curve='CapitalOutline', translate=[-150000, 80000], angle=15)",
                     "remixAddRegion('World', id='port', op='keep', polygon=[[0, 0], [20000, 0], [20000, 15000], [0, 15000]])",
                     "remixAddRegion('World', id='slum', op='delete', latLon=[[24.80, -107.40], [24.80, -107.38], [24.78, -107.38]], source='capital')"},
    });

    ctx.registerCommand({
        .name = "remixPin",
        .doc = "Pin a point of the base map: to= moves it (the land around follows smoothly, compressing empty land between regions), "
               "no to holds it still. The warp never folds: it is integrated in small checked steps, and remixCheck reports any fold "
               "left when pins ask for the impossible. The same id replaces; remove= drops one and clear=True drops all. Returns the "
               "pin and the remix's stats.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .flags = {{"at", "at", T::List, nullptr, "[x, z] on the base map, cm."},
                  {"to", "to", T::List, nullptr, "[x, z] where it goes; default at (an anchor)."},
                  {"id", "id", T::String, "", "Pin id; the same id replaces; empty makes one."},
                  {"remove", "rm", T::String, nullptr, "Remove the pin with this id instead."},
                  {"clear", "cl", T::Bool, false, "Remove every pin."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "remixPin";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            RemixSpec s = ref.spec;
            if (a.get<bool>("clear", false))
            {
                s.pins.clear();
                store(ref, s);
                return {{"pins", 0}};
            }
            if (a.has("remove"))
            {
                const std::string id = a.get<std::string>("remove");
                const auto it = std::find_if(s.pins.begin(), s.pins.end(), [&](const Pin& x) { return x.id == id; });
                if (it == s.pins.end())
                    throw CommandError(cmd + ": no pin '" + id + "'");
                s.pins.erase(it);
                store(ref, s);
                return {{"removed", id}, {"pins", s.pins.size()}};
            }
            std::string id = a.get<std::string>("id", "");
            Pin* existing = nullptr;
            for (Pin& p : s.pins)
                if (p.id == id)
                    existing = &p;
            if (!existing && !a.has("at"))
                throw CommandError(cmd + ": give at=[x, z]");
            Pin pin = existing ? *existing : Pin();
            if (id.empty())
                id = uniqueId("pin", [&](const std::string& x) { return std::any_of(s.pins.begin(), s.pins.end(), [&](const Pin& p) { return p.id == x; }); });
            pin.id = id;
            if (a.has("at"))
                pin.at = point(a.flags["at"], "at", cmd);
            pin.to = a.has("to") ? point(a.flags["to"], "to", cmd) : (existing && !a.has("at") ? pin.to : pin.at);
            if (existing)
                *existing = pin;
            else
                s.pins.push_back(pin);
            store(ref, s);
            json out = {{"id", id}, {"at", {pin.at.x, pin.at.y}}, {"to", {pin.to.x, pin.to.y}}};
            if (!s.sources.empty())
                out["stats"] = summary(*result(s, cmd));
            return out;
        },
        .examples = {"remixPin('World', id='coastHold', at=[-200000, 0])", "remixPin('World', at=[50000, 20000], to=[30000, 20000])"},
    });

    ctx.registerCommand({
        .name = "remixSpec",
        .doc = "Read a remix's spec, or replace it with spec= (lila://schema/remix-spec). Returns the spec.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .flags = {{"spec", "sp", T::Any, nullptr, "The new spec."}},
        .run = [](const Args& a) -> json {
            const std::string cmd = "remixSpec";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            if (!a.has("spec"))
                return toJson(ref.spec);
            RemixSpec s;
            try
            {
                s = specFromJson(a.flags["spec"]);
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
            store(ref, s);
            return toJson(s);
        },
        .examples = {"s = remixSpec('World')\ns['seams']['width'] = 2500\nremixSpec('World', spec=s)"},
    });

    ctx.registerCommand({
        .name = "remixView",
        .doc = "The remix's viewport view: overlay (regions, pins, seams, cut road ends and rivers over the terrain) and before (the "
               "terrain shows the base map, sources placed and nothing moved, for comparing). Returns both.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .flags = {{"overlay", "ov", T::Bool, nullptr, "Show the overlay."}, {"before", "b", T::Bool, nullptr, "Show the base map instead of the remix."}},
        .run = [](const Args& a) -> json {
            const std::string cmd = "remixView";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            if (a.has("overlay"))
            {
                RemixSpec s = ref.spec;
                s.view["overlay"] = a.get<bool>("overlay");
                store(ref, s);
                ref.spec = s;
            }
            if (a.has("before"))
                ops::execute("setAttr", {{"args", {ref.modifier + "." + kBeforeAttr, a.get<bool>("before")}}});
            const json before = ops::execute("getAttr", {{"args", {ref.modifier + "." + kBeforeAttr}}});
            return {{"overlay", ref.spec.view.value("overlay", true)}, {"before", before.is_boolean() ? before.get<bool>() : false}};
        },
        .examples = {"remixView('World', before=True)", "remixView('World', overlay=False)"},
    });

    ctx.registerCommand({
        .name = "remixApply",
        .doc = "Write the remix out and put its features on the terrain: the remixed OSM (every node, way and relation moved, cut and "
               "repaired, Overpass JSON that Terrain, Settlement and Buildings read), the heights as a GeoTIFF and a .lilahf with "
               "land cover masks, and the spec, under the workspace's openworld/remix/<name>. features=True imports the OSM onto the "
               "remix terrain with terrainImportOsm (replacing the last import). Returns the files, the remix's stats, the import's "
               "counts and remixCheck's summary.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .flags = {{"features", "f", T::Bool, true, "Import the remixed OSM onto the terrain as curves."},
                  {"folder", "fo", T::String, nullptr, "Where the files go; default the workspace's openworld/remix/<name>."},
                  {"perFeature", "pf", T::Bool, false, "One curve per feature instead of one per kind."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "remixApply";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            if (ref.spec.sources.empty())
                throw CommandError(cmd + ": the remix has no sources (remixSource adds one)");
            const std::shared_ptr<const RemixResult> r = result(ref.spec, cmd);
            std::vector<std::string> notes;
            Graph& g = Graph::current();
            const std::string name = ref.mesh.empty() ? std::string("remix") : g.find(ref.mesh)->name();
            const fs::path dir = a.has("folder") ? paths::resolvePath(a.get<std::string>("folder")) : outputFolder(name, &notes);
            json files = json::object();
            try
            {
                fs::create_directories(dir);
                const fs::path osm = dir / "remix.osm.json";
                std::ofstream(osm, std::ios::binary) << writeOsmDocJson(r->osm);
                files["osm"] = paths::toUtf8(osm);
                const fs::path hf = dir / "remix.lilahf";
                terrain::writeHeightfield(hf, r->after);
                files["heightfield"] = paths::toUtf8(hf);
                const fs::path tif = dir / "remix_dem.tif";
                terrain::writeGeoTiff(tif, demOf(*r));
                files["dem"] = paths::toUtf8(tif);
                const fs::path spec = dir / "remix.json";
                std::ofstream(spec, std::ios::binary) << toJson(ref.spec).dump(2);
                files["spec"] = paths::toUtf8(spec);
                files["attribution"] = paths::toUtf8(terrain::writeAttribution(dir, "OpenStreetMap", terrain::osmAttribution()));
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": " + e.what());
            }
            json out = {{"files", files}, {"stats", summary(*r)}, {"origin", {r->originLatLon.x, r->originLatLon.y}}};
            if (a.get<bool>("features", true) && !ref.mesh.empty() && !r->osm.empty())
            {
                ops::execute("setAttr", {{"args", {ref.modifier + "." + kBeforeAttr, false}}});
                Node* mesh = g.find(ref.mesh);
                const int attr = mesh->type().attrIndex("remixFeatures");
                if (attr >= 0)
                {
                    const json last = ops::execute("getAttr", {{"args", {ref.mesh + ".remixFeatures"}}});
                    if (last.is_string() && !last.get<std::string>().empty() && g.find(last.get<std::string>()))
                        ops::execute("delete", {{"args", {last.get<std::string>()}}});
                }
                else
                    ops::execute("addAttr", {{"args", {ref.mesh}}, {"longName", "remixFeatures"}, {"dataType", "string"}, {"doc", "The group of the remix's imported OSM features."}});
                const json imported = ops::execute("terrainImportOsm", {{"args", {files["osm"]}},
                                                                        {"terrain", ref.mesh},
                                                                        {"origin", {r->originLatLon.x, r->originLatLon.y}},
                                                                        {"landcover", false},
                                                                        {"perFeature", a.get<bool>("perFeature", false)},
                                                                        {"name", name + "_RemixOSM"}});
                const std::string grp = imported.contains("curves") && imported["curves"].contains("group") ? imported["curves"]["group"].get<std::string>() : std::string();
                ops::execute("setAttr", {{"args", {ref.mesh + ".remixFeatures", grp}}});
                out["features"] = {{"group", grp}, {"counts", imported.value("features", json::object())}, {"outside", imported.value("outside", 0)}};
                if (const size_t coined = imported.value("coinedNames", size_t(0)))
                    notes.push_back(std::to_string(coined) + " names coined: every listed " + imported.value("culture", std::string()) + " word matched a real name");
            }
            const json check = checkRemix(ref.spec, *r);
            out["check"] = {{"ok", check["ok"]}, {"counts", check["counts"]}};
            out["notes"] = notes;
            return out;
        },
        .examples = {"r = remixApply('World')\nr['files']['osm'], r['check']", "remixApply('World', features=False, folder='exports/remix')"},
    });

    ctx.registerCommand({
        .name = "remixCheck",
        .doc = "What a remix broke: rivers that no longer drain (that did in the sources), road and coastline ends a cut left "
               "unjoined, regions that end up overlapping, folds in the warp and sources that could not be read. Each issue has a "
               "kind, an id (way, node or regions), where it is on the map and what is wrong. Returns ok, counts by kind and issues.",
        .positionalDoc = "remix mesh or modifier (optional)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "remixCheck";
            const std::vector<std::string> pos = a.positional.get<std::vector<std::string>>();
            RemixRef ref = findRemix(pos.empty() ? std::string() : pos[0], cmd);
            json out = checkRemix(ref.spec, *result(ref.spec, cmd));
            out["stats"] = summary(*result(ref.spec, cmd));
            return out;
        },
        .examples = {"c = remixCheck('World')\nc['ok'], c['counts'], [i['kind'] for i in c['issues']]"},
    });

    mcp::Server& server = mcp::Server::get();
    server.addResource(ctx, {.uri = "lila://schema/remix-spec",
                             .name = "World Remix spec schema",
                             .description = "JSON schema of a World Remix spec: sources, scales, regions, pins, warp and seam settings. Read before "
                                            "writing one for remixCreate or remixSpec.",
                             .mimeType = "application/json",
                             .read = [] { return remixSpecSchema().dump(2); }});
}
} // namespace openworld
