#pragma once

#include "core/Export.h"
#include "motion/Character.h"
#include "motion/Generate.h"
#include "scene/Node.h"

#include <string>
#include <variant>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Generated motion in a scene: spots as nodes (motionSpot, a locator whose transform places the spot), journeys built
 * from path points, curves and spots, and generated clips keyed onto characters as takes.
 */
namespace lila::motion
{
using scene::Graph;
using scene::NodeId;

inline constexpr const char* kSpotType = "motionSpot";

/** The spot a motionSpot node holds; throws std::invalid_argument for another node. */
LILA_SCENE_API Spot spotOf(Graph& g, NodeId node);
/** Stores a spot on a node. Undoable. */
LILA_SCENE_API void setSpot(Graph& g, NodeId node, const Spot& spot);
/** A new motionSpot node holding spot, at a world position (cm) facing yaw degrees. Undoable. */
LILA_SCENE_API NodeId createSpot(Graph& g, const Spot& spot, const glm::dvec3& position, double yaw, const std::string& name = {});
/** Where a node stands for motion: its world origin on the ground in SOMA space (meters) and its heading about +Y. */
LILA_SCENE_API std::pair<glm::vec2, float> groundFrame(Graph& g, NodeId node);
/** A captured target: the character's poses from time t at 30 Hz, relative to the spot node. */
LILA_SCENE_API SpotTarget captureTarget(Graph& g, const CharacterMap& map, NodeId spot, double t, int poses);

/** A waypoint: a world point (cm), a curve node to walk along, or a spot node to perform at. */
using Waypoint = std::variant<glm::dvec3, NodeId>;
/** Splits waypoints into legs: points and curves make paths, each spot ends a leg (walking to its first pose). */
LILA_SCENE_API std::vector<Leg> legsFrom(Graph& g, const std::vector<Waypoint>& waypoints, glm::vec2 start);

struct KeyOptions
{
    /** New take to key into (replacing one of that name); empty keys the current animation. */
    std::string take;
    double start = 0.0;
    MapOptions map;
    /** Key the lock channel of the character's IK chains where its feet are planted. */
    bool lockFeet = true;
};

/** Keys a generated clip onto the character. Undoable. */
LILA_SCENE_API retarget::RunReport keyClip(Graph& g, const CharacterMap& map, const Clip& clip, const KeyOptions& options);

LILA_SCENE_API void registerTypes();
/** motionStatus, motionDevice, motionFollowPath, motionSpot, motionSpotTarget, motionSpotSave, motionGaits, motionBuildGaits. */
LILA_SCENE_API void registerCommands();
} // namespace lila::motion
