#pragma once

#include "groom/GroomEvaluator.h"

#include <groomlab/display.h>

namespace lila::groom
{
/** What a DisplayBuilder draws of an evaluation: its strands, guides, overlays, cards and versions. `stroke`: a live
 * brush stroke's view. */
groomlab::DisplayInputs displayInputs(const GroomEvaluator::Result& view, bool stroke = false);
} // namespace lila::groom
