#pragma once

#include "core/Export.h"

#include <string>

namespace lila::ui
{
/** Places a layer's projection (or, with mask, the mask's) on its texture set's mesh in the viewport. */
LILA_UI_API void startProjectionPlacement(const std::string& set, const std::string& layer, const std::string& mask = {});
} // namespace lila::ui
