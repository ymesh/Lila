#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace terrain
{
/**
 * A soft range over one input field: 1 inside [lo, hi], easing to 0 over fade outside it (smoothstep). Inputs are a mask
 * of the heightfield or a derived field (see surfaceFieldNames). Heights are cm, slope and aspect degrees.
 */
struct Term
{
    std::string input;
    double lo = -1e30;
    double hi = 1e30;
    double fade = 0.0;
    bool invert = false;
    /** Blur radius over the input before the range is read, cm: class masks from a grid soften into gradients the layer's noise can break up. */
    double blur = 0.0;

    double eval(double v) const;
    nlohmann::json toJson() const;
};

/**
 * Reads a rule's "when": {input: [min, max] or {range, min, max, above, below, fade, invert}, biome: names, layer: names}
 * into terms (all must hold) and the weight masks it names; a list of such objects goes to any. Layers are only
 * accepted when layers is given. Throws std::runtime_error.
 */
void readWhen(const nlohmann::json& when, std::vector<Term>& all, std::vector<std::vector<Term>>& any, std::vector<std::string>& biomes,
              const std::string& owner, std::vector<std::string>* layers = nullptr);

/** Noise that breaks up a rule's edges: smoothstep(a + amount * fbm(world xz / scale)). */
struct Breakup
{
    double amount = 0.0;
    double scale = 3000.0;
};

/**
 * One terrain layer: where it lies (terms multiplied, times the summed weight of its biomes when it names any), its
 * material and its look in previews and engines.
 */
struct LayerRule
{
    std::string name;
    /** A material library id (Meadowlight's by default); empty paints the colour. */
    std::string material;
    glm::dvec3 color{0.5};
    bool colorGiven = false;
    double roughness = 0.85;
    /** Texture repeat in cm; 0 takes the material's own size. */
    double tileCm = 0.0;
    double opacity = 1.0;
    std::vector<Term> terms;
    std::vector<std::string> biomes;
    Breakup noise;
};

/**
 * Terrain layers, bottom first. The first is the base: it takes whatever the layers above leave. Each layer above
 * covers its rule's share of what is left under the ones above it, so the weights always sum to 1.
 */
struct MaterialRules
{
    std::vector<LayerRule> layers;
    uint32_t seed = 0;

    nlohmann::json toJson() const;
    /** Throws std::runtime_error naming what is wrong. */
    static MaterialRules fromJson(const nlohmann::json& j);
};

/** The mask a layer's weight lives in. */
std::string layerMask(const std::string& layer);
/** The mask a biome's weight lives in. */
std::string biomeMask(const std::string& biome);
/** Derived fields every rule can read besides the masks. */
const std::vector<std::string>& surfaceFieldNames();

/**
 * The fields rules read over one heightfield: its masks, or derived ones (height, slope, aspect, curvature, nearCliff,
 * and the analysis fields such as wet) computed on first use.
 */
class SurfaceFields
{
public:
    explicit SurfaceFields(const Heightfield& h);
    /** Null when the heightfield has no such mask and it isn't a derived field. */
    const std::vector<float>* get(const std::string& name);
    /** get(name) blurred over radius cm (box filter, three passes), cached. */
    const std::vector<float>* blurred(const std::string& name, double radius);
    const Heightfield& heightfield() const { return mH; }

private:
    const Heightfield& mH;
    std::map<std::string, std::vector<float>> mDerived;
    std::shared_ptr<const Heightfield> mShared;
};

/** A layer's or biome's share of the terrain: mean weight, the area that is, and where it is the largest weight. */
struct Coverage
{
    std::string name;
    std::string mask;
    double coverage = 0.0;
    double areaM2 = 0.0;
    double dominant = 0.0;
};

struct RulesResult
{
    std::vector<Coverage> layers;
    /** Inputs the rules named that the terrain doesn't have; those terms read 0. */
    std::vector<std::string> missing;
    /** Largest |sum of weights - 1| over every sample. */
    double maxSumError = 0.0;
};

/** Writes layer_<name> for every layer. Deterministic: the same heightfield and rules give the same bytes. */
RulesResult applyMaterialRules(Heightfield& h, const MaterialRules& r);

/** Mean weight, area and dominance of each mask, in the order given. */
std::vector<Coverage> coverageOf(const Heightfield& h, const std::vector<std::string>& names, const std::vector<std::string>& masks);

/** How climate is worked out over the terrain. Heights cm, temperatures degrees C. */
struct Climate
{
    double seaLevel = 0.0;
    /** At sea level. */
    double temperature = 14.0;
    /** Cooling per 100 m of height. */
    double lapseRate = 0.65;
    /** Warming of slopes facing the sun at full steepness, cooling of those facing away. */
    double aspectEffect = 3.0;
    /** Direction the sun stands in, degrees clockwise from north (-Z): 180 is the northern hemisphere's south. */
    double sunAzimuth = 180.0;
    /** Rain over open ground, 0 to 1. */
    double rain = 0.5;
    /** Direction the wet wind blows toward, degrees clockwise from north. */
    double windAzimuth = 90.0;
    /** How much of the rain a range takes out of the wind behind it, 0 to 1. */
    double rainShadow = 0.6;
    /** How far upwind the shadow looks, cm. */
    double shadowDistance = 2000000.0;
    /** How much wetter the windward slopes get. */
    double orographic = 0.3;
    /** How much water on the ground (wet: rivers, valley floors) adds to moisture. */
    double groundWater = 0.6;
};

/** A biome: where it fits by temperature, moisture, height and slope, or where a painted mask says. */
struct BiomeRule
{
    std::string name;
    std::vector<Term> terms;
    /** A painted mask that forces this biome where it is 1. */
    std::string paint;
};

struct BiomeSpec
{
    Climate climate;
    std::vector<BiomeRule> biomes;
    /** How sharply biomes change: higher gives harder edges. */
    double sharpness = 4.0;
    /** An imported landcover mask: classes by value (0 to 1 times scale), each the biome it is, mixed in by weight. */
    std::string landcover;
    double landcoverScale = 255.0;
    double landcoverWeight = 1.0;
    std::map<std::string, std::vector<int>> landcoverClasses;

    nlohmann::json toJson() const;
    static BiomeSpec fromJson(const nlohmann::json& j);
};

/** Temperature as written to the temperature mask: 0 to 1 over -30 to 40 degrees C. */
double temperatureToMask(double celsius);
double maskToTemperature(double m);

/** Writes temperature, moisture and biome_<name> per biome (summing to 1). */
RulesResult applyBiomes(Heightfield& h, const BiomeSpec& s);

/** Snow, alpine, forest, meadow, scrub and wetland over a temperate climate. */
BiomeSpec defaultBiomes();
/**
 * Layers for a terrain as it is: grass at the base, forest on hillsides (or in the forest biome), dirt on worn ground,
 * gravel on scree, rock on steep slopes, riverbed in the channels, sand at the shore when a sea level is given, snow up
 * high (by temperature when the terrain has biomes; with a sea level, the top of the land above it and never below 3 km;
 * else the top of its height range) and road wherever a road mask exists.
 */
MaterialRules defaultMaterialRules(const Heightfield& h, std::optional<double> seaLevel = std::nullopt);
/** The Meadowlight material and colour a layer of this name gets when its rule names none. */
std::string defaultMaterialFor(const std::string& layer);
glm::dvec3 defaultColorFor(const std::string& layer);
/** The mean base colour (linear) of a shipped library material, measured once from its graph; nullopt for others. */
std::optional<glm::dvec3> knownMaterialColor(const std::string& id);

/**
 * The masks packed four to an RGBA image, in the order given, as 8-bit values whose four channels in every image sum to
 * 255 across all images (largest remainder), so weights summing to 1 stay summing to 1. One byte array per image,
 * rows along +X, the first row the -Z edge.
 */
std::vector<std::vector<uint8_t>> packSplats(const Heightfield& h, const std::vector<std::string>& masks);

/** <name>_splat<N>.png per four masks; returns [{file, channels: [mask, ...]}]. */
nlohmann::json writeSplats(const Heightfield& h, const std::vector<std::string>& masks, const std::filesystem::path& folder, const std::string& name);

/** Colour per sample: the layers' colours by their weights. */
std::vector<glm::vec3> blendColors(const Heightfield& h, const std::vector<std::string>& masks, const std::vector<glm::dvec3>& colors);

/** Image orientation: map has its first row on the -Z edge (north up); uv lays it over the terrain mesh's uv set 0. */
enum class ImageLayout : uint8_t
{
    Map,
    Uv,
};

/** A grid (one value per sample) resampled to width x height in a layout, rows top first. */
std::vector<float> gridImage(const Heightfield& h, const std::vector<float>& values, int width, int height, ImageLayout layout);
} // namespace terrain
