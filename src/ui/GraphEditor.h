#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <QWidget>

#include <string>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSlider;
class QToolButton;
class QTreeWidget;

namespace lila::ui::controls
{
class Segmented;
}

namespace lila::ui
{
namespace graphed
{
class CurveView;
class GraphModel;
}

/**
 * Maya's graph editor, extended: the selection's animated channels on the left, their curves on the right, speed and
 * acceleration strips under them. Alt+MMB pans, Alt+RMB zooms (drag sideways for time, up and down for value), the
 * wheel zooms both. LMB selects keys (click or box; Shift toggles, Ctrl removes, Ctrl+Shift adds; a click on a curve
 * picks its keys) and drags them with the current tool: Move (W, with a box whose edges scale the selection), Rotate
 * (E, turns each key's tangents) or Scale (R, about the chosen pivot). MMB drags with the tool from anywhere. Shift
 * constrains to one axis; a number typed mid-drag sets the dragged key's value (Enter commits, Esc cancels). RMB click
 * opens the context menu, an RMB flick the tangent marking menu. Every edit runs through commands, so it undoes.
 */
class LILA_UI_API GraphEditor : public QWidget
{
public:
    explicit GraphEditor(QWidget* parent = nullptr);
    ~GraphEditor() override;

    /** The editor hotkeys act on: the last one focused. */
    static GraphEditor* instance();

    /** Fits the view to the selected keys, or to every shown curve. */
    void frame(bool selectedOnly);
    void deleteSelectedKeys();
    /** 0 move, 1 rotate tangents, 2 scale. */
    void setTool(int tool);
    void toggleFalloff();
    /** W, E or R down and up: see graphed::CurveView::toolKeyDown. */
    void toolKeyDown(int tool);
    void toolKeyUp();
    /** B down and up: a tap toggles falloff, holding it while dragging sets the radius. */
    void falloffKeyDown();
    void falloffKeyUp();
    /** 0 absolute, 1 stacked, 2 normalized. */
    void setViewMode(int mode);
    /** Remembers the target curves as they are now, to compare against and swap back to. */
    void snapshotBuffer();
    void swapBuffer();
    /** Breaks (lock false) or unifies the selected keys' tangent handles. */
    void lockTangents(bool lock);
    /** The channels, curves and key selection the editor shows. */
    graphed::GraphModel& model();
    /** Runs a key command on the selected keys, or every key of the target curves when none are selected. */
    void runOnKeys(const std::string& command, const nlohmann::json& call);
    /** Adds or removes a modifier on the target curves through curveModifier, with its option box values. */
    void runOnCurves(const std::string& command, const nlohmann::json& call);

protected:
    void focusInEvent(QFocusEvent* e) override;

private:
    /** The Channel Box sync listener (attrView). */
    int mSyncListener = 0;
    friend class graphed::CurveView;

    void scheduleRebuild();
    void rebuild();
    void reloadCurves();
    void applyVisibility();
    void applyTangent(const char* type);
    void applyInfinity(bool pre, const char* mode);
    void refreshKeyFields();
    void editKeyField(bool time);
    void fillContextMenu(QMenu* menu);
    /** Pre and post infinity, loop key. */
    void fillInfinityMenu(QMenu* menu);
    void fillCurveMenu(QMenu* menu);
    void syncToolbar();
    /** Shows the selected keys' shared tangent type and broken state on the toolbar, none when mixed. */
    void syncTangentState();
    void saveSettings();

    QTreeWidget* mChannels = nullptr;
    graphed::CurveView* mView = nullptr;
    QLineEdit* mKeyTime = nullptr;
    QLineEdit* mKeyValue = nullptr;
    QLabel* mValueUnit = nullptr;
    controls::Segmented* mToolSeg = nullptr;
    controls::Segmented* mViewSeg = nullptr;
    controls::Segmented* mTangentSeg = nullptr;
    controls::Segmented* mBrokenSeg = nullptr;
    QComboBox* mPivot = nullptr;
    QToolButton* mFalloff = nullptr;
    QDoubleSpinBox* mFalloffRadius = nullptr;
    QComboBox* mTweenMode = nullptr;
    QSlider* mTween = nullptr;
    int mListener = 0;
    bool mQueued = false;
    bool mCurvesQueued = false;
};
} // namespace lila::ui
