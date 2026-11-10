#include "ui/MenuSearch.h"
#include "ui/MenuSets.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "scene/Graph.h"
#include "ui/AchievementRules.h"
#include "ui/ApiReference.h"
#include "ui/DocsUi.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Outliner.h"
#include "ui/OutlinerFilter.h"
#include "ui/OutlinerLogic.h"
#include "ui/Panels.h"
#include "ui/TearOff.h"
#include "ui/Theme.h"
#include "ui/TokenCompleter.h"

#include <QApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <map>
#include <vector>

namespace lila::ui::menuSearch
{
namespace
{
QString plain(const QAction* a)
{
    // Menus append the shortcut after a tab; the search box shows it in its own column.
    QString t = a->text().section('\t', 0, 0);
    t.remove('&');
    return t;
}

QStringList keywordsOf(const QAction* a)
{
    QStringList out;
    if (const Action* action = actions::find(a->data().toString().toStdString()))
        for (const std::string& k : action->keywords)
            out << QString::fromStdString(k);
    // The Windows menu lists each dock's own toggle action, which carries no action id.
    if (auto* dock = qobject_cast<QDockWidget*>(a->parent()); dock && a == dock->toggleViewAction())
        if (const panels::Panel* p = panels::registry().find(dock->objectName().toStdString()))
            for (const std::string& k : p->keywords)
                out << QString::fromStdString(k);
    return out;
}

bool keywordMatch(const QStringList& keywords, const QString& query)
{
    return !query.isEmpty() &&
           std::any_of(keywords.begin(), keywords.end(), [&](const QString& k) { return k.startsWith(query, Qt::CaseInsensitive); });
}

bool keywordMatch(const QAction* a, const QString& query)
{
    return !query.isEmpty() && keywordMatch(keywordsOf(a), query);
}

/** Hides or shows a row. QMenu leaves a hidden widget row's widget where it was, drawn over the rows still shown. */
void setRowVisible(QAction* a, bool on)
{
    a->setVisible(on);
    if (auto* wa = qobject_cast<QWidgetAction*>(a); wa && wa->defaultWidget())
        wa->defaultWidget()->setVisible(on);
}

bool matches(const QAction* a, const QString& text, int depth = 0)
{
    if (plain(a).contains(text, Qt::CaseInsensitive) || keywordMatch(a, text))
        return true;
    // A widget row has no text of its own; its labels name it.
    if (auto* wa = qobject_cast<const QWidgetAction*>(a); wa && wa->defaultWidget())
        for (const QLabel* l : wa->defaultWidget()->findChildren<QLabel*>())
            if (l->text().contains(text, Qt::CaseInsensitive))
                return true;
    if (QMenu* sub = a->menu(); sub && depth < 8)
        for (const QAction* c : sub->actions())
            if (c->isVisible() && !c->isSeparator() && matches(c, text, depth + 1))
                return true;
    return false;
}

struct Filter
{
    QString text;
    int width = 0; // the menu's width before filtering, held so it does not jump as rows hide
    QPointer<QWidgetAction> header;
    QPointer<QLabel> label;
    std::vector<QPointer<QAction>> hidden;
};

class TypeToFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        auto* menu = qobject_cast<QMenu*>(obj);
        if (!menu)
            return false;
        switch (e->type())
        {
        case QEvent::KeyPress: return key(menu, static_cast<QKeyEvent*>(e));
        case QEvent::Show: inherit(menu); return false;
        case QEvent::Hide:
            if (auto it = mFilters.find(menu); it != mFilters.end())
            {
                restore(menu, it->second);
                mFilters.erase(it);
            }
            return false;
        default: return false;
        }
    }

private:
    bool key(QMenu* menu, QKeyEvent* e)
    {
        if (e->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))
            return false;
        Filter& f = mFilters[menu];
        QString typed = e->text();
        if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Escape)
        {
            if (f.text.isEmpty())
                return false;
            f.text = e->key() == Qt::Key_Escape ? QString() : f.text.chopped(1);
        }
        // Space on an empty search keeps its usual job of running the highlighted row.
        else if (typed.size() == 1 && typed[0].isPrint() && (typed != " " || !f.text.isEmpty()))
            f.text += typed;
        else
            return false;
        apply(menu, f);
        return true;
    }

    /** A submenu opened from a filtered menu keeps filtering by the same text, unless its own name matched. */
    void inherit(QMenu* menu)
    {
        for (QObject* o : menu->menuAction()->associatedObjects())
            if (auto it = mFilters.find(qobject_cast<QMenu*>(o)); it != mFilters.end() && !it->second.text.isEmpty())
            {
                if (plain(menu->menuAction()).contains(it->second.text, Qt::CaseInsensitive))
                    return;
                Filter& f = mFilters[menu];
                f.text = it->second.text;
                apply(menu, f);
                return;
            }
    }

    static void restore(QMenu* menu, Filter& f)
    {
        for (QAction* a : f.hidden)
            if (a)
                setRowVisible(a, true);
        f.hidden.clear();
        if (f.header)
        {
            menu->removeAction(f.header);
            f.header->deleteLater();
        }
        f.header.clear();
        f.label.clear();
    }

    /** Shows only the rows matching the text, changing just the rows whose state differs so the menu does not flash. */
    static void apply(QMenu* menu, Filter& f)
    {
        if (f.text.isEmpty())
        {
            restore(menu, f);
            menu->setActiveAction(nullptr);
            return;
        }
        if (!f.width)
            f.width = menu->width();
        auto ours = [&f](QAction* a) { return std::find(f.hidden.begin(), f.hidden.end(), a) != f.hidden.end(); };
        QList<QAction*> rows = menu->actions();
        rows.removeAll(f.header.data());
        std::vector<QAction*> shown;
        for (QAction* a : rows)
            if ((a->isVisible() || ours(a)) && !a->isSeparator() && matches(a, f.text))
                shown.push_back(a);

        const QString title = shown.empty() ? QString("No match: %1").arg(f.text) : QString("Search: %1").arg(f.text);
        if (f.label)
            f.label->setText(title);
        else
        {
            // The menu resizes as rows hide, so the header needs its text and width before then, or a search with no
            // match shrinks the menu to nothing.
            f.label = new QLabel(title, menu);
            f.label->setContentsMargins(10, 4, 10, 4);
            f.label->setEnabled(false);
            const int frame = 2 * (menu->style()->pixelMetric(QStyle::PM_MenuPanelWidth, nullptr, menu) +
                                   menu->style()->pixelMetric(QStyle::PM_MenuHMargin, nullptr, menu));
            f.label->setMinimumWidth(std::max(0, f.width - frame));
            f.header = new QWidgetAction(menu);
            f.header->setDefaultWidget(f.label);
            menu->insertAction(rows.value(0), f.header);
        }

        menu->setUpdatesEnabled(false);
        QAction* first = nullptr;
        for (QAction* a : rows)
        {
            const bool hiddenByUs = ours(a);
            if (!a->isVisible() && !hiddenByUs)
                continue;
            if (std::find(shown.begin(), shown.end(), a) == shown.end())
            {
                if (!hiddenByUs)
                {
                    setRowVisible(a, false);
                    f.hidden.emplace_back(a);
                }
                continue;
            }
            if (hiddenByUs)
            {
                setRowVisible(a, true);
                std::erase(f.hidden, QPointer<QAction>(a));
            }
            // Highlighting a submenu row would pop it open, so Enter goes to the first plain row.
            if (!first && a->isEnabled() && !a->menu())
                first = a;
        }
        menu->setUpdatesEnabled(true);
        menu->setActiveAction(first);
    }

    std::map<QMenu*, Filter> mFilters;
};

struct Item
{
    QPointer<QAction> action;
    QString label;
    QString path;
};

void collect(QMenu* menu, const QString& path, std::vector<Item>& out, int depth)
{
    // Lists of files (recent scenes, recovered scenes) are history, not commands.
    if (depth > 8 || menu->property(kNoSearchProperty).toBool())
        return;
    // Menus that fill themselves when opened (cameras, layouts) are filled now so their rows can be found.
    Q_EMIT menu->aboutToShow();
    for (QAction* a : menu->actions())
    {
        if (a->isSeparator() || !a->isVisible() || qobject_cast<QWidgetAction*>(a))
            continue;
        QString label = plain(a);
        if (label.isEmpty())
            continue;
        if (QMenu* sub = a->menu())
            collect(sub, path + " > " + label, out, depth + 1);
        else
        {
            // The rows a menu picks between (layouts) say what they are, so "Groom" the layout reads apart from the panel.
            if (a->isCheckable())
                label += menu->property(kSearchSuffixProperty).toString();
            out.push_back({a, label, path});
        }
    }
}

/** A menu, or one titled section of it, that tears off. */
struct TearOffItem
{
    QPointer<QMenu> menu;
    QString section;
    QString label;
    QString path;
    QStringList keywords; // of the rows it holds directly, so a search by keyword finds the section to tear off
};

void collectTearOffs(QMenu* menu, const QString& label, const QString& path, std::vector<TearOffItem>& out, int depth)
{
    if (depth > 8 || menu->property(kNoSearchProperty).toBool())
        return;
    Q_EMIT menu->aboutToShow();
    const size_t self = out.size();
    out.push_back({menu, {}, label, path});
    size_t holder = self;
    const QString inside = path.isEmpty() ? label : path + " > " + label;
    for (QAction* a : menu->actions())
    {
        if (!a->isVisible() || qobject_cast<QWidgetAction*>(a))
            continue;
        if (a->isSeparator() && !a->text().isEmpty())
        {
            holder = out.size();
            out.push_back({menu, a->text(), a->text(), inside});
        }
        else if (QMenu* sub = a->menu(); sub && !plain(a).isEmpty())
            collectTearOffs(sub, plain(a), inside, out, depth + 1);
        else if (!a->isSeparator())
            out[holder].keywords << keywordsOf(a);
    }
}

/** Every word must appear in the label or the path; rows whose label starts with the query come first. */
int score(const QString& label, const QString& path, const QString& query)
{
    if (query.isEmpty())
        return 1;
    QString hay = label + ' ' + path;
    for (const QString& w : query.split(' ', Qt::SkipEmptyParts))
        if (!hay.contains(w, Qt::CaseInsensitive))
            return 0;
    if (label.startsWith(query, Qt::CaseInsensitive))
        return 3;
    return label.contains(query, Qt::CaseInsensitive) ? 2 : 1;
}

enum class Mode
{
    Menus,
    Scene,
    TearOffs,
    Api,
};

QString& lastQuery(Mode m)
{
    static QString q[4];
    return q[int(m)];
}

/** Every word in the name, kind or summary; name matches first, then commands and functions ahead of flags and members. */
int apiScore(const ApiSymbol& s, const QString& query)
{
    int rank = s.kind == "command" ? 3 : s.kind == "function" || s.kind == "class" ? 2 : s.kind == "flag" ? 1 : 0;
    if (query.isEmpty())
        return rank + 1;
    QString hay = s.name + ' ' + s.kind + ' ' + s.summary;
    for (const QString& w : query.split(' ', Qt::SkipEmptyParts))
        if (!hay.contains(w, Qt::CaseInsensitive))
            return 0;
    QString last = s.name.section('.', -1);
    int match = 1;
    if (s.name.compare(query, Qt::CaseInsensitive) == 0 || last.compare(query, Qt::CaseInsensitive) == 0)
        match = 5;
    else if (s.name.startsWith(query, Qt::CaseInsensitive) || last.startsWith(query, Qt::CaseInsensitive))
        match = 4;
    else if (s.name.contains(query, Qt::CaseInsensitive))
        match = 3;
    return match * 10 + rank + 1;
}

constexpr int kActionRole = Qt::UserRole;
constexpr int kNodeRole = Qt::UserRole + 1;
constexpr int kParentPathRole = Qt::UserRole + 2; // "|group|sub|" in front of the row's name
constexpr int kSymbolRole = Qt::UserRole + 3;     // index into the box's API symbols
constexpr int kTearOffRole = Qt::UserRole + 4;    // index into the box's tear-offs

enum Column
{
    kLabel,
    kWhere,
    kKey,
    kOptions,
    kColumns,
};

/** The scene finder's path cell: the ancestors dim, the node's own name bright, eliding the ancestors first. */
class PathDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QString name = opt.text;
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);
        QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w).adjusted(2, 0, -2, 0);
        p->save();
        p->setFont(opt.font);
        const QFontMetrics fm = p->fontMetrics();
        QString shownName = fm.elidedText(name, Qt::ElideRight, r.width());
        int nameW = fm.horizontalAdvance(shownName);
        QString head = fm.elidedText(index.data(kParentPathRole).toString(), Qt::ElideLeft, std::max(0, r.width() - nameW));
        QRect headRect;
        p->setPen(themeColor("dimText"));
        p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, head, &headRect);
        p->setPen(themeColor("brightText"));
        // Placed after the rect the ancestors actually drew into, which can be wider than their measured advance.
        p->drawText(r.adjusted(head.isEmpty() ? 0 : headRect.width(), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, shownName);
        p->restore();
    }
};

struct SceneRow
{
    scene::NodeId id;
    QString name;
    QString parentPath;
    std::string fullPath;
};

class SearchBox;
QPointer<SearchBox>& openBoxInstance()
{
    static QPointer<SearchBox> box;
    return box;
}

/**
 * The Ctrl+T / Ctrl+F / Shift+T box. Menus mode lists every menu item and runs it, or opens its option box; Scene mode
 * lists every node the outliner shows and selects it; TearOffs mode lists every menu and section and tears it off under
 * the cursor; Api mode, opened from the script editor, lists the scripting API and inserts a call. The other mode's hotkey, a tab or Ctrl+Tab switches in place.
 */
class SearchBox : public QFrame
{
public:
    SearchBox(QMenuBar* bar, QWidget* over) : QFrame(over, Qt::Popup), mBar(bar)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);
        mTabs = new QTabBar(this);
        mTabs->setFocusPolicy(Qt::NoFocus);
        mTabs->setDrawBase(false);
        mTabs->setExpanding(false);
        mTabs->setTabData(mTabs->addTab("Menus"), int(Mode::Menus));
        mTabs->setTabData(mTabs->addTab("Tear-offs"), int(Mode::TearOffs));
        mTabs->setTabData(mTabs->addTab("Scene"), int(Mode::Scene));
        connect(mTabs, &QTabBar::currentChanged, this, [this](int i) {
            if (i >= 0 && modeAt(i) != mMode)
                setMode(modeAt(i));
        });
        lay->addWidget(mTabs);
        mEdit = new QLineEdit(this);
        mEdit->setClearButtonEnabled(true);
        mList = new QTreeWidget(this);
        mList->setColumnCount(kColumns);
        mList->setHeaderHidden(true);
        mList->setRootIsDecorated(false);
        mList->setUniformRowHeights(true);
        mList->setFocusPolicy(Qt::NoFocus);
        mList->setIconSize(QSize(16, 16));
        mList->setMouseTracking(true);
        mList->header()->setStretchLastSection(false);
        mList->header()->setSectionResizeMode(kLabel, QHeaderView::Stretch);
        for (int c : {kWhere, kKey, kOptions})
            mList->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        mHint = new QLabel(this);
        mHint->setContentsMargins(4, 0, 4, 0);
        mHint->setStyleSheet("color: #969696;");
        lay->addWidget(mEdit);
        lay->addWidget(mList);
        lay->addWidget(mHint);
        mPathDelegate = new PathDelegate(mList);
        mEdit->installEventFilter(this);
        connect(mEdit, &QLineEdit::textChanged, this, [this] { refill(); });
        completion::attach(mEdit, [this](const QString& token) {
            QStringList out;
            if (mMode == Mode::Scene)
                for (const std::string& s : outliner::filterCompletions(token.toStdString(), outliner::nodeTypeNames()))
                    out << QString::fromStdString(s);
            return out;
        });
        connect(mList, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* row, int column) {
            mList->setCurrentItem(row);
            Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
            if (mMode == Mode::Scene)
                pick(mods);
            else if (mMode == Mode::Api)
                (column == kOptions || (mods & Qt::ShiftModifier)) ? apiDocs(column == kOptions && (mods & Qt::ShiftModifier)) : apiInsert();
            else if (mMode == Mode::TearOffs)
                placeTearOff();
            else if (column == kOptions || (mods & Qt::ShiftModifier))
                openOptions();
            else
                run();
        });

        int w = std::min(760, over->width() - 40);
        resize(w, 420);
        move(over->mapToGlobal(QPoint((over->width() - w) / 2, 60)));
    }

    ~SearchBox() override { lastQuery(mMode) = mEdit->text(); }

    Mode mode() const { return mMode; }
    bool hasApi() const { return bool(mTarget.insert); }

    void setApiTarget(ApiTarget target)
    {
        mTarget = std::move(target);
        if (tabOf(Mode::Api) < 0)
            mTabs->setTabData(mTabs->insertTab(tabOf(Mode::Scene), "Scripting API"), int(Mode::Api));
    }

    void start(Mode m)
    {
        mHome = m == Mode::Api ? Mode::Api : Mode::Menus;
        setMode(m);
        show();
        mEdit->setFocus();
    }

    void setMode(Mode m)
    {
        if (mStarted && m == mMode)
            return;
        auto menuish = [](Mode x) { return x == Mode::Menus || x == Mode::TearOffs; };
        // Menus and Tear-offs search the same menus, so the query carries across.
        bool carry = mStarted && menuish(mMode) && menuish(m);
        if (mStarted)
            lastQuery(mMode) = mEdit->text();
        if (carry)
            lastQuery(m) = mEdit->text();
        mStarted = true;
        mMode = m;
        {
            QSignalBlocker block(mTabs);
            mTabs->setCurrentIndex(tabOf(m));
        }
        bool scene = m == Mode::Scene;
        bool api = m == Mode::Api;
        bool tearOffs = m == Mode::TearOffs;
        if (scene)
            collectScene();
        else if (tearOffs)
        {
            if (mTearOffs.empty() && mBar)
                for (QAction* a : mBar->actions())
                    if (QMenu* menu = a->menu(); menu && menuSets::reachable(a))
                        collectTearOffs(menu, plain(a), {}, mTearOffs, 0);
        }
        else if (api)
        {
            if (mSymbols.empty())
                mSymbols = apiSymbols();
        }
        else if (mItems.empty() && mBar)
            for (QAction* a : mBar->actions())
                if (QMenu* menu = a->menu(); menu && menuSets::reachable(a))
                    collect(menu, plain(a), mItems, 0);
        mList->setItemDelegateForColumn(kLabel, scene ? mPathDelegate : nullptr);
        mList->setColumnHidden(kKey, scene || tearOffs);
        mList->setColumnHidden(kOptions, scene || tearOffs);
        mEdit->setPlaceholderText(scene      ? "Search the scene: names, wildcards (pCube*), type:mesh, |group|path, !exclude"
                                  : api      ? "Search the scripting API: commands, flags, lila classes and functions"
                                  : tearOffs ? "Search menus and menu sections to tear off"
                                             : "Search menus");
        auto key = [](const char* id, const char* fallback) {
            QString k = Hotkeys::get().keyFor(id);
            return k.isEmpty() ? QString(fallback) : k;
        };
        mHint->setText(tearOffs ? QString("Enter tears it off under the cursor, click to drop it    Esc or right click puts it back")
                       : scene  ? QString("Enter selects    Shift+Enter adds    Ctrl+Enter toggles    %1 searches menus")
                                   .arg(key("window.searchMenus", "Ctrl+T"))
                       : api ? QString("Enter inserts the call    Shift+Enter or the book shows the docs    Ctrl+Tab switches")
                             : QString("Enter runs    Shift+Enter or the square opens the options    %1 searches the scene")
                                   .arg(key("window.searchScene", "Ctrl+F")));
        QSignalBlocker block(mEdit);
        mEdit->setText(lastQuery(m));
        mEdit->selectAll();
        refill();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != mEdit || e->type() != QEvent::KeyPress)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        bool menusKey = Hotkeys::get().isKeyFor(k, "window.searchMenus");
        bool sceneKey = Hotkeys::get().isKeyFor(k, "window.searchScene");
        bool tearOffsKey = Hotkeys::get().isKeyFor(k, "window.searchTearOffs");
        bool apiKey = hasApi() && Hotkeys::get().isKeyFor(k, "script.searchApi");
        if (menusKey || sceneKey || tearOffsKey || apiKey)
        {
            // One key can mean both menus and the API (Ctrl+T); it then means whichever the box opened as.
            Mode want = sceneKey             ? Mode::Scene
                        : tearOffsKey        ? Mode::TearOffs
                        : menusKey && apiKey ? mHome
                        : apiKey             ? Mode::Api
                                             : Mode::Menus;
            if (want == mMode)
                close();
            else
                setMode(want);
            return true;
        }
        if (k->key() == Qt::Key_Tab && k->modifiers() == Qt::ControlModifier)
        {
            setMode(modeAt((tabOf(mMode) + 1) % mTabs->count()));
            return true;
        }
        int n = mList->topLevelItemCount();
        int row = mList->currentItem() ? mList->indexOfTopLevelItem(mList->currentItem()) : -1;
        switch (k->key())
        {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
        {
            if (!n)
                return true;
            int step = k->key() == Qt::Key_Down ? 1 : k->key() == Qt::Key_Up ? -1 : k->key() == Qt::Key_PageDown ? 10 : -10;
            mList->setCurrentItem(mList->topLevelItem(std::clamp(row + step, 0, n - 1)));
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mMode == Mode::Scene)
                pick(k->modifiers());
            else if (mMode == Mode::Api)
                (k->modifiers() & Qt::ShiftModifier) ? apiDocs() : apiInsert();
            else if (mMode == Mode::TearOffs)
                placeTearOff();
            else if (k->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))
                openOptions();
            else
                run();
            return true;
        case Qt::Key_Escape: close(); return true;
        default: return false;
        }
    }

private:
    Mode modeAt(int tab) const { return Mode(mTabs->tabData(tab).toInt()); }

    int tabOf(Mode m) const
    {
        for (int i = 0; i < mTabs->count(); ++i)
            if (modeAt(i) == m)
                return i;
        return -1;
    }

    void refill()
    {
        mList->clear();
        if (mMode == Mode::Scene)
            fillScene();
        else if (mMode == Mode::Api)
            fillApi();
        else if (mMode == Mode::TearOffs)
            fillTearOffs();
        else
            fillMenus();
        if (mList->topLevelItemCount())
            mList->setCurrentItem(mList->topLevelItem(0));
    }

    void fillMenus()
    {
        QString q = mEdit->text().trimmed();
        std::vector<std::pair<int, const Item*>> hits;
        for (const Item& it : mItems)
            if (it.action)
                if (int s = score(it.label, it.path, q); s || keywordMatch(it.action, q))
                    hits.emplace_back(-std::max(s, 1), &it);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
        for (auto& [s, it] : hits)
        {
            std::string id = it->action->data().toString().toStdString();
            QString key = id.empty() ? QString() : Hotkeys::get().keyFor(id);
            auto* row = new QTreeWidgetItem(mList, {it->label, it->path, key, QString()});
            row->setData(kLabel, kActionRole, QVariant::fromValue(static_cast<QObject*>(it->action.data())));
            row->setDisabled(!it->action->isEnabled());
            if (it->action->isCheckable())
            {
                // Shown, not clickable: clicking the row triggers the action, which flips it.
                row->setFlags(row->flags() & ~Qt::ItemIsUserCheckable);
                row->setCheckState(kLabel, it->action->isChecked() ? Qt::Checked : Qt::Unchecked);
            }
            row->setForeground(kWhere, dim);
            row->setForeground(kKey, dim);
            if (!id.empty() && optionBox::available(id))
            {
                row->setIcon(kOptions, icon("app-window"));
                row->setToolTip(kOptions, "Options (Shift+Enter, or Shift+click the row)");
            }
        }
    }

    void fillTearOffs()
    {
        QString q = mEdit->text().trimmed();
        std::vector<std::pair<int, size_t>> hits;
        for (size_t i = 0; i < mTearOffs.size(); ++i)
            if (mTearOffs[i].menu)
                if (int s = score(mTearOffs[i].label, mTearOffs[i].path, q); s || keywordMatch(mTearOffs[i].keywords, q))
                    hits.emplace_back(-std::max(s, 1), i);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
        for (auto& [s, i] : hits)
        {
            const TearOffItem& it = mTearOffs[i];
            auto* row = new QTreeWidgetItem(mList, {it.label, it.path});
            row->setData(kLabel, kTearOffRole, qulonglong(i));
            row->setForeground(kWhere, dim);
        }
    }

    void placeTearOff()
    {
        QTreeWidgetItem* row = mList->currentItem();
        size_t i = row ? size_t(row->data(kLabel, kTearOffRole).toULongLong()) : mTearOffs.size();
        if (i >= mTearOffs.size() || !mTearOffs[i].menu)
            return;
        QPointer<QMenu> menu = mTearOffs[i].menu;
        QString section = mTearOffs[i].section;
        close();
        // Once the box has gone, so the next click lands on the placing and not on the box.
        QTimer::singleShot(0, [menu, section] {
            if (menu)
                tearoff::tearOffToPlace(menu, section);
        });
    }

    void fillApi()
    {
        constexpr size_t kMaxRows = 300;
        QString q = mEdit->text().trimmed();
        std::vector<std::pair<int, size_t>> hits;
        for (size_t i = 0; i < mSymbols.size(); ++i)
            if (int s = apiScore(mSymbols[i], q))
                hits.emplace_back(-s, i);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        if (hits.size() > kMaxRows)
            hits.resize(kMaxRows);
        QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
        for (auto& [s, i] : hits)
        {
            const ApiSymbol& sym = mSymbols[i];
            auto* row = new QTreeWidgetItem(mList, {sym.name, sym.summary, sym.kind, QString()});
            row->setData(kLabel, kSymbolRole, qulonglong(i));
            row->setForeground(kWhere, dim);
            row->setForeground(kKey, dim);
            row->setToolTip(kWhere, sym.summary);
            row->setIcon(kOptions, icon("book-2"));
            row->setToolTip(kOptions, "Docs (Shift+Enter, or Shift+click the row). Shift+click the book opens them in the web browser");
        }
    }

    const ApiSymbol* currentSymbol() const
    {
        QTreeWidgetItem* row = mList->currentItem();
        size_t i = row ? size_t(row->data(kLabel, kSymbolRole).toULongLong()) : mSymbols.size();
        return i < mSymbols.size() ? &mSymbols[i] : nullptr;
    }

    void apiInsert()
    {
        const ApiSymbol* sym = currentSymbol();
        if (!sym)
            return;
        QString code = sym->insert;
        auto insert = mTarget.insert;
        close();
        // After the box has closed, so focus is back in the editor that receives the text.
        QTimer::singleShot(0, [insert, code] { insert(code); });
    }

    /** Shift+Enter and Shift+click on the row use Shift to mean docs, so only Shift+click on the book means the browser. */
    void apiDocs(bool inBrowser = false)
    {
        const ApiSymbol* sym = currentSymbol();
        if (!sym || !mTarget.showDocs)
            return;
        QString key = sym->key;
        auto showDocs = mTarget.showDocs;
        close();
        QTimer::singleShot(0, [showDocs, key, inBrowser] {
            docs::RouteScope route(inBrowser ? docs::Route::Browser : docs::Route::Panel);
            showDocs(key);
        });
    }

    /** Snapshot of the hierarchy when the scene mode opens; the scene does not change while the box has focus. */
    void collectScene()
    {
        mScene.clear();
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : outliner::hierarchyNodes(g))
        {
            const scene::Node* n = g.node(id);
            std::string path = g.fullPath(id);
            QString full = QString::fromStdString(path);
            QString name = QString::fromStdString(n->name());
            mScene.push_back({id, name, full.left(full.size() - name.size()), std::move(path)});
        }
    }

    void fillScene()
    {
        constexpr size_t kMaxRows = 500;
        scene::Graph& g = scene::Graph::current();
        outliner::Filter f(mEdit->text().toStdString());
        f.setFuzzy(true);
        std::vector<std::pair<int, const SceneRow*>> hits;
        for (const SceneRow& r : mScene)
            if (const scene::Node* n = g.node(r.id))
                if (int s = f.score(n->name(), r.fullPath, &n->type()))
                    hits.emplace_back(-s, &r);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        if (hits.size() > kMaxRows)
            hits.resize(kMaxRows);
        const std::vector<scene::NodeId>& sel = g.selection();
        QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
        for (auto& [s, r] : hits)
        {
            const scene::Node* n = g.node(r->id);
            auto* row = new QTreeWidgetItem(mList, {r->name, QString::fromStdString(n->type().name)});
            row->setData(kLabel, kNodeRole, QVariant::fromValue<qulonglong>(r->id));
            row->setData(kLabel, kParentPathRole, r->parentPath);
            row->setIcon(kLabel, icon(QString::fromStdString(outliner::iconName(*n))));
            row->setToolTip(kLabel, QString::fromStdString(r->fullPath));
            row->setForeground(kWhere, dim);
            if (std::find(sel.begin(), sel.end(), r->id) != sel.end())
            {
                QFont bold = row->font(kLabel);
                bold.setBold(true);
                row->setFont(kLabel, bold);
            }
        }
    }

    /** Selects the highlighted node: replacing the selection and closing, or with Shift adding, with Ctrl toggling. */
    void pick(Qt::KeyboardModifiers mods)
    {
        QTreeWidgetItem* row = mList->currentItem();
        scene::Node* n = row ? scene::Graph::current().node(row->data(kLabel, kNodeRole).toULongLong()) : nullptr;
        if (!n)
            return;
        json call = {{"args", {n->path()}}};
        bool keepOpen = mods & (Qt::ShiftModifier | Qt::ControlModifier);
        if (mods & Qt::ControlModifier)
            call["toggle"] = true;
        else if (mods & Qt::ShiftModifier)
            call["add"] = true;
        try
        {
            ops::execute("select", call, CallSource::UI);
            achievementRules::sceneSearchSelected();
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        revealSelectionInOutliners();
        if (!keepOpen)
        {
            close();
            return;
        }
        int current = mList->indexOfTopLevelItem(row);
        refill();
        if (QTreeWidgetItem* again = mList->topLevelItem(current))
            mList->setCurrentItem(again);
    }

    QPointer<QAction> currentAction() const
    {
        QTreeWidgetItem* row = mList->currentItem();
        return row ? qobject_cast<QAction*>(row->data(kLabel, kActionRole).value<QObject*>()) : nullptr;
    }

    void run()
    {
        QPointer<QAction> a = currentAction();
        close();
        if (a && a->isEnabled())
        {
            a->trigger();
            achievementRules::commandSearchRan();
        }
    }

    /** The highlighted item's option box; a row without one just runs. */
    void openOptions()
    {
        QPointer<QAction> a = currentAction();
        std::string id = a ? a->data().toString().toStdString() : std::string();
        if (id.empty() || !optionBox::available(id))
            return run();
        close();
        QTimer::singleShot(0, [id] { optionBox::open(id); });
    }

    QPointer<QMenuBar> mBar;
    Mode mMode = Mode::Menus;
    Mode mHome = Mode::Menus;
    ApiTarget mTarget;
    std::vector<ApiSymbol> mSymbols;
    QTabBar* mTabs = nullptr;
    bool mStarted = false;
    std::vector<Item> mItems;
    std::vector<SceneRow> mScene;
    std::vector<TearOffItem> mTearOffs;
    QLineEdit* mEdit = nullptr;
    QTreeWidget* mList = nullptr;
    QLabel* mHint = nullptr;
    PathDelegate* mPathDelegate = nullptr;
};

void openIn(Mode m, QMenuBar* bar, QWidget* over, ApiTarget target = {})
{
    QPointer<SearchBox>& box = openBoxInstance();
    if (box && box->isVisible())
    {
        if (target.insert)
            box->setApiTarget(std::move(target));
        if (box->mode() == m)
            box->close();
        else
            box->setMode(m);
        return;
    }
    box = new SearchBox(bar, over);
    if (target.insert)
        box->setApiTarget(std::move(target));
    box->start(m);
}
} // namespace

void install()
{
    static QPointer<TypeToFilter> f;
    if (!f)
    {
        f = new TypeToFilter(qApp);
        qApp->installEventFilter(f);
    }
}

void openBox(QMenuBar* bar, QWidget* over)
{
    openIn(Mode::Menus, bar, over);
}

void openSceneFinder(QMenuBar* bar, QWidget* over)
{
    openIn(Mode::Scene, bar, over);
}

void openTearOffSearch(QMenuBar* bar, QWidget* over)
{
    openIn(Mode::TearOffs, bar, over);
}

void openApiSearch(QMenuBar* bar, QWidget* over, ApiTarget target)
{
    openIn(Mode::Api, bar, over, std::move(target));
}
} // namespace lila::ui::menuSearch
