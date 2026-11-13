#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;
}

namespace lila::scene::anim
{
struct TimeSelection;
}

/**
 * Animation ghosting: the selected characters' evaluated meshes and skeletons at other times, drawn faintly around the
 * current frame. Frames are captured by stepping the graph's time on the main thread and restoring it, cached per time,
 * and thrown away only when keys or connections change.
 */
namespace lila::scene::ghosts
{
/** Prefs (file "general"). */
inline constexpr const char* kEnabled = "/anim/ghost/enabled";
inline constexpr const char* kMode = "/anim/ghost/mode"; // "keys" or "frames"
inline constexpr const char* kBefore = "/anim/ghost/before";
inline constexpr const char* kAfter = "/anim/ghost/after";
inline constexpr const char* kStep = "/anim/ghost/step";     // frames apart, or every nth key
inline constexpr const char* kOpacity = "/anim/ghost/opacity";
inline constexpr const char* kShow = "/anim/ghost/show";     // "mesh", "skeleton" or "both"
inline constexpr const char* kBeforeColor = "/anim/ghost/beforeColor"; // sRGB "#rrggbb"
inline constexpr const char* kAfterColor = "/anim/ghost/afterColor";
inline constexpr const char* kRange = "/anim/ghost/range"; // spread across the time slider's range while one is selected
inline constexpr const char* kDefaultBeforeColor = "#d9645a";
inline constexpr const char* kDefaultAfterColor = "#5a9bd9";

struct Settings
{
    bool enabled = false;
    bool keys = true; // at keys, rather than every `step` frames
    int before = 2;
    int after = 2;
    int step = 1;
    double opacity = 0.3;
    bool mesh = true;
    bool skeleton = false;
    /** While the time slider has a range selected, ghosts spread across it (and its falloff) instead of around now. */
    bool range = true;

    LILA_SCENE_API static Settings fromPrefs();
};

/** The most ghosts a range spreads, so a long range stays light to draw and to preview. */
inline constexpr int kMaxRangeGhosts = 16;

/** {time, offset}: offset -1 is the nearest ghost before now, 1 the nearest after. */
using GhostTime = std::pair<double, int>;
/** keys must be sorted. Times within half a frame of now are skipped. */
LILA_SCENE_API std::vector<GhostTime> ghostTimes(const Settings& s, double now, const std::vector<double>& keys, double fps);
/**
 * Ghosts across a time range: its keys (or every step frames) from the falloff's start to its end, at most
 * kMaxRangeGhosts of them evenly picked, offsets counting out from now on each side. Empty when the range is inactive.
 */
LILA_SCENE_API std::vector<GhostTime> rangeGhostTimes(const Settings& s, double now, const std::vector<double>& keys, double fps,
                                                      const anim::TimeSelection& range);

struct MeshGhost
{
    NodeId node = kNoNode;
    MeshPtr mesh;
    glm::dmat4 world{1.0};
};

struct Frame
{
    double time = 0.0;
    std::vector<MeshGhost> meshes;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> bones; // parent joint to child joint, world space
};

struct Ghost
{
    std::shared_ptr<const Frame> frame;
    int offset = 0;
    /** Ghosts on this ghost's side of now, for fading the farther ones. */
    int count = 1;
    /** The range's falloff weight at the ghost's time (1 without a range): how much a range edit moves this frame. */
    double weight = 1.0;
};

class LILA_SCENE_API Cache
{
public:
    explicit Cache(Graph& g);
    ~Cache();
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    /**
     * Ghosts of the selection's characters at the graph's current time, capturing any frame not cached yet. Nothing is
     * captured while a channel holds an unkeyed edit, since stepping the time would drop it; the cached frames still show.
     */
    std::vector<Ghost> ghosts(const Settings& s);

    /** Drops every cached frame. */
    void invalidate();

    /** The times ghosts show at under s, around the graph's current time. */
    std::vector<GhostTime> shownTimes(const Settings& s);
    /** The ghosted nodes as they are evaluated now, as the frame at time t. The caller has set the time. */
    std::shared_ptr<const Frame> captureNow(double t);
    /**
     * Frames shown in place of the cached ones at their times, while a drag previews an edit across time; the cache
     * itself is left alone, so clearing the preview shows the frames as keyed.
     */
    void setPreview(std::vector<std::shared_ptr<const Frame>> frames);
    void clearPreview() { mPreview.clear(); }
    bool previewing() const { return !mPreview.empty(); }

    /** Frames captured since construction, for tests and the HUD. */
    uint64_t captures() const { return mCaptures; }
    size_t cached() const { return mFrames.size(); }
    /** The nodes ghosted: the selection's characters, transforms and joints. */
    const std::vector<NodeId>& targets() const { return mTargets; }

private:
    void refreshTargets();
    /** The times shown now: across the time slider's range while one is selected and s.range, else around now. */
    std::vector<GhostTime> timesNow(const Settings& s) const;
    std::shared_ptr<const Frame> capture(double t);
    bool anyHeld() const;

    Graph& mGraph;
    int mListener = 0;
    std::vector<NodeId> mSelection;
    bool mTargetsDirty = true;
    std::vector<NodeId> mTargets;
    std::vector<NodeId> mMeshes;
    std::vector<double> mKeys;
    bool mKeysValid = false;
    std::map<int64_t, std::shared_ptr<const Frame>> mFrames; // by time in ticks
    std::map<int64_t, std::shared_ptr<const Frame>> mPreview;
    uint64_t mCaptures = 0;
};

/** The cache for Graph::current(). */
LILA_SCENE_API Cache& cache();

/** ghosting: query or set the ghost prefs. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::ghosts
