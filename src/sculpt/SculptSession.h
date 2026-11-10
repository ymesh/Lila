#pragma once

#include "core/Export.h"

#include "geo/Ops.h"
#include "scene/Node.h"
#include "scene/Plug.h"
#include "sculpt/Sculpt.h"
#include "sculpt/SubdLevels.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

/**
 * Sculpt mode on a scene mesh: strokes edit the mesh itself, at any of its subdivision levels. Level 0 is the mesh's cage;
 * each level above is a Catmull-Clark subdivision of the one below carrying its own detail. Exiting leaves the mesh at the
 * level being sculpted and stores every level on the mesh (hidden attrs, saved with the scene), so the next Enter Sculpt
 * picks them all up again. Edits to the mesh outside sculpt mode that keep its topology ride along into the levels.
 *
 * Commands: sculptEnter, sculptExit, sculptCancel, sculptLevel, sculptAddLevel, sculptDeleteHigher, sculptDeleteLower,
 * sculptReconstruct, sculptRemesh, sculptDuplicate, sculptDeleteLevels, sculptStroke, sculptMask, sculptColor,
 * sculptState, transferShape. Entering, exiting, each level change and each stroke are single undo steps.
 */
namespace lila::sculpt
{
/**
 * Hidden attrs on a mesh with stored levels: its cage, its finest level (with the mask, freeze and polypaint layers),
 * the level the mesh is at, that level's point and face counts (which tell when an edit changed its topology), and a
 * stamp of its positions at exit (which tells when points were moved since).
 */
inline constexpr const char* kCageAttr = "sculptCage";
inline constexpr const char* kFinestAttr = "sculptFinest";
inline constexpr const char* kLevelAttr = "sculptLevel";
inline constexpr const char* kPointsAttr = "sculptPoints";
inline constexpr const char* kFacesAttr = "sculptFaces";
inline constexpr const char* kStampAttr = "sculptStamp";
/** Point layers on a stored finest level holding the mask and freeze state. */
inline constexpr const char* kMaskLayer = "sculptMask";
inline constexpr const char* kFreezeLayer = "sculptFreeze";
/** The color set polypaint lives in, on the mesh after Exit Sculpt. */
inline constexpr const char* kColorSet = "polypaint";
inline constexpr int kMaxLevels = 6;

struct Session
{
    scene::NodeId node = scene::kNoNode;
    /** The mesh's base point capacity and face count at Enter, to tell whether exiting can keep it and move its points. */
    geo::Index basePoints = 0;
    geo::Index baseFaces = 0;
    /** The level the mesh was at on Enter: while the stack still holds it, Exit moves the mesh's points instead of
     *  replacing the mesh, keeping whatever was edited on it outside sculpt mode. */
    std::shared_ptr<const SubdLevel> base;
    /** The subdivision levels, shared by every session of this sculpt. */
    std::shared_ptr<const SubdStack> stack;
    /** The level being sculpted, 0 (the cage) to stack->top(). */
    int level = 0;
    /** What each level above `level` adds, [k] for level k; null adds nothing. */
    std::vector<DetailPtr> detail;
    SculptMesh sculpt;
    /** The symmetry the sculpt mesh was last set up with: its layer name, empty when off. */
    std::string symmetryKey;

    /** A stroke the viewport tool already applied live, adopted by the sculptStroke call carrying this token. */
    uint64_t liveToken = 0;
    StrokeRecord live;
};

/** The levels a mesh carries from an earlier Exit Sculpt. */
struct StoredLevels
{
    /** The level the mesh is at. */
    int level = 0;
    /** The finest level. */
    int levels = 0;
    /** Whether the mesh's topology still fits that level; false after an edit like an extrude. */
    bool fits = true;
};

/** The mesh's stored levels, or nothing when it has none. */
LILA_SCENE_API std::optional<StoredLevels> storedLevels(const scene::Node& mesh);

/**
 * Called after a top-level command changed the topology of meshes with stored levels, so they no longer fit. The UI
 * asks whether to keep the edit (sculptDeleteLevels) or undo it. Without a handler the levels stay until Enter Sculpt
 * drops them.
 */
LILA_SCENE_API void setLevelsBrokenHandler(std::function<void(const std::vector<scene::NodeId>&, std::string_view command)> fn);

/** The open session, or null. */
LILA_SCENE_API std::shared_ptr<Session> active();

/** Called on the main thread whenever the session opens, closes, or its proxy changes through a command or undo. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);
LILA_SCENE_API void notify();

/** Follows the mesh's symmetry setting: call before each stroke. */
LILA_SCENE_API void syncSymmetry(Session& s);

LILA_SCENE_API uint64_t nextLiveToken();

/** Gets the tumble point a stroke's undo or redo goes back to (StrokeRecord::pivotBefore and pivotAfter). */
LILA_SCENE_API void setPivotHandler(std::function<void(const std::optional<glm::dvec3>&)> handler);

LILA_SCENE_API void registerCommands();
} // namespace lila::sculpt
