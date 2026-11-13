#pragma once

#include "core/Export.h"

#include "scene/IkSolver.h"
#include "scene/Node.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Full-body IK: solves a whole skeleton at once so every pinned joint reaches its target, spreading the motion over
 * the joints between them by stiffness and keeping everything else as close to a reference pose as it can.
 *
 * The reference is what decides where the unpinned joints go. Posing by hand passes the current pose, so only what
 * the pins need moves; an auto-poser passes the pose it predicts and the solve snaps that onto the pins.
 */
namespace lila::scene::fbik
{
/** One joint of the solve. Bones are ordered parents first. */
struct Bone
{
    /** Index of the parent bone in Problem::bones; -1 for a root of the solve. */
    int parent = -1;
    /** World matrix of a root's parent outside the solve. Ignored for bones with a parent. */
    glm::dmat4 parentWorld{1.0};
    /** local = before * R * after (Transform::splitLocal); R is what the solve turns. */
    glm::dmat4 before{1.0}, after{1.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
    /** The R the solve pulls toward; usually the starting rotation. */
    glm::dquat reference{1.0, 0.0, 0.0, 0.0};
    /** False holds R as it is (twist joints, joints something else drives). */
    bool solve = true;
    /** 0 turns freely, toward 1 the joint resists turning and its neighbours take the motion. */
    double stiffness = 0.0;
    /** Multiplies Problem::pull for this joint. */
    double pull = 1.0;
    /** Roots only: the solve may also move the bone, by an offset in its parent's space. */
    bool translate = false;
    glm::dvec3 offset{0.0};
    /** Held on R (its frame and post are unused: R is already the rotate). */
    ik::JointLimit limit;
    /**
     * How far the bone may lengthen, as a multiple of its offset from the parent (the translation in before); 1 never
     * stretches. Bones only stretch when turning alone leaves a pin out of reach.
     */
    double maxStretch = 1.0;
};

/** A pin: where a bone's origin (and optionally its world rotation) has to be. */
struct Effector
{
    int bone = -1;
    /** The point on the bone, in its space, that has to reach position: its origin by default. */
    glm::dvec3 point{0.0};
    std::optional<glm::dvec3> position;
    std::optional<glm::dquat> rotation;
    double weight = 1.0;
    /** A suggestion rather than a pin: it shapes the pose with the reference pull, and the pins are then met exactly. */
    bool soft = false;
};

struct Problem
{
    std::vector<Bone> bones;
    std::vector<Effector> effectors;
    /** cm per radian when weighing a position error against a rotation error; about a limb's length. */
    double lengthScale = 50.0;
    /**
     * How strongly joints keep their reference while the pins are being reached. Each joint's share also grows with how
     * much of the skeleton it carries, so the spine holds its pose more than a wrist does.
     */
    double pull = 0.1;
    /** Iterations with the reference pull, then iterations that only close the remaining pin error. */
    int iterations = 30;
    int finishIterations = 30;
    /** Pin error, cm, below which the solve stops. */
    double tolerance = 1e-4;
};

struct Solution
{
    std::vector<glm::dquat> rotation;
    /** The translating roots' offsets; zero elsewhere. */
    std::vector<glm::dvec3> offset;
    /** What each bone's offset from its parent is multiplied by; 1 where nothing stretched. */
    std::vector<double> stretch;
    /** The largest pin position error left, cm. */
    double error = 0.0;
    int iterations = 0;
};

/** World matrices of the bones for the rotations, offsets and stretches given (empty stretch: none). */
LILA_SCENE_API std::vector<glm::dmat4> forward(const Problem& p, const std::vector<glm::dquat>& rotation, const std::vector<glm::dvec3>& offset,
                                               const std::vector<double>& stretch = {});

/** Solves p (damped least squares over every bone at once). Pure: safe on any thread. */
LILA_SCENE_API Solution solve(const Problem& p);

// Scene skeletons -------------------------------------------------------------------------------------------------

struct Pin
{
    NodeId joint = kNoNode;
    std::optional<glm::dvec3> position;
    std::optional<glm::dquat> rotation;
    double weight = 1.0;
};

struct PoseOptions
{
    std::vector<Pin> pins;
    /** Whether the top joint may move; it does when only rotations cannot reach the pins. */
    bool moveRoot = true;
    /** Overrides Bone::stiffness per joint; unset joints use 0. */
    std::vector<std::pair<NodeId, double>> stiffness;
    /**
     * Lets bones lengthen to reach pins turning cannot: the joints after the start of an IK handle with stretch on, up to
     * its maxStretch, and the joints in maxStretch. Stretched joints get their translate multiplied.
     */
    bool stretch = false;
    /** Overrides Bone::maxStretch per joint when stretch is on. */
    std::vector<std::pair<NodeId, double>> maxStretch;
    /** Keys the joints' rotate (and the root's translate) after posing. */
    bool key = false;
    /**
     * Lets the auto-poser decide where the unpinned body goes, for skeletons with UE5 Manny's joint names. Without the
     * shipped model or those joints, posing works as without it.
     */
    bool autoPose = false;
};

/**
 * The skeleton under root as a Problem: every joint below it, solved unless it is not a deform joint or something
 * other than an IK handle drives its rotation (twist joints). Starting rotations are the joints' current ones, which
 * are also the reference. index receives each bone's joint.
 */
LILA_SCENE_API Problem problemFor(Graph& g, NodeId root, const PoseOptions& opts, std::vector<NodeId>& index);

/**
 * Poses the skeleton under root so each pin reaches its target, writing the joints' rotate (and the root's translate,
 * and stretched joints' translate) undoably. Returns the largest pin error left, cm. Throws std::invalid_argument.
 */
LILA_SCENE_API double pose(Graph& g, NodeId root, const PoseOptions& opts);

/** fullBodyIk. Idempotent. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::fbik
