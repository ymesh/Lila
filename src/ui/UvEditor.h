#pragma once

#include "core/Export.h"

#include <QStringList>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QBoxLayout;
class QHBoxLayout;
class QHideEvent;
class QLabel;
class QResizeEvent;
class QScrollArea;
class QSplitter;
class QShowEvent;
class QSpinBox;
class QTimer;
class QToolButton;

namespace lila::ui
{
/** Dock object name for the UV editor. */
inline constexpr const char* kUvEditorDock = "uvEditor";

class UvView;
class UvPackPanel;

/**
 * UV editor: a 2D view of the hilited meshes' uvs with selection synced to the 3D component selection, a toolbar of
 * tools, modes and display, and the UV Toolkit with the uv commands in sections. The toolkit sits beside the square
 * canvas in a wide editor and below it in a tall one, where its sections fall into two fixed columns.
 */
class LILA_UI_API UvEditor : public QWidget
{
    Q_OBJECT

public:
    explicit UvEditor(QWidget* parent = nullptr);
    ~UvEditor() override;

    UvView* view() const { return mView; }
    /** Shows the UV Toolkit with its Pack options open. */
    void showPackOptions();
    /** Shows the UV Toolkit column beside the view. */
    void showToolkit();

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    /** Runs a command from the UI (echoed), logging failures; returns its result or null. */
    nlohmann::json runCommand(const char* command, const nlohmann::json& call = nlohmann::json::object());
    void refreshSets();
    /** Layout stats under the view, below `lead` (a command's result) when given. Shown while Info is on. */
    void showLayout(const QString& lead = {});
    /** uvPack with the Pack panel's options and the editor's resolution and padding. */
    void runPack();
    /** Puts the toolkit beside or below the view, whichever leaves the canvas closer to square. */
    void placeToolkit();

    UvView* mView = nullptr;
    UvPackPanel* mPackPanel = nullptr;
    QToolButton* mPackOptions = nullptr;
    QToolButton* mPackSection = nullptr;
    QToolButton* mToolkitToggle = nullptr;
    QScrollArea* mToolkitScroll = nullptr;
    QSplitter* mSplit = nullptr;
    bool mToolkitSized = false;
    bool mToolkitBelow = false;
    int mSectionColumns = 0;
    QHBoxLayout* mSectionRow = nullptr;
    QBoxLayout* mSectionColumn[2] = {};
    std::vector<QWidget*> mSections;
    /** The extensions' sections (uvToolkit::add), rebuilt when they change; hidden while there are none. */
    QWidget* mExtensionSections = nullptr;
    int mExtensionListener = 0;
    void rebuildExtensionSections();
    QComboBox* mSet = nullptr;
    QToolButton* mAddSet = nullptr;
    QToolButton* mDeleteSet = nullptr;
    QComboBox* mDisplay = nullptr;
    QSpinBox* mResolution = nullptr;
    QDoubleSpinBox* mPadding = nullptr;
    QDoubleSpinBox* mDensity = nullptr;
    QDoubleSpinBox* mMoveU = nullptr;
    QDoubleSpinBox* mMoveV = nullptr;
    QDoubleSpinBox* mRotate = nullptr;
    QDoubleSpinBox* mScaleU = nullptr;
    QDoubleSpinBox* mScaleV = nullptr;
    QLabel* mStatus = nullptr;
    QToolButton* mInfo = nullptr;
    /** Refreshes the Info stats a moment after edits, so a drag does not re-measure every frame. */
    QTimer* mInfoRefresh = nullptr;
    int mGraphListener = 0;
    int mComponentListener = 0;
    int mLiveListener = 0;
    int mExecListener = 0;
    /** The uv set box waits for a hidden editor to show, or for the command making a burst of changes to finish. */
    bool mSetsStale = false;
};

/** Registers "window.uvEditor" and the uv.* actions (unwrap, auto, pack...) for menus, hotkeys and shelves. */
LILA_UI_API void registerUvActions();

/** UV sets of the meshes the UV editor shows, with the current set, uv0 and uv1, sorted. */
LILA_UI_API QStringList uvSetNames();

/** How "window.uvEditor" toggles the panel: the main window raises its dock, or tucks it away when showing. */
LILA_UI_API void setUvEditorOpener(std::function<void()> open);

/** Shows the UV editor with its Pack options open: the uv.pack* actions' option box. */
LILA_UI_API void showUvPackOptions();

/** Shows the UV editor with its UV Toolkit column. */
LILA_UI_API void showUvToolkit();

/** Centres the UV view under the cursor on the point there, zooming like the viewport's Frame Under Cursor. False when the cursor is over no UV view. */
LILA_UI_API bool frameUvUnderCursor();
} // namespace lila::ui
