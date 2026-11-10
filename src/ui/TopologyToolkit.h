#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QDoubleSpinBox;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace lila::ui
{
/** Dock object name for the Topology Toolkit. */
inline constexpr const char* kTopologyToolkitDock = "topologyToolkit";

namespace controls
{
class Segmented;
}

/**
 * The Topology Toolkit: the retopo session (reference, retopo mesh, symmetry), the topology tools as tiles, their settings,
 * the active construct's settings, the list of live constructs and guides, auto retopo and the quality numbers.
 */
class LILA_UI_API TopologyToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit TopologyToolkit(QWidget* parent = nullptr);
    ~TopologyToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    /** A tile for a registered action, or null when it isn't one. */
    QToolButton* tile(const std::string& id, const char* iconName, const QString& label, QWidget* parent);
    /** A labelled push button for a registered action, or null when it isn't one. */
    QToolButton* push(const std::string& id, const QString& label, QWidget* parent);
    QToolButton* track(QToolButton* b, const std::string& id);
    /** A settings row's field: a slider when the range is a real bound, a plain value field otherwise. */
    QDoubleSpinBox* prefField(const char* key, double def, double lo, double hi, int decimals, const QString& unit, bool slider, const char* tip,
                              QWidget* parent);
    void refreshState();
    void rebuildConstructPanel();
    /** Deletes the active construct and makes its neighbour in the list active. */
    void deleteActiveConstruct();

    QLabel* mReference = nullptr;
    QLabel* mRetopo = nullptr;
    controls::Segmented* mSymmetry = nullptr;
    QLabel* mStats = nullptr;
    QLabel* mActiveLabel = nullptr;
    QWidget* mConstructPanel = nullptr;
    QVBoxLayout* mConstructLayout = nullptr;
    QListWidget* mList = nullptr;
    QTimer* mRefresh = nullptr;
    std::vector<QToolButton*> mButtons;
    std::string mPanelKey;
    int mGraphListener = 0;
    int mToolListener = 0;
    int mPrefListener = 0;
};

/** Registers "window.topologyToolkit", which shows or hides the dock named kTopologyToolkitDock. */
LILA_UI_API void registerTopologyToolkitActions();
} // namespace lila::ui
