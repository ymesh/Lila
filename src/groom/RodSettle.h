#pragma once

#include "groom/Settle.h"

namespace lila::groom
{
/**
 * Settle on GroomLab's rod solver (GuideSolver: inextensible rods, bend, friction, scalp collision at the locks' typical
 * thickness, guide repulsion), then a short pass of settle() for the contacts at each lock's own thickness. Same options
 * and stats as settle(); without a scalp it is settle().
 */
LILA_SCENE_API void settleOnRods(Splines& curves, const std::vector<std::vector<double>>& radii, const Surface* scalp, const SettleOptions& o,
                                 SettleStats* stats = nullptr);
} // namespace lila::groom
