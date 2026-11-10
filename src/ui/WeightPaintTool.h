#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Paint skin weights on the selected skinned mesh (action "rig.paintWeights"), or the weight map of one of its deformers
 * or a painted layer of a field they read ("deform.paintWeights"), or a weight layer's mask. Shows the Paint Weights
 * palette (influences with lock and receiver, brush, Strict, mirror, flood, layers) and the current influence's weights
 * through the weight colour ramp, maps and masks in gray. LMB paints; Ctrl inverts Add, Shift relaxes; B + drag or
 * [ ] resize; Esc exits. Each stroke is one undo step.
 *
 * Removed weight goes to the receiver, else to the one other unlocked joint with weight at the vertex. With Strict on
 * (the default) a vertex where more than one joint could take it is refused and flashes; off, it is split in
 * proportion and marked automatic.
 */
LILA_UI_API void startWeightPaint();

/** The Paint Weights palette and the weight actions it offers (accept, relax, settle, layers). */
LILA_UI_API void registerWeightPaint();
} // namespace lila::ui
