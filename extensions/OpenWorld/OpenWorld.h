#pragma once

#include <lila/LevelToolkit.h>

#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * Open World: maps for open-world games built from real places. World Remix is its first part; site kits, zones and the
 * world spec register here too. Every tool sits in Level > Open World and in the Level Toolkit's Open World section,
 * added through openworld::addTools so the section is one list however many parts add to it.
 */
namespace openworld
{
/** The Level menu's submenu. */
inline constexpr const char* kMenu = "Level/Open World";
/** The Level Toolkit section's id. */
inline constexpr const char* kSection = "openworld";

/**
 * Adds actions (registered already) to Level > Open World, in order, and their toolkit items to the Open World section
 * (re-registered with every item added so far). An empty action in menuOrder adds a separator.
 */
void addTools(lila::ext::Context& ctx, const std::vector<std::string>& menuOrder, const std::vector<lila::ui::levelToolkit::Item>& items);

/** Part registrations, called from the extension's entry point. */
void registerRemix(lila::ext::Context& ctx);
void registerRemixUi(lila::ext::Context& ctx);
/** Site kits (Sites*.cpp). */
void registerSites(lila::ext::Context& ctx);
/** Drops the listener that folds site mesh moves into their specs. */
void unregisterSites();
void registerSitesUi(lila::ext::Context& ctx);
} // namespace openworld
