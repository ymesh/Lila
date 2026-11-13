#pragma once

#include "core/Export.h"
#include "motion/Engine.h"
#include "retarget/Retarget.h"
#include "retarget/SceneRetarget.h"

#include <memory>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Generated SOMA motion on Lila characters and back. A CharacterMap pairs a character's skeleton with the SOMA skeleton
 * through retargeting (chains mapped automatically, the character's retarget pose aligned to SOMA's), so any humanoid
 * the retargeter can characterise works: UE Manny and Quinn, Mixamo, MetaHuman, BVH and the like.
 *
 * Spaces: SOMA poses are meters, Lila is centimeters, both Y up; positions scale by 100, rotations carry over.
 */
namespace lila::motion
{
/** Lila works in centimeters, the model in meters. */
inline constexpr double kCmPerMeter = 100.0;

/** The SOMA skeleton as retarget data (centimeters), its rig (chains named the retargeter's way) and its reference pose. */
struct SomaRig
{
    retarget::Skeleton skeleton;
    retarget::Rig rig;
    retarget::Pose reference;
};
LILA_SCENE_API const SomaRig& somaRig();

/** A SOMA local pose as a retarget pose of somaRig().skeleton, and back. */
LILA_SCENE_API retarget::Pose toRetargetPose(const Pose& soma);
LILA_SCENE_API Pose fromRetargetPose(const retarget::Pose& pose);

struct MapOptions
{
    /** Move the character's root joint (the one above the pelvis at the ground) under the pelvis, turned with it. */
    bool rootMotion = true;
    /** Hold feet still where the model predicts contact. */
    bool plantFeet = true;
    /** A contact at or above this counts as planted. */
    double contactThreshold = 0.5;
};

class LILA_SCENE_API CharacterMap
{
public:
    /** From plain data: the character's skeleton, its retarget rig and its pose now. Throws std::invalid_argument. */
    CharacterMap(const retarget::Skeleton& skeleton, const retarget::Rig& rig, const retarget::Pose& current);
    /** For the skeleton joint belongs to; characterises it first when it has no retarget rig (undoable). */
    static CharacterMap forJoint(scene::Graph& g, scene::NodeId joint);

    /** The scene skeleton when made by forJoint. */
    const retarget::SceneSkeleton& scene() const;
    const retarget::Skeleton& skeleton() const;
    const retarget::Rig& rig() const;
    /** Character size over SOMA size. */
    double scale() const;

    /** The character's pose for one SOMA pose. */
    retarget::Pose toCharacter(const Pose& soma) const;
    /** A generated clip on the character: one pose per frame, feet planted from the contacts (per goal chain). */
    retarget::Result toCharacter(const std::vector<Pose>& frames, const std::vector<Contacts>& contacts, const MapOptions& options = {}) const;
    /** The SOMA pose for the character's joints' world matrices (Skeleton order, centimeters). */
    Pose toSoma(const std::vector<glm::dmat4>& characterWorld) const;
    /** The SOMA pose of the character in the scene at time t (evaluates the graph there and puts the time back). */
    Pose sampleSoma(scene::Graph& g, double t) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> mImpl;
};

/** Heading (radians about +Y, 0 facing +Z) of a SOMA pose, from its hips. */
LILA_SCENE_API float headingOf(const Pose& soma);
/** The pose turned by yaw about +Y through its root and moved so its root's ground point lands at ground (x, z). */
LILA_SCENE_API Pose placed(const Pose& soma, float yaw, glm::vec2 ground);
} // namespace lila::motion
