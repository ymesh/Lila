#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace lila::io
{
/**
 * Exports the whole scene to path with the translator for its extension. options are the export command's flags;
 * selectionOnly and unrealCheck default to False on commands that have them. Returns what the command returned and
 * throws CommandError (or the exporter's exception) on failure, including when the file wasn't written.
 */
LILA_SCENE_API nlohmann::json exportScene(const std::filesystem::path& path, nlohmann::json options = nlohmann::json::object());

/**
 * A scene setting, saved with the scene as sceneData "exportOnSave": {enabled, path, options}. While enabled, every
 * save also exports the whole scene to path (relative to the scene file) in the format its extension names, so an
 * engine can watch a plain FBX or glTF.
 */
namespace exportOnSave
{
LILA_SCENE_API nlohmann::json setting();
/** The file a save of scene writes; empty when the setting is off or has no path. */
LILA_SCENE_API std::filesystem::path target(const std::filesystem::path& scene);
/** The io::onSaved hook. Logs and carries on when the export fails, so the save itself stands. */
LILA_SCENE_API void onSceneSaved(const std::filesystem::path& scene);
/** Commands: exportOnSave. */
LILA_SCENE_API void registerCommands();
} // namespace exportOnSave
} // namespace lila::io
