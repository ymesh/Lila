#pragma once

#include "core/Export.h"

#include "scene/AutoPose.h"
#include "scene/FullBodyIk.h"
#include "scene/Node.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Grappling: contacts between characters (a hand on a wrist, an arm round a neck, hands under a body to lift it), solved
 * together. A grapple node holds its contacts as children; each contact holds a joint of one character to a point riding
 * a joint of another, with a keyable weight that blends it in and out. Every frame the characters are solved at once:
 * each closes its share of every contact's gap (by mass, or by who leads), the auto-poser places what nobody holds, and
 * their bodies are kept from passing through each other. The result is a simulation in the sim framework, so it caches
 * per frame and bakes onto an additive layer as plain keys.
 *
 * The solve itself (Figure, Link, solve) is pure, for tests and any thread.
 */
namespace lila::scene::grapple
{
inline constexpr const char* kType = "grapple";
inline constexpr const char* kContactType = "grappleContact";
/** An aim: turns what a joint holds so an axis points at a target, the body following through the solve. */
inline constexpr const char* kAimType = "grappleAim";

/** A collision capsule riding a bone: the segment a to b in the bone's space, cm. */
struct Capsule
{
    int bone = -1;
    glm::dvec3 a{0.0}, b{0.0};
    double radius = 5.0;
};

/** One character in the solve. */
struct Figure
{
    /** Its skeleton at the animated pose: each bone's rotation is also its reference. */
    fbik::Problem problem;
    std::vector<Capsule> capsules;
    /** kg; shares each contact's gap with the other side by mass. */
    double mass = 70.0;
    /** Bones held where the animation has them, position and rotation (planted feet, pins, locked chains). */
    std::vector<int> holds;
    /** The auto-poser's points by bone; -1 where the skeleton has none (then it is not used for this figure). */
    std::array<int, autopose::kPointCount> points{};
    Figure() { points.fill(-1); }
};

/** A contact between two figures (or two parts of one). */
struct Link
{
    int source = -1, sourceBone = -1;
    /** On the source bone, in its space; usually its origin. */
    glm::dvec3 sourcePoint{0.0};
    int target = -1, targetBone = -1;
    /** On the target bone, in its space. */
    glm::dvec3 targetPoint{0.0};
    /** The source bone's world rotation relative to the target bone's at full weight; none leaves it turning freely. */
    std::optional<glm::dquat> rotation;
    /** 0 leaves both as animated, 1 closes the gap; in between closes that share of it. */
    double weight = 1.0;
    /** How much of the gap the target closes: 0 the source reaches all the way, 1 the target is brought to it. */
    double share = 0.5;
};

struct Options
{
    /** Rounds of solving every figure in turn with the others as they are. */
    int iterations = 12;
    /** cm: contact gap and penetration below which the rounds stop. */
    double tolerance = 0.05;
    bool collide = true;
    /** cm kept between bodies of different figures. */
    double margin = 0.5;
    bool autoPose = true;
};

struct Result
{
    std::vector<fbik::Solution> poses;
    /** Largest contact gap left (cm), against what each contact's weight asks for. */
    double contactError = 0.0;
    /** Deepest overlap left between capsules of different figures, cm (less the margin). */
    double penetration = 0.0;
    int iterations = 0;
};

/** The share of a contact the target closes when they share it by mass: the heavier side moves less. */
LILA_SCENE_API double shareByMass(double sourceMass, double targetMass);

/** World points of a capsule posed by its bone's world matrix. */
LILA_SCENE_API std::pair<glm::dvec3, glm::dvec3> capsuleWorld(const Capsule& c, const glm::dmat4& bone);

/** Closest points between segments p0-p1 and q0-q1. */
LILA_SCENE_API std::pair<glm::dvec3, glm::dvec3> closestPoints(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& q0, const glm::dvec3& q1);

/** Distance along a ray to a capsule's surface; none for a miss. */
LILA_SCENE_API std::optional<double> rayCapsule(const glm::dvec3& origin, const glm::dvec3& dir, const glm::dvec3& a, const glm::dvec3& b, double radius);

/** How an aim places what it turns. */
enum class AimStyle
{
    Turn,     // turns about the pivot where it is
    Shoulder, // the pivot goes to the anchor (a stock to the shoulder), then turns
    Extend,   // the pivot goes out from the anchor toward the target by reach (a pistol at arm's length)
    Guard,    // as Extend, dropped below the line to the target (a blade held in guard)
};

/** One aim's pose at a frame, world space. */
struct Aim
{
    /** The holding joint's world matrix as animated. */
    glm::dmat4 holder{1.0};
    /** The frame whose +X is aimed (a muzzle, a blade's tip); its origin lies on the aimed line. */
    glm::dmat4 axis{1.0};
    /** The point the held thing turns about (a stock, a grip). */
    glm::dvec3 pivot{0.0};
    glm::dvec3 target{0.0, 0.0, 1000.0};
    AimStyle style = AimStyle::Turn;
    /** Shoulder: where the pivot goes. Extend and Guard: where reach is measured from. */
    glm::dvec3 anchor{0.0};
    /** Extend and Guard: cm from the anchor to the pivot. */
    double reach = 50.0;
    /** Guard: cm the pivot sits below the line from the anchor to the target. */
    double drop = 0.0;
    glm::dvec3 up{0.0, 1.0, 0.0};
};

/** The holder's world matrix with the aim fully on: the held thing moved and turned rigidly with it. Pure. */
LILA_SCENE_API glm::dmat4 aimHolder(const Aim& a);
/** How far off the target the aimed axis points with the holder at holder, degrees. Pure. */
LILA_SCENE_API double aimError(const Aim& a, const glm::dmat4& holder);

/** Solves the figures together. Pure. */
LILA_SCENE_API Result solve(const std::vector<Figure>& figures, const std::vector<Link>& links, const Options& options = {});

// Scene ------------------------------------------------------------------------------------------------------------

/** How a figure's capsules were fitted, as the grapple's figures report it. */
enum class Fit
{
    Markers, // physics markers' capsules
    Skin,    // each joint's dominant region of its skinned meshes
    Bones,   // bone-sized, no skin to fit to
};
LILA_SCENE_API const char* fitName(Fit f);

/** What a grapple keeps per character: fitted capsules (by joint) and the settings the solve reads. */
struct FigureSetup
{
    NodeId root = kNoNode;
    double mass = 70.0;
    /** Keep the feet where the animation puts them. */
    bool plant = true;
    Fit fit = Fit::Bones;
    struct Body
    {
        NodeId joint = kNoNode;
        glm::dvec3 a{0.0}, b{0.0};
        double radius = 5.0;
    };
    std::vector<Body> bodies;
};

/** The top joint of the skeleton joint is in. */
LILA_SCENE_API NodeId skeletonRoot(Graph& g, NodeId joint);

/**
 * Capsules for every deform joint under root: from physics markers on the joints where there are any, else fitted to
 * each joint's dominant region of its skinned meshes, else bone-sized. Mass from their volume at a body's density. At
 * the current pose; evaluates the graph.
 */
LILA_SCENE_API FigureSetup fitFigure(Graph& g, NodeId root);

/** The grapple's figures as stored (none fitted yet: empty). */
LILA_SCENE_API std::vector<FigureSetup> figuresOf(Graph& g, NodeId grapple);
/** Stores the figures on the grapple, undoably. */
LILA_SCENE_API void setFigures(Graph& g, NodeId grapple, const std::vector<FigureSetup>& figures);

/** The grapple's contact nodes, in id order. */
LILA_SCENE_API std::vector<NodeId> contactsOf(Graph& g, NodeId grapple);
/** The grapple a contact (or the grapple itself) belongs to; kNoNode otherwise. */
LILA_SCENE_API NodeId grappleOf(Graph& g, NodeId node);
/** The grapple's aims, in id order. */
LILA_SCENE_API std::vector<NodeId> aimsOf(Graph& g, NodeId grapple);
/** A contact's source and target joints; kNoNode where the named joint is gone. */
LILA_SCENE_API std::pair<NodeId, NodeId> contactJoints(Graph& g, NodeId contact);
/** World points of a contact now: the source's point and the target point it is held to. */
LILA_SCENE_API std::pair<glm::dvec3, glm::dvec3> contactPoints(Graph& g, NodeId contact);

/** The figure's capsules posed now, world space, with their joints. For picking and drawing. */
struct PosedCapsule
{
    NodeId joint = kNoNode;
    glm::dvec3 a{0.0}, b{0.0};
    double radius = 0.0;
};
LILA_SCENE_API std::vector<PosedCapsule> posedCapsules(Graph& g, const FigureSetup& figure);

LILA_SCENE_API void registerTypes();
/**
 * A contact's frame mode: with its frame attr naming a transform, the source is held each frame so its grip (gripOffset,
 * in the source joint's space) matches that transform's world frame: riding the target joint when it has one, else in
 * the world. For hands on props whose grips move with the animation.
 */
LILA_SCENE_API bool contactFollowsFrame(Graph& g, NodeId contact);

/** grappleContact, grappleKey, grappleAim, grappleFigure, fitGrappleBodies, bakeGrapple, grappleInfo. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::grapple
