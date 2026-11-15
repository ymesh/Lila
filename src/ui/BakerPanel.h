#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QImage>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lila::bake
{
struct BakeEvent;
struct Unlink;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
inline constexpr const char* kBakerDock = "baker";

/**
 * The Baker: a view over one bakeProject node and its bakeGroup nodes. Toolbar (project, target and its one-click
 * "Bake for <target>", Bake, Bake Selected, Cancel, progress, backend badge), the groups tree (Low/High/Cage rows,
 * meshes dropped from the outliner, auto-group, one group per selected mesh, inline lint), the selected group's
 * distances and each high's bake-only modifier stack, then tabs for the maps, the outputs and the last bake's
 * thumbnails. Every edit runs a bake command from the UI, so it is undoable and echoed.
 */
class LILA_UI_API BakerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BakerPanel(QWidget* parent = nullptr);
    ~BakerPanel() override;

    /** The project shown (node path), empty when the scene has none. */
    std::string project() const { return mProject; }
    void setProject(const std::string& path);
    /** Rebuilds every section from the scene now. */
    void refresh();
    /** Shows a bake result (the bake command's return): stats, warnings and thumbnails, loaded off the main thread. */
    void showResult(const nlohmann::json& result);
    /** Adds or drops the summary's Lighting row and its button: there while an occlusion result shows under sculpt lighting. */
    void syncLightingNote();
    /**
     * Adds or drops the summary's Preview row and its button: there while a baked result shows and some low is shown without
     * a modifier its export runs, so the maps preview on a mesh they were not baked for.
     */
    void syncPreviewNote();
    /** Shows the lows the Preview row counts as they export (showExportMesh). */
    void showExportMesh();
    /**
     * Adds or drops the summary's Cache row and its button: there, in the error colour and first, while the shown result's
     * snapshots did not fit the cache, so those meshes are prepared again on every bake.
     */
    void syncCacheNote();
    /** Sets the snapshot cache pref to what the shown result needed, with headroom. */
    void raiseSnapshotCache();
    /** Wires the last bake's maps into the lows' materials (bakeAssign) and toasts what went where. */
    void assignResult();
    /** Thumbnails still loading. */
    bool thumbnailsPending() const { return mThumbJobs > 0; }
    /** Where the shown result's textures went, else where the project's would go; empty with no project. */
    QString outputFolder() const;
    /** The thumbnails' right-click menu for the selected ones: Open, Open Containing Folder, Copy Path, Copy File Name. */
    QMenu* thumbnailMenu(QWidget* parent);
    /**
     * Removes the rows selected in the groups tree from their group: Low and High rows unassign their mesh, a Cage row
     * goes back to the auto cage, a group row deletes the group. Scene meshes are never touched. One undo step.
     */
    void removeSelectedRows();
    /** Removes the maps selected in the Maps tab from the project. One undo step. */
    void removeSelectedMaps();

protected:
    void showEvent(QShowEvent* e) override;

private:
    QWidget* buildToolbar();
    QWidget* buildGroups();
    QWidget* buildGroupDetails();
    QWidget* buildMaps();
    QWidget* buildOutput();
    QWidget* buildResults();

    void scheduleRefresh();
    void refreshProjects();
    void refreshTargets();
    void refreshGroups();
    void refreshDetails();
    void refreshMaps();
    void refreshMapSettings();
    /** The selected meshes stop occluding (and, with projection, projecting onto) low, or every low of the group when empty. */
    void unlinkSelected(const std::string& group, const std::string& low, bool projection);
    /** Unlinks a material from the low, or from every low of the group when low is empty. */
    void unlinkMaterial(const std::string& group, const std::string& low, const std::string& material, bool projection);
    /** The materials on the highs (and self lows) of every group in the current group's project. */
    std::vector<std::string> unlinkableMaterials() const;
    void setUnlinks(const std::string& group, const std::vector<bake::Unlink>& unlinks);
    /** Selected mesh and material rows leave their unlink; a selected low row drops its whole unlink. */
    void removeSelectedUnlinks();
    /** R G B A rows for a channel pack map: each a source map and its channel (or luminance/max), or a constant. */
    void addPackChannels(const std::string& map, const nlohmann::json& channels);
    void refreshOutput();
    void updatePreview();
    void updateButtons();
    /** One status row in place of the result rows: Baking, Cancelled, Failed. */
    void showStatus(const QString& text, const QString& detail = {}, bool error = false);
    void scheduleLint();
    /** Whether id is, or is under, one of mLintWatch's meshes. */
    bool watchedByLint(uint64_t id) const;
    void updateLintWatch();
    /** full also loads the highs for the range and cage checks. */
    void runLint(bool full);
    void pollProgress();

    /** Runs a bake command from the UI with project= filled in; null (and a logged warning) on failure. */
    nlohmann::json run(const char* command, nlohmann::json call, bool withProject = true);
    void bake(const std::vector<std::string>& groups);
    void bakeForTarget(bool dryRun);
    /** Saves the project's settings as a preset target named in the inline row, then selects it. */
    void savePreset();
    std::vector<std::string> selectedGroups() const;
    std::string currentGroup() const;
    void dropNodes(const QStringList& names, QTreeWidgetItem* target);
    void groupMenu(const QPoint& at);
    /** A running bake's events (async bakes report as they go): thumbnails fill in, then the result shows. */
    void onBakeEvent(const bake::BakeEvent& e);
    void addThumbnail(const nlohmann::json& output, uint64_t generation);
    /** Sets the stack of the current High row, or of every high when the current row is elsewhere in a group. */
    void setStack(const nlohmann::json& stack);
    /** The current High row's stack, else the stack every high of the current group shares (empty when they differ). */
    nlohmann::json currentStack() const;
    /** Typed fields for the selected stack modifier's parameters, from its node type's input plugs. */
    void refreshParams();

    std::string mProject;
    std::string mTarget;
    bool mUpdating = false;
    bool mPending = false;
    int mGraphListener = 0;
    int mExecListener = 0;
    int mBakeListener = 0;
    int mPrefListener = 0;
    int mArrangeListener = 0;
    /** The shown result baked an occlusion map, which sculpt lighting all but hides. */
    bool mResultOcclusion = false;
    /** Bytes of the shown result's snapshots the cache dropped, and what the cache held then; 0 when all fit. */
    double mCacheDropped = 0.0, mCacheBudget = 0.0;
    /** GB the cache was raised to from the row, 0 when not. */
    double mCacheRaisedTo = 0.0;
    /** A baked (not planned) result is shown. */
    bool mResultBaked = false;
    /** A syncPreviewNote is queued for a modifier that changed. */
    bool mPreviewPending = false;
    /** The backend of the bake running now, empty when none. */
    std::string mRunningBackend;
    std::string mShownResult;
    std::string mMapFormKey;
    nlohmann::json mLint;
    /** The project mLint was run on: a different current project relints. */
    std::string mLintedProject;
    /** Paths of the current project's lows, cages and high nodes: an edit under one relints. */
    std::vector<std::string> mLintWatch;
    int mThumbJobs = 0;
    uint64_t mThumbGeneration = 0;

    QComboBox* mProjects = nullptr;
    QToolButton* mProjectMenu = nullptr;
    QComboBox* mTargets = nullptr;
    /** The inline Save Settings as Preset row under the project row. */
    QWidget* mPresetRow = nullptr;
    QLineEdit* mPresetName = nullptr;
    QComboBox* mPresetScope = nullptr;
    QCheckBox* mPresetReplace = nullptr;
    QToolButton* mBakeTarget = nullptr;
    QToolButton* mBake = nullptr;
    QToolButton* mBakeSelected = nullptr;
    QToolButton* mCancel = nullptr;
    QToolButton* mArrange = nullptr;
    QProgressBar* mProgress = nullptr;
    QToolButton* mBackend = nullptr;
    QToolButton* mLintButton = nullptr;

    QTreeWidget* mGroups = nullptr;
    /** Issue rows of the groups tree folded or unfolded against their default (a check open, its meshes folded), by key. */
    QStringList mLintToggled;
    QWidget* mDetails = nullptr;
    /** "No group selected" over the Group tab's disabled fields. */
    QLabel* mDetailsEmpty = nullptr;
    QDoubleSpinBox* mFrontal = nullptr;
    QDoubleSpinBox* mRear = nullptr;
    QDoubleSpinBox* mCageOffset = nullptr;
    QWidget* mCageRow = nullptr;
    QToolButton* mShowCage = nullptr;
    QToolButton* mLivePreview = nullptr;
    QLabel* mCageStatus = nullptr;
    std::string mCageGroup;
    QCheckBox* mBackfaces = nullptr;
    QComboBox* mAoScope = nullptr;
    QTreeWidget* mUnlinks = nullptr;
    /** Low rows of the unlinks tree folded by the user, by their text. */
    QStringList mUnlinksCollapsed;
    QCheckBox* mSelf = nullptr;
    QWidget* mStackBox = nullptr;
    QLabel* mStackTitle = nullptr;
    QComboBox* mPreset = nullptr;
    QListWidget* mStack = nullptr;
    QWidget* mStackButtons = nullptr;
    QWidget* mParams = nullptr;
    QFormLayout* mParamForm = nullptr;
    std::string mParamKey;
    /** Show new parameter values in the built fields without rebuilding them. */
    std::vector<std::function<void(const nlohmann::json& params)>> mParamSetters;

    QTabWidget* mTabs = nullptr;
    QTreeWidget* mMaps = nullptr;
    QToolButton* mAddMap = nullptr;
    QWidget* mMapSettings = nullptr;
    QFormLayout* mMapForm = nullptr;

    QLineEdit* mFolder = nullptr;
    QLabel* mFolderResolved = nullptr;
    QCheckBox* mAutoAssign = nullptr;
    QCheckBox* mRebakeOnSave = nullptr;
    QLineEdit* mName = nullptr;
    /** The files the name template gives: a row per texture set, its files under it. */
    QTreeWidget* mPreview = nullptr;
    /** Texture sets unfolded in the preview. */
    QStringList mPreviewOpen;
    /** The texture set the format, bits, resolution, supersampling and padding rows edit; "" is the project's. */
    QComboBox* mTextureSet = nullptr;
    QToolButton* mTextureSetReset = nullptr;
    QComboBox* mFormat = nullptr;
    QComboBox* mBits = nullptr;
    QComboBox* mResolution = nullptr;
    QComboBox* mSupersampling = nullptr;
    QSpinBox* mPadding = nullptr;

    /** The shown result as rows: files, time, misses, folder, assignment, then warnings by kind. */
    QTreeWidget* mSummary = nullptr;
    /** The summary and its clear button, above the thumbnails in a splitter. */
    QWidget* mSummaryBox = nullptr;
    QToolButton* mAssign = nullptr;
    /** Switches the viewport to environment lighting; shown with the summary's Lighting row. */
    QToolButton* mEnvironmentLighting = nullptr;
    /** Runs showExportMesh on the lows the Preview row counts; shown with that row. */
    QToolButton* mShowExportMesh = nullptr;
    /** Raises the snapshot cache to fit the shown result; shown with the Cache row. */
    QToolButton* mRaiseCache = nullptr;
    QListWidget* mThumbs = nullptr;
    /** The folder of the result shown, empty before one. */
    QString mResultFolder;

    QTimer* mPoll = nullptr;
    QTimer* mLintTimer = nullptr;
};

/**
 * A written map scaled to fit size x size, on neutral grey where it is transparent. Reads anything core/ImageIO reads
 * (PNG 8/16, EXR, TGA); values are clamped to 0-1 as stored. Null on failure. Any thread.
 */
LILA_UI_API QImage bakeThumbnail(const std::filesystem::path& file, int size);

/** The outputs of a bake result tiled into one contact sheet with each file's map and UDIM under it. Any thread. */
LILA_UI_API QImage bakeContactSheet(const nlohmann::json& result, int tile = 192, int columns = 0);

/** The panel, window.baker, the bake.* actions and the Bake menu. */
LILA_UI_API void registerBakerPanel();

/** MCP tools bake, bake_lint and bake_inspect. */
LILA_UI_API void registerBakerMcpTools();
} // namespace lila::ui
