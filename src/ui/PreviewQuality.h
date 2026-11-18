#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

/**
 * Viewport preview quality for texturing: displacement preview (meshes subdivided and moved by their texture set's
 * height), the path-traced viewport, and texture set snapshots with wipe compare.
 */
namespace lila::ui::previewQuality
{
/** The commands and the viewport drawer; registerUi calls it. Tests and batch use it alone. */
LILA_UI_API void registerCommands();
/** registerCommands, then the actions and menu entries. Once, after the actions registry. */
LILA_UI_API void registerUi();

/** The node attrs that turn displacement preview on for a mesh and set its detail (device pixels per edge). */
inline constexpr const char* kDisplacementAttr = "displacementPreview";
inline constexpr const char* kDetailAttr = "displacementDetail";

/** What displacement preview did for a mesh node in the last frame drawn: "displaced", or why it could not. */
LILA_UI_API std::string displacementStatus(uint64_t node);
} // namespace lila::ui::previewQuality
