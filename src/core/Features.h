#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

/**
 * How finished each area of Lila is, read from the roadmap shipped with the build (resources/roadmap/roadmap.json, the
 * file the website's roadmap page shows) and the map from its cards to extensions, menus and actions
 * (resources/roadmap/features.json). Menus, tooltips, the Extension Manager and extension autoloading all ask here, so
 * the app says what the roadmap says.
 */
namespace lila::features
{
enum class Status
{
    Ready,
    InDevelopment, // usable, with bugs to expect
    NotReady,      // not to be used yet: its extensions stay off until the user turns them on
    Planned,
};

struct Feature
{
    std::string id;    // the roadmap card's id, "soft-net"
    std::string title; // "Soft Net"
    Status status = Status::Ready;
    std::string docs; // the card's docs page, empty for none
    std::vector<std::string> extensions;
    std::vector<std::string> menus;   // menu paths, "Curves/Groom": every row under one belongs to the feature
    std::vector<std::string> actions; // action ids or their prefixes, "groom."
};

/** "In development", "Not ready for use"; empty for Ready. The wording of the roadmap page. */
LILA_FOUNDATION_API const char* label(Status s);

LILA_FOUNDATION_API const std::vector<Feature>& all();
LILA_FOUNDATION_API const Feature* find(std::string_view id);
LILA_FOUNDATION_API const Feature* ofExtension(std::string_view extension);
/** By the longest action id or prefix that matches, else by the extension that owns the action. */
LILA_FOUNDATION_API const Feature* ofAction(std::string_view action, std::string_view owner = {});
/** By the longest mapped menu path that is path or holds it. */
LILA_FOUNDATION_API const Feature* ofMenu(std::string_view path);

/** Ready when the extension belongs to no feature. */
LILA_FOUNDATION_API Status extensionStatus(std::string_view extension);

/** Replaces what the shipped files gave, for tests. A card with a note is NotReady, as on the roadmap page. */
LILA_FOUNDATION_API void set(const nlohmann::json& roadmap, const nlohmann::json& map);
/** Back to the shipped files. */
LILA_FOUNDATION_API void reset();

inline constexpr const char* kRoadmapUrl = "https://lila-3d.github.io/roadmap.html";
} // namespace lila::features
