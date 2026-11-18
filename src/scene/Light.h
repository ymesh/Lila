#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Transform.h"

#include <functional>
#include <string>

namespace lila::ext
{
class Context;
}

namespace lila::scene
{
/**
 * A light, as the formats Lila reads and writes describe one: kind, colour, intensity with its unit, falloff, cone
 * and area size on a locator-like transform. Spot, directional and area lights shine down their local -Z, as a
 * camera looks.
 *
 * The node only carries the data, so lights survive import, save and export whatever is installed. Lighting and
 * drawing them is up to a light provider (lights::registerProvider), which extensions register. A light no provider
 * claims, or only a placeholder provider, is a placeholder: the outliner flags it so it is obvious at a glance.
 */
class Light : public Locator
{
public:
    enum Attr
    {
        kLightType = Locator::kDrawSize + 1,
        kColor,
        kIntensity,
        kIntensityUnits,
        kUseTemperature,
        kTemperature,
        kRange,
        kInnerConeAngle,
        kOuterConeAngle,
        kWidth,
        kHeight,
        kSourceRadius,
        kCastShadows,
        kTexture,
        kProvider,
        kSourceFormat,
        kExtra,
        kLensFlare,
        kLensFlareIntensity,
        kLensFlareScale,
        kCount
    };
};

namespace lights
{
/** Matches the lightType enum's order. */
enum class Kind
{
    Point,
    Spot,
    Directional,
    Area, // a rectangle, width by height
    Disc, // width is the diameter
    Dome, // surrounds the scene, lit by texture when it has one
};

/** Matches the intensityUnits enum's order. */
enum class Units
{
    Unitless,
    Candela,
    Lumens,
    Lux,
    Nits,
    Watts,
};

/** Everything a light carries, read with settings() and written with setSettings(). Lengths cm, angles degrees. */
struct Settings
{
    Kind kind = Kind::Point;
    glm::dvec3 color{1.0}; // linear
    double intensity = 1.0;
    Units units = Units::Unitless;
    bool useTemperature = false;
    double temperature = 6500.0; // kelvin
    double range = 0.0;          // 0 = no cutoff
    double innerConeAngle = 0.0; // half angle, full brightness inside
    double outerConeAngle = 45.0; // half angle, dark outside
    double width = 100.0;
    double height = 100.0;
    double sourceRadius = 0.0;
    bool castShadows = true;
    std::string texture;      // dome image or IES profile
    std::string provider;     // provider id preferred for this light, empty for any
    std::string sourceFormat; // what it was read as ("fbx:Point", "maya:aiAreaLight", "gltf:spot"), empty when made here
    std::string extra;        // JSON of what the source format had beyond these, written back by the same format

    bool operator==(const Settings&) const = default;
};

LILA_SCENE_API const char* kindName(Kind kind);
LILA_SCENE_API Settings settings(const Node& light);
/**
 * The light as it shines: its lengths times its world scale. Width and height take the X and Y scale; range and source
 * radius the mean scale (a directional light's source radius is an angle and stays). Cone angles never scale.
 */
LILA_SCENE_API Settings worldSettings(const Node& light);
LILA_SCENE_API void setSettings(Graph& g, NodeId light, const Settings& s);
/** Creates a light node under parent with s applied; the new node. */
LILA_SCENE_API Node& create(Graph& g, const std::string& name, NodeId parent, const Settings& s);

/** The light's colour with its temperature folded in when it uses one, linear. */
LILA_SCENE_API glm::dvec3 effectiveColor(const Settings& s);
/** Linear RGB of a black body at kelvin, normalised to a peak of 1. */
LILA_SCENE_API glm::dvec3 kelvinToLinear(double kelvin);

/**
 * Lights and draws the lights it handles. Several can be installed: the first non-placeholder provider that handles a
 * light owns it (the light's own provider attr is asked first), and a placeholder provider only stands in when none do.
 */
struct Provider
{
    std::string id;
    std::string name;
    /** Draws a stand-in and nothing else. Lights it owns are flagged in the outliner. */
    bool placeholder = false;
    /** Empty handles every light. */
    std::function<bool(const Node& light)> handles;
};

/** Lila's own provider's id: the viewport and renders light what it owns. */
inline constexpr const char* kLilaProvider = "lila";

LILA_SCENE_API Registry<Provider>& providers();
/** Registers through ctx so it goes when the extension unloads. Throws std::invalid_argument when id is taken. */
LILA_SCENE_API void registerProvider(ext::Context& ctx, Provider provider);
/** The provider that owns light, null when none handles it. */
LILA_SCENE_API const Provider* providerFor(const Node& light);
/** No provider, or only a placeholder one, owns light. False for anything that is not a light. */
LILA_SCENE_API bool isPlaceholder(const Node& light);
} // namespace lights

LILA_SCENE_API void registerLightTypes();
LILA_SCENE_API void registerLightCommands();
} // namespace lila::scene
