#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::scene::ik
{
/**
 * How far one joint may turn, in the space of its rotate: the joint's local rotation is frame * rotate * post
 * (jointOrient and rotateAxis for a joint), and the limits are on rotate.
 */
struct JointLimit
{
    enum class Kind
    {
        None,
        /** Turns about one axis only, within [minAngle, maxAngle]. */
        Hinge,
        /** The bone swings at most cone radians away from where it points at rotate zero; twist about it is free. */
        Cone,
    };
    Kind kind = Kind::None;
    glm::dvec3 axis{0.0, 0.0, 1.0};
    double minAngle = -3.141592653589793, maxAngle = 3.141592653589793;
    /** The bone's direction (toward its child) at rotate zero. */
    glm::dvec3 bone{1.0, 0.0, 0.0};
    double cone = 3.141592653589793;
    /** 0 turns freely; toward 1 the joint keeps its starting rotation and the rest of the chain takes the motion. */
    double stiffness = 0.0;
    /** The rotate the solve starts from (Maya's preferred angle), which picks the bend of a straight chain. */
    std::optional<glm::dquat> preferred;
    glm::dquat frame{1.0, 0.0, 0.0, 0.0}, post{1.0, 0.0, 0.0, 0.0};

    bool limits() const { return kind != Kind::None || stiffness > 0.0; }
};

/** rotate moved to the nearest rotation the limit allows. */
LILA_SCENE_API glm::dquat clampRotate(const JointLimit& limit, const glm::dquat& rotate);

/** One solve of a joint chain, everything in world space. */
struct Problem
{
    /** The chain's joints from its start to its end effector, as they are before solving. At least two. */
    std::vector<glm::dmat4> world;
    /** The world matrix of the start joint's parent (identity for a root). */
    glm::dmat4 parentWorld{1.0};
    glm::dvec3 target{0.0};
    /** Where the chain's bend points; unset leaves the solver to keep the current bend plane. */
    std::optional<glm::dvec3> pole;
    /** Roll of the bend plane around the start-to-target axis, radians. A spline solve twists the end this much past the start. */
    double twist = 0.0;
    /** Spline solves: the curve the chain lies along, world space, densely sampled from its start. */
    std::vector<glm::dvec3> curve;
    /** Spline solves: turn of the whole chain about its bones, radians. */
    double roll = 0.0;
    /** Spline solves: spread the joints over the whole curve instead of keeping the bone lengths. */
    bool fitCurve = false;
    int maxIterations = 20;
    double tolerance = 1e-4; // cm
    /** Empty, or one per joint but the end effector. */
    std::vector<JointLimit> limits;
    /** Soft IK: over the last this many cm of reach the chain eases out instead of snapping straight. */
    double softness = 0.0;
    /** How far the chain may stretch to reach, as a multiple of its length; 1 never stretches. */
    double maxStretch = 1.0;
};

/**
 * The solved local rotation of every joint in Problem::world except the end effector, each relative to its parent's
 * solved world frame (the start joint's to parentWorld). Joints keep their translations and scales.
 */
using Solution = std::vector<glm::dquat>;

/** A curve as world points, the form an ikHandle's inCurve takes. Kind "curvePoints"; only ever connected, never stored. */
struct LILA_SCENE_API CurvePoints final : Data
{
    std::vector<glm::dvec3> points;
    const char* kind() const override { return "curvePoints"; }
    nlohmann::json toJson() const override { return {{"kind", kind()}}; }
    size_t bytes() const override { return points.size() * sizeof(glm::dvec3); }
};

/** An IK solver (Maya's MPxIkSolver): ikRPsolver, ikSCsolver, a spline solver, or an extension's own. */
struct Solver
{
    std::string name; // "ikRPsolver"
    std::string doc;
    /** Chain length it accepts; maxJoints 0 means any. */
    int minJoints = 2;
    int maxJoints = 0;
    /** Called on worker threads during evaluation, so it must only read the problem. */
    std::function<Solution(const Problem&)> solve;
};

LILA_SCENE_API Registry<Solver>& solvers();
/** Throws std::invalid_argument when the name is taken or solve is unset. */
LILA_SCENE_API const Solver& registerSolver(Solver solver);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the name is taken. */
LILA_SCENE_API const Solver* registerSolver(ext::Context& ctx, Solver solver);
LILA_SCENE_API const Solver* findSolver(std::string_view name);

struct Result
{
    Solution rotation;
    /** What every bone's length was multiplied by (the translate of each joint after the start). */
    double stretch = 1.0;
};

/**
 * Runs solver on p with the parts every solver shares: starts from the preferred angles, applies soft IK and stretch,
 * and holds the solution to the limits (the built-in solvers already solve within them).
 */
LILA_SCENE_API Result solve(const Solver& solver, const Problem& p);
} // namespace lila::scene::ik
