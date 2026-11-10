#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QFormLayout;
class QFileSystemWatcher;
class QPoint;
class QDoubleSpinBox;
class QIcon;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace lila::brush::ui
{
class StrokeOptions;
}

namespace lila::ui
{
namespace controls
{
class Segmented;
class SliderField;
} // namespace controls
class FlowLayout;
class FixedRowLayout;

/** Dock object name for the sculpt panel. */
inline constexpr const char* kSculptDock = "sculptPanel";

/**
 * Sculpt tool panel: Enter/Exit Sculpt with the proxy options, the brush grid (registered brushes included),
 * radius/strength/falloff/spacing and which of them brushes share, symmetry, mask and freeze, presets, polypaint, display,
 * what Exit leaves, and the standalone reprojection tools in its overflow menu.
 */
class LILA_UI_API SculptPanel : public QWidget
{
public:
    explicit SculptPanel(QWidget* parent = nullptr);
    ~SculptPanel() override;

private:
    void syncFromSettings();
    void refreshSession();
    QToolButton* brushButton(const QIcon& icon, const QString& label, const QString& doc, QLayout* flow = nullptr);
    void rebuildCustomBrushes();
    void fillMaterials();
    void rebuildPresets();
    void savePreset();
    void importPresets();
    void presetMenu(size_t index, const QPoint& globalPos);
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

    QWidget* mBrushBox = nullptr;
    FixedRowLayout* mBrushFlow = nullptr;
    QButtonGroup* mBrushes = nullptr;
    /** Registered brush names, in button order. */
    std::vector<std::string> mCustomBrushes;
    int mBrushRegistryListener = 0;
    QWidget* mPresetBox = nullptr;
    QLabel* mPresetEmpty = nullptr;
    FlowLayout* mPresetFlow = nullptr;
    /** Preset ids, in button order. */
    std::vector<std::string> mPresets;
    int mPresetListener = 0;
    QFileSystemWatcher* mPresetWatcher = nullptr;
    bool mPresetsQueued = false;
    QToolButton* mColor = nullptr;
    QCheckBox* mPaint = nullptr;
    QCheckBox* mCavityPaint = nullptr;
    QCheckBox* mShareRadius = nullptr;
    QCheckBox* mShareStrength = nullptr;
    QCheckBox* mShareStroke = nullptr;
    QCheckBox* mWireframe = nullptr;
    controls::SliderField* mWireOpacity = nullptr;
    QCheckBox* mSeeThrough = nullptr;
    controls::SliderField* mOpacity = nullptr;
    QComboBox* mMaterial = nullptr;
    QComboBox* mFalloff = nullptr;
    controls::SliderField* mRadius = nullptr;
    controls::SliderField* mStrength = nullptr;
    controls::SliderField* mSpacing = nullptr;
    QCheckBox* mFrontFaces = nullptr;
    QWidget* mLevelsBox = nullptr;
    QLabel* mLevelLabel = nullptr;
    QToolButton* mLevelDown = nullptr;
    QToolButton* mLevelUp = nullptr;
    QToolButton* mAddLevel = nullptr;
    QToolButton* mReconstruct = nullptr;
    QToolButton* mDeleteLower = nullptr;
    QToolButton* mDeleteHigher = nullptr;
    QToolButton* mDuplicate = nullptr;
    QToolButton* mDuplicateHide = nullptr;
    QCheckBox* mSymmetryLine = nullptr;
    QCheckBox* mSculptLighting = nullptr;
    QCheckBox* mKeepVolume = nullptr;
    controls::SliderField* mPinchDepth = nullptr;
    QFormLayout* mBrushForm = nullptr;
    controls::SliderField* mLightAngle = nullptr;
    int mPrefListener = 0;
    controls::Segmented* mSymmetry = nullptr;
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
    QLabel* mStatus = nullptr;
    QPushButton* mEnter = nullptr;
    QPushButton* mExit = nullptr;
    int mSettingsListener = 0;
    int mSessionListener = 0;
    int mGraphListener = 0;
    bool mRefreshQueued = false;
};

/**
 * Registers the sculpt actions (window.sculpt, sculpt.tool, sculpt.enter, sculpt.exit, sculpt.cancel, the level and duplicate actions, sculpt.brush.*,
 * sculpt.mask*, sculpt.freezeClear, mesh.relaxKeepForm, mesh.reproject, mesh.transferShape), the sculptBrush command and the viewport drawer.
 */
LILA_UI_API void registerSculptUi();
LILA_UI_API bool sculptPanelOpen();
/** A mesh is among the selection, so Enter Sculpt has something to sculpt. */
LILA_UI_API bool meshSelected();
} // namespace lila::ui
