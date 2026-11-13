#pragma once

// Graph-side animation operations behind Puppeteer's Stacker, Cycle Maker and walk generator. The commands in
// AnimCommands.cpp wrap these; the panels call the commands.

#include "KeyMath.h"
#include "WalkCycle.h"

#include <lila/Anim.h>
#include <lila/Graph.h>
#include <lila/Poses.h>

#include <map>
#include <string>
#include <vector>

namespace puppeteer::anim
{
using lila::scene::Graph;
using lila::scene::NodeId;
using lila::scene::PlugRef;
using Character = lila::scene::poses::Character;
using json = nlohmann::json;

/**
 * The character a node belongs to: its Puppeteer character node's skeleton, pose library and template when it has one,
 * else the skeleton it is in. Throws std::invalid_argument for a node with none.
 */
Character resolveCharacter(Graph& g, NodeId node);

/** Every curve animating the character in its active take, through every anim layer; withTracks false leaves out the control tracks' curves (the character keys). */
std::vector<NodeId> characterCurves(Graph& g, const Character& ch, bool withTracks = true);
/** Sorted, unique key times of those curves. */
std::vector<double> keyTimesOf(Graph& g, const std::vector<NodeId>& curves);
/** The latest loop key time among the curves, NaN when none loops. */
double loopTimeOf(Graph& g, const std::vector<NodeId>& curves);

/** Moves every key of the curves through the warp. Undoable. */
void warpCurves(Graph& g, const std::vector<NodeId>& curves, const TimeWarp& w);
/** Deletes the keys at t; curves left without keys are deleted. Undoable. */
void removeKeysAt(Graph& g, const std::vector<NodeId>& curves, double t);

/**
 * The animated channels of nodes at time t, as the active layer's curves hold them:
 * { node: { attr: value, translate: [x, y, z], rotate: [x, y, z] (Euler degrees in the node's order) } }.
 */
json poseAt(Graph& g, const std::vector<NodeId>& nodes, double t);

/** How a KeyWriter treats values and channels. */
struct WriteMode
{
    /**
     * false: values are layer-local, as poseAt reads them, and go straight into the active layer's curves. true: values
     * are what the channel should evaluate to, keyed through setKey, which converts them for the active layer.
     */
    bool absolute = false;
    /** A channel with no curve gets one (true) or just takes the value as its static value (false). */
    bool keyMissing = true;
};

/**
 * Keys poses into the active layer's curves, caching each curve so it is written once on commit. Nodes are matched by
 * name without namespace within scope. A "rotate" of four numbers is a quaternion (x, y, z, w).
 */
class KeyWriter
{
public:
    KeyWriter(Graph& g, const std::vector<NodeId>& scope, WriteMode mode = {});
    void write(const json& pose, double t, const std::vector<std::string>& channels = {});
    /** Writes the cached curves. Undoable. Returns the channel keys set. */
    int commit();

private:
    void scalar(const PlugRef& plug, double t, double v);
    void rotation(NodeId node, int attr, double t, glm::dvec3 euler, const json& raw);

    Graph& mGraph;
    WriteMode mMode;
    std::map<std::string, NodeId> mByName;
    std::map<NodeId, lila::scene::anim::Curve> mScalars;
    std::map<NodeId, lila::scene::anim::RotationCurve> mRotations;
    int mKeys = 0;
};

/** Makes the character's first take when it has animation but no take, so a new take doesn't take that animation. */
void adoptIntoTake(Graph& g, const Character& ch);

/** The pose a library entry holds for the character (bind, ref ...); throws std::invalid_argument when missing. */
lila::scene::poses::Pose libraryPose(const Character& ch, const std::string& name);

/** The mirrored pose: sides swapped, centre nodes reflected across the plane facing axis. */
json mirrorPose(Graph& g, const Character& ch, const json& pose, char axis);

// Breakdown / extreme marks, kept on a hidden keyMarks node per character.
inline constexpr const char* kMarksType = "keyMarks";
Marks marksOf(Graph& g, const Character& ch);
void setMarks(Graph& g, const Character& ch, const Marks& marks);

/** Semantic labels (pelvis, thigh_l ...) of the character's joints: from its rig template, else by name. */
std::map<NodeId, std::string> jointLabels(Graph& g, const Character& ch);
/** The label a joint name suggests (UE5 names, common DCC and mocap names), or empty. */
std::string labelFromName(const std::string& name);

struct WalkResult
{
    std::string take;
    int frames = 0;
    int keys = 0;
    std::vector<std::string> joints;
};

/**
 * Keys a looping walk or run into a new take for the character: one cycle from frame 0, the loop key at its end. The
 * rest pose is restPose from the character's pose library when given (else its ref or bind pose when it has one),
 * otherwise the pose it holds now. Undoable.
 */
WalkResult generateWalk(Graph& g, const Character& ch, const WalkParams& params, const std::string& takeName, const std::string& restPose);
} // namespace puppeteer::anim
