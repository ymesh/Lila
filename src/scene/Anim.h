#pragma once

#include "core/Export.h"

#include "scene/AnimCurve.h"
#include "scene/PlaybackClock.h"
#include "scene/Transform.h"

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

namespace lila::scene::anim
{
/** Scene playback settings. Times are seconds; the UI shows frames at fps. */
struct LILA_SCENE_API Playback
{
    double minTime = 0.0; // playback range
    double maxTime = 4.0;
    double animStart = 0.0; // outer range of the range slider
    double animEnd = 4.0;
    double fps = 30.0;
    bool loop = true;
    /** A user pref rather than scene data, so fromJson leaves it alone. */
    PlaybackSpeed speed = PlaybackSpeed::RealTime;
    /** Real-time multiplier of PlaybackSpeed::Custom; also a user pref. */
    double customSpeed = 1.0;

    json toJson() const;
    void fromJson(const json& j);
};

LILA_SCENE_API Playback& playback();
/** Notifies listeners after playback settings, the play state or auto key change. */
LILA_SCENE_API void playbackChanged();
LILA_SCENE_API int addPlaybackListener(std::function<void()> fn);
LILA_SCENE_API void removePlaybackListener(int id);

/** How a range edit eases out across the falloff, from the range's edge (1) to the falloff's far end (0). */
enum class RangeFalloff : uint8_t
{
    Smooth, // smoothstep: eases away from the range and into the untouched frames
    Linear,
    Sharp,  // drops quickly from the range, a long thin tail
    Soft,   // holds near the range, then drops to the untouched frames
    Constant, // full weight to the falloff's end, then nothing
};
LILA_SCENE_API const char* rangeFalloffName(RangeFalloff f);
LILA_SCENE_API std::optional<RangeFalloff> rangeFalloffFromName(std::string_view name);
/** The falloff's weight at x, 1 at the range's edge and 0 at the far end of the falloff. */
LILA_SCENE_API double rangeFalloffWeight(RangeFalloff f, double x);

/**
 * The time slider's selected range (Shift + drag on it, or the Curve Overlay's strip), for edits that act across frames,
 * such as Strings poses and Curve Overlay drags. A UI selection, not scene data: it is never saved. Falloff, in frames,
 * eases those edits out either side, shaped by the falloff type.
 */
struct TimeSelection
{
    bool active = false;
    double start = 0.0, end = 0.0; // seconds, start <= end
    double falloff = 0.0;           // frames before the range, and after it unless falloffAfter is set
    double falloffAfter = -1.0;     // frames after the range; below 0 the same as falloff
    RangeFalloff shape = RangeFalloff::Smooth;
    /** Range edits key every this many frames across the range and its falloff; 0 keys only the frames already keyed. */
    int every = 0;

    double before() const { return falloff; }
    double after() const { return falloffAfter < 0.0 ? falloff : falloffAfter; }
};
LILA_SCENE_API TimeSelection& timeSelection();
/** How much of an edit across the selection lands at time: 1 inside, easing to 0 over the falloff, 0 beyond. */
LILA_SCENE_API double timeSelectionWeight(const TimeSelection& s, double seconds);
/**
 * The frames (seconds) a range edit acts on: the keyed times given that fall in the range or its falloff, and with every
 * above 0 also every `every` frames from the falloff's start, the range's ends and the last eased frame each side.
 */
LILA_SCENE_API std::vector<double> timeSelectionFrames(const TimeSelection& s, const std::vector<double>& keyed = {});

LILA_SCENE_API double frameOf(double seconds);
LILA_SCENE_API double secondsOf(double frame);
/** Nearest whole frame, in seconds. */
LILA_SCENE_API double snapToFrame(double seconds);

/** Runs playback; the UI installs a timer here. Without one, play only flips the state. */
struct PlaybackDriver
{
    std::function<void(int direction)> start;
    std::function<void()> stop;
};
LILA_SCENE_API void setPlaybackDriver(PlaybackDriver driver);
LILA_SCENE_API void play(int direction);
LILA_SCENE_API void stop();
/** 0 stopped, 1 forward, -1 backward. */
LILA_SCENE_API int playDirection();

LILA_SCENE_API bool autoKey();
LILA_SCENE_API void setAutoKey(bool on);

/** Curve node driving plug, or kNoNode. */
LILA_SCENE_API NodeId curveFor(Graph& g, const PlugRef& plug);

/** Keyable channels of a node as scalar plugs: Vec3 attrs become three component plugs. */
LILA_SCENE_API std::vector<PlugRef> keyablePlugs(Graph& g, NodeId node);

struct AnimatedPlug
{
    PlugRef plug;
    NodeId curve = kNoNode;
};
/** Channels of a node driven by curves, in attribute order. */
LILA_SCENE_API std::vector<AnimatedPlug> animatedPlugs(Graph& g, NodeId node);

/** True when the curve node has a key at t. Never throws, even on malformed keys. */
LILA_SCENE_API bool hasKeyAt(const Node& curve, double t);

/** Sorted, unique key times of every curve on the nodes. */
LILA_SCENE_API std::vector<double> keyTimes(Graph& g, const std::vector<NodeId>& nodes);

/**
 * Keys plug at time, creating and connecting a curve (animCurveTL/TA/TU by the attribute) when it has none.
 * Undoable through the graph. Returns the curve. Throws std::invalid_argument when something else drives the plug.
 */
LILA_SCENE_API NodeId setKey(Graph& g, const PlugRef& plug, double time, double value, std::optional<Tangent> in = {},
              std::optional<Tangent> out = {});

/** Removes keys in [t0, t1]; deletes the curve when none are left. Returns how many went. */
LILA_SCENE_API int removeKeys(Graph& g, NodeId curve, double t0, double t1);

/** Key times closer than this, in seconds, count as the same time. */
inline constexpr double kTimeEps = 1e-6;

/** The curve type that keys the attribute: animCurveTA for rotations, animCurveTL for translations, else animCurveTU. */
LILA_SCENE_API const char* curveTypeFor(const AttrDef& a);
/** "translateX" for a component plug, else the attribute name. */
LILA_SCENE_API std::string channelName(Graph& g, const PlugRef& p);

// Rotation curves (animCurveRQ). curveFor and animatedPlugs report one for each component of the rotate it drives.

/**
 * True when keys on plug (a transform's rotate or one of its components) go to a rotation curve: one already drives
 * it, or nothing does, no anim layer takes the key and the node's rotationInterpolation is slerp or smooth. Called
 * before writing a key: a key router may make the layer the key goes to.
 */
LILA_SCENE_API bool keysAsRotation(Graph& g, const PlugRef& plug);
/**
 * Keys the whole rotate plug at time with Euler degrees (in the node's rotateOrder), creating and connecting an
 * animCurveRQ when it has none. Undoable. Returns the curve.
 */
LILA_SCENE_API NodeId setRotationKey(Graph& g, const PlugRef& rotate, double time, const glm::dvec3& euler,
                                     std::optional<Tangent> in = {}, std::optional<Tangent> out = {});
/** Key times of a scalar or rotation curve node. Never throws. */
LILA_SCENE_API std::vector<double> curveKeyTimes(const Node& curve);
/** A curve node's value at t: a rotation curve's Euler component (0..2), or a scalar curve's value. */
LILA_SCENE_API double curveValueAt(const Node& curve, int component, double t);
/**
 * Converts the rotate curves of a node to `to`, keeping key times and the pose at every key: three Euler curves
 * become one rotation curve or back, or a rotation curve changes interpolation. Also sets the node's
 * rotationInterpolation. Undoable. Returns whether anything changed.
 */
LILA_SCENE_API bool convertRotationCurves(Graph& g, NodeId node, RotationInterpolation to);

/**
 * Loop key on curves (scalar or rotation; anything else is skipped): on, the last key mirrors the first and post
 * infinity cycles; loopTime adds the loop key there first when it is past the last key. Off restores constant post
 * infinity. Undoable. Returns the latest loop key time, NaN when no curve loops. Takes build on this.
 */
LILA_SCENE_API double setCurvesLoop(Graph& g, const std::vector<NodeId>& curves, bool on, std::optional<double> loopTime = {});
/** Whether a curve node has its loop flag on. */
LILA_SCENE_API bool curveLoops(const Node& curve);
/** Times of the loop keys (the last key of each looping curve) on the nodes, sorted and unique. */
LILA_SCENE_API std::vector<double> loopKeyTimes(Graph& g, const std::vector<NodeId>& nodes);

/** convertRotationCurves, eulerFilter, setLoopKey, clearLoopKey; registered by registerCommands. */
void registerRotationCommands();

LILA_SCENE_API void registerCommands();
} // namespace lila::scene::anim
