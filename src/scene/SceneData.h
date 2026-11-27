#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace lila::scene::data
{
using json = nlohmann::json;

/**
 * Arbitrary JSON values stored in the scene file under string keys, for tools and scripts that need to keep data with
 * a scene. Keys are free-form; prefix them with the tool's name ("myTool.settings") to stay clear of others.
 */
LILA_SCENE_API const json& all();
/** The value under key, or fallback when there is none. */
LILA_SCENE_API json get(const std::string& key, const json& fallback = nullptr);
/** Undoable. A null value removes the key. */
LILA_SCENE_API void set(const std::string& key, const json& value);
/** Drops everything without undo, as a new or opened scene does. */
LILA_SCENE_API void clear();

/** Saved with the scene as "data": { key: value }. */
LILA_SCENE_API json toJson();
/** Replaces everything without undo. */
LILA_SCENE_API void fromJson(const json& j);
/**
 * Called after fromJson, while the scene is still loading: for state derived from the data that anything evaluating
 * the rest of the load must already see. A callback an extension registers is removed when the extension unloads.
 */
LILA_SCENE_API void onLoaded(std::function<void()> fn);

/** Commands: sceneData. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::data
