#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "bake/TexelRaster.h"
#include "geo/Mesh.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * One-click cage painting (bakeAutoPaintCage): per low point, how far the auto cage must push out so every texel's ray
 * starts outside the high and meets it on the way in, as close to the high as that allows.
 *
 * Samples spread over each low triangle walk outward along the cage direction, counting high crossings: the last exit
 * from high material the walk started inside (an exit with no matching entry) is the distance the cage needs there;
 * crossings through separate pieces floating above pair off and don't count. Each sample's need plus a margin becomes a
 * constraint on its triangle's interpolated push, solved per point by projection (the smallest push that satisfies
 * them, never below the margin), clamped and smoothed upward. The group's frontal distance is set so painted offsets
 * sit near 1 and its rear distance from inward hits. Then the low is rasterised and traced as a bake would: samples
 * that still miss add constraints (and grow the rear distance) and the solve repeats, keeping the best result.
 */
namespace lila::bake
{
class Backend;
struct BakeInputs;
struct LowInput;

/** Rays of a raster pass: how many, how many missed (no valid hit), and how many of those met a back face. */
struct LILA_SCENE_API CageCoverage
{
    uint64_t samples = 0;
    uint64_t misses = 0;
    uint64_t backfaces = 0;

    double missRate() const { return samples ? double(misses) / double(samples) : 0.0; }
    json toJson() const;
};

/**
 * Traces one low's raster (lowIndex its place in the group, for its links) (one sample per texel at resolution x resolution) against a prepared backend's group as a
 * bake would with these cage parameters. missed, when given, receives the samples that missed.
 */
LILA_SCENE_API CageCoverage cageCoverage(const LowInput& low, uint32_t lowIndex, const CageParams& cage, size_t group, const Backend& backend, int resolution,
                                         std::vector<TexelSample>* missed = nullptr);

struct CageEstimateOptions
{
    /** Texels per side of the coverage passes. */
    int resolution = 512;
    /** Coverage passes that may add constraints after the first solve. */
    int iterations = 4;
    /** Kept between the cage and the high, as a fraction of the lows' bounds diagonal. */
    double margin = 0.004;
    /**
     * Each low's cage stops just short of the group's other lows in front of it, and of its own surface across a fold (an
     * arm over the torso), so it never pokes through a mesh lying on it.
     */
    bool clearance = true;
};

/** A low the estimate shapes: its mesh as the bake exports it, in world space by world. */
struct CageLow
{
    std::shared_ptr<const geo::Mesh> mesh;
    glm::dmat4 world{1.0};
    std::string name;
};

struct LILA_SCENE_API CageEstimate
{
    /** bakeOffset per point slot of each low, in CageLow order. */
    std::vector<std::vector<float>> offsets;
    double frontal = 0.0;
    double rear = 0.0;
    double cageOffset = 1.0;
    /** Coverage with the estimate, summed over the lows. */
    CageCoverage after;
    /** Coverage passes run after the first solve. */
    int iterations = 0;
    /** Missed samples with no high anywhere along their line, which no cage can fix. */
    uint64_t unreachable = 0;
    /** Seconds per phase. */
    json timings = json::object();

    json toJson() const;
};

/**
 * The graph-free estimate. in holds the group (index group, highs included) and backend is prepared with in and has a
 * tracer (the CPU backend does). settings supplies skew and cage offset.
 */
LILA_SCENE_API CageEstimate estimateCage(std::span<const CageLow> lows, const GroupSettings& settings, const BakeInputs& in, size_t group,
                                         const Backend& backend, const CageEstimateOptions& options = {});

/**
 * Estimates a bake group's cage from the scene and, when apply is set, writes it: bakeOffset on each low's base mesh,
 * the group's frontal and rear distances, and an editable cage moved to the result. One undo step. A low whose
 * modifiers change its points is estimated as exported and the result mapped back to its base points; after is then
 * measured through the stack. Returns {group, frontal, rear, before: coverage as the group bakes now, after,
 * iterations, unreachable, mapped: those lows, timings: seconds per phase}. Reports progress under one cancellable root
 * (tracing runs on a worker while the main thread pumps); a cancel throws progress::Cancelled. Throws std::runtime_error.
 */
LILA_SCENE_API json estimateGroupCage(scene::Graph& g, scene::NodeId group, const CageEstimateOptions& options = {}, bool apply = true);

/** Coverage of a group as it would bake now (Auto distances measured), summed over its lows. Throws std::runtime_error. */
LILA_SCENE_API CageCoverage groupCoverage(scene::Graph& g, scene::NodeId group, int resolution = 512);

/** The mesh point behind each corner of its bake soup (lowFromMesh's triangles and corner order under world). */
LILA_SCENE_API std::vector<geo::Index> soupCornerPoints(const geo::Mesh& mesh, const glm::dmat4& world);

/** bakeEstimateCage and bakeCageCoverage. */
LILA_SCENE_API void registerCageCommands();
} // namespace lila::bake
