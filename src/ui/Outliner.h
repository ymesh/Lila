#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "ui/OutlinerFilter.h"

#include <QTreeWidget>
#include <QWidget>

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QMenu;

namespace lila::ui
{
/** Mime format of an outliner MMB drag: the dragged nodes' names, one per line, so views can take the drop. */
inline constexpr const char* kNodeNamesMime = "application/x-lila-node-names";

/**
 * Maya's outliner: the DAG hierarchy with a type icon per row, referenced rows badged and italic, hidden rows dimmed.
 * Each row ends in its issue badge and count and a visibility eye; clicking the eye toggles the object's visibility.
 * LMB drag selects a range of rows; MMB drag moves rows onto another to parent them, or between rows to place them
 * there; dropped on a viewport, a camera row makes that view look through it. Right-click for the common object actions, F with the cursor over it to reveal the selection. Rows start
 * collapsed and keep their open state across rebuilds; ancestors of the selection are shaded.
 */
class LILA_UI_API Outliner : public QTreeWidget
{
    Q_OBJECT

public:
    explicit Outliner(QWidget* parent = nullptr);
    ~Outliner() override;

    /** Expands the parents of the selected objects and scrolls the first one into view. */
    void revealSelected();
    /** Expands the parents of the rows and scrolls the first one into view. */
    void reveal(const std::vector<scene::NodeId>& ids, QAbstractItemView::ScrollHint hint = QAbstractItemView::EnsureVisible);
    /** Starts editing the name of the first selected object's row. */
    void renameSelected();
    void expandEverything();
    void collapseEverything();
    /** Rebuilds the rows once the event loop comes round, however many changes arrive first. */
    void scheduleRebuild();
    /** Re-reads each row's icon and colour on the next event loop pass, without rebuilding the tree. */
    void scheduleRestyle();
    /** Shows only the rows the filter matches, with their ancestors dimmed and opened so the hierarchy reads. */
    void setFilter(outliner::Filter filter);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    /** The rename key pressed again while a name is being edited cancels the edit. */
    void keyPressEvent(QKeyEvent* e) override;

private:
    void rebuild();
    /**
     * Updates the rows' icons, fonts and colors from their nodes' type, reference and visibility. Only rows that can be
     * seen (every parent expanded) are touched; expanding a row restyles what it shows. With `only`, just those nodes'
     * rows and the rows under them.
     */
    void restyle(const std::unordered_set<scene::NodeId>* only = nullptr);
    /** Restyles the subtrees of the nodes edited since the last tick, once per event loop tick. */
    void scheduleRowRestyle(scene::NodeId id);
    void syncSelectionFromScene();
    void pushSelectionToScene();
    void showContextMenu(const QPoint& pos);
    /** Object sets as rows after the hierarchy, each with its members as rows under it. */
    void addSetRows();
    /** The right-click menu of a set row or one of its member rows. */
    void showSetMenu(QTreeWidgetItem* item, const QPoint& globalPos);
    /** The outliner::RowSource groups after the sets. */
    void addSourceRows();
    void startMove();
    /** Whether pos is on a row's visibility eye; with toggle, flips that object's visibility. */
    bool eyeAt(const QPoint& pos, bool toggle);
    /** Shades the rows of the selection's unselected ancestors. */
    void shadeAncestors();
    /** Hides the rows the filter leaves out and opens the ones leading to a match. */
    void applyFilter();

    std::unordered_map<scene::NodeId, QTreeWidgetItem*> mItems;
    /** Rows start collapsed, as in Maya; these were opened. */
    std::set<scene::NodeId> mExpanded;
    std::vector<uint64_t> mEventSubs;
    /** Set rows start collapsed; these were opened. */
    std::set<scene::NodeId> mOpenSets;
    /** RowSource groups that were opened. */
    std::set<std::string> mOpenSources;
    int mListener = 0;
    int mExecListener = 0;
    bool mQueued = false;
    /** Selection changes inside a command sync once it finishes, not once per change. */
    bool mSelectionStale = false;
    bool mRestyleQueued = false;
    std::unordered_set<scene::NodeId> mDirtyRows;
    bool mSyncing = false;
    /** Rows being moved by an MMB drag. */
    std::vector<scene::NodeId> mMoving;
    QPoint mMovePress;
    /** Set while an LMB drag selects rows, so the scene hears one selection at the end instead of one per row. */
    bool mDragSelecting = false;
    bool mSelectionPending = false;
    /** A rebuild was skipped while the pane was collapsed to nothing; it runs when the pane opens. */
    bool mStale = false;
    outliner::Filter mFilter;
    /** Ancestors kept only to place a match, drawn dimmed. */
    std::set<scene::NodeId> mKept;
};

/** Expands every visible outliner down to the selected objects and scrolls to them. */
LILA_UI_API void revealSelectionInOutliners();

/** Expands every visible outliner down to these nodes and scrolls to the first. */
LILA_UI_API void revealInOutliners(const std::vector<scene::NodeId>& ids);

/**
 * The outliner panel: a filter row (filter field, filter options, + for new groups and sets, ... for the rarer actions
 * including every iconBars::kOutliner item), then Maya's hidden second outliner under the first, collapsed to a grab
 * handle at the bottom.
 * Drag the handle up to open it; each pane scrolls, expands and filters on its own. The split is remembered. Each pane
 * has a filter box on top (see outliner::Filter for the syntax).
 */
class LILA_UI_API OutlinerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinerPanel(QWidget* parent = nullptr);
};
} // namespace lila::ui

namespace lila::ui::outliner
{
/** A row of a RowSource: something listed in the outliner that is not a scene node. */
struct ExtraRow
{
    std::string id;
    std::string label;
    std::string icon;
    std::string tooltip;
    bool hidden = false; // drawn dimmed
};

/**
 * Rows the outliner lists after the hierarchy under a group row of their own, for things that live outside the scene
 * graph (reference overlays). Clicking a row activates it; the group row hides when there are no rows.
 */
struct RowSource
{
    std::string id;
    std::string title;
    std::string icon;
    std::function<std::vector<ExtraRow>()> rows;
    std::function<void(const std::string&)> activate;
    /** The row shown selected, empty for none. */
    std::function<std::string()> current;
    std::function<void(const std::string& id, const std::string& name)> rename;
    /** Fills the right-click menu of a row. */
    std::function<void(QMenu*, const std::string&)> menu;
};

LILA_UI_API void addRowSource(RowSource source);
/** Rebuilds every outliner's rows once the event loop comes round; call when a source's rows change. */
LILA_UI_API void refreshRowSources();
} // namespace lila::ui::outliner
