#pragma once

#include "core/Export.h"

#include <QMainWindow>
#include <QPointer>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <vector>

#include "io/Examples.h"
#include "io/Samples.h"
#include "io/Recovery.h"

class QDialog;
class QDockWidget;
class QMenu;
class QLabel;
class QTimer;
class QToolButton;

namespace lila::ui
{
namespace menus
{
class MenuBuilder;
}
namespace panels
{
struct Panel;
}
class DockSidebars;
class LayoutController;
class ViewportLayout;

class LILA_UI_API MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    /**
     * Run once the window is up: offers crash recovery for stale (crashed) sessions, then the startup dialog when
     * showStartup and recovery did not open anything.
     */
    void startup(const std::vector<io::recovery::Session>& stale, bool showStartup);
    /** File > Open on a given file (.lila, .lilaa, .ma, .mb), as the command line opens one. */
    void openFile(const std::filesystem::path& path) { openScene(path); }

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void moveEvent(QMoveEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void registerActions();
    QMenu* refreshKeysOnShow(QMenu* menu);
    /** Lila's own panels, in the order their docks are made (a panel's place() may refer to earlier ones). */
    void registerPanels();
    /** The default arrangement: toolkits tabbed on the left, the outliner over the inspector tabs on the right. */
    void arrangeDefaultDocks();
    void createPanelDock(const panels::Panel& p);
    void removePanelDock(const std::string& id);
    void buildMenus();
    /** Shows the menu bar menus of the active menu set and hides the rest. */
    void applyMenuSet();
    void raiseToolsFor(const std::string& set);
    void buildPanelMenus();
    void buildStatusBar();
    QDockWidget* addDock(const QString& title, const QString& objectName, QWidget* content, Qt::DockWidgetArea area);
    void run(const char* command, const nlohmann::json& call = nlohmann::json::object());
    void restoreLayout();
    void applyDefaultDockSizes();
    void resetLayout();
    /**
     * QMainWindow::restoreState with the progress pump held off: a dock shown while Qt applies the state can start work
     * that pumps events, and those events can delete objects the dock layout is still using.
     */
    bool restoreDocks(const QByteArray& state);
    /** Brings a window that a layout grew past its screen back onto it. */
    void fitToScreen();
    /** The menu bar and shelf dock as the /ui/showMenuBar and /ui/showShelf prefs say. */
    void applyChromePrefs();
    /** Raises the channel box in its tab group, or the tab that was up before it when it already is (Maya's Ctrl+A). */
    void toggleChannelBox();
    void toggleUvEditor();
    void renameSelection();
    /** Create > Camera from View: a camera node posed and lensed like the active view's camera, selected. */
    void createCameraFromView();
    void scheduleLayoutSave();
    void saveLayout();
    void refreshRecentFolders();
    void refreshWorkspaceLabel();
    /** Puts the workspace name at the right of the menu bar, beside the layout switcher. */
    void openFolder();
    void showAbout();
    bool confirmDiscard();
    /** target: save there without asking (Increment and Save). */
    bool saveScene(bool saveAs, const std::filesystem::path& target = {});
    void incrementAndSave();
    void showVersions();
    bool openScene(const std::filesystem::path& path);
    void reloadScene();
    void openRecoveredScene(const std::filesystem::path& file);
    void openExample(const io::examples::Example& example);
    void openSample(const io::samples::Sample& sample);
    /** welcome: examples and recent scenes; otherwise recent scenes alone. */
    void showStartupDialog(bool welcome);
    /** Every panel's show toggle, grouped by what it is for. */
    void fillEditorsMenu(QMenu* menu);
    void addRecentFile(const std::filesystem::path& path);
    void refreshTitle();
    void refreshSourceStatus();
    void importFile();
    void convertMayaFolder();
    void exportSelection();
    void exportWebViewer();
    void sendToUnreal();

    ViewportLayout* mViewports = nullptr;
    QPointer<QMenu> mRecentMenu;
    QPointer<QDialog> mStartupDialog;
    menus::MenuBuilder* mMenus = nullptr;
    int mPanelListener = 0;
    QTimer* mAutosaveTimer = nullptr;
    QToolButton* mWorkspaceLabel = nullptr;
    class TopBar* mTopBar = nullptr;
    QLabel* mSourceLabel = nullptr;
    QToolButton* mMcpButton = nullptr;
    QTimer* mLayoutTimer = nullptr;
    bool mRestoring = false;
    QByteArray mDefaultState;
    DockSidebars* mSidebars = nullptr;
    LayoutController* mLayouts = nullptr;
    QPointer<QDockWidget> mTabBeforeChannelBox;
    /** Docks Maximize Viewport hid, with whether each was the showing tab, for its second press to bring back. */
    std::vector<std::pair<QPointer<QDockWidget>, bool>> mMaximizeHidden;
    bool mShelfShown = true;
};
} // namespace lila::ui
