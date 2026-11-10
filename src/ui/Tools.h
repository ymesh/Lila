#pragma once

#include "core/Export.h"

#include <functional>
#include <string>

class QMouseEvent;

namespace lila::ui
{
enum class Tool
{
    Select,
    Move,
    Rotate,
    Scale,
};

/** Snap modes, held like Maya: X grid, C curve/edge, V vertex. */
enum SnapFlags : unsigned
{
    SnapNone = 0,
    SnapGrid = 1 << 0,
    SnapEdge = 1 << 1,
    SnapVertex = 1 << 2,
    SnapRotateHold = 1 << 3, // J held: flips the rotate increment snap for the drag, like Maya
    SnapSurface = 1 << 4,    // N held: moves slide along the mesh surface under the cursor
};

/** A manipulator tool's increment snap, from the general prefs /manipulator/snap/<move|rotate|scale>/{enabled,step}. */
struct IncrementSnap
{
    bool enabled = false;
    double step = 0.0;
    /** Move only (/manipulator/snap/move/absolute): land on a world grid of step instead of stepping from the start. */
    bool absolute = false;
};

/** App-wide tool state. Every viewport reads it; hotkeys and menus change it. */
namespace tools
{
LILA_UI_API Tool current();
LILA_UI_API void setCurrent(Tool t);
LILA_UI_API const char* name(Tool t);
/** Bumped when a tool is picked while already current: manipulators drop the handle MMB drags remember. */
LILA_UI_API unsigned handleResets();

/** Pivot editing: hold D, or toggle with Insert. */
LILA_UI_API bool pivotEdit();
LILA_UI_API void setPivotEdit(bool on);

/**
 * Clone drag: hold Y. A drag on a manipulator handle in object mode duplicates the objects first (in the duplicate
 * option box's mode) and moves the copies, leaving the originals where they were. A tap of Y without a clone drag runs
 * Repeat Last Tool instead.
 */
LILA_UI_API bool cloneDrag();
LILA_UI_API void setCloneDrag(bool on);
/** Called when a clone drag happens, so releasing the clone key doesn't count as a tap. */
LILA_UI_API void markCloneDragUsed();

LILA_UI_API unsigned snaps();
LILA_UI_API void setSnap(SnapFlags flag, bool on);

/** The increment snap in effect for t right now, with J's hold applied to rotate. */
LILA_UI_API IncrementSnap incrementSnap(Tool t);
/** "move", "rotate" or "scale": the pref key segment for t's increment snap. */
LILA_UI_API const char* snapKey(Tool t);

/** Called after any change; returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
/** Tells the listeners about a change made outside this module, such as the component pivot. */
LILA_UI_API void changed();

/** True while B is held: LMB drag in the viewport sets the soft-selection radius. */
LILA_UI_API bool softSelectAdjusting();
/** A radius drag happened while the soft select key was down, so releasing it doesn't toggle. */
LILA_UI_API void markSoftSelectAdjusted();
/**
 * A brush tool's press starts a radius drag: B held (or the tool's own armed key) with LMB, which drags from the current
 * radius, or MMB, which starts from the smallest. Marks the soft select key used.
 */
LILA_UI_API bool brushRadiusDrag(const QMouseEvent* e, bool armed);

LILA_UI_API void registerActions();
} // namespace tools
} // namespace lila::ui
