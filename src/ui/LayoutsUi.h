#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QObject>

#include <functional>
#include <set>
#include <string>
#include <vector>

class QComboBox;
class QToolButton;
class QDockWidget;
class QMainWindow;
class QMenu;
class QWidget;

namespace lila::ui
{
class DockSidebars;
class ViewportLayout;

/** Registers how to build a dock that is made on first use, so a layout that opens it can create it. */
LILA_UI_API void addLazyDock(const std::string& name, std::function<QDockWidget*()> create);

/**
 * Named layouts in the main window: the Windows > Layouts menu, the switcher in the menu bar's corner, the manager
 * dialog and the window.layout.* actions. Installs the hooks behind the applyLayout and saveLayout commands.
 */
class LILA_UI_API LayoutController : public QObject
{
    Q_OBJECT

public:
    /** resetToDefault puts every dock back where the window first built it. */
    LayoutController(QMainWindow* window, DockSidebars* sidebars, ViewportLayout* viewports, std::function<void()> resetToDefault);
    ~LayoutController() override;

    QMenu* menu() const { return mMenu; }
    /** The layout tab strip (Model, UV, Sculpt... and a + menu with the rest), for the window's top bar. */
    QWidget* tabs() const { return mTabs; }

    nlohmann::json capture() const;
    void apply(const nlohmann::json& layout);

    void saveAs();
    void manage();
    /** Adds an applyLayout button to the shelf showing in the shelf dock (or the first shelf). */
    void addToShelf(const std::string& name);

private:
    void fillMenu();
    void refreshSwitcher();
    /** A layout tab's right-click menu: reset, revert, save, shelf, manage. */
    void tabMenu(const std::string& name, QPoint at);
    /** A layout.apply.<name> action per layout, so every layout is keybindable. */
    void refreshLayoutActions();
    void run(const char* command, const nlohmann::json& call);
    QDockWidget* dock(const QString& name) const;
    void ensureDocks(const nlohmann::json& docks);
    void ensureDock(const std::string& name);
    void applyPreset(const nlohmann::json& preset);
    /** Rebuilds the listed columns at the outer edge of their dock area, left to right, each stacked top to bottom. */
    void applyColumns(const nlohmann::json& columns);
    std::string currentShelf() const;

    QMainWindow* mWindow;
    DockSidebars* mSidebars;
    ViewportLayout* mViewports;
    std::function<void()> mReset;
    QMenu* mMenu = nullptr;
    QWidget* mTabs = nullptr;
    QToolButton* mMore = nullptr;
    std::vector<QToolButton*> mTabButtons;
    int mPrefListener = 0;
    int mContributedListener = 0;
    std::set<std::string> mLayoutActions;
};
} // namespace lila::ui
