#pragma once

#include "core/Export.h"
#include "core/Features.h"

#include <QStringList>

#include <string>

namespace lila::ui
{
class OverlayPainter;
class ViewportWidget;

/**
 * What the UI shows of core/Features.h: the status line in tooltips, the tags in menus, the Roadmap action, and the
 * viewport banner for features that are not ready for use.
 */
namespace featureStatus
{
/** The feature an action belongs to: by its id or owning extension, else by a mapped menu that lists it. */
LILA_UI_API const features::Feature* ofAction(const std::string& actionId);

/** True for the statuses the UI marks: in development and not ready for use. */
LILA_UI_API bool marked(features::Status s);

/** "In development" or "Not ready for use" for an action of a marked feature; empty otherwise. */
LILA_UI_API std::string label(const std::string& actionId);

/** The status an action row shows in the menu at path: its own feature's, else the menu's. */
LILA_UI_API features::Status rowStatus(const std::string& actionId, const std::string& path);

/** Opens the roadmap page in the browser. */
LILA_UI_API void openRoadmap();

/** Titles of the not-ready features used this session whose extensions are still loaded: what the banner names. */
LILA_UI_API QStringList inUse();

/**
 * The banner at the top of every view while inUse() has anything, under the Drone Camera's when that view flies. width
 * is what is being drawn into: the view's own, or a capture's.
 */
void draw(OverlayPainter& p, const ViewportWidget* view, double width);

/** help.roadmap, and the watch on action runs that feeds inUse(). */
LILA_UI_API void registerActions();
} // namespace featureStatus
} // namespace lila::ui
