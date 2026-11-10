#pragma once

#include "core/Export.h"

#include "scene/Skin.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

/**
 * Painting skin weights under the artist's control: brush samples that report where they refused, relax, settle
 * (fit the weights so linear skinning reaches a settled shape) and the commands over them, plus weight layer commands.
 */
namespace lila::scene::skinpaint
{
/** How many points each PaintOutcome got, and which points were refused. */
struct LILA_SCENE_API PaintReport
{
    std::array<size_t, 6> counts{};
    std::vector<uint32_t> refused;

    void add(uint32_t p, skin::PaintOutcome o);
    void merge(const PaintReport& other);
    size_t refusedCount() const { return refused.size(); }
    /** The refusal most points got, or Painted when there were none. */
    skin::PaintOutcome mainRefusal() const;
    json toJson() const;
};

/** Paints each (point, falloff) with strength * falloff. */
LILA_SCENE_API PaintReport paintPoints(WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, skin::PaintMode mode, bool invert, float value,
                                       float strength, const std::vector<char>& locked, const skin::PaintOptions& o, bool dryRun = false);

/** A short reason and fix for a refusal, for the HUD and command results. */
LILA_SCENE_API const char* refusalHint(skin::PaintOutcome o);

enum class SettleTarget
{
    /** Delta mush of the posed mesh: the pose with its pinches and collapses smoothed out, the rest detail kept. */
    Pose,
    /** The posed mesh relaxed toward its rest edge lengths, with points kept apart where it folds onto itself. */
    Simulate,
};

struct SettleOptions
{
    SettleTarget target = SettleTarget::Pose;
    int smoothIterations = 10;
    double smoothStep = 0.5;
    int simIterations = 40;
    bool contact = true;
    /** Pull toward the current weights; higher keeps more of what is painted. */
    double regularize = 0.05;
};

/** Linear skinning of the listed points with the table's weights and the object-space influence matrices. */
LILA_SCENE_API std::vector<glm::dvec3> skinPoints(const geo::Mesh& rest, const WeightTable& t, const std::vector<glm::dmat4>& mats,
                                                  const std::vector<uint32_t>& pts);

/** Points within `rings` edges of the seeds, the seeds first. */
LILA_SCENE_API std::vector<uint32_t> grow(const geo::Mesh& m, const std::vector<uint32_t>& seeds, int rings);

/**
 * Where the region's points should be, by point id (others left as posed). posed holds positions for the region and
 * enough rings around it: settleRings(o) of them.
 */
LILA_SCENE_API std::vector<glm::dvec3> settleTargets(const geo::Mesh& rest, const std::vector<glm::dvec3>& posed, const std::vector<uint32_t>& region,
                                                     const SettleOptions& o);
/** Rings around a region settleTargets reads. */
LILA_SCENE_API int settleRings(const SettleOptions& o);

/**
 * Fits points' weights so linear skinning reaches target positions over one or more samples: each point's own and
 * one-ring unlocked influences, non-negative, summing to what the locked ones leave, at most maxInfluences, pulled
 * toward the current weights by the regulariser.
 */
class LILA_SCENE_API WeightFit
{
public:
    WeightFit(const geo::Mesh& rest, const WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, const std::vector<char>& locked, int maxInfluences);
    /** One pose: influence matrices (object space) and target positions by point id. */
    void addSample(const std::vector<glm::dmat4>& mats, const std::vector<glm::dvec3>& targets);
    /** Blends each point toward its fit by its amount; the points become authored. */
    void solve(WeightTable& t, double regularize) const;

private:
    struct Point
    {
        uint32_t id = 0;
        float amount = 1.0f;
        std::vector<uint32_t> candidates;
        std::vector<double> current;
        std::vector<std::pair<uint32_t, float>> lockedWeights;
        double room = 1.0;
        std::vector<double> a, b; // normal equations, candidates^2 and candidates
    };
    const geo::Mesh& mRest;
    std::vector<Point> mPoints;
    int mMaxInf;
};

/** Settles the points in the current pose, blending each by its amount. */
LILA_SCENE_API void settlePoints(Graph& g, NodeId skin, WeightTable& t, const std::vector<std::pair<uint32_t, float>>& pts, const std::vector<char>& locked,
                                 int maxInfluences, const SettleOptions& o);

/** Sets a layer's mask; points it uncovers start from the layers below. Undoable. */
LILA_SCENE_API void setLayerMask(Graph& g, NodeId skin, int layer, const std::vector<float>& mask);

/** Pref key of the weight paint Strict toggle (bool, on by default). */
inline constexpr const char* kStrictPref = "/rig/weightPaintStrict";
LILA_SCENE_API bool strictDefault();

/** floodSkinWeights, acceptSkinWeights, selectAutomaticWeights, relaxSkinWeights, settleSkinWeights, the layer commands, the skinWeights validator. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::skinpaint
