#pragma once

#include "core/Export.h"

/**
 * The scene's "layout" UI section: the mode, layout and sculpt mesh it was saved in. Opening the scene returns to them,
 * so each scene reopens where its work left off. The "general" pref kRestorePref turns it off.
 */
namespace lila::ui::sceneLayout
{
inline constexpr const char* kRestorePref = "/ui/sceneLayout";

/** Once, after the window's layouts and modes are set up. */
LILA_UI_API void install();
} // namespace lila::ui::sceneLayout
