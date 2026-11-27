#pragma once

#include "Erosion.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * Landform processes beyond the stepped stream-power erosion of Erosion.h: the analytic stream-power solution at an age,
 * multi-scale amplification, debris flow, thermal erosion with a talus angle per material, droplet rills, sediment
 * deposition, snow, and the Erode preset chain that runs them in order. CPU, deterministic at any thread count. Lengths
 * and heights are cm in the heightfield's own space.
 *
 * Masks they write (0 to 1): flow and lakes (drainage), wear (removed), deposit (laid down), sediment (deposited depth),
 * wet (where water ran), talus (scree from thermal and debris), snow.
 */
namespace terrain
{
/** Where the landform steps get their drainage: drain(), or drainGpu() (the same result, faster) when gpu is set. */
struct DrainSource
{
    bool gpu = false;
    /** Why the GPU was not used, from the first time it was not; empty while it ran every time. */
    std::string fallback;

    Drainage operator()(const Heightfield& h);
};

/** Stream power with n = 1 solved along the drainage tree for a time, instead of stepped (Tzathas 2024). */
struct AnalyticParams
{
    /** 0 is the input, 1 the steady state almost everywhere: valleys cut back from the outlets as it grows. */
    double age = 0.5;
    /** Erodibility multiplier; the hardness mask slows it locally. */
    double strength = 1.0;
    /** Exponent on drainage area (m in K A^m S). */
    double areaExponent = 0.5;
    std::string hardnessMask;
    /** Mask scaling the uplift that builds the steady-state ranges (empty: uniform). */
    std::string upliftMask;
    /** Rescale the result to the input's height range. */
    bool keepRange = true;
    /** Write flow, lakes, wear and deposit (costs one more drainage pass). */
    bool writeMasks = true;
};

void erodeAnalytic(Heightfield& h, const AnalyticParams& p, DrainSource* drains = nullptr);

/** Catmull-Rom resampling of the heights onto another grid; masks bilinear. */
Heightfield upsampledCubic(const Heightfield& h, int nx, int nz, double cell, glm::dvec2 origin);

/** Low resolution to high resolution keeping the drainage (after Schott 2024): upsample, add detail, erode the detail. */
struct AmplifyParams
{
    /** Samples per input cell along each side. */
    int factor = 2;
    /** Detail noise height as a fraction of the local relief over one input cell. */
    double detail = 0.35;
    /** Age of the fine-scale analytic erosion that carves the detail along the drainage. */
    double age = 0.06;
    uint32_t seed = 0;
};

/** h at factor times the resolution over the same area. */
Heightfield amplified(const Heightfield& h, const AmplifyParams& p);
/**
 * coarse brought onto fine's grid, its large shapes kept and fine's own detail (fine minus its low-pass) laid over them,
 * then eroded at the fine scale: what the preset uses after shaping the macro form at low resolution.
 */
Heightfield amplifiedOnto(const Heightfield& coarse, const Heightfield& fine, double age, uint32_t seed, DrainSource* drains = nullptr);

/** Debris flow (after Jain 2024): steep, small catchments scour; material runs out and settles as cones below. */
struct DebrisParams
{
    double strength = 1.0;
    /** Slopes steeper than this scour, degrees. */
    double scourAngle = 32.0;
    /** Below this the flow drops what it carries, degrees. */
    double settleAngle = 14.0;
    /** Catchments larger than this are rivers, not debris flows, m2. */
    double maxArea = 2.0e6;
    int iterations = 3;
};

void debrisFlow(Heightfield& h, const DebrisParams& p, DrainSource* drains = nullptr);

/** Talus angle per sample: base, then each (mask, angle) mixed over it by the mask's value, in order. */
std::vector<float> talusAngles(const Heightfield& h, double baseAngle, const std::vector<std::pair<std::string, double>>& materials);

/** Thermal erosion with a talus angle (degrees) per sample; writes the talus mask (where scree landed). */
void thermalMaterial(Heightfield& h, const std::vector<float>& angles, int iterations);

/** Droplet hydraulic erosion: many small water drops carve rills and drop sediment as they slow. */
struct DropletParams
{
    /** Drops per sample. */
    double density = 0.2;
    int maxSteps = 64;
    double inertia = 0.1;
    double capacity = 6.0;
    double erodeRate = 0.3;
    double depositRate = 0.25;
    double evaporation = 0.02;
    /** Erosion footprint radius, samples. */
    int radius = 2;
    /** Height units: a drop sees heights divided by cell * heightScale, so rills scale with the grid. */
    double strength = 1.0;
    uint32_t seed = 0;
};

void droplets(Heightfield& h, const DropletParams& p);

/** Sediment carried down the drainage and laid down where it slows: alluvial fans, filled valley floors and lakes. */
struct DepositionParams
{
    /** Material weathered off slopes, cm per step at slope 1. */
    double supply = 40.0;
    /** How much the flow can carry: capacity = transport * area^m * slope. */
    double transport = 2.0;
    double areaExponent = 0.5;
    /** Fraction of the excess load dropped per sample. */
    double depositRate = 0.35;
    /** Fill lakes with what reaches them. */
    bool fillLakes = true;
    std::string hardnessMask;
    int iterations = 2;
};

void deposition(Heightfield& h, const DepositionParams& p, DrainSource* drains = nullptr);

/** Snow: lies above the snowline on slopes gentle enough to hold it, gathers in hollows and slides off steep ground. */
struct SnowParams
{
    /** Height where snow starts, cm; NaN takes the 60th percentile height. */
    double snowline;
    /** Height over which cover goes from none to full, cm; 0 is a tenth of the relief. */
    double band = 0.0;
    /** Steepest slope that holds snow, degrees. */
    double maxSlope = 48.0;
    /** Extra snow in hollows, 0 to 1. */
    double hollows = 0.5;
    /** Depth at full cover, cm: added to the heights when addDepth. */
    double depth = 200.0;
    bool addDepth = false;
    /** Slide steps: snow on ground steeper than maxSlope runs down. */
    int slide = 8;

    SnowParams();
};

void snow(Heightfield& h, const SnowParams& p);

/** The Erode preset chain: macro form -> amplify -> debris and thermal -> rills -> deposition (-> snow on alpine). */
struct PresetParams
{
    std::string preset = "natural";
    /** 0 leaves the heightfield alone; 1 is very old. */
    double age = 0.5;
    uint32_t seed = 0;
    /** Mask of hard rock (1): slower erosion and steeper talus. */
    std::string hardnessMask;
    /** Run the steps that have a GPU version there. */
    bool gpu = false;
    /**
     * Keep the input's landforms wider than formRadius (cm; 0 is 300 m) as they are: no macro reshaping, only the
     * fine-scale stream power (along the input's own drainage), debris, thermal, rills and deposition. For real elevation.
     */
    bool keepForm = false;
    double formRadius = 0.0;
};

const std::vector<std::string>& erodePresetNames();
bool isErodePreset(const std::string& name);

/** What one preset run did: each step with its time, ms. */
struct PresetReport
{
    std::vector<std::pair<std::string, double>> steps;
    /** Steps that wanted the GPU and ran on the CPU, with why. */
    std::vector<std::string> fallbacks;
    nlohmann::json toJson() const;
};

void erodePreset(Heightfield& h, const PresetParams& p, PresetReport* report = nullptr);

/** Max-combines src, normalised to 0..1 by its own peak, into mask name. */
void accumulateMask(Heightfield& h, const std::string& name, const std::vector<float>& src);

/** FNV-1a over the heights and every mask (names and values), for determinism checks. */
uint64_t heightfieldHash(const Heightfield& h);
} // namespace terrain
