#pragma once

#include "core/Export.h"

#include "scene/FullBodyIk.h"
#include "scene/Node.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Strings: posing whose behaviour comes from the moment, not from the rig. A drag is solved by a small plan built from
 * the mode, the grabbed joint and what holds still, evaluated on a snapshot of the skeleton and thrown away. Nothing is
 * created in the scene and nothing is left behind but the joints' new rotations, so undo is the key change alone.
 *
 * Everything here is pure over an fbik::Problem (the skeleton's snapshot), so it runs per frame on any thread.
 */
namespace lila::scene::strings
{
enum class Mode
{
    /**
     * FK: the grabbed joint turns and its children follow, except what holds below it outside its own limb (a pinned
     * hand stays while the chest turns, planted feet while the hips turn), and isolated joints keep their world rotation.
     */
    Forward,
    /** The hierarchy reverses at the grabbed joint: what holds still stays, and the rest of the body swings. */
    Backward,
    /** The grabbed joint is an effector: its limb bends to put it where it is dragged. */
    Ik,
    /** The whole body moves to put the grabbed joint where it is dragged, the holds staying put. */
    FullBody,
    /** The rule web picks per joint; without a rule, moves are IK and turns are Forward. */
    Default,
    /** The grabbed joint alone: its children keep their world rotation (and on a move their place), its parent stays. */
    Free,
    /** Plain FK: everything below the grabbed joint follows, holds and isolation ignored. */
    ForwardAll,
    /** Backward without holds: the whole body swings rigidly about the anchor; nothing bends to keep other holds. */
    BackwardAll,
    /** Both sides give: a turn swings the parts below by half and the body above by half the other way about the joint,
     *  and a move carries the body half way while the limb bends for the rest. Holds outside the parts below stay. */
    Bidirectional,
    /** Bidirectional without holds or isolation: both sides turn rigidly about the joint, and a move carries the whole body. */
    All,
};

/** Every mode, in the order the Spinner arc and the menus show them. */
inline constexpr Mode kModes[] = {Mode::Default, Mode::Free,          Mode::Forward, Mode::ForwardAll, Mode::Backward,
                                  Mode::BackwardAll, Mode::Bidirectional, Mode::All,     Mode::Ik,         Mode::FullBody};

LILA_SCENE_API const char* modeName(Mode m);
LILA_SCENE_API std::optional<Mode> modeFromName(std::string_view name);
/** The "All" variant of Forward, Backward and Bidirectional, and back; other modes have none and come back unchanged. */
LILA_SCENE_API Mode allVariant(Mode m, bool all);
LILA_SCENE_API bool isAllVariant(Mode m);

/** A joint that has to end at a world matrix: an anchor where it is, or where glue or attach puts it. */
struct Hold
{
    int bone = -1;
    glm::dmat4 world{1.0};
    /** False holds only the position. */
    bool rotation = true;
};

/** What a character's rig declares about posing it (its rule web), by bone. */
struct Rules
{
    /** Limbs, each top to end: a bone in one bends with the bones above it in the same limb. */
    std::vector<std::vector<int>> chains;
    /** Default mode: the mode each listed bone poses in; bones not listed follow the move-IK, turn-Forward rule. */
    std::vector<std::pair<int, Mode>> defaults;
};

/** One interaction: the grabbed joint and what is done to it. */
struct Request
{
    Mode mode = Mode::Forward;
    int grabbed = -1;
    /** World rotation about the grabbed joint's origin. */
    std::optional<glm::dquat> turn;
    /** World offset of the grabbed joint, cm. */
    std::optional<glm::dvec3> move;
    /** What holds still, from the pose before the interaction. Forward ignores them. */
    std::vector<Hold> holds;
    /** The bone that carries the body (the hips): Backward compensates there, and limbs end below it. */
    int hips = -1;
    /** IK: bones above the grabbed one that bend; 0 runs to the next branch of the skeleton. */
    int chainLength = 0;
    /** Above 1 lets limb bones lengthen, up to this multiple, when turning alone cannot meet the holds. */
    double maxStretch = 1.0;
    /** The rig's declared limbs; without them limbs run to the next branch. */
    const Rules* rules = nullptr;
    /**
     * Bones that keep their world rotation while the interaction carries them (Isolate Rotation): a head that stays
     * level as the chest turns. Forward, Backward, Bidirectional and Default honour it; the All variants, IK and Full
     * Body don't. The grabbed bone is never isolated.
     */
    std::vector<int> isolate;
};

/** A pose of the problem's skeleton: what fbik::forward takes. */
struct Pose
{
    std::vector<glm::dquat> rotation;
    std::vector<glm::dvec3> offset;
    std::vector<double> stretch;
};

struct Result
{
    Pose pose;
    /** Largest hold error left, cm. */
    double error = 0.0;
    /** Backward: the bone that stayed still while the body swung around it; -1 otherwise. */
    int anchor = -1;
    /** False when the request cannot move the grabbed joint (above the hips in Backward or IK); the pose is unchanged. */
    bool handled = true;
    /** The mode the solve ran in: Default's pick, else the request's. */
    Mode resolved = Mode::Forward;
};

/** The problem's own pose: its rotations, no offsets, nothing stretched. */
LILA_SCENE_API Pose startPose(const fbik::Problem& p);
LILA_SCENE_API std::vector<glm::dmat4> worlds(const fbik::Problem& p, const Pose& s);

/** Whether bone sits at or below ancestor. */
LILA_SCENE_API bool isUnder(const fbik::Problem& p, int bone, int ancestor);
/**
 * The bones that bend to bring bone to a target, bone first: the bones above it in its declared chain when rules has
 * one, else bone and its parents up to, not including, the first that branches into two or more limbs (twist and
 * corrective helpers don't count), the hips, or stop. Bones the problem does not solve are left out. With length > 0,
 * that many bones up from bone whatever the rules and branches say, still stopping at the hips or stop.
 */
LILA_SCENE_API std::vector<int> limbOf(const fbik::Problem& p, int bone, int hips, int stop = -1, int length = 0, const Rules* rules = nullptr);

/** The mode Default poses r's grabbed joint in: its rule, else IK for a move of a joint with a limb or the hips, else Forward. */
LILA_SCENE_API Mode resolveDefault(const fbik::Problem& p, const Request& r);

/** Whether a mode can move the grabbed joint's origin (put it somewhere), rather than only turn it in place. */
LILA_SCENE_API bool movesOrigin(Mode m, bool grabbedIsHips);

/** Solves one interaction from the problem's pose. */
LILA_SCENE_API Result solve(const fbik::Problem& p, const Request& r);

/**
 * Brings each hold's bone to its world by bending its limb (limbOf) from pose s, leaving every other bone as it is.
 * soft gives positions the limbs lean toward without having to meet. Returns the largest hold error left, cm.
 */
LILA_SCENE_API double holdLimbs(const fbik::Problem& p, Pose& s, const std::vector<Hold>& holds, int hips, double maxStretch = 1.0,
                                const std::vector<std::pair<int, glm::dvec3>>& soft = {}, const Rules* rules = nullptr);

/** A skeleton's snapshot for solving, with each bone's joint. */
struct Skeleton
{
    fbik::Problem problem;
    std::vector<NodeId> joints;
    int hips = -1;
    Rules rules;
    LILA_SCENE_API int boneOf(NodeId joint) const;
};

/** The skeleton under root as it is now. Evaluates the graph. Throws std::invalid_argument. */
LILA_SCENE_API Skeleton snapshot(Graph& g, NodeId root, const std::vector<std::pair<NodeId, double>>& stiffness = {});
/**
 * Where node is at each time (seconds), for a motion trail: the time is stepped on the main thread, only what node depends
 * on is evaluated, and the time is put back. Empty while a channel holds an unkeyed edit, since stepping would drop it.
 */
LILA_SCENE_API std::vector<glm::dvec3> trail(Graph& g, NodeId node, const std::vector<double>& times);

/** The joint's world matrix when its skin was bound, from any skin it influences; none when it drives no skin. */
LILA_SCENE_API std::optional<glm::dmat4> bindWorld(Graph& g, NodeId joint);

/** Writes the pose's rotations, offsets and stretches into the joints, undoably. Returns the plugs changed ("node.rotate"). */
LILA_SCENE_API std::vector<std::string> apply(Graph& g, const Skeleton& k, const Pose& s);

/**
 * Line of action: where each joint of a body path goes so the path takes a stroke's shape. Arc length along the path maps
 * to arc length along the stroke, which runs from the end nearer the path's first joint. With window < 0 the stroke is
 * scaled to the path's length and the path keeps its first joint where it is; with window >= 0 the path lies on the
 * stroke unscaled, its first joint at that arc length along it (a path stroke the body travels along).
 */
LILA_SCENE_API std::vector<glm::dvec3> conformTargets(const std::vector<glm::dvec3>& path, std::vector<glm::dvec3> stroke, double window = -1.0);

/** The stroke point conformTargets puts at a fraction (0 to 1) of the path's arc length: where a bone CV or a curve control there goes. */
LILA_SCENE_API glm::dvec3 conformAt(const std::vector<glm::dvec3>& path, std::vector<glm::dvec3> stroke, double window, double fraction);

/** How far along a path (0 to 1 of its arc length) the point nearest p lies. */
LILA_SCENE_API double pathFraction(const std::vector<glm::dvec3>& path, const glm::dvec3& p);

/** A stroke's arc length. */
LILA_SCENE_API double strokeLength(const std::vector<glm::dvec3>& stroke);

/**
 * Poses the path's bones (top first) onto targets with full-body IK, each bone resisting by its stiffness, the holds kept
 * and the hips free to move when hips >= 0; with hips < 0 only the path and what hangs below it turn.
 */
LILA_SCENE_API Result conform(const fbik::Problem& p, const std::vector<int>& path, const std::vector<glm::dvec3>& targets, const std::vector<Hold>& holds,
                              int hips, double maxStretch = 1.0);
} // namespace lila::scene::strings
