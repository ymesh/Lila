#include "scene/SkyAtmosphere.h"

#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "scene/SceneIndex.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

namespace lila::scene::skyAtmosphere
{
namespace
{
constexpr uint32_t kStored = AttrInput | AttrStorable;

AttrDef number(std::string name, std::string shortName, double def, std::string doc, std::optional<double> lo = {},
               std::optional<double> hi = {})
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Float, .defaultValue = def,
            .flags = kStored | AttrKeyable, .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef vec3(std::string name, std::string shortName, glm::dvec3 def, std::string doc, bool colour)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def,
              .flags = kStored | AttrKeyable | (colour ? uint32_t(AttrColor) : 0u), .doc = std::move(doc)};
    if (colour)
        a.minValue = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + (colour ? "RGB" : "XYZ")[i];
        a.childShort[i] = shortName + (colour ? "rgb" : "xyz")[i];
    }
    return a;
}

std::vector<AttrDef> attrs()
{
    std::vector<AttrDef> a(kCount);
    a[kEnabled] = {.name = "enabled", .shortName = "en", .type = PlugType::Bool, .defaultValue = true, .flags = kStored,
                   .doc = "Draw this sky. The first enabled one in the scene applies."};
    a[kTransformMode] = {.name = "transformMode", .shortName = "tfm", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                         .enumNames = {"planetTopAtAbsoluteWorldOrigin", "planetTopAtComponentTransform", "planetCenterAtComponentTransform"},
                         .doc = "Where the planet sits: its ground at the world origin, or its ground or centre at the node's position."};
    a[kBottomRadius] = number("bottomRadius", "brd", 6360.0, "The planet's radius, km, times the node's scale.", 1.0, 100000.0);
    a[kGroundAlbedo] = vec3("groundAlbedo", "gal", glm::dvec3(0.401978), "The planet's ground colour, lighting the air above it and seen from high up.", true);
    a[kAtmosphereHeight] = number("atmosphereHeight", "ath", 60.0, "How high the air reaches above the ground, km.", 1.0, 200.0);
    a[kMultiScatteringFactor] = number("multiScatteringFactor", "msf", 1.0, "Light scattered more than once: 0 leaves only single scattering, darker at dusk.", 0.0, 2.0);
    a[kRayleighScatteringScale] = number("rayleighScatteringScale", "rss", 0.0331, "Strength of the air's Rayleigh scattering, the blue of the sky, per km.", 0.0, 2.0);
    a[kRayleighScattering] = vec3("rayleighScattering", "rsc", glm::dvec3(0.175287, 0.409607, 1.0), "Rayleigh scattering per colour, times its scale.", true);
    a[kRayleighExponentialDistribution] = number("rayleighExponentialDistribution", "red", 8.0, "Height over which the air's density falls to 1/e, km.", 0.01, 20.0);
    a[kMieScatteringScale] = number("mieScatteringScale", "mss", 0.003996, "Strength of haze and dust scattering, per km.", 0.0, 5.0);
    a[kMieScattering] = vec3("mieScattering", "msc", glm::dvec3(1.0), "Mie scattering per colour, times its scale.", true);
    a[kMieAbsorptionScale] = number("mieAbsorptionScale", "mas", 0.000444, "Light haze and dust absorb, per km.", 0.0, 5.0);
    a[kMieAbsorption] = vec3("mieAbsorption", "mab", glm::dvec3(1.0), "Mie absorption per colour, times its scale.", true);
    a[kMieAnisotropy] = number("mieAnisotropy", "man", 0.8, "How strongly haze scatters forward, making a glow round the sun, 0 to 0.999.", 0.0, 0.999);
    a[kMieExponentialDistribution] = number("mieExponentialDistribution", "med", 1.2, "Height over which haze thins to 1/e, km.", 0.01, 20.0);
    a[kOtherAbsorptionScale] = number("otherAbsorptionScale", "oas", 0.001881, "Strength of the ozone layer's absorption, per km; it keeps dusk skies blue.", 0.0, 0.2);
    a[kOtherAbsorption] = vec3("otherAbsorption", "oab", glm::dvec3(0.345561, 1.0, 0.045188), "Ozone absorption per colour, times its scale.", true);
    a[kOtherTentTipAltitude] = number("otherTentTipAltitude", "ota", 25.0, "Height where the ozone layer is densest, km.", 0.0, 60.0);
    a[kOtherTentTipValue] = number("otherTentTipValue", "otv", 1.0, "The ozone layer's density at its densest.", 0.0, 1.0);
    a[kOtherTentWidth] = number("otherTentWidth", "otw", 15.0, "How far the ozone layer reaches either side of its densest height, km.", 0.01, 60.0);
    a[kSkyLuminanceFactor] = vec3("skyLuminanceFactor", "slf", glm::dvec3(1.0), "Scales the sky's light, per colour, for art direction.", true);
    a[kAerialPerspectiveViewDistanceScale] = number("aerialPerspectiveViewDistanceScale", "apd", 1.0, "Scales distances for the haze over far objects: above 1 hazes them sooner.", 0.0, 3.0);
    a[kAerialPerspectiveStartDepth] = number("aerialPerspectiveStartDepth", "aps", 0.1, "No haze nearer the camera than this, km.", 0.001, 10.0);
    a[kHeightFogContribution] = number("heightFogContribution", "hfc", 1.0, "How much the sky's light colours height fog (a post process's fog).", 0.0, 1.0);
    a[kTransmittanceMinLightElevationAngle] = number("transmittanceMinLightElevationAngle", "tml", -90.0,
                                                     "Sun height (degrees) below which sunlight on the scene stops reddening and dimming.", -90.0, 90.0);
    return a;
}

glm::dmat4 worldMatrix(const Node& sky)
{
    // Pulled through the graph: the stored world matrix is stale until something evaluates it.
    Graph* g = const_cast<Graph*>(sky.graph());
    return g ? std::get<glm::dmat4>(g->value({sky.id(), Transform::kWorldMatrix, -1})) : sky.as<glm::dmat4>(Transform::kWorldMatrix);
}
} // namespace

int firstAttr()
{
    const NodeType* t = types::find("skyAtmosphere");
    return t ? int(t->attrs.size()) - kCount : 0;
}

NodeId active(const Graph& g)
{
    const int first = firstAttr();
    for (const Node* n : sceneIndex::ofType(g, "skyAtmosphere"))
        if (n->as<bool>(first + kEnabled) && effectivelyVisible(g, n->id()))
            return n->id();
    return kNoNode;
}

glm::dvec3 worldPosition(const Node& sky)
{
    return glm::dvec3(worldMatrix(sky)[3]);
}

double planetScale(const Node& sky)
{
    const double volume = std::abs(glm::determinant(glm::dmat3(worldMatrix(sky))));
    return volume > 1e-30 ? std::cbrt(volume) : 1.0;
}

double scaled(const Node& sky, Attr attr)
{
    const double v = sky.as<double>(firstAttr() + attr);
    switch (attr)
    {
    case kBottomRadius:
    case kAtmosphereHeight:
    case kRayleighExponentialDistribution:
    case kMieExponentialDistribution:
    case kOtherTentTipAltitude:
    case kOtherTentWidth:
    case kAerialPerspectiveStartDepth: return v * planetScale(sky);
    case kRayleighScatteringScale:
    case kMieScatteringScale:
    case kMieAbsorptionScale:
    case kOtherAbsorptionScale: return v / planetScale(sky);
    default: return v;
    }
}

glm::dvec3 planetCenter(const Node& sky, bool zUp)
{
    const int first = firstAttr();
    const glm::dvec3 up = zUp ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    const double radiusCm = scaled(sky, kBottomRadius) * 1.0e5;
    const glm::dvec3 position = worldPosition(sky);
    switch (sky.as<int64_t>(first + kTransformMode))
    {
    case 1: return position - up * radiusCm;
    case 2: return position;
    default: return -up * radiusCm;
    }
}

NodeId sunLight(const Graph& g)
{
    NodeId first = kNoNode;
    for (const Node* n : sceneIndex::ofType(g, "light"))
    {
        if (lights::Kind(n->as<int64_t>(Light::kLightType)) != lights::Kind::Directional || !effectivelyVisible(g, n->id(), true))
            continue;
        if (n->as<bool>(Light::kAtmosphereSun))
            return n->id();
        if (first == kNoNode)
            first = n->id();
    }
    return first;
}

void registerTypes()
{
    if (types::find("skyAtmosphere"))
        return;
    NodeType t;
    t.name = "skyAtmosphere";
    t.icon = "sun-high";
    t.base = types::find("locator");
    t.dag = true;
    t.doc = "A planet's sky and the haze over distant things, lit by the sun (a directional light), from the ground up to orbit. "
            "It lights the scene in place of the environment. Unreal's SkyAtmosphere, attribute for attribute.";
    t.ownAttrs = attrs();
    t.create = [] { return std::make_unique<Locator>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    if (ops::find("skyAtmosphere"))
        return;
    ops::registerCommand({
        .name = "skyAtmosphere",
        .doc = "Create a sky atmosphere and, when the scene has no directional light, a sun to light it, and select the sky. Returns its name.",
        .flags = {
            {"name", "n", T::String, nullptr, "Name; made unique."},
            {"existing", "e", T::Bool, false, "Return the active sky if there is one instead of creating another."},
            {"sun", "s", T::Bool, true, "Add a sun when the scene has no directional light."},
            {"sunElevation", "se", T::Float, 35.0, "The new sun's height above the horizon, degrees.", {}, {}, -90.0, 90.0},
            {"sunAzimuth", "sa", T::Float, 30.0, "The new sun's turn about the up axis, degrees."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.get<bool>("existing", false))
                if (NodeId id = active(g); id != kNoNode)
                {
                    ops::execute("select", {{"args", {g.node(id)->name()}}});
                    return g.node(id)->name();
                }
            const json sky = ops::execute("createNode", {{"args", {"skyAtmosphere"}}, {"name", a.get<std::string>("name", "skyAtmosphere")}});
            if (a.get<bool>("sun", true) && sunLight(g) == kNoNode)
            {
                const std::string sun = ops::execute("light", {{"type", "directional"}, {"name", "sun"}, {"intensity", 3.0}, {"skipSelect", true}});
                const double elevation = a.get<double>("sunElevation", 35.0), azimuth = a.get<double>("sunAzimuth", 30.0);
                // A directional light shines down its -Z; tip it below the horizon by the elevation, then turn it about up.
                const glm::dvec3 rotate = georef::sceneZUp() ? glm::dvec3(90.0 - elevation, 0.0, azimuth) : glm::dvec3(-elevation, azimuth, 0.0);
                ops::execute("setAttr", {{"args", {sun + ".rotate", json::array({rotate.x, rotate.y, rotate.z})}}});
                ops::execute("setAttr", {{"args", {sun + ".atmosphereSun", true}}});
            }
            ops::execute("select", {{"args", {sky}}});
            return sky;
        },
    });
    ops::registerCommand({
        .name = "fogVolume",
        .doc = "Create a box filled with fog: a cube with a new volume material. Returns the box.",
        .flags = {
            {"name", "n", T::String, "fogVolume", "Name of the box; made unique."},
            {"size", "sz", T::Float, 400.0, "Edge length, cm.", {}, {}, 1.0},
            {"density", "d", T::Float, 0.5, "The material's volumeDensity, per metre.", {}, {}, 0.0},
        },
        .run = [](const Args& a) -> json {
            const double size = a.get<double>("size", 400.0);
            json box = ops::execute("polyCube", {{"width", size}, {"height", size}, {"depth", size}, {"name", a.get<std::string>("name", "fogVolume")}});
            if (box.is_array())
                box = box.at(0);
            const std::string up = georef::sceneZUp() ? "translateZ" : "translateY";
            ops::execute("setAttr", {{"args", {box.get<std::string>() + "." + up, size * 0.5}}});
            const std::string material = ops::execute("createMaterial", {{"args", {"M_Fog"}}, {"shadingModel", "volume"},
                                                                         {"volumeDensity", a.get<double>("density", 0.5)}}).get<std::string>();
            ops::execute("assignMaterial", {{"args", {material, box}}});
            ops::execute("select", {{"args", {box}}});
            return box;
        },
    });
}
} // namespace lila::scene::skyAtmosphere
