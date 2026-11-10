#pragma once

#include "ui/MainWindow.h"


#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Version.h"
#include "core/Workspace.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "io/SceneTemplate.h"
#include "mcp/McpServer.h"
#include "render/Renderer.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/RenderSettings.h"
#include "ui/Actions.h"
#include "ui/GameExporter.h"
#include "ui/MeshSequenceExporter.h"
#include "ui/AnimatedSend.h"
#include "ui/Hotbox.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/ImagePlaneUi.h"
#include "ui/Issues.h"
#include "ui/Layouts.h"
#include "ui/LayoutsUi.h"
#include "ui/Animation.h"
#include "ui/ApiReference.h"
#include "ui/DocsUi.h"
#include "ui/Ghosting.h"
#include "ui/GraphEditor.h"
#include "ui/SpinScrub.h"
#include "ui/WheelGuard.h"
#include "ui/Theme.h"
#include "ui/TimeSlider.h"
#include "ui/AttributeEditor.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/AnimLayersPanel.h"
#include "ui/OffsetsPanel.h"
#include "ui/RetargetUi.h"
#include "ui/RenamePanel.h"
#include "ui/UnfrozenTransforms.h"
#include "ui/DisplayAids.h"
#include "ui/LodDisplay.h"
#include "ui/DockDragCancel.h"
#include "ui/DockSidebar.h"
#include "ui/Environments.h"
#include "ui/ExtensionManagerDialog.h"
#include "ui/ChannelBox.h"
#include "ui/CursorBadges.h"
#include "ui/Outliner.h"
#include "ui/MaterialEditor.h"
#include "ui/MenuSearch.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/SceneLayout.h"
#include "ui/ModelingToolkit.h"
#include "ui/TopologyToolkit.h"
#include "ui/CurveToolkit.h"
#include "ui/LevelToolkit.h"
#include "ui/ModifierStack.h"
#include "ui/OptionBox.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/CommandPalette.h"
#include "ui/UvEditor.h"
#include "ui/PreferencesDialog.h"
#include "ui/ProgressOverlay.h"
#include "ui/ReferenceEditor.h"
#include "ui/RenderSettingsDialog.h"
#include "ui/NotesPanel.h"
#include "ui/Presenter.h"
#include "ui/SetsUi.h"
#include "ui/Shelf.h"
#include "ui/StatusOutput.h"
#include "ui/SceneDialogs.h"
#include "ui/SourceControlPanel.h"
#include "ui/BakerPanel.h"
#include "ui/TextureGraphUi.h"
#include "ui/MaterialBrowser.h"
#include "ui/TextureSetPanel.h"
#include "ui/TexPipelineUi.h"
#include "ui/BatchRunPanel.h"
#include "ui/SpacesUi.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "vcs/SourceControl.h"
#include "ui/Terminal.h"
#include "ui/Theme.h"
#include "ui/ToolBox.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/ScriptEditor.h"
#include "ui/ScriptPanel.h"
#include "ui/UndoHistory.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportRenderers.h"
#include "ui/ViewportToolbar.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QBoxLayout>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QWidgetAction>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#endif

/** Helpers shared by the MainWindow source files. Private to them. */
namespace lila::ui::mainWindowDetail
{
inline constexpr const char* kLayoutFile = "layouts/main";
inline constexpr const char* kChannelBoxDock = "channelBox";
// A saved layout older than this is dropped once, so everyone gets a changed default.
inline constexpr int kLayoutVersion = 6;
inline const QStringList kDefaultCollapsed = {};
/** Default widths of the tools column (left) and the data column (right), in logical pixels. */
inline constexpr int kToolsColumnWidth = 312;
inline constexpr int kDataColumnWidth = 392;
inline constexpr int kDefaultSidePanelWidth = 340;

/** Maya's help line: the status bar shows what's under the cursor straight away, without waiting for the tooltip. */
class HelpLine : public QObject
{
public:
    HelpLine(QLabel* label, QObject* parent) : QObject(parent), mLabel(label) {}

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::Enter:
            if (auto* w = qobject_cast<QWidget*>(obj); w && !w->toolTip().isEmpty())
                show(w->toolTip());
            break;
        case QEvent::Leave:
            if (auto* w = qobject_cast<QWidget*>(obj); w && !w->toolTip().isEmpty())
                show({});
            break;
        case QEvent::Show:
            if (auto* menu = qobject_cast<QMenu*>(obj); menu && !menu->property("lilaHelpLine").toBool())
            {
                menu->setProperty("lilaHelpLine", true);
                connect(menu, &QMenu::hovered, this, [this](QAction* a) { show(a->statusTip().isEmpty() ? a->toolTip() : a->statusTip()); });
                connect(menu, &QMenu::aboutToHide, this, [this] { show({}); });
            }
            break;
        default:
            break;
        }
        return false;
    }

private:
    void show(const QString& text)
    {
        // Tooltips may be rich text; the help line is one plain line.
        QString plain = Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
        mLabel->setText(plain.section(QChar('\n'), 0, 0));
    }

    QLabel* mLabel;
};

/** Shown on screen: for a tabbed dock, its tab is the current one. */
inline bool showingTab(QDockWidget* d)
{
    return dockShowing(d);
}

/** Closes the widget it filters on Escape (which a popup QLineEdit otherwise ignores) or on the key bound to toggleAction. */
struct EscapeCloses final : QObject
{
    EscapeCloses(QObject* parent, std::string toggleAction = {}) : QObject(parent), toggleAction(std::move(toggleAction)) {}
    std::string toggleAction;
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::ShortcutOverride)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() != Qt::Key_Escape && (toggleAction.empty() || !Hotkeys::get().isKeyFor(k, toggleAction)))
            return false;
        e->accept();
        if (e->type() == QEvent::KeyPress)
            static_cast<QWidget*>(obj)->close();
        return true;
    }
};

/**
 * Restores, raises and activates a window even while another app is in front, as when Lila is closed from the
 * taskbar: Windows refuses SetForegroundWindow to a background process unless it shares the foreground thread's input.
 */
inline void bringToFront(QWidget* w)
{
    if (w->isMinimized())
        w->showNormal();
    w->raise();
    w->activateWindow();
#ifdef _WIN32
    HWND hwnd = HWND(w->winId());
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    HWND front = GetForegroundWindow();
    DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0, self = GetCurrentThreadId();
    bool attached = frontThread && frontThread != self && AttachThreadInput(frontThread, self, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    if (attached)
        AttachThreadInput(frontThread, self, FALSE);
#endif
}

/** The crash report about a session being recovered, else the newest GUI crash the user has not seen yet. */
inline std::filesystem::path pendingCrashReport(const std::vector<io::recovery::Session>& stale, bool recovering)
{
    for (const std::filesystem::path& dir : crash::reports())
    {
        int64_t pid = crash::reportPid(dir);
        bool fromStale = std::any_of(stale.begin(), stale.end(), [&](const io::recovery::Session& s) { return s.pid == pid; });
        if (crash::acknowledged(dir) && !(fromStale && recovering))
            continue;
        if (fromStale || crash::reportField(dir, "app", "mode") == "gui")
            return dir;
    }
    return {};
}

/** Marks every crash report seen, so a run of crashes is reported once rather than once per launch. */
inline void acknowledgeCrashReports()
{
    for (const std::filesystem::path& dir : crash::reports())
        if (!crash::acknowledged(dir))
            crash::acknowledge(dir);
}

inline bool runTranslator(io::translators::Op op, const QString& file, const std::string& translator)
{
    try
    {
        io::translators::run(op, std::filesystem::path(file.toStdWString()), json::object(), CallSource::UI, translator);
        return true;
    }
    catch (const CommandCancelled& e)
    {
        log::info("{}", e.what());
        return false;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

} // namespace lila::ui::mainWindowDetail
