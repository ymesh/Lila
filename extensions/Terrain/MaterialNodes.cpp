// The terrain surface nodes: Terrain Biomes and Terrain Layers write weight masks onto the heightfield; Terrain Ground
// Cover scatters items over a terrain read through Object Info.

#include "GroundCover.h"
#include "MaterialNodes.h"
#include "Materials.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>

#include <glm/gtc/matrix_transform.hpp>

#include <map>
#include <mutex>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
using gn::ExecContext;
using gn::GeometryPtr;
using LayerSocket = gn::SocketType;

constexpr const char* kSurfaceCategory = "Terrain";

gn::SocketDecl surfaceGeo(std::string id, std::string doc = {}) { return {.id = std::move(id), .type = LayerSocket::Geometry, .doc = std::move(doc)}; }

gn::SocketDecl surfaceIn(std::string id, LayerSocket t, json def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc), .min = lo, .max = hi};
}

gn::PropDecl surfaceProp(std::string id, std::string def, std::string doc, std::string editor = "code")
{
    return {.id = std::move(id), .defaultValue = std::move(def), .doc = std::move(doc), .editor = std::move(editor)};
}

json jsonProp(const ExecContext& c, const char* id)
{
    const std::string text = c.prop(id);
    if (text.empty())
        return json();
    try
    {
        return json::parse(text);
    }
    catch (const std::exception& e)
    {
        throw gn::ExecError(c.node.name + ": " + id + " is not JSON: " + e.what());
    }
}

struct PublishedSurfaces
{
    std::mutex mutex;
    std::map<std::pair<uint64_t, std::string>, json> results;
};

PublishedSurfaces& publishedSurfaces()
{
    static PublishedSurfaces p;
    return p;
}

void publishSurface(uint64_t modifier, const std::string& node, json result)
{
    PublishedSurfaces& p = publishedSurfaces();
    std::lock_guard lock(p.mutex);
    p.results[{modifier, node}] = std::move(result);
}

json rulesJson(const RulesResult& r)
{
    json layers = json::array();
    for (const Coverage& c : r.layers)
        layers.push_back({{"name", c.name}, {"mask", c.mask}, {"coverage", c.coverage}, {"areaM2", c.areaM2}, {"dominant", c.dominant}});
    return {{"layers", layers}, {"missing", r.missing}, {"maxSumError", r.maxSumError}};
}

Heightfield inputHeightfield(const ExecContext& c, const GeometryPtr& g)
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

gn::NodeDecl biomesDecl()
{
    return {
        .type = "TerrainBiomes",
        .label = "Terrain Biomes",
        .category = kSurfaceCategory,
        .doc = "Climate and biomes over the heightfield. Writes temperature (0 to 1 over -30 to 40 C: colder with height, warmer "
               "on slopes facing the sun), moisture (rain, less in the lee of ranges, more on windward slopes and where water "
               "gathers) and biome_<name> per biome, the biome weights summing to 1 everywhere. Biomes fit by temperature (C), "
               "moisture, height and slope, or follow a painted mask or an imported landcover mask. terrainBiomes writes the spec.",
        .props = {surfaceProp("biomes", "", "The spec, JSON: {climate, biomes: [{name, when, paint}], sharpness, landcover}; empty uses the defaults.")},
        .sockets = {.inputs = {surfaceGeo("heightfield")}, .outputs = {surfaceGeo("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = inputHeightfield(c, g);
                BiomeSpec spec;
                try
                {
                    const json j = jsonProp(c, "biomes");
                    spec = j.is_null() ? defaultBiomes() : BiomeSpec::fromJson(j);
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                publishSurface(c.env.owner, c.node.name, rulesJson(applyBiomes(h, spec)));
                c.out("heightfield", replaceHeightfield(g, std::move(h)));
            },
    };
}

gn::NodeDecl layersDecl()
{
    return {
        .type = "TerrainLayers",
        .label = "Terrain Layers",
        .category = kSurfaceCategory,
        .doc = "Material layers over the heightfield from rules: each layer (grass, rock, dirt, sand, snow, gravel, road, riverbed "
               "or your own) lies where its ranges over masks and derived fields hold (height, slope, aspect, curvature, nearCliff, "
               "flow, wear, deposit, wet, temperature, moisture, any painted mask) and in the biomes it names, with noise to break "
               "up its edges. Writes layer_<name> per layer; the first layer is the base and the weights sum to 1 everywhere. "
               "Put it last, after erosion and stamps. terrainMaterialRules writes the rules.",
        .props = {surfaceProp("rules", "", "The rules, JSON: {seed, layers: [{name, material, color, when, noise, ...}]}, bottom first; empty uses the defaults.")},
        .sockets = {.inputs = {surfaceGeo("heightfield"), surfaceIn("seed", LayerSocket::Int, 0, "Moves the edge noise."),
                               surfaceIn("material", LayerSocket::String, "", "Material the terrain's mesh is drawn with; terrainMaterialPreview sets it.")},
                    .outputs = {surfaceGeo("heightfield")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("heightfield");
                Heightfield h = inputHeightfield(c, g);
                MaterialRules rules;
                try
                {
                    const json j = jsonProp(c, "rules");
                    rules = j.is_null() ? defaultMaterialRules(h) : MaterialRules::fromJson(j);
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                rules.seed += uint32_t(c.i("seed"));
                publishSurface(c.env.owner, c.node.name, rulesJson(applyMaterialRules(h, rules)));
                GeometryPtr out = replaceHeightfield(g, std::move(h));
                if (const std::string material = c.s("material"); !material.empty())
                {
                    // An empty mesh carrying the slot: the heightfield's faces join it as the output mesh and take slot 0.
                    geo::Mesh slot = out->mesh ? *out->mesh : geo::Mesh();
                    if (slot.materialNames().empty())
                    {
                        slot.materialNames() = {material};
                        slot.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
                    }
                    gn::Geometry withSlot = *out;
                    withSlot.mesh = std::make_shared<const geo::Mesh>(std::move(slot));
                    out = gn::makeGeometry(std::move(withSlot));
                }
                c.out("heightfield", out);
            },
    };
}

/** An item's radius across the ground at scale 1: its extent from the pivot, perpendicular to the scene's up. */
double itemRadius(const geo::Mesh& m)
{
    const bool zUp = math::upAxis() == math::UpAxis::Z;
    double r = 0.0;
    for (geo::Index p = 0; p < m.numPoints(); ++p)
    {
        const glm::dvec3& q = m.position(p);
        r = std::max(r, zUp ? std::hypot(q.x, q.y) : std::hypot(q.x, q.z));
    }
    return std::max(1.0, r);
}

/** The rotation turning a onto b (unit vectors). */
glm::dmat4 turnOnto(const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 axis = glm::cross(a, b);
    const double s = glm::length(axis), c = glm::dot(a, b);
    if (s < 1e-9)
        return c > 0.0 ? glm::dmat4(1.0) : glm::rotate(glm::dmat4(1.0), 3.141592653589793, glm::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1));
    return glm::rotate(glm::dmat4(1.0), std::atan2(s, c), axis / s);
}

gn::NodeDecl coverDecl()
{
    return {
        .type = "TerrainGroundCover",
        .label = "Terrain Ground Cover",
        .category = kSurfaceCategory,
        .doc = "Scatters items over a terrain by rules: each rule's items go where its ranges, biomes and layers fit, as densely "
               "as it asks, kept clear of each other by their footprints (the item's size across, or the rule's footprint). The "
               "terrain comes from the Object Info node named in terrain, each item from the Object Info node Item_<name>. "
               "Outputs instances with rule, item, fit, random, cellX and cellZ attributes, ready for Settle Instances and for "
               "unrealPlaceInstances (HISM). terrainGroundCover builds the group.",
        .props = {surfaceProp("rules", "", "The rules, JSON: {seed, maxCount, rules: [{name, items, density, footprint, when, scale, align, turn, sink, solid}]}."),
                  surfaceProp("terrain", "Terrain", "The Object Info node reading the terrain.", "")},
        .sockets = {.inputs = {surfaceIn("seed", LayerSocket::Int, 0, "Another seed gives another scatter."),
                               surfaceIn("density", LayerSocket::Float, 1.0, "Multiplies every rule's density.", 0.0)},
                    .outputs = {surfaceGeo("instances", "The pieces, as instances.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                CoverSpec spec;
                try
                {
                    spec = CoverSpec::fromJson(jsonProp(c, "rules"));
                }
                catch (const std::exception& e)
                {
                    throw gn::ExecError(c.node.name + ": " + e.what());
                }
                spec.seed += uint32_t(c.i("seed"));
                spec.densityScale *= std::max(0.0, c.f("density"));
                auto terrainIt = c.env.objects.find(c.prop("terrain"));
                if (terrainIt == c.env.objects.end() || !terrainIt->second.mesh)
                {
                    publishSurface(c.env.owner, c.node.name, {{"error", "no terrain: Object Info node " + c.prop("terrain") + " reads nothing"}});
                    c.out("instances", gn::makeGeometry({}));
                    return;
                }
                const Heightfield h = heightfieldFromGridMesh(*terrainIt->second.mesh);
                if (h.empty())
                    throw gn::ExecError(c.node.name + ": " + c.prop("terrain") + " is not a terrain's grid mesh");
                const glm::dmat4 terrainWorld = terrainIt->second.matrix;

                auto inst = std::make_shared<gn::Instances>();
                std::map<std::string, int> refOf;
                std::vector<std::vector<double>> radius(spec.rules.size());
                std::vector<std::vector<int>> refs(spec.rules.size());
                json missingItems = json::array();
                for (size_t r = 0; r < spec.rules.size(); ++r)
                    for (const std::string& item : spec.rules[r].items)
                    {
                        auto it = c.env.objects.find("Item_" + item);
                        if (it == c.env.objects.end() || !it->second.mesh || it->second.mesh->numPoints() == 0)
                        {
                            missingItems.push_back(item);
                            radius[r].push_back(50.0);
                            refs[r].push_back(-1);
                            continue;
                        }
                        auto [at, fresh] = refOf.emplace(item, int(inst->references.size()));
                        if (fresh)
                        {
                            gn::Geometry ref;
                            ref.mesh = it->second.mesh;
                            inst->references.push_back(gn::makeGeometry(std::move(ref)));
                        }
                        radius[r].push_back(itemRadius(*it->second.mesh));
                        refs[r].push_back(at->second);
                    }

                const CoverResult res = scatterCover(h, spec, radius);
                const bool zUp = math::upAxis() == math::UpAxis::Z;
                const glm::dvec3 up = zUp ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
                const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(glm::dmat3(terrainWorld)));
                const glm::dmat4 toOwner = glm::inverse(c.env.ownerMatrix);
                std::vector<const CoverPiece*> kept;
                for (const CoverPiece& p : res.pieces)
                {
                    const int ref = refs[size_t(p.rule)][size_t(p.item)];
                    if (ref < 0)
                        continue;
                    const CoverRule& rule = spec.rules[size_t(p.rule)];
                    const glm::dvec3 n = glm::normalize(normalMatrix * p.normal);
                    const glm::dvec3 target = glm::normalize(glm::mix(up, n, rule.align));
                    const glm::dvec3 at = glm::dvec3(terrainWorld * glm::dvec4(p.position, 1.0)) - target * (rule.sink * p.scale);
                    glm::dmat4 m = glm::translate(glm::dmat4(1.0), at) * glm::rotate(glm::dmat4(1.0), p.turn, target) * turnOnto(up, target) *
                                   glm::scale(glm::dmat4(1.0), glm::dvec3(p.scale));
                    inst->add(toOwner * m, ref);
                    kept.push_back(&p);
                }
                auto ruleAttr = inst->attrs.getOrAdd<int32_t>("rule");
                auto itemAttr = inst->attrs.getOrAdd<int32_t>("item");
                auto fitAttr = inst->attrs.getOrAdd<float>("fit");
                auto randomAttr = inst->attrs.getOrAdd<float>("random");
                auto slopeAttr = inst->attrs.getOrAdd<float>("slope");
                auto downhillAttr = inst->attrs.getOrAdd<glm::vec3>("downhill");
                auto cellXAttr = inst->attrs.getOrAdd<int32_t>("cellX");
                auto cellZAttr = inst->attrs.getOrAdd<int32_t>("cellZ");
                for (size_t k = 0; k < kept.size(); ++k)
                {
                    ruleAttr[k] = kept[k]->rule;
                    itemAttr[k] = kept[k]->item;
                    fitAttr[k] = kept[k]->fit;
                    randomAttr[k] = kept[k]->random;
                    slopeAttr[k] = float(kept[k]->slope);
                    cellXAttr[k] = kept[k]->cellX;
                    cellZAttr[k] = kept[k]->cellZ;
                    downhillAttr[k] = glm::vec3(glm::normalize(glm::dvec3(terrainWorld * glm::dvec4(kept[k]->downhill, 0.0))));
                }

                json rules = json::array();
                for (size_t r = 0; r < spec.rules.size(); ++r)
                {
                    const CoverRuleStats& s = res.rules[r];
                    const glm::dvec2 e = h.extent();
                    rules.push_back({{"name", spec.rules[r].name},
                                     {"candidates", s.candidates},
                                     {"placed", s.placed},
                                     {"blocked", s.blocked},
                                     {"meanFit", s.meanFit},
                                     {"perHectare", double(s.placed) / std::max(1e-9, e.x * e.y / 1e8)}});
                }
                publishSurface(c.env.owner, c.node.name,
                               {{"rules", rules}, {"count", kept.size()}, {"thinned", res.thinned}, {"missing", res.missing}, {"missingItems", missingItems}});
                gn::Geometry out;
                out.instances = std::move(inst);
                c.out("instances", gn::makeGeometry(std::move(out)));
            },
    };
}
} // namespace

json surfaceResult(uint64_t modifier, const std::string& node)
{
    PublishedSurfaces& p = publishedSurfaces();
    std::lock_guard lock(p.mutex);
    auto it = p.results.find({modifier, node});
    return it == p.results.end() ? json() : it->second;
}

bool registerSurfaceNodes(ext::Context& ctx)
{
    for (gn::NodeDecl d : {biomesDecl(), layersDecl(), coverDecl()})
        if (!gn::registerDecl(ctx, std::move(d)))
            return false;
    return true;
}
} // namespace terrain
