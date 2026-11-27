// Terrain surface commands: biomes, material layer rules, the viewport preview on the terrain's own material, mask and
// colour images, ground cover, and what exports and the Unreal send take from them.

#include "GroundCover.h"
#include "MaterialNodes.h"
#include "Materials.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/SceneFile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
using scene::Graph;
using scene::Node;
using MT = ArgType;
namespace fs = std::filesystem;

constexpr const char* kBiomesNode = "Biomes";
constexpr const char* kLayersNode = "Layers";
constexpr const char* kCoverNode = "Cover";

/** A terrain as these commands see it: mesh, modifier, heightfield in its own space and its world matrix. */
struct SurfTerrain
{
    Node* mesh = nullptr;
    Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> local;
    glm::dmat4 world{1.0};
};

SurfTerrain surfFind(Graph& g, const std::string& name, const std::string& cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    g.evaluate();
    SurfTerrain f;
    if (gn::isGnModifier(*n))
    {
        f.modifier = n;
        f.local = gn::modifierHeightfield(n->id());
        const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
        f.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
    }
    else
    {
        f.mesh = n;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend() && !f.local; ++it)
            if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto h = gn::modifierHeightfield(m->id()))
                {
                    f.modifier = m;
                    f.local = std::move(h);
                }
    }
    if (!f.local || f.local->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    f.world = f.mesh ? f.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    return f;
}

SurfTerrain surfOne(const Args& a, const std::string& cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    return surfFind(Graph::current(), names[0], cmd);
}

std::string surfIdentifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Terrain" + out;
    return out;
}

std::string surfName(const SurfTerrain& f) { return surfIdentifier(f.mesh ? f.mesh->name() : std::string("Terrain")); }

fs::path surfFolder()
{
    const fs::path& scene = io::currentPath();
    if (!scene.empty())
        return scene.parent_path() / (scene.stem().string() + "_terrain");
    if (const fs::path& root = Workspace::get().root(); !root.empty())
        return root / "terrain";
    return fs::temp_directory_path() / "lila_terrain";
}

struct SurfGroup
{
    Node* node = nullptr;
    gn::Group group;
};

SurfGroup surfGroup(const SurfTerrain& f, const std::string& cmd)
{
    Node* node = f.modifier ? gn::groupOfModifier(Graph::current(), *f.modifier) : nullptr;
    if (!node)
        throw CommandError(cmd + ": the terrain has no group");
    try
    {
        return {node, gn::readGroup(*node)};
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void surfCommit(SurfGroup& t, const std::string& cmd)
{
    try
    {
        gn::commitGroup(Graph::current(), *t.node, std::move(t.group));
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

gn::GNode* surfNodeOfType(gn::Group& grp, const std::string& type)
{
    for (gn::GNode& n : grp.nodes)
        if (n.type == type)
            return &n;
    return nullptr;
}

const gn::GNode* surfNodeOfType(const gn::Group& grp, const std::string& type)
{
    for (const gn::GNode& n : grp.nodes)
        if (n.type == type)
            return &n;
    return nullptr;
}

/** The group's node of this type, else a new one spliced in at the end of the chain into the output (biomes before layers). */
gn::GNode& surfEnsure(gn::Group& grp, const std::string& type, const std::string& name, const std::string& cmd)
{
    if (gn::GNode* n = surfNodeOfType(grp, type))
        return *n;
    auto linkInto = [&](const std::string& node, const std::string& socket) -> gn::GLink* {
        for (gn::GLink& l : grp.links)
            if (l.toNode == node && l.toSocket == socket)
                return &l;
        return nullptr;
    };
    auto named = [&](const std::string& n) -> gn::GNode* {
        for (gn::GNode& x : grp.nodes)
            if (x.name == n)
                return &x;
        return nullptr;
    };
    std::string to = "GroupOutput", socket = "geometry";
    if (type == "TerrainBiomes")
        for (gn::GLink* l = linkInto(to, socket); l; l = linkInto(to, socket))
        {
            const gn::GNode* from = named(l->fromNode);
            if (!from || from->type != "TerrainLayers")
                break;
            to = from->name;
            socket = "heightfield";
        }
    gn::GLink* l = linkInto(to, socket);
    if (!l)
        throw CommandError(cmd + ": the terrain's group has no heightfield running into " + to);
    const gn::GNode* toNode = named(to);
    const gn::GNode* fromNode = named(l->fromNode);
    const glm::dvec2 at = toNode && fromNode ? (toNode->position + fromNode->position) * 0.5 + glm::dvec2(0.0, 200.0) : glm::dvec2(0.0);
    gn::GNode n;
    n.name = grp.uniqueNodeName(name);
    n.type = type;
    n.position = at;
    const std::string fromName = l->fromNode, fromSocket = l->fromSocket;
    l->fromNode = n.name;
    l->fromSocket = "heightfield";
    grp.links.push_back({fromName, fromSocket, n.name, "heightfield"});
    grp.nodes.push_back(std::move(n));
    return grp.nodes.back();
}

json jsonValue(const gn::GNode& n, const char* prop)
{
    auto it = n.values.find(prop);
    if (it == n.values.end() || !it->is_string() || it->get<std::string>().empty())
        return json();
    return json::parse(it->get<std::string>(), nullptr, false);
}

json coverageJson(const std::vector<Coverage>& c)
{
    json out = json::array();
    for (const Coverage& x : c)
        out.push_back({{"name", x.name}, {"mask", x.mask}, {"coverage", x.coverage}, {"areaM2", x.areaM2}, {"dominant", x.dominant}});
    return out;
}

void warnAll(const std::string& cmd, const std::vector<std::string>& warnings)
{
    for (const std::string& w : warnings)
        log::warn("{}: {}", cmd, w);
}

/** The rules a terrain's Terrain Layers node runs: its own, or the defaults it falls back to (said so). */
std::optional<MaterialRules> rulesOf(const SurfTerrain& f, bool* defaults = nullptr)
{
    Node* node = f.modifier ? gn::groupOfModifier(Graph::current(), *f.modifier) : nullptr;
    if (!node)
        return std::nullopt;
    gn::Group grp;
    try
    {
        grp = gn::readGroup(*node);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    const gn::GNode* n = surfNodeOfType(grp, "TerrainLayers");
    if (!n)
        return std::nullopt;
    const json j = jsonValue(*n, "rules");
    if (defaults)
        *defaults = j.is_null();
    try
    {
        return j.is_null() || j.is_discarded() ? defaultMaterialRules(*f.local) : MaterialRules::fromJson(j);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------------- looks

/** The library material ids, scale and name, read once. */
struct LibraryIndex
{
    std::mutex mutex;
    bool read = false;
    json entries = json::array();
};

LibraryIndex& libraryIndex()
{
    static LibraryIndex l;
    return l;
}

/** A library entry by id, by id's last part (meadowGrass for Meadowlight/meadowGrass), or by name; null when none. */
json findLibraryMaterial(const std::string& want)
{
    if (want.empty())
        return json();
    LibraryIndex& l = libraryIndex();
    std::lock_guard lock(l.mutex);
    if (!l.read)
    {
        l.read = true;
        try
        {
            l.entries = ops::execute("materialLibrary", json::object());
        }
        catch (const std::exception& e)
        {
            log::warn("terrain layers: the material library can't be read: {}", e.what());
            l.entries = json::array();
        }
    }
    auto lower = [](std::string s) {
        std::string o;
        for (char c : s)
            if (std::isalnum(static_cast<unsigned char>(c)))
                o += char(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    const std::string w = lower(want);
    for (const json& e : l.entries)
        if (e.value("id", std::string()) == want)
            return e;
    for (const json& e : l.entries)
    {
        const std::string id = e.value("id", std::string());
        const std::string tail = id.substr(id.find_last_of('/') == std::string::npos ? 0 : id.find_last_of('/') + 1);
        if (lower(tail) == w || lower(fs::path(tail).stem().string()) == w || lower(e.value("name", std::string())) == w)
            return e;
    }
    return json();
}

/** How a layer looks: its library material (resolved id), colour and where that came from, tile size, roughness. */
struct SurfLook
{
    std::string name;
    std::string mask;
    std::string material;
    glm::dvec3 color{0.5};
    std::string colorFrom;
    double tileCm = 400.0;
    double roughness = 0.85;
};

SurfLook surfLookOf(const LayerRule& l, std::vector<std::string>& warnings)
{
    SurfLook look;
    look.name = l.name;
    look.mask = layerMask(l.name);
    look.roughness = l.roughness;
    const json entry = findLibraryMaterial(l.material);
    if (!l.material.empty() && entry.is_null())
        warnings.push_back("layer " + l.name + ": no library material '" + l.material + "'; it is drawn in its colour");
    if (!entry.is_null())
    {
        look.material = entry.value("id", std::string());
        look.tileCm = entry.value("scaleCm", 400.0);
    }
    if (l.tileCm > 0.0)
        look.tileCm = l.tileCm;
    if (l.colorGiven)
    {
        look.color = l.color;
        look.colorFrom = "rule";
    }
    else if (std::optional<glm::dvec3> c = knownMaterialColor(look.material))
    {
        look.color = *c;
        look.colorFrom = "material";
    }
    else if (!look.material.empty())
    {
        look.color = l.color;
        look.colorFrom = "default";
        warnings.push_back("layer " + l.name + ": " + look.material + "'s colour isn't known; it takes the default for " + l.name + " (give color= to set it)");
    }
    else
    {
        look.color = l.color;
        look.colorFrom = "default";
    }
    return look;
}

json lookJson(const SurfLook& l)
{
    return {{"name", l.name},
            {"mask", l.mask},
            {"material", l.material},
            {"color", json::array({l.color.x, l.color.y, l.color.z})},
            {"colorFrom", l.colorFrom},
            {"tileSize", l.tileCm},
            {"roughness", l.roughness}};
}

float srgbEncode(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return float(v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
}

ImageLayout layoutOf(const Args& a)
{
    const std::string l = a.get<std::string>("layout", "map");
    if (l != "map" && l != "uv")
        throw CommandError("layout is map or uv");
    return l == "uv" ? ImageLayout::Uv : ImageLayout::Map;
}

std::pair<int, int> imageSize(const Heightfield& h, const Args& a)
{
    const int r = int(a.get<int64_t>("resolution", 0));
    if (r <= 0)
        return {h.nx, h.nz};
    // The longer side gets r pixels.
    const double aspect = double(h.nz - 1) / double(std::max(1, h.nx - 1));
    return h.nx >= h.nz ? std::pair{r, std::max(2, int(std::lround(r * aspect)))} : std::pair{std::max(2, int(std::lround(r / aspect))), r};
}

void writeLayerImage(const fs::path& path, int w, int h, const std::vector<float>& values, int bits)
{
    image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
    for (size_t k = 0; k < values.size(); ++k)
    {
        const float v = std::clamp(values[k], 0.0f, 1.0f);
        img.rgba[k * 4] = img.rgba[k * 4 + 1] = img.rgba[k * 4 + 2] = v;
        img.rgba[k * 4 + 3] = 1.0f;
    }
    image::write(path, img, bits);
}

// ---------------------------------------------------------------------------------------------------- ground cover items

/** A placeholder for an item no scene object stands in for, built at the origin and hidden. */
geo::Mesh placeholderItem(const std::string& kind)
{
    const bool zUp = math::upAxis() == math::UpAxis::Z;
    auto P = [&](double x, double y, double z) { return zUp ? glm::dvec3(x, z, y) : glm::dvec3(x, y, z); };
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    auto ring = [&](int n, double r, double y, double phase = 0.0) {
        const geo::Index first = geo::Index(pts.size());
        for (int i = 0; i < n; ++i)
        {
            const double t = phase + 6.283185307179586 * i / n;
            pts.push_back(P(r * std::cos(t), y, r * std::sin(t)));
        }
        return first;
    };
    auto cap = [&](geo::Index first, int n, bool up) {
        sizes.push_back(geo::Index(n));
        for (int i = 0; i < n; ++i)
            idx.push_back(first + geo::Index(up ? n - 1 - i : i));
    };
    auto band = [&](geo::Index a, geo::Index b, int n) {
        for (int i = 0; i < n; ++i)
        {
            const geo::Index i1 = geo::Index((i + 1) % n);
            sizes.push_back(4);
            idx.insert(idx.end(), {a + geo::Index(i), a + i1, b + i1, b + geo::Index(i)});
        }
    };
    auto apex = [&](geo::Index ringFirst, int n, double y) {
        const geo::Index top = geo::Index(pts.size());
        pts.push_back(P(0.0, y, 0.0));
        for (int i = 0; i < n; ++i)
        {
            sizes.push_back(3);
            idx.insert(idx.end(), {ringFirst + geo::Index(i), ringFirst + geo::Index((i + 1) % n), top});
        }
    };
    if (kind == "tree")
    {
        const geo::Index t0 = ring(6, 20.0, 0.0), t1 = ring(6, 20.0, 250.0);
        cap(t0, 6, false);
        band(t0, t1, 6);
        const geo::Index c0 = ring(8, 220.0, 200.0);
        cap(c0, 8, false);
        apex(c0, 8, 900.0);
    }
    else if (kind == "grassTuft")
    {
        for (int k = 0; k < 3; ++k)
        {
            const double t = 3.141592653589793 * k / 3.0, c = std::cos(t) * 20.0, s = std::sin(t) * 20.0;
            const geo::Index f = geo::Index(pts.size());
            pts.insert(pts.end(), {P(-c, 0.0, -s), P(c, 0.0, s), P(c * 0.6, 45.0, s * 0.6), P(-c * 0.6, 45.0, -s * 0.6)});
            sizes.push_back(4);
            idx.insert(idx.end(), {f, f + 1, f + 2, f + 3});
        }
    }
    else
    {
        // Rocks and debris: a squat lumpy prism.
        const double r = kind == "debris" ? 25.0 : 60.0;
        const geo::Index b0 = ring(7, r, -0.2 * r, 0.3), b1 = ring(7, r * 0.85, 0.45 * r, 0.7);
        cap(b0, 7, false);
        band(b0, b1, 7);
        apex(b1, 7, 0.8 * r);
    }
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

std::string makePlaceholder(Graph& g, const std::string& item, const std::string& prefix)
{
    const std::string kind = item.find("tree") != std::string::npos ? "tree"
                             : item.find("grass") != std::string::npos ? "grassTuft"
                             : item.find("debris") != std::string::npos ? "debris"
                                                                         : "rock";
    json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", prefix + "_" + surfIdentifier(item)}});
    const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    Node* mesh = g.find(path);
    g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(placeholderItem(kind))));
    ops::execute("setAttr", {{"args", {path + ".visibility", false}}});
    return path;
}

// ---------------------------------------------------------------------------------------------------- commands

json describeLayers(const SurfTerrain& f, const MaterialRules& rules, std::vector<std::string>& warnings)
{
    std::vector<std::string> names, masks;
    for (const LayerRule& l : rules.layers)
    {
        names.push_back(l.name);
        masks.push_back(layerMask(l.name));
    }
    const std::vector<Coverage> cov = coverageOf(*f.local, names, masks);
    json layers = json::array();
    for (size_t i = 0; i < cov.size(); ++i)
    {
        json j = coverageJson({cov[i]})[0];
        const SurfLook look = surfLookOf(rules.layers[i], warnings);
        j["material"] = look.material;
        j["color"] = json::array({look.color.x, look.color.y, look.color.z});
        j["colorFrom"] = look.colorFrom;
        layers.push_back(j);
    }
    return layers;
}

/** Linear colour as the '#rrggbb' sRGB the heightfield draw's layers take. */
std::string displayHex(glm::dvec3 linear)
{
    auto channel = [](double c) {
        c = std::clamp(c, 0.0, 1.0);
        const double s = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        return int(std::lround(s * 255.0));
    };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", channel(linear.x), channel(linear.y), channel(linear.z));
    return buf;
}

/** The heightfield draw's paint layers (the mesh's terrainLayers attribute, as terrainDisplayLayers writes it): each layer_<name> mask in its colour. */
json syncDisplayLayers(const SurfTerrain& f, const MaterialRules& rules, std::vector<std::string>& warnings)
{
    json list = json::array();
    if (!f.mesh)
        return list;
    for (const LayerRule& l : rules.layers)
    {
        const SurfLook look = surfLookOf(l, warnings);
        list.push_back({{"name", l.name}, {"mask", look.mask}, {"color", displayHex(look.color)}, {"roughness", look.roughness}, {"tiling", look.tileCm}});
    }
    const std::string mesh = f.mesh->path();
    if (f.mesh->type().attrIndex("terrainLayers") < 0)
        ops::execute("addAttr", {{"args", {mesh}}, {"longName", "terrainLayers"}, {"dataType", "string"}, {"hidden", true},
                                 {"doc", "Paint layers the viewport's heightfield draw shows (JSON)."}});
    ops::execute("setAttr", {{"args", {mesh + ".terrainLayers", list.dump()}}});
    return list;
}

void registerRuleCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainBiomes",
        .doc = "Work out climate and biomes over a terrain: temperature (colder with height at lapseRate per 100 m, warmer on "
               "slopes facing the sun), moisture (rain, less in the lee of ranges along the wind, more on windward slopes and "
               "where water gathers), and a weight mask biome_<name> per biome, summing to 1 everywhere. Biomes fit by ranges "
               "over temperature (C), moisture, height, slope or any mask, follow a painted mask (paint=), or an imported "
               "landcover mask (ESA WorldCover classes by default). Layer rules and ground cover read the biomes by name.\n\n"
               "Adds a Terrain Biomes node to the terrain's group (before Terrain Layers), or updates it. Returns each biome's "
               "coverage (mean weight), area in m2 and where it dominates, the temperature and moisture ranges, and any inputs "
               "the rules named that the terrain lacks.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"biomes", "b", MT::Any, nullptr, "[{name, when: {temperature: [lo, hi], moisture: [lo, hi], height, slope, <mask>}, paint}], or default."},
                  {"climate", "cl", MT::Any, nullptr,
                   "{seaLevel, temperature (C at sea level), lapseRate, aspectEffect, sunAzimuth, rain, windAzimuth, rainShadow, "
                   "shadowDistance, orographic, groundWater}; keys left out keep their values."},
                  {"sharpness", "sh", MT::Float, nullptr, "How hard biome edges are (default 4).", {}, {}, 0.1, 64.0},
                  {"landcover", "lc", MT::Any, nullptr, "{mask, scale (255), weight, classes: {biome: [codes]}}: an imported landcover mask."},
                  {"reset", "", MT::Bool, false, "Start from the defaults instead of the terrain's current spec."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "terrainBiomes";
            SurfTerrain f = surfOne(a, cmd);
            SurfGroup t = surfGroup(f, cmd);
            gn::GNode& node = surfEnsure(t.group, "TerrainBiomes", kBiomesNode, cmd);
            json spec = a.get<bool>("reset", false) ? json() : jsonValue(node, "biomes");
            if (spec.is_null() || spec.is_discarded())
                spec = defaultBiomes().toJson();
            if (a.has("biomes"))
            {
                const json b = a.flags["biomes"];
                spec["biomes"] = b.is_string() && b.get<std::string>() == "default" ? defaultBiomes().toJson()["biomes"] : b;
            }
            if (a.has("climate"))
            {
                if (!a.flags["climate"].is_object())
                    throw CommandError("terrainBiomes: climate is a dict");
                spec["climate"].update(a.flags["climate"]);
            }
            if (a.has("sharpness"))
                spec["sharpness"] = a.get<double>("sharpness", 4.0);
            if (a.has("landcover"))
                spec["landcover"] = a.flags["landcover"];
            BiomeSpec parsed;
            try
            {
                parsed = BiomeSpec::fromJson(spec);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainBiomes: ") + e.what());
            }
            node.values["biomes"] = parsed.toJson().dump();
            const std::string nodeName = node.name;
            surfCommit(t, cmd);

            f = surfFind(Graph::current(), f.mesh ? f.mesh->path() : f.modifier->path(), cmd);
            const json published = surfaceResult(f.modifier->id(), nodeName);
            std::vector<std::string> names, masks;
            for (const BiomeRule& b : parsed.biomes)
            {
                names.push_back(b.name);
                masks.push_back(biomeMask(b.name));
            }
            json out = {{"node", nodeName}, {"biomes", coverageJson(coverageOf(*f.local, names, masks))}, {"spec", parsed.toJson()}};
            auto range = [&](const char* mask, bool celsius) {
                const std::vector<float>* m = f.local->findMask(mask);
                if (!m || m->empty())
                    return json();
                auto [lo, hi] = std::minmax_element(m->begin(), m->end());
                double sum = 0.0;
                for (float v : *m)
                    sum += v;
                auto conv = [&](double v) { return celsius ? maskToTemperature(v) : v; };
                return json{{"min", conv(*lo)}, {"max", conv(*hi)}, {"mean", conv(sum / double(m->size()))}};
            };
            out["temperature"] = range("temperature", true);
            out["moisture"] = range("moisture", false);
            out["missing"] = published.value("missing", json::array());
            out["maxSumError"] = published.value("maxSumError", 0.0);
            for (const json& m : out["missing"])
                log::warn("terrainBiomes: the terrain has no {}; ranges over it read 0", m.get<std::string>());
            return out;
        },
        .examples = {"terrainBiomes('Terrain')['biomes']",
                     "terrainBiomes('Terrain', climate={'temperature': 8, 'windAzimuth': 270, 'rainShadow': 0.8})",
                     "terrainBiomes('Terrain', biomes=[{'name': 'tundra', 'when': {'temperature': [None, 2]}}, {'name': 'taiga', 'when': {'temperature': [2, 12]}}])"},
    });

    ctx.registerCommand({
        .name = "terrainMaterialRules",
        .doc = "Set a terrain's material layers: grass, rock, dirt, sand, snow, gravel, road, riverbed or your own, each placed "
               "by rules over masks and derived fields, with noise to break up its edges. Layers go bottom first; the first is "
               "the base and takes whatever the others leave, and each layer above covers its rule's share of what is left "
               "under the ones above it, so the weights (masks layer_<name>) always sum to 1.\n\n"
               "A layer is {name, material (a library id; Meadowlight's by default for the usual names), color ([r, g, b] "
               "linear, else the material's mean colour), roughness, tileSize (cm), opacity, when: {input: [min, max] or "
               "{range, fade, invert, blur (cm)}, biome: name(s)}, noise: {amount, scale (cm)}}. Inputs: height (cm), slope and aspect "
               "(degrees), curvature, nearCliff, temperature (C), moisture, flow, wear, deposit, lakes, wet and every other "
               "mask, painted ones included. With no layers it uses defaults fitted to the terrain (sand only with seaLevel=).\n\n"
               "Adds a Terrain Layers node at the end of the terrain's group, or updates it. Returns each layer's coverage "
               "(mean weight, the share of the terrain), area in m2, where it dominates, its material and colour, the largest "
               "error of the weight sum, and any inputs the rules named that the terrain lacks (those ranges read 0). The layers also "
               "become the heightfield draw's paint layers (terrainDisplayLayers), each layer_<name> in its colour, returned as displayLayers.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"layers", "l", MT::Any, nullptr, "The layers, bottom first; 'default' for the defaults."},
                  {"set", "s", MT::Any, nullptr, "One layer to add or replace by name, keeping the others (new ones go on top)."},
                  {"remove", "rm", MT::List, nullptr, "Layer names to drop."},
                  {"seed", "sd", MT::Int, nullptr, "Moves the edge noise."},
                  {"seaLevel", "sea", MT::Float, nullptr, "Defaults: height of the sea, cm, for a sand layer at the shore."},
                  {"query", "q", MT::Bool, false, "Change nothing; return the current layers and coverage."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "terrainMaterialRules";
            SurfTerrain f = surfOne(a, cmd);
            SurfGroup t = surfGroup(f, cmd);
            const bool query = a.get<bool>("query", false);
            if (query && !surfNodeOfType(t.group, "TerrainLayers"))
                return {{"layers", json::array()}, {"node", nullptr}};
            gn::GNode& node = surfEnsure(t.group, "TerrainLayers", kLayersNode, cmd);
            std::optional<double> sea;
            if (a.has("seaLevel"))
                sea = a.get<double>("seaLevel", 0.0);
            json current = jsonValue(node, "rules");
            MaterialRules rules;
            try
            {
                if (a.has("layers"))
                {
                    const json l = a.flags["layers"];
                    rules = l.is_string() && l.get<std::string>() == "default" ? defaultMaterialRules(*f.local, sea) : MaterialRules::fromJson(json{{"layers", l}});
                    if (!current.is_null() && !current.is_discarded())
                        rules.seed = current.value("seed", 0u);
                }
                else if (current.is_null() || current.is_discarded())
                    rules = defaultMaterialRules(*f.local, sea);
                else
                    rules = MaterialRules::fromJson(current);
                if (a.has("set"))
                {
                    json list = rules.toJson()["layers"];
                    const json one = a.flags["set"];
                    const std::string name = one.value("name", std::string());
                    auto it = std::find_if(list.begin(), list.end(), [&](const json& x) { return x["name"] == name; });
                    if (it != list.end())
                        *it = one;
                    else
                        list.push_back(one);
                    const uint32_t seed = rules.seed;
                    rules = MaterialRules::fromJson(json{{"layers", list}});
                    rules.seed = seed;
                }
                if (a.has("remove"))
                {
                    std::set<std::string> gone;
                    for (const json& n : a.flags["remove"])
                        gone.insert(n.get<std::string>());
                    std::erase_if(rules.layers, [&](const LayerRule& l) { return gone.count(l.name) > 0; });
                    if (rules.layers.empty())
                        throw std::runtime_error("that removes every layer");
                }
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainMaterialRules: ") + e.what());
            }
            if (a.has("seed"))
                rules.seed = uint32_t(a.get<int64_t>("seed", 0));
            const std::string nodeName = node.name;
            if (!query)
            {
                node.values["rules"] = rules.toJson().dump();
                surfCommit(t, cmd);
                f = surfFind(Graph::current(), f.mesh ? f.mesh->path() : f.modifier->path(), cmd);
            }
            const json published = surfaceResult(f.modifier->id(), nodeName);
            std::vector<std::string> warnings;
            json out = {{"node", nodeName}, {"layers", describeLayers(f, rules, warnings)}, {"rules", rules.toJson()}};
            out["missing"] = published.value("missing", json::array());
            out["maxSumError"] = published.value("maxSumError", 0.0);
            for (const json& m : out["missing"])
                warnings.push_back("the terrain has no " + m.get<std::string>() + "; ranges over it read 0");
            if (!query)
                out["displayLayers"] = syncDisplayLayers(f, rules, warnings);
            warnAll(cmd, warnings);
            out["warnings"] = warnings;
            return out;
        },
        .examples = {"terrainMaterialRules('Terrain')['layers']",
                     "terrainMaterialRules('Terrain', set={'name': 'moss', 'when': {'moisture': [0.7, 1], 'slope': [0, 25]}, 'noise': 0.3})",
                     "terrainMaterialRules('Terrain', layers=[{'name': 'grass'}, {'name': 'rock', 'when': {'slope': [35, 90]}}, "
                     "{'name': 'snow', 'when': {'height': {'above': 180000, 'fade': 3000}}}])"},
    });
}

void registerImageCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainMaskImages",
        .doc = "Write a terrain's masks (layer and biome weights, flow, wear, deposit, moisture, painted ones...) as grey "
               "images, to read in texture graphs (an Image node) or another tool. layout='uv' lays them over the terrain "
               "mesh's uv set 0, as its texture sets read them; 'map' puts the -Z edge at the top. Returns {files: {mask: "
               "file}, resolution, layout}.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"masks", "m", MT::List, nullptr, "Masks to write; default every one."},
                  {"folder", "f", MT::String, "", "Where; defaults to the bake folder's masks/."},
                  {"layout", "lo", MT::String, "uv", "uv or map.", {"uv", "map"}},
                  {"resolution", "r", MT::Int, int64_t(0), "Pixels along the longer side; 0 is one per sample.", {}, {}, 0.0, 16384.0},
                  {"bits", "b", MT::Int, int64_t(16), "8 or 16.", {}, {}, 8.0, 16.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const SurfTerrain f = surfOne(a, "terrainMaskImages");
            const Heightfield& h = *f.local;
            std::vector<std::string> masks;
            if (a.has("masks"))
                for (const json& m : a.flags["masks"])
                    masks.push_back(m.get<std::string>());
            else
                for (const auto& [name, v] : h.masks)
                    if (v.size() == h.size())
                        masks.push_back(name);
            fs::path folder = a.get<std::string>("folder", "");
            if (folder.empty())
                folder = surfFolder() / "masks";
            fs::create_directories(folder);
            const ImageLayout layout = layoutOf(a);
            const auto [w, hgt] = imageSize(h, a);
            const int bits = a.get<int64_t>("bits", 16) <= 8 ? 8 : 16;
            json files = json::object();
            const std::string stem = surfName(f);
            for (const std::string& m : masks)
            {
                const std::vector<float>* v = h.findMask(m);
                if (!v || v->size() != h.size())
                    throw CommandError("terrainMaskImages: the terrain has no mask " + m);
                const fs::path file = folder / (stem + "_" + m + ".png");
                writeLayerImage(file, w, hgt, gridImage(h, *v, w, hgt, layout), bits);
                files[m] = file.generic_string();
            }
            return {{"files", files}, {"resolution", json::array({w, hgt})}, {"layout", layout == ImageLayout::Uv ? "uv" : "map"}};
        },
        .examples = {"terrainMaskImages('Terrain', masks=['layer_rock', 'flow'])['files']"},
    });

    ctx.registerCommand({
        .name = "terrainSatmap",
        .doc = "Bake a terrain's colour from its material layers: each sample the layers' colours mixed by their weights "
               "(a layer's colour is its rule's, else its material's mean, else a default for its name), as an sRGB PNG for "
               "distant views, minimaps and far LODs. shade= multiplies in soft light from the sun direction. Returns the file "
               "and each layer's colour and where it came from.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"file", "f", MT::String, "", "PNG to write; defaults to <bake folder>/<terrain>_satmap.png."},
                  {"resolution", "r", MT::Int, int64_t(0), "Pixels along the longer side; 0 is one per sample.", {}, {}, 0.0, 16384.0},
                  {"layout", "lo", MT::String, "map", "map (-Z edge at the top) or uv (over the mesh's uv set 0).", {"map", "uv"}},
                  {"shade", "sh", MT::Float, 0.0, "0 to 1: how much hillshade to multiply in.", {}, {}, 0.0, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "terrainSatmap";
            const SurfTerrain f = surfOne(a, cmd);
            const Heightfield& h = *f.local;
            const std::optional<MaterialRules> rules = rulesOf(f);
            if (!rules)
                throw CommandError("terrainSatmap: the terrain has no material layers; set them with terrainMaterialRules");
            std::vector<std::string> warnings, masks;
            std::vector<glm::dvec3> colors;
            json layers = json::array();
            for (const LayerRule& l : rules->layers)
            {
                const SurfLook look = surfLookOf(l, warnings);
                masks.push_back(look.mask);
                colors.push_back(look.color);
                layers.push_back(lookJson(look));
            }
            const std::vector<glm::vec3> linear = blendColors(h, masks, colors);
            const double shade = a.get<double>("shade", 0.0);
            std::vector<float> chan[3];
            for (int c = 0; c < 3; ++c)
            {
                std::vector<float> v(h.size());
                for (size_t k = 0; k < v.size(); ++k)
                    v[k] = linear[k][c];
                if (shade > 0.0)
                    for (int z = 0; z < h.nz; ++z)
                        for (int x = 0; x < h.nx; ++x)
                        {
                            const glm::dvec2 p = h.samplePos(x, z);
                            const glm::dvec3 n = h.normalAt(p.x, p.y);
                            const double lambert = std::max(0.0, glm::dot(n, glm::normalize(glm::dvec3(-0.4, 0.8, -0.45))));
                            v[h.index(x, z)] *= float(1.0 - shade + shade * lambert / 0.8);
                        }
                chan[c] = std::move(v);
            }
            const ImageLayout layout = layoutOf(a);
            const auto [w, hgt] = imageSize(h, a);
            image::Image img{w, hgt, std::vector<float>(size_t(w) * size_t(hgt) * 4, 1.0f)};
            for (int c = 0; c < 3; ++c)
            {
                const std::vector<float> px = gridImage(h, chan[c], w, hgt, layout);
                for (size_t k = 0; k < px.size(); ++k)
                    img.rgba[k * 4 + size_t(c)] = srgbEncode(px[k]);
            }
            fs::path file = a.get<std::string>("file", "");
            if (file.empty())
                file = surfFolder() / (surfName(f) + "_satmap.png");
            fs::create_directories(file.parent_path());
            image::write(file, img, 8);
            warnAll(cmd, warnings);
            return {{"file", file.generic_string()}, {"resolution", json::array({w, hgt})}, {"layers", layers}, {"warnings", warnings}};
        },
        .examples = {"terrainSatmap('Terrain', resolution=2048, shade=0.5)['file']"},
    });

    ctx.registerCommand({
        .name = "terrainMaterialPreview",
        .doc = "Show a terrain's material layers in the viewport through its own material: a texture set on the terrain whose "
               "layer stack has one layer per terrain layer (its library material, tiled to its real size, or its colour), each "
               "masked by its weight so the stack mixes them exactly as the weights do. Rebuilt on every call; the masks are "
               "images in the bake folder, so run it again after changing the terrain or its rules. Tiling is capped at "
               "resolution/32 repeats so a texel stays visible (said in the result when it was). Returns the material, texture "
               "set and its layers.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"resolution", "r", MT::Int, int64_t(2048), "Texture set size.", {}, {}, 64.0, 8192.0},
                  {"materials", "m", MT::Bool, true, "Library materials; False draws every layer in its colour."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "terrainMaterialPreview";
            const SurfTerrain f = surfOne(a, cmd);
            if (!f.mesh)
                throw CommandError("terrainMaterialPreview: the terrain has no mesh to put a material on");
            const std::optional<MaterialRules> rules = rulesOf(f);
            if (!rules)
                throw CommandError("terrainMaterialPreview: the terrain has no material layers; set them with terrainMaterialRules");
            const Heightfield& h = *f.local;
            Graph& g = Graph::current();
            const std::string stem = surfName(f);
            const std::string setName = stem + "_Ground", material = "M_" + stem + "_Ground";
            const int res = int(std::clamp<int64_t>(a.get<int64_t>("resolution", 2048), 64, 8192));
            const bool useMaterials = a.get<bool>("materials", true);

            if (g.find(setName))
                ops::execute("delete", {{"args", {setName}}});
            if (!g.find(material))
                ops::execute("createMaterial", {{"args", {material}}});
            // The mesh comes out of the terrain's geometry nodes, so its Terrain Layers node carries the material slot.
            {
                SurfGroup t = surfGroup(f, cmd);
                gn::GNode* layers = surfNodeOfType(t.group, "TerrainLayers");
                if (!layers)
                    throw CommandError("terrainMaterialPreview: the terrain has no Terrain Layers node; run terrainMaterialRules");
                if (layers->values.value("material", json()) != json(material))
                {
                    layers->values["material"] = material;
                    surfCommit(t, cmd);
                }
            }
            ops::execute("texCreateSet", {{"args", {setName}}, {"resolution", res}, {"materials", {material}}});

            // Stack masks: each layer covers its share of everything up to it, so normal blending reproduces the weights.
            const fs::path folder = surfFolder() / "preview";
            fs::create_directories(folder);
            std::vector<double> below(h.size(), 0.0);
            const glm::dvec2 e = h.extent();
            std::vector<std::string> warnings;
            json layers = json::array();
            for (size_t i = 0; i < rules->layers.size(); ++i)
            {
                const LayerRule& l = rules->layers[i];
                const SurfLook look = surfLookOf(l, warnings);
                const std::vector<float>* w = h.findMask(look.mask);
                if (!w)
                    throw CommandError("terrainMaterialPreview: the terrain has no " + look.mask + "; it hasn't evaluated its layers");
                std::vector<float> stack(h.size());
                for (size_t k = 0; k < h.size(); ++k)
                {
                    const double total = below[k] + (*w)[k];
                    stack[k] = total > 1e-9 ? float((*w)[k] / total) : 0.0f;
                    below[k] = total;
                }
                json layer = lookJson(look);
                std::string id;
                if (useMaterials && !look.material.empty())
                {
                    const double want = std::max(e.x, e.y) / std::max(1.0, look.tileCm);
                    const double cap = res / 32.0;
                    const double tiles = std::min(want, cap);
                    if (want > cap)
                        layer["tilesCapped"] = {{"wanted", want}, {"used", tiles}};
                    const json added = ops::execute("applyLibraryMaterial", {{"args", {look.material}}, {"set", setName}, {"layer", true}, {"tiles", tiles}});
                    const json lid = added.value("layer", json());
                    id = lid.is_object() ? lid.value("id", std::string()) : lid.is_string() ? lid.get<std::string>() : std::string();
                    layer["tiles"] = tiles;
                    layer["kind"] = "material";
                }
                else
                {
                    const json c = json::array({look.color.x, look.color.y, look.color.z, 1.0});
                    id = ops::execute("texAddLayer", {{"args", {setName, "fill"}}, {"name", l.name}, {"material", {{"baseColor", c}, {"roughness", look.roughness}}}})
                             .value("id", std::string());
                    layer["kind"] = "color";
                }
                if (id.empty())
                    throw CommandError("terrainMaterialPreview: the texture set took no layer for " + l.name);
                ops::execute("texSetLayer", {{"args", {setName, id, {{"name", l.name}}}}});
                if (i > 0)
                {
                    const fs::path file = folder / (stem + "_" + l.name + "_stack.png");
                    writeLayerImage(file, res, res, gridImage(h, stack, res, res, ImageLayout::Uv), 16);
                    ops::execute("texAddMask", {{"args", {setName, id, "paint"}}, {"image", file.generic_string()}});
                    layer["maskImage"] = file.generic_string();
                }
                layer["layer"] = id;
                layers.push_back(layer);
            }
            const json display = syncDisplayLayers(f, *rules, warnings);
            warnAll(cmd, warnings);
            return {{"material", material}, {"set", setName}, {"layers", layers}, {"displayLayers", display}, {"resolution", res}, {"warnings", warnings}};
        },
        .examples = {"terrainMaterialPreview('Terrain')['layers']", "terrainMaterialPreview('Terrain', materials=False, resolution=1024)"},
    });
}

void registerCoverCommand(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "terrainGroundCover",
        .doc = "Scatter ground cover over a terrain by rules: rocks on scree (the deposit mask), trees by moisture or in the "
               "forest biome, grass on the grass layer, debris below cliffs, or your own. Each rule is {name, items, density "
               "(per m2 where it fits fully), footprint (cm kept clear; default the item's size across), when: {input: [min, "
               "max], biome: name(s), layer: name(s)}, scale: [min, max], align (0 upright, 1 on the normal), turn (degrees), "
               "sink (cm), solid (later rules keep clear of it), minFit, faceDownhill (turn +Z downhill)}, or a slope band preset by "
               "name: scree (the cliffTalus band below cliffs), ledgePlants (flat spots near cliffs) or hangingRoots (cliff tops, "
               "facing downhill), also as {preset, ...overrides}. Instances carry rule, item, fit, random, slope and downhill. "
               "Pieces go where they fit, kept with probability equal "
               "to the fit, and never inside another piece's footprint.\n\n"
               "items= maps each item name to a mesh; an item no mesh stands for gets a hidden placeholder (said in the "
               "result). The scatter is live: a new mesh <terrain>_GroundCover whose geometry nodes (Terrain Ground Cover) "
               "follow the terrain and the item meshes; settle=True drops the pieces with Set Dressing's physics. Send it to "
               "Unreal as foliage-style instances with unrealPlaceInstances(<mesh>, mode='hism'), or sendToUnreal=True. "
               "Returns the mesh, group and per rule the pieces placed, per hectare, those blocked by footprints, and the "
               "thinning when maxCount was reached.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"rules", "r", MT::Any, nullptr, "The rules (or preset names: scree, ledgePlants, hangingRoots); 'default' (the default) for rocks, trees, debris and grass. A rule's slot field names the slot type its pieces get (\"tree.palm\"); without one it is plant.<rule name>."},
                  {"items", "it", MT::Any, nullptr, "{item name: mesh} for the rules' items."},
                  {"seed", "sd", MT::Int, int64_t(1), "Another seed gives another scatter."},
                  {"density", "d", MT::Float, 1.0, "Multiplies every rule's density.", {}, {}, 0.0},
                  {"maxCount", "mc", MT::Int, int64_t(200000), "Most pieces; past it every rule thins by the same factor.", {}, {}, 0.0},
                  {"settle", "st", MT::Bool, false, "Drop the pieces with physics (Set Dressing's Settle Instances)."},
                  {"sendToUnreal", "ue", MT::Bool, false, "Then send it to Unreal as HISM instances."},
                  {"name", "n", MT::String, "", "The ground cover mesh; defaults to <terrain>_GroundCover."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            const char* cmd = "terrainGroundCover";
            const SurfTerrain f = surfOne(a, cmd);
            if (!f.mesh)
                throw CommandError("terrainGroundCover: the terrain has no mesh");
            Graph& g = Graph::current();
            const std::string stem = surfName(f);
            CoverSpec spec;
            try
            {
                const json r = a.flags.value("rules", json());
                spec = r.is_null() || (r.is_string() && r.get<std::string>() == "default") ? defaultCover(*f.local) : CoverSpec::fromJson(r);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("terrainGroundCover: ") + e.what());
            }
            spec.seed = uint32_t(a.get<int64_t>("seed", 1));
            spec.maxCount = int(std::max<int64_t>(0, a.get<int64_t>("maxCount", 200000)));

            const json given = a.flags.value("items", json::object());
            if (!given.is_object())
                throw CommandError("terrainGroundCover: items is {item name: mesh}");
            std::map<std::string, std::string> items;
            json placeholders = json::array();
            for (const CoverRule& r : spec.rules)
                for (const std::string& item : r.items)
                {
                    if (items.count(item))
                        continue;
                    if (given.contains(item))
                    {
                        Node* n = g.find(given[item].get<std::string>());
                        if (!n || !n->type().isA("mesh"))
                            throw CommandError("terrainGroundCover: item " + item + ": no mesh named '" + given[item].get<std::string>() + "'");
                        items[item] = n->path();
                    }
                    else if (Node* n = g.find(item); n && n->type().isA("mesh"))
                        items[item] = n->path();
                    else if (Node* p = g.find(stem + "_" + surfIdentifier(item)); p && p->type().isA("mesh"))
                    {
                        items[item] = p->path();
                        placeholders.push_back(item);
                    }
                    else
                    {
                        items[item] = makePlaceholder(g, item, stem);
                        placeholders.push_back(item);
                    }
                }
            for (const json& p : placeholders)
                log::warn("terrainGroundCover: no mesh for item {}; a hidden placeholder {} stands in (give items={{'{}': '<mesh>'}})", p.get<std::string>(),
                          items[p.get<std::string>()], p.get<std::string>());

            const bool settle = a.get<bool>("settle", false);
            if (settle && !gn::findDecl("SettleInstances"))
                throw CommandError("terrainGroundCover: settle needs the Stagehand extension (Set Dressing), which isn't loaded");
            json nodes = json::array({{{"name", "Terrain"}, {"type", "ObjectInfo"}, {"position", json::array({-400, 0})}, {"props", {{"object", f.mesh->path()}}}},
                                      {{"name", kCoverNode},
                                       {"type", "TerrainGroundCover"},
                                       {"position", json::array({0, 0})},
                                       {"props", {{"rules", spec.toJson().dump()}, {"terrain", "Terrain"}}}}});
            int row = 1;
            for (const auto& [item, mesh] : items)
                nodes.push_back({{"name", "Item_" + surfIdentifier(item)},
                                 {"type", "ObjectInfo"},
                                 {"position", json::array({-400, 140 * row++})},
                                 {"props", {{"object", mesh}}}});
            json links = json::array({"GroupInput.seed -> Cover.seed", "GroupInput.density -> Cover.density"});
            std::string last = std::string(kCoverNode) + ".instances";
            if (settle)
            {
                nodes.push_back({{"name", "Colliders"}, {"type", "ObjectInfo"}, {"position", json::array({0, -300})},
                                 {"props", {{"object", f.mesh->path()}, {"transformSpace", "world"}}}});
                nodes.push_back({{"name", "Settle"}, {"type", "SettleInstances"}, {"position", json::array({300, 0})}});
                links.push_back(last + " -> Settle.instances");
                links.push_back("Colliders.geometry -> Settle.colliders");
                last = "Settle.instances";
            }
            // Every piece becomes a slot: its rule picks the type, the rule's grid cell is its stable path.
            {
                const json raw = a.flags.value("rules", json());
                std::string types;
                for (size_t r = 0; r < spec.rules.size(); ++r)
                {
                    std::string type = "plant." + surfIdentifier(spec.rules[r].name);
                    if (raw.is_object() && raw.contains("rules") && raw["rules"].is_array() && r < raw["rules"].size() && raw["rules"][r].contains("slot"))
                        type = raw["rules"][r]["slot"].get<std::string>();
                    types += (types.empty() ? "" : ",") + type;
                }
                nodes.push_back({{"name", "Slots"},
                                 {"type", "Slots"},
                                 {"position", json::array({600, 0})},
                                 {"values",
                                  {{"type", types.empty() ? std::string("plant") : types},
                                   {"owner", "terrain:" + stem + "/cover"},
                                   {"typeAttribute", "rule"},
                                   {"pathAttributes", "rule,cellX,cellZ"},
                                   {"tags", "groundcover"}}}});
                links.push_back(last + " -> Slots.instances");
                last = "Slots.instances";
            }
            links.push_back(last + " -> GroupOutput.geometry");
            json inputs = json::array({{{"name", "seed"}, {"type", "int"}, {"default", 0}, {"description", "Another seed gives another scatter."}},
                                       {{"name", "density"}, {"type", "float"}, {"default", a.get<double>("density", 1.0)}, {"min", 0.0}, {"softMax", 4.0},
                                        {"description", "Multiplies every rule's density."}}});

            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = stem + "_GroundCover";
            const std::string group = surfIdentifier(name) + "_Scatter";
            Node* host = g.find(name);
            if (host && !host->type().isA("mesh"))
                throw CommandError("terrainGroundCover: " + name + " is not a mesh");
            if (!host)
            {
                json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
                host = g.find(made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>());
            }
            const json spec2 = {{"name", group}, {"description", "Ground cover scattered over " + f.mesh->name() + " by rules."}, {"inputs", inputs},
                                {"nodes", nodes}, {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec2}}, {"replace", true}});
            bool applied = false;
            for (scene::NodeId m : scene::modifiers::stack(g, host->id()))
                if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
                    if (Node* grpNode = gn::groupOfModifier(g, *mod); grpNode && gn::readGroup(*grpNode).name == group)
                        applied = true;
            if (!applied)
                ops::execute("gnApply", {{"args", {host->path(), group}}});
            g.evaluate();
            Node* modifier = nullptr;
            for (scene::NodeId m : scene::modifiers::stack(g, host->id()))
                if (Node* mod = g.node(m); mod && gn::isGnModifier(*mod))
                    modifier = mod;
            const json published = modifier ? surfaceResult(modifier->id(), kCoverNode) : json();
            if (published.contains("error"))
                throw CommandError("terrainGroundCover: " + published["error"].get<std::string>());
            json out = {{"mesh", host->path()}, {"group", group}, {"modifier", modifier ? modifier->path() : std::string()}};
            json itemsOut = json::object();
            for (const auto& [item, mesh] : items)
                itemsOut[item] = mesh;
            out["items"] = itemsOut;
            out["placeholders"] = placeholders;
            for (const char* k : {"rules", "count", "thinned", "missing", "missingItems"})
                out[k] = published.value(k, json());
            if (out["thinned"].is_number() && out["thinned"].get<double>() < 1.0)
                log::warn("terrainGroundCover: past maxCount {}; every rule thinned to {:.3f} of its density", spec.maxCount, out["thinned"].get<double>());
            for (const json& m : out.value("missing", json::array()))
                log::warn("terrainGroundCover: the terrain has no {}; ranges over it read 0", m.get<std::string>());
            if (a.get<bool>("sendToUnreal", false))
                out["unreal"] = ops::execute("unrealPlaceInstances", {{"args", {host->path()}}, {"mode", "hism"}});
            return out;
        },
        .examples = {"terrainGroundCover('Terrain')['rules']",
                     "terrainGroundCover('Terrain', rules=[{'name': 'pines', 'items': ['pine'], 'density': 0.02, 'when': {'biome': 'forest', 'slope': [0, 30]}}], "
                     "items={'pine': 'SM_Pine'})"},
    });
}
} // namespace

// ---------------------------------------------------------------------------------------------------- export hooks

json surfaceSplats(const Heightfield& h, Node* modifier, const fs::path& folder, const std::string& name)
{
    if (!modifier)
        return json();
    SurfTerrain f;
    f.modifier = modifier;
    f.local = std::make_shared<const Heightfield>(h);
    const std::optional<MaterialRules> rules = rulesOf(f);
    if (!rules)
        return json();
    std::vector<std::string> masks;
    for (const LayerRule& l : rules->layers)
        if (h.findMask(layerMask(l.name)))
            masks.push_back(layerMask(l.name));
    return writeSplats(h, masks, folder, name);
}

json surfaceLandscapeParams(json& params, Node* modifier, bool rvt)
{
    if (!modifier)
        return json();
    SurfTerrain f;
    f.modifier = modifier;
    f.local = gn::modifierHeightfield(modifier->id());
    if (!f.local)
        return json();
    const std::optional<MaterialRules> rules = rulesOf(f);
    if (!rules)
        return json();
    std::vector<std::string> warnings;
    json layers = json::array();
    // Unreal layer names are FNames, which ignore case: Riverbed and riverBed would be one layer.
    auto folded = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    };
    std::set<std::string> taken;
    for (json& l : params["layers"])
        taken.insert(folded(l.value("name", std::string())));
    for (const LayerRule& rule : rules->layers)
    {
        const SurfLook look = surfLookOf(rule, warnings);
        for (json& l : params["layers"])
            if (l.value("mask", std::string()) == look.mask)
            {
                // The paint layer takes the layer's own name (Grass) unless a mask already uses it.
                std::string ue = rule.name;
                ue[0] = char(std::toupper(static_cast<unsigned char>(ue[0])));
                if (!taken.count(folded(ue)))
                {
                    taken.erase(folded(l.value("name", std::string())));
                    l["name"] = ue;
                    taken.insert(folded(ue));
                }
                l["weightBlend"] = true;
                layers.push_back({{"name", l["name"]},
                                  {"color", json::array({look.color.x, look.color.y, look.color.z})},
                                  {"roughness", look.roughness},
                                  {"tileSize", look.tileCm},
                                  {"material", look.material}});
            }
    }
    warnAll("terrainSendToUnreal", warnings);
    if (layers.empty())
        return json();
    const bool userMaterial = params.contains("material") && params["material"].is_string() && !params["material"].get<std::string>().empty();
    if (!userMaterial)
        params["makeMaterial"] = false;
    return {{"id", params["id"]}, {"label", params["label"]}, {"layers", layers}, {"rvt", rvt}, {"assign", !userMaterial}};
}

json sendSurfaceMaterial(const std::shared_ptr<link::RpcClient>& client, const json& surface, int timeoutMs)
{
    if (surface.is_null() || !client)
        return json();
    const json features = link::Link::get().status().value("features", json::array());
    if (!features.is_array() || std::find(features.begin(), features.end(), json("terrainMaterial")) == features.end())
    {
        log::warn("terrainSendToUnreal: the LilaLink plugin predates terrain materials; layers went without the generated material. Rebuild it from Lila's ue/LilaLink");
        return {{"error", "the LilaLink plugin predates terrain materials; rebuild it from Lila's ue/LilaLink"}};
    }
    link::RpcResult r = client->call("setTerrainMaterial", surface, timeoutMs);
    if (!r.ok)
    {
        log::warn("terrainSendToUnreal: terrain material: {}", r.error);
        return {{"error", r.error}};
    }
    return r.result;
}

void registerSurfaceCommands(ext::Context& ctx)
{
    registerRuleCommands(ctx);
    registerImageCommands(ctx);
    registerCoverCommand(ctx);
}
} // namespace terrain
