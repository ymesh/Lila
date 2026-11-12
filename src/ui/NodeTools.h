#pragma once

#include "core/Export.h"

/**
 * Node tools in the UI: gizmo handles for gn modifiers on selected meshes, a viewport tool for groups flagged as tools,
 * and their actions under Mesh > Node Tools (gn/Tool.h has the runtime).
 */
namespace lila::ui::nodeTools
{
LILA_UI_API void registerNodeToolUi();
} // namespace lila::ui::nodeTools
