#pragma once

#include "core/Export.h"
#include "retarget/Retarget.h"
#include "retarget/Semantic.h"
#include "scene/Node.h"

#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Retargeting in a scene. A retargetRig node holds a skeleton's rig (connected to its top joint, so renames keep it);
 * a retargeter node holds the settings between two skeletons plus each side's retarget pose for that pairing, as UE's
 * IK Retargeter asset does. Running one samples the source over time and keys the target, into a take.
 */
namespace lila::retarget
{
using scene::Graph;
using scene::NodeId;

inline constexpr const char* kRigType = "retargetRig";
inline constexpr const char* kRetargeterType = "retargeter";

/** A scene skeleton: its top joint, its joints in Skeleton order, and the plain-data copy. */
struct SceneSkeleton
{
    NodeId top = scene::kNoNode;
    std::vector<NodeId> joints;
    Skeleton skeleton;
    int indexOf(NodeId joint) const;
};

/** The skeleton joint belongs to, from its topmost joint ancestor down. Throws std::invalid_argument for a non-joint. */
LILA_SCENE_API SceneSkeleton sceneSkeleton(Graph& g, NodeId joint);
/** The joints' R and translate as evaluated now. */
LILA_SCENE_API Pose currentPose(Graph& g, const SceneSkeleton& s);
/** World matrices of the joints at each time, sampled by evaluating the graph (the time is put back after). */
LILA_SCENE_API Frames sample(Graph& g, const SceneSkeleton& s, double start, double end, double fps);
/** The animated time range of the skeleton's joints, or nothing when none are keyed. */
LILA_SCENE_API std::optional<std::pair<double, double>> keyedRange(Graph& g, const SceneSkeleton& s);

/** The retargetRig node for a skeleton's top joint, or kNoNode. */
LILA_SCENE_API NodeId rigNodeFor(Graph& g, NodeId top);
/** The top joint a retargetRig node belongs to, or kNoNode. */
LILA_SCENE_API NodeId rigSkeletonTop(Graph& g, NodeId rigNode);
LILA_SCENE_API Rig rigOf(Graph& g, NodeId rigNode);
LILA_SCENE_API void setRig(Graph& g, NodeId rigNode, const Rig& rig);
/**
 * Characterises the skeleton as posed now into its retargetRig node (made when missing, else replaced). Undoable.
 * Returns the node.
 */
LILA_SCENE_API NodeId characterizeSkeleton(Graph& g, NodeId joint, const std::string& name = {});
/** The skeleton's rig node, characterising it first when it has none. */
LILA_SCENE_API NodeId ensureRig(Graph& g, NodeId top);

/** The retargeter from source to target (top joints), or kNoNode. */
LILA_SCENE_API NodeId retargeterFor(Graph& g, NodeId sourceTop, NodeId targetTop);
/**
 * A retargeter between two skeletons (characterising either when needed), with chains mapped automatically and the
 * target's retarget pose aligned to the source's. Undoable.
 */
LILA_SCENE_API NodeId createRetargeter(Graph& g, NodeId sourceTop, NodeId targetTop, const std::string& name = {}, bool align = true);
LILA_SCENE_API NodeId sourceTopOf(Graph& g, NodeId retargeter);
LILA_SCENE_API NodeId targetTopOf(Graph& g, NodeId retargeter);
LILA_SCENE_API Settings settingsOf(Graph& g, NodeId retargeter);
LILA_SCENE_API void setSettings(Graph& g, NodeId retargeter, const Settings& s);
/** The target's (or source's) retarget pose for this pairing: the retargeter's own, else the rig's. */
LILA_SCENE_API Rig pairedRig(Graph& g, NodeId retargeter, bool target);
/** Stores a retarget pose on the retargeter for one side. Undoable. */
LILA_SCENE_API void setPairedPose(Graph& g, NodeId retargeter, bool target, const Skeleton& s, const Pose& pose);
/** Aligns the target's retarget pose to the source's for the mapped chains. Undoable. */
LILA_SCENE_API void alignPairedPose(Graph& g, NodeId retargeter);

struct RunOptions
{
    /** Unset: the source's keyed range, else the playback range. */
    std::optional<double> start, end;
    /** 0: the scene's. */
    double fps = 0.0;
    /** Take to key into; empty keys the target's current animation. A take of that name is replaced. */
    std::string take;
    /** Keys the lock channel of the target's IK chains where feet are planted. */
    bool lockChains = true;
    /**
     * "fk": chain to chain (same kind of body). "semantic": body-independent, for bodies with other limbs (a biped clip on
     * a dog or a spider). "auto": semantic when the leg counts differ.
     */
    std::string mode = "auto";
    /** Semantic mode: the gait the target's legs keep (semantic::gaitFromName). */
    std::string gait = "auto";
    /** Semantic mode: semantic::ApplyOptions::secondary and armsAsForelegs. */
    double secondary = 0.6;
    bool armsAsForelegs = false;
};

struct LILA_SCENE_API RunReport
{
    std::string take;
    std::string mode;
    int frames = 0;
    double start = 0.0, end = 0.0, fps = 30.0;
    double scale = 1.0;
    double ikError = 0.0;
    std::vector<std::string> notes;
    /** Target chain -> frames planted. */
    std::vector<std::pair<std::string, int>> planted;
    int curvesCopied = 0;
    json toJson() const;
};

/**
 * Keys a solved clip onto a skeleton from start at fps (one key per pose), into opts.take when it names one (replacing a
 * take of that name), and keys the lock channel of its IK chains where res.planted says (opts.lockChains). Undoable.
 */
LILA_SCENE_API RunReport keyResult(Graph& g, const SceneSkeleton& target, const Rig& targetRig, const Pose& targetNow, const Result& res,
                                   double start, double fps, const RunOptions& opts);
/** Retargets the source's motion onto the target with the retargeter's settings, keying the target. Undoable. */
LILA_SCENE_API RunReport run(Graph& g, NodeId retargeter, const RunOptions& opts);
/** A skeleton's motion as a body-independent clip (characterising it first when needed), over opts' range and fps. */
LILA_SCENE_API semantic::Clip encodeSkeleton(Graph& g, NodeId source, const RunOptions& opts);
/** Solves a body-independent clip on a skeleton and keys it, starting at opts.start. Undoable. */
LILA_SCENE_API RunReport applySemantic(Graph& g, const semantic::Clip& clip, NodeId target, const RunOptions& opts);

/**
 * Keys a joint per frame: translate and/or Euler rotate (degrees, in its rotate order), each continuous. A rotation
 * curve when the joint's rotate keys as one, Euler curves otherwise. Linear tangents. Undoable.
 */
LILA_SCENE_API void keyJoint(Graph& g, NodeId joint, const std::vector<double>& times, const std::vector<glm::dvec3>* translate,
                             const std::vector<glm::dvec3>* euler);

LILA_SCENE_API void registerTypes();
/** characterize, editRetargetRig, retargeter, editRetargeter, alignRetargetPose, retarget, importBvh. Idempotent. */
LILA_SCENE_API void registerCommands();
} // namespace lila::retarget
