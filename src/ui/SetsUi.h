#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>

class QLabel;
class QMenu;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
inline constexpr const char* kQuickSelectSetsDock = "quickSelectSets";

/**
 * Every quick select set in one list (all object sets with the filter button): clicking a row selects its members,
 * Ctrl or Shift adds them; double-click renames. The toolbar and the right-click menu create a set from the selection,
 * add or remove the selection, and delete.
 */
class LILA_UI_API QuickSelectSetsPanel : public QWidget
{
public:
    explicit QuickSelectSetsPanel(QWidget* parent = nullptr);
    ~QuickSelectSetsPanel() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scheduleRefresh();
    void refresh();
    void contextMenu(const QPoint& at);
    void rename(QTreeWidgetItem* item);
    /** The set the toolbar acts on: the current row, else empty. */
    std::string currentSet() const;

    QTreeWidget* mTree = nullptr;
    QToolButton* mAllSets = nullptr;
    QToolButton* mSelect = nullptr;
    QLabel* mEmpty = nullptr;
    int mGraphListener = 0;
    bool mPending = false;
    bool mFilling = false;
    bool mEditing = false; // a rename is open; a rebuild would end it
};
} // namespace lila::ui

namespace lila::ui::sets
{
/**
 * "sets.create" (Create > Sets > Set), "sets.createQuickSelect" (Create > Sets > Quick Select Set...) and
 * "window.quickSelectSets" (the panel).
 */
LILA_UI_API void registerActions(QWidget* dialogParent);

/** Fills the menu with the quick select sets each time it opens, one click selecting a set's members. */
LILA_UI_API void attachQuickSelectMenu(QMenu* menu);
} // namespace lila::ui::sets
