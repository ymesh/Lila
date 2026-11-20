#pragma once

#include "core/Export.h"

#include <string>

namespace lila::ui
{
/**
 * Registers window.hotkeyEditor, window.markingMenuEditor, help.hotkeyCheatSheet and help.hotkeyCheatSheetClose,
 * and installs the "Assign Hotkey..." right-click on menus. Call after the QApplication exists.
 */
LILA_UI_API void registerEditorActions();

LILA_UI_API void openHotkeyEditor();
LILA_UI_API void openMarkingMenuEditor();
/** Opens the editor on a menu, with the item at ptr ("/N", "/overflow/0") selected when it exists. */
LILA_UI_API void openMarkingMenuEditor(const std::string& menu, const std::string& ptr = {});
} // namespace lila::ui
