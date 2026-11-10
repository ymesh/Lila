#include "ui/TopBar.h"

#include "link/UnrealLink.h"
#include "ui/Actions.h"
#include "ui/Icons.h"
#include "ui/MenuContext.h"
#include "ui/MenuSets.h"
#include "ui/Menus.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace lila::ui
{
namespace
{
/** A menu title's horizontal padding on the bar (the app sheet's QMenuBar::item padding, both sides). */
constexpr int kMenuPadding = 24;

/**
 * The link state as a dot: filled when connected, a ring when not, so it reads without its colour. Drawn at the
 * screen's pixel ratio so it stays sharp at any UI scale.
 */
QIcon linkDot(bool connected, qreal dpr)
{
    const int size = 16;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF dot(4.5, 4.5, 7, 7);
    if (connected)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#6cc24a"));
        p.drawEllipse(dot);
    }
    else
    {
        p.setPen(QPen(themeColor("dimText"), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(dot);
    }
    return QIcon(pm);
}

void addActionRow(QMenu* menu, const char* id)
{
    if (const Action* a = actions::find(id))
        menu->addAction(icon(QString::fromStdString(actions::iconFor(id))), QString::fromStdString(a->label), [id] { actions::run(nlohmann::json(id)); })
            ->setToolTip(QString::fromStdString(actions::tooltip(id)));
}
} // namespace

TopBar::TopBar(QMenuBar* bar, QWidget* layoutTabs, QWidget* folder, QWidget* modes, QWidget* parent) : QWidget(parent), mBar(bar)
{
    setObjectName("topBar");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(8);
    // On the menu bar itself, placed by hand: they stay at the window's centre whatever menus the workspace shows.
    mTabs = layoutTabs;
    if (layoutTabs)
        layoutTabs->setParent(bar);
    row->addStretch(1);
    mFolder = folder;
    if (folder)
        row->addWidget(folder);
    mModes = modes;
    if (modes)
        row->addWidget(modes);

    mPin = new QToolButton(this);
    mPin->setObjectName("menuPin");
    mPin->setIcon(icon("pin"));
    mPin->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mPin->setToolTip("The menu bar is pinned to this menu set. Click to let it follow what you are doing again.");
    connect(mPin, &QToolButton::clicked, this, [] { menuContext::unpin(); });
    row->addWidget(mPin);

    auto* search = mSearch = new QToolButton(this);
    search->setObjectName("topSearch");
    search->setIcon(icon("search"));
    search->setText("Search commands");
    search->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    search->setToolTip(QString::fromStdString(actions::tooltip("window.searchMenus")));
    search->setFixedWidth(240);
    connect(search, &QToolButton::clicked, this, [] { actions::run(nlohmann::json("window.searchMenus")); });
    row->addWidget(search);

    mUnreal = new QToolButton(this);
    mUnreal->setObjectName("unrealSend");
    mUnreal->setText("Send to Unreal");
    mUnreal->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mUnreal->setPopupMode(QToolButton::MenuButtonPopup);
    auto* menu = new QMenu(mUnreal);
    connect(menu, &QMenu::aboutToShow, menu, [menu] {
        menu->clear();
        for (const char* id : {"unreal.connect", "unreal.sync", "unreal.sendBake", "unreal.expressionRig", "animated.sendAgain", "animated.sendOptions"})
            addActionRow(menu, id);
    });
    mUnreal->setMenu(menu);
    connect(mUnreal, &QToolButton::clicked, this, [] { actions::run(nlohmann::json("unreal.send")); });
    row->addWidget(mUnreal);

    // The link has no change signal; its state is a cheap read.
    auto* poll = new QTimer(this);
    poll->setInterval(1500);
    connect(poll, &QTimer::timeout, this, [this] { refreshUnreal(); });
    poll->start();
    refreshUnreal();

    menuContext::onChanged([this] { refreshPin(); });
    refreshPin();
    bar->installEventFilter(this);
}

void TopBar::refreshPin()
{
    const std::string set = menuContext::pinned();
    mPin->setVisible(!set.empty());
    mPin->setText(QString::fromStdString(set) + " Menus");
    relayout();
}

void TopBar::refreshUnreal()
{
    const nlohmann::json status = link::Link::get().status();
    const bool connected = status.value("connected", false);
    if (connected == mConnected && !mUnreal->icon().isNull())
        return;
    mConnected = connected;
    mUnreal->setIcon(linkDot(connected, devicePixelRatioF()));
    const std::string project = status.value("project", std::string());
    mUnreal->setToolTip(connected ? QString("Send the selection to Unreal%1. The arrow has Sync, Send Bake and the rest.")
                                        .arg(project.empty() ? QString() : " (" + QString::fromStdString(project) + ")")
                                  : QString("Unreal is not connected: sending connects first. Start the Unreal Editor with the LilaLink "
                                            "plugin; the arrow has Connect to Unreal."));
}

void TopBar::relayout()
{
    // Judged against the widest menu set, so the controls keep one size as the menus follow the workspace or tool.
    const int room = mBar->width() - widestMenus() - 16;
    // Short of room, the search field drops to its icon and the folder name to its tooltip before any menu is pushed off.
    auto setCompact = [this](bool compact) {
        mSearch->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        mSearch->setFixedWidth(compact ? 30 : 240);
        if (mFolder)
            mFolder->setVisible(!compact);
        // The mode list stays reachable from Ctrl+T (each mode is an action) when there is no room for it here.
        if (mModes)
            mModes->setMaximumWidth(compact ? 0 : QWIDGETSIZE_MAX);
        layout()->invalidate();
    };
    setCompact(false);
    if (layout()->sizeHint().width() + tabsReserve() > room)
        setCompact(true);
    const int width = layout()->sizeHint().width() + tabsReserve();
    if (width == mWidth)
    {
        placeTabs();
        return;
    }
    mWidth = width;
    updateGeometry();
    // The menu bar places its corner from sizeHint only when it is resized, so it is told it was.
    QResizeEvent resized(mBar->size(), mBar->size());
    QApplication::sendEvent(mBar, &resized);
}

int TopBar::menusWidth(const std::function<bool(const QAction*, size_t)>& shown) const
{
    // Measured from the titles, not actionGeometry: a menu already pushed into the overflow has no geometry.
    int width = 16;
    const QFontMetrics fm = mBar->fontMetrics();
    const QList<QAction*> rows = mBar->actions();
    for (qsizetype i = 0; i < rows.size(); ++i)
        if (shown(rows[i], size_t(i)))
            width += fm.horizontalAdvance(QString(rows[i]->text()).remove('&')) + kMenuPadding;
    return width;
}

int TopBar::widestMenus() const
{
    // The bar's rows are the menu bar menus in registry order.
    const std::vector<const menus::Menu*> defs = menus::menusIn(menus::Area::MenuBar);
    int widest = 0;
    for (const std::string& set : menuContext::sets())
        widest = std::max(widest, menusWidth([&](const QAction* a, size_t i) {
                              return menuSets::reachable(a) && (i >= defs.size() || menuSets::shows(*defs[i], set));
                          }));
    return widest;
}

int TopBar::tabsReserve() const
{
    return mTabs ? mTabs->sizeHint().width() + layout()->spacing() : 0;
}

void TopBar::placeTabs()
{
    if (!mTabs)
        return;
    const QSize size = mTabs->sizeHint();
    const int gap = layout()->spacing();
    int menusRight = 0;
    for (QAction* a : mBar->actions())
        if (const QRect g = mBar->actionGeometry(a); a->isVisible() && g.isValid())
            menusRight = std::max(menusRight, g.right());
    auto* more = mBar->findChild<QToolButton*>("qt_menubar_ext_button");
    if (more && more->isVisible())
        menusRight = std::max(menusRight, more->geometry().right());
    // From the row's size rather than its widgets' places, which lag a resize.
    const int clusterLeft = width() - layout()->sizeHint().width();
    const int cornerLeft = mapTo(mBar, QPoint(0, 0)).x();
    const int hi = mapTo(mBar, QPoint(clusterLeft, 0)).x() - gap - size.width();
    // Clear of the menus shown now; past an overflow, right of it all, in the room the corner reserves.
    const bool overflow = more && more->isVisible();
    const int lo = std::min(overflow ? cornerLeft : menusRight + gap, hi);
    // Centred, or else right of the widest menu set, so the menus changing with the workspace or tool never move them.
    const int x = std::clamp(std::max(mBar->width() / 2 - size.width() / 2, widestMenus() + gap), lo, hi);
    mTabs->setGeometry(x, (mBar->height() - size.height()) / 2, size.width(), size.height());
    mTabs->raise();
}

void TopBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // After the row has placed the right-hand controls, which the tabs must stay clear of.
    QTimer::singleShot(0, this, [this] { placeTabs(); });
}

void TopBar::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    placeTabs();
}

QSize TopBar::sizeHint() const
{
    const QSize row = layout()->sizeHint();
    return QSize(std::max(mWidth, row.width() + tabsReserve()), std::max(row.height(), mTabs ? mTabs->sizeHint().height() : 0));
}

bool TopBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mBar && (event->type() == QEvent::Resize || event->type() == QEvent::ActionChanged || event->type() == QEvent::ActionAdded ||
                            event->type() == QEvent::ActionRemoved))
        QTimer::singleShot(0, this, [this] { relayout(); });
    return QWidget::eventFilter(watched, event);
}
} // namespace lila::ui
