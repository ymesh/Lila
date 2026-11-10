#pragma once

#include <QWidget>

#include <functional>

class QAction;
class QMenuBar;
class QToolButton;

namespace lila::ui
{
/**
 * The right part of the menu bar row: the layout tabs centred on the window (right of the menus when they would meet), then the folder name, the
 * mode (when more than one is installed), a chip naming a pinned menu set, the command search and Send to Unreal.
 */
class TopBar : public QWidget
{
public:
    TopBar(QMenuBar* bar, QWidget* layoutTabs, QWidget* folder, QWidget* modes, QWidget* parent);

    /** Sizes the bar to the room the menus leave; call when the menus shown change. */
    void relayout();
    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

private:
    void refreshPin();
    void refreshUnreal();
    /** The room the tabs need beside the row: they always fit between the menus and the right-hand controls. */
    int tabsReserve() const;
    /** The menu titles' width, counting the rows shown says to (the row and its index in the bar). */
    int menusWidth(const std::function<bool(const QAction*, size_t)>& shown) const;
    /** The width of the widest menu set's menus. */
    int widestMenus() const;
    void placeTabs();

    QMenuBar* mBar;
    QToolButton* mPin = nullptr;
    QToolButton* mUnreal = nullptr;
    QToolButton* mSearch = nullptr;
    QWidget* mFolder = nullptr;
    QWidget* mModes = nullptr;
    QWidget* mTabs = nullptr;
    bool mConnected = false;
    int mWidth = 0;
};
} // namespace lila::ui
