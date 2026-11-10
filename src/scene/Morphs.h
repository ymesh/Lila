#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/** One morph shape's sparse offsets from the morph deformer's input, object space. */
struct LILA_SCENE_API MorphDeltas final : Data
{
    std::vector<uint32_t> indices;
    std::vector<glm::dvec3> deltas;
    const char* kind() const override { return "morphDeltas"; }
    size_t bytes() const override { return indices.size() * sizeof(uint32_t) + deltas.size() * sizeof(glm::dvec3); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** A target's in-between shapes, each reached at its own weight on the way to the full shape at 1. Sorted by weight. */
struct LILA_SCENE_API MorphInBetweens final : Data
{
    struct Shape
    {
        double weight = 0.5;
        std::vector<uint32_t> indices;
        std::vector<glm::dvec3> deltas;
    };
    std::vector<Shape> shapes;
    const char* kind() const override { return "morphInBetweens"; }
    size_t bytes() const override;
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/**
 * Morph targets: a `morph` deformer under any skinCluster (so morphs apply before skinning, as in Unreal) fed by a
 * chain of `morphTarget` nodes, one keyable weight each. A target holds its full shape (sparse offsets or a live
 * mesh) and optional in-betweens, and a path between them: straight, a smooth curve through them, circular arcs, or
 * a swing about a pivot. A response curve eases the weight first. A corrective is an ordinary target whose weight a
 * poseDriver (scene/Drivers.h) sets.
 */
namespace morphs
{
enum TargetAttr
{
    kWeight,
    kMorphName,
    kDeltas,
    kTargetMesh,
    kPrevious,
    kLink,
    kInBetweens,
    kEditMesh,
    kEditWeight,
    kPointWeights,
    kSliderMin,
    kSliderMax,
    kInterpolation,
    kPivot,
    kResponse,
    kFitShapes,
    kFitTolerance,
};

/** How points travel from one shape to the next (the target's interpolation attr). */
enum class Interpolation : int64_t
{
    Linear, // straight between shapes
    Smooth, // a cubic through the shapes
    Arc,    // circular arcs through each point's shapes; needs an in-between
    Pivot,  // swings about the pivot, the distance to it blending
};

/**
 * Exports carry Linear and Smooth targets as their own shapes. Arc and Pivot targets are compiled: the full shape, plus
 * up to fitShapes more shapes (<name>_p2 ...) fit to the path, each with its weight as a function of the target's.
 */
inline bool compiles(Interpolation m) { return m == Interpolation::Arc || m == Interpolation::Pivot; }

/** The morph deformer in a mesh's stack, or kNoNode. */
LILA_SCENE_API NodeId find(Graph& g, NodeId mesh);
/** The morph a morphTarget feeds, or kNoNode. */
LILA_SCENE_API NodeId morphOf(Graph& g, NodeId target);
/** The morphTarget nodes of a morph, first added first. */
LILA_SCENE_API std::vector<NodeId> targetNodes(Graph& g, NodeId morph);
LILA_SCENE_API bool isTarget(const Node& n);

/**
 * How much of each shape applies at weight w. knots are the shapes' weights (in-betweens and the full shape's 1),
 * sorted; an implied empty shape sits at 0. Linear between neighbouring knots, or with smooth a cubic through them
 * (Catmull-Rom tangents), the end segments extended past them in a straight line.
 */
LILA_SCENE_API std::vector<double> shapeCoefficients(const std::vector<double>& knots, double w, bool smooth = false);

/** The response curve: Control Rig's easing `type` (the response attr) on 0..1, mirrored below 0 and straight past 1. */
LILA_SCENE_API double respond(int64_t type, double w);

/** What exporters write: one entry per shape, an in-between as its own target ("Smile_50") with its coefficient as weight. */
struct Target
{
    NodeId node = kNoNode;
    std::string name;
    double weight = 0.0; // coefficient times the morph's envelope
    std::vector<uint32_t> indices;
    std::vector<glm::dvec3> deltas; // the target's own point weights applied
};

/** The morph's targets in order, in-betweens expanded, live targets resolved against its current input. Evaluates the graph. */
LILA_SCENE_API std::vector<Target> targets(Graph& g, NodeId morph);
/** Just the weights of targets(), in the same order, cheap enough to sample every frame. */
LILA_SCENE_API std::vector<double> weights(Graph& g, NodeId morph);

/** One target's exported shape names, in targets() order. */
LILA_SCENE_API std::vector<std::string> shapeNames(Graph& g, NodeId target);
/** One target's exported shape weights at its weight w, envelope left out, in targets() order. */
LILA_SCENE_API std::vector<double> shapeWeights(Graph& g, NodeId target, double w);
/** Whether the target's shape weights are anything but its own weight on its one shape. */
LILA_SCENE_API bool mapped(const Node& target);

/** How well a compiled (Arc or Pivot) target's exported shapes follow its path; shapes 0 for one that isn't compiled. */
struct Fit
{
    int shapes = 0;
    double error = 0.0; // the largest distance between path and export over the slider range, cm
    bool capped = false; // fitShapes ran out before error got under fitTolerance
};
LILA_SCENE_API Fit fit(Graph& g, NodeId target);

/**
 * Where moving points travel as the weight runs over the slider's range, object space: one polyline of steps + 1
 * positions per point, at most maxPaths of them, the farthest moving first.
 */
LILA_SCENE_API std::vector<std::vector<glm::dvec3>> paths(Graph& g, NodeId target, int steps, size_t maxPaths);

/** Puts a new morph in the mesh's stack, under any skinCluster. Undoable. */
LILA_SCENE_API NodeId create(Graph& g, NodeId mesh, const std::string& name);
/** Appends a target given as offsets (importers). Returns the morphTarget node. Undoable. */
LILA_SCENE_API NodeId addTarget(Graph& g, NodeId morph, const std::string& morphName, std::shared_ptr<const MorphDeltas> deltas, double weight);
/** Adds or replaces the in-between at weight. Undoable. */
LILA_SCENE_API void setInBetween(Graph& g, NodeId target, double weight, std::shared_ptr<const MorphDeltas> deltas);
/** The target's own per-point weight map (a PointWeights, scene/Deformers.h) over all its shapes; null for none. Undoable. */
LILA_SCENE_API void setPointWeights(Graph& g, NodeId target, DataPtr weights);

/**
 * Pose-space inversion: the full-shape offsets for `target` that make its mesh's evaluated points (object space,
 * after every modifier above the morph, the skin included) match `sculpt` at the current pose and weights. Solved per
 * point with Newton steps on the stack's numeric Jacobian, so it holds for linear and dual quaternion skinning and
 * other point-wise deformers. The target's coefficient must be nonzero now. Sets nothing; residual gets the largest
 * remaining distance.
 */
LILA_SCENE_API std::shared_ptr<MorphDeltas> invertSculpt(Graph& g, NodeId target, const std::vector<glm::dvec3>& sculpt, double* residual = nullptr);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace morphs
} // namespace lila::scene
