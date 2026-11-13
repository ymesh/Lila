#pragma once

// Character keys and control tracks. A key on a character keys every control (the character keys, the Stacker's
// rows). A control track is an additive layer on one control on top of them: unkeyed it is an offset (one value the
// whole take), keyed it has its own timing. The key target says where posing goes: Character (the character keys,
// solved so the pose you see is the pose you get with the tracks on top) or Control (the posed control's track).

#include "AnimOps.h"

#include <lila/Extension.h>

#include <optional>

namespace puppeteer::anim
{
inline constexpr const char* kTracksType = "controlTracks";

enum class KeyTarget : int64_t
{
    Character,
    Control,
};

/** The character's controlTracks node (target, tracks on/off), kNoNode when it has none. */
NodeId tracksNode(Graph& g, const Character& ch);
KeyTarget keyTarget(Graph& g, const Character& ch);
/** Undoable. */
void setKeyTarget(Graph& g, const Character& ch, KeyTarget t);
bool tracksEnabled(Graph& g, const Character& ch);
/** Turns every track of the character off or on together; each track's own mute is kept. Undoable. */
void setTracksEnabled(Graph& g, const Character& ch, bool on);

/** A layer whose weight comes from a controlTracks node. */
bool isTrack(Graph& g, NodeId layer);
/** The joint a track is on, kNoNode when its members are gone. */
NodeId trackControl(Graph& g, NodeId track);
NodeId trackOf(Graph& g, NodeId control);
/** The control's track, made (an offset at zero, translate and rotate) when it has none. Undoable. */
NodeId ensureTrack(Graph& g, NodeId control);
/** The character's tracks, bottom of the layer stack first. */
std::vector<NodeId> tracksOf(Graph& g, const Character& ch);
/** Curves feeding the track's layer inputs; empty for an offset. */
std::vector<NodeId> trackCurves(Graph& g, NodeId track);

/** Keys and curve lookups go to one layer (kNoNode: the base animation) while it lives. */
class RouteScope
{
public:
    explicit RouteScope(NodeId layer);
    ~RouteScope();
    RouteScope(const RouteScope&) = delete;
    RouteScope& operator=(const RouteScope&) = delete;

private:
    std::optional<NodeId> mPrevious;
};

/** Edits on an offset track key it (Set Key) instead of moving its one value, while it lives. */
class ExplicitKeyScope
{
public:
    ExplicitKeyScope();
    ~ExplicitKeyScope();
    ExplicitKeyScope(const ExplicitKeyScope&) = delete;
    ExplicitKeyScope& operator=(const ExplicitKeyScope&) = delete;

private:
    bool mPrevious;
};

/** Gives every character key curve without a key at t one at its own value, so the key stays whole. Returns how many. */
int fillCharacterKey(Graph& g, const Character& ch, double t);

/** How far a merged control may stray from the layered sum on any frame: degrees, scene units. */
inline constexpr double kMergeRotateTolerance = 0.01;
inline constexpr double kMergeTranslateTolerance = 0.001;

struct MergeResult
{
    std::string control;
    /** Keys on the control after the merge, addedKeys of them between its own and the track's. */
    int keys = 0;
    int addedKeys = 0;
    /** Largest difference from the layered result over every frame the merge spans: degrees and scene units. */
    double rotateError = 0.0;
    double translateError = 0.0;
};
/**
 * Merges a track into the character keys: the control is keyed at its own keys and the track's, each at the summed
 * value, then at the worst frame of every span where the curve leaves the sum by more than the tolerance, again until
 * every frame is within it (at worst a key on every frame, which matches exactly), and the track goes. Undoable.
 */
MergeResult mergeTrack(Graph& g, NodeId track);
/** Deletes the track; the character keys underneath are untouched. Undoable. */
void deleteTrack(Graph& g, NodeId track);

/** The node type, the key router, the auto key fill and the track commands. */
void registerTracks(lila::ext::Context& ctx);
} // namespace puppeteer::anim
