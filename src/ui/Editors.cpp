#include "ui/Editors.h"

#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DockSidebar.h"
#include "ui/HotkeyCheatSheet.h"
#include "ui/HotkeyEditor.h"
#include "ui/MarkingMenuEditor.h"
#include "ui/UserMenuEditor.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QMainWindow>
#include <QPointer>

namespace lila::ui
{
namespace
{
/** One instance per editor, as a tool window over the main window. */
template <class T> void openSingle(QPointer<T>& slot)
{
    if (!slot)
    {
        slot = new T(mainWindow());
        slot->setWindowFlag(Qt::Window);
        slot->setAttribute(Qt::WA_DeleteOnClose);
    }
    slot->show();
    slot->raise();
    slot->activateWindow();
    flashLocate(slot);
}

QPointer<HotkeyEditor> gHotkeyEditor;
QPointer<MarkingMenuEditor> gMarkingMenuEditor;
} // namespace

void openHotkeyEditor()
{
    openSingle(gHotkeyEditor);
}

void openMarkingMenuEditor()
{
    openSingle(gMarkingMenuEditor);
}

void openMarkingMenuEditor(const std::string& menu, const std::string& ptr)
{
    openSingle(gMarkingMenuEditor);
    gMarkingMenuEditor->showMenu(menu, ptr);
}

void registerEditorActions()
{
    actions::add({"window.hotkeyEditor", "Hotkey Editor...", "Windows", [] {
                      if (!closeShownWindow<HotkeyEditor>())
                          openHotkeyEditor();
                  }, nullptr,
                  "Browse, search and rebind hotkeys"});
    actions::add({"window.importMayaHotkeys", "Import Maya Hotkeys...", "Windows", [] { importMayaHotkeysDialog(nullptr); },
                  nullptr, "A new keymap from a Maya hotkey set"});
    actions::add({.id = "window.markingMenuEditor", .label = "Marking Menu Editor...", .category = "Windows", .run = [] {
                      if (!closeShownWindow<MarkingMenuEditor>())
                          openMarkingMenuEditor();
                  },
                  .doc = "Edit marking menu slots, labels and targets", .keywords = {"pie menu", "radial menu"}});
    actions::add({"window.userMenuEditor", "Menu Editor...", "Windows", [] {
                      if (!closeShownWindow<UserMenuEditor>())
                          openUserMenuEditor();
                  },
                  nullptr, "Build your own menus for the menu bar or inside any menu; they tear off and show in Shift+T"});
    actions::add({"help.hotkeyCheatSheet", "Hotkey Cheat Sheet", "Help", [] { HotkeyCheatSheet::open(); }, nullptr,
                  "Show the hotkeys live in the focused panel: hold to peek, tap to keep it open and filter"});
    actions::add({"help.hotkeyCheatSheetClose", "Close Hotkey Cheat Sheet", "Help", [] { HotkeyCheatSheet::released(); },
                  nullptr, "Hide the hotkey cheat sheet"});
    installMenuHotkeyHook();
}
} // namespace lila::ui
