#pragma once

#include "core/Export.h"

class QPainter;
class QRectF;

/**
 * Wipe-compare of texture set snapshots (tex/Snapshots.h) against the live set or another snapshot: the 3D viewports draw
 * the snapshot left of a draggable line, the 2D View the same for the channel it shows.
 */
namespace lila::ui::texCompare
{
/** texCompare, the viewport drawer and the wipe's drag. */
LILA_UI_API void registerCommands();
/** Take Snapshot and Compare with Snapshot, in the Texture menu. */
LILA_UI_API void registerActions();
/** The 2D View: the snapshot's copy of the shown channel over imageRect, left of the wipe. */
LILA_UI_API void paint2D(QPainter& p, const QRectF& imageRect);
} // namespace lila::ui::texCompare
