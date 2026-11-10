#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class QMenu;
class QTimer;
class QToolButton;

namespace lila::ui
{
class ViewportWidget;

namespace controls
{
class Segmented;
}

/**
 * The one row along the top of a viewport panel. Left: component mode and shading as segmented controls, then Symmetry
 * and Soft Select as toggles whose arrow opens their settings. Lighting as a popover beside shading. Right: the Snap and Overlays popovers, the camera menu,
 * framing and the view layout. As the panel narrows, labels drop to icons first, then whole items fold into a chevron
 * menu, rarest first.
 */
class LILA_UI_API ViewportHeader : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportHeader(ViewportWidget* view, QWidget* parent = nullptr);
    ~ViewportHeader() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    struct Item
    {
        QWidget* widget;
        bool right;
        int keep;                             // higher keeps its place longer as the row narrows; separators ignore it
        std::function<void(QMenu*)> overflow; // the item's controls as chevron menu entries; null for separators
        std::vector<QToolButton*> labelled;   // buttons whose labels drop to icons alone when the row is short
        /** Widths last measured labelled [0] and icon-only [1], valid while the label text width stays labelText. */
        int measured[2] = {0, 0};
        int labelText = 0;
    };

    void add(QWidget* w, bool right, int keep, std::function<void(QMenu*)> overflow, std::vector<QToolButton*> labelled = {});
    void addSeparator(bool right);
    QToolButton* button(const char* iconName, const QString& text);
    /** A labelled button opening a menu built as it opens; stayOpen keeps it open while checkable rows are clicked. */
    QToolButton* popover(const char* iconName, const QString& text, std::function<void(QMenu*)> fill, bool stayOpen = true);
    QToolButton* actionButton(const char* id, const char* iconName, int keep);

    /** Builds the items the user's header layout lists (viewportHeader::config), in order. */
    void buildItems();
    void rebuildItems();
    void buildEntry(const nlohmann::json& entry);
    void buildSelectionMasks();
    void fillCommandMenu(QMenu* m, const nlohmann::json& items);
    void addJointSizeRow(QMenu* m);
    void fillRendererMenu(QMenu* m);
    void contextMenu(const QPoint& at);
    void buildModes();
    void buildShading();
    void buildSymmetry();
    void buildSoftSelect();
    void buildCamera();

    void fillSymmetryMenu(QMenu* m);
    void fillSoftSelectMenu(QMenu* m, bool withToggle);
    void fillSnapMenu(QMenu* m);
    void fillOverlaysMenu(QMenu* m);
    void fillLightingMenu(QMenu* m);
    /** A menu row running action id in this view, checkable when the action has a state. */
    QAction* actionRow(QMenu* m, const std::string& id, bool enabled = true);
    void fillCameraMenu(QMenu* m);

    /** Makes this panel's view the active one, so actions that target "the viewport" act on it. */
    void activateView();
    void refresh();
    void refreshSymmetry();
    void refreshTooltips();
    void layoutItems();

    ViewportWidget* mView;
    std::vector<Item> mItems;
    nlohmann::json mConfig;
    bool mRight = false;
    int mEntryIndex = -1;
    std::vector<std::pair<QToolButton*, std::string>> mActionButtons;
    controls::Segmented* mModes = nullptr;
    controls::Segmented* mShading = nullptr;
    QToolButton* mSymmetry = nullptr;
    QToolButton* mSoftSelect = nullptr;
    QToolButton* mCamera = nullptr;
    QToolButton* mQuad = nullptr;
    QToolButton* mChevron = nullptr;
    QTimer* mSymmetryRefresh = nullptr;
    int mPrefListener = 0;
    int mToolsListener = 0;
    int mComponentListener = 0;
    int mGraphListener = 0;
    int mRendererListener = 0;
};
} // namespace lila::ui
