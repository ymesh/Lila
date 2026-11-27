#include "ui/Outliner.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "io/References.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/Light.h"
#include "scene/ObjectSets.h"
#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/IconBars.h"
#include "ui/Icons.h"
#include "ui/Issues.h"
#include "ui/OutlinerLogic.h"
#include "ui/Theme.h"
#include "ui/TokenCompleter.h"
#include "ui/UiCommands.h"
#include "ui/ViewportToolbar.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDrag>
#include <QMimeData>
#include <QDropEvent>
#include <QHash>
#include <QHeaderView>
#include <QKeySequence>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace lila::ui
{
namespace
{
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kStyleRole = Qt::UserRole + 2; // last applied style, so restyle only touches rows that changed
constexpr int kUnloadedRefRole = Qt::UserRole + 3; // namespace of an unloaded reference's placeholder row
// Set rows carry the set's id here rather than in kIdRole, so clicking one selects its members, not the set node.
constexpr int kSetRole = Qt::UserRole + 4;
constexpr int kSetMemberRole = Qt::UserRole + 5; // a member row's member string, as setMembers lists it
constexpr int kSourceRole = Qt::UserRole + 6;    // the outliner::RowSource id of a group row and its rows
constexpr int kSourceRowRole = Qt::UserRole + 7; // the ExtraRow id of a source's row
constexpr int kVisRole = Qt::UserRole + 8;       // RowVis: whether the row has an eye and how it reads
constexpr int kIssueRole = Qt::UserRole + 9;     // worst own issue: severity * 2 + blocking, -1 for none
constexpr int kIssueCountRole = Qt::UserRole + 10;

enum RowVis
{
    kNoEye = 0,
    kShown,
    kHiddenSelf,
    kHiddenAbove,
};

constexpr int kOutlinerRowHeight = 24;
constexpr int kEyeSize = 16;
constexpr int kEyePad = 6;

/** The visibility eye's hit and draw area in a row's rect. */
QRect outlinerEyeRect(const QRect& row)
{
    return QRect(row.right() - kEyePad - kEyeSize + 1, row.top() + (row.height() - kEyeSize) / 2, kEyeSize, kEyeSize);
}

/** Rows 24 px tall; the issue badge with its count, then the visibility eye, at the row's right edge. */
class OutlinerRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(kOutlinerRowHeight);
        return s;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const int vis = index.data(kVisRole).toInt();
        const int issue = index.data(kIssueRole).isValid() ? index.data(kIssueRole).toInt() : -1;
        if (vis == kNoEye && issue < 0)
            return QStyledItemDelegate::paint(painter, option, index);

        QStyleOptionViewItem full(option);
        initStyleOption(&full, index);
        const QWidget* w = option.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        // The row's fill spans the trailing marks too; the name is laid out short of them so it elides first.
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &full, painter, w);
        const QRect eye = outlinerEyeRect(option.rect);
        int right = vis != kNoEye ? eye.left() - 4 : option.rect.right() - kEyePad;
        QString count;
        QRect badge;
        const QFontMetrics fm(option.font);
        if (issue >= 0)
        {
            const int n = index.data(kIssueCountRole).toInt();
            count = n > 1 ? QString::number(n) : QString();
            const int textW = count.isEmpty() ? 0 : fm.horizontalAdvance(count) + 3;
            badge = QRect(right - textW - 12 + 1, option.rect.top() + (option.rect.height() - 12) / 2, 12, 12);
            right = badge.left() - 6;
        }
        QStyleOptionViewItem opt(option);
        opt.rect.setRight(right);
        QStyledItemDelegate::paint(painter, opt, index);

        painter->save();
        if (issue >= 0)
        {
            const auto sev = scene::diagnostics::Severity(issue / 2);
            const qreal dpr = painter->device()->devicePixelRatioF();
            QPixmap pm = issues::badge(sev, issue % 2 != 0, int(std::lround(12 * dpr)));
            pm.setDevicePixelRatio(dpr);
            painter->drawPixmap(badge.topLeft(), pm);
            if (!count.isEmpty())
            {
                painter->setPen(issues::color(sev));
                painter->setFont(option.font);
                painter->drawText(QRect(badge.right() + 4, option.rect.top(), fm.horizontalAdvance(count) + 2, option.rect.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, count);
            }
        }
        if (vis != kNoEye)
        {
            const bool selected = option.state & QStyle::State_Selected;
            const QIcon::Mode mode = vis == kShown ? (selected ? QIcon::Active : QIcon::Normal) : QIcon::Disabled;
            icon(vis == kHiddenSelf ? "eye-off" : "eye").paint(painter, eye, Qt::AlignCenter, mode);
        }
        painter->restore();
    }
};

std::vector<outliner::RowSource>& rowSources()
{
    static std::vector<outliner::RowSource> s;
    return s;
}

const outliner::RowSource* rowSource(const QTreeWidgetItem* item)
{
    if (!item || !item->data(0, kSourceRole).isValid())
        return nullptr;
    const std::string id = item->data(0, kSourceRole).toString().toStdString();
    for (const outliner::RowSource& src : rowSources())
        if (src.id == id)
            return &src;
    return nullptr;
}

std::vector<Outliner*> gOutliners;

/** The outliner under the cursor, else the first visible one. */
Outliner* activeOutliner()
{
    for (QWidget* w = QApplication::widgetAt(QCursor::pos()); w; w = w->parentWidget())
        if (auto* o = qobject_cast<Outliner*>(w))
            return o;
    for (Outliner* o : gOutliners)
        if (o->isVisible())
            return o;
    return nullptr;
}

/** How a row's issues show on its icon: its own worst, and below it the worst of a collapsed row's descendants. */
struct RowIssues
{
    std::optional<issues::Worst> own;
    std::optional<scene::diagnostics::Severity> below;

    QString key() const
    {
        QString k;
        if (own)
            k += QString("/e%1%2").arg(int(own->severity)).arg(own->blocking ? "b" : "");
        if (below)
            k += QString("/u%1").arg(int(*below));
        return k;
    }
};

/**
 * A type icon, dimmed for hidden rows, with a chain-link badge in the corner for referenced ones. Slashed rows get a
 * diagonal line through the glyph, as Tabler's "-off" icons have. An issue badge sits bottom left; a dot top right
 * says a collapsed row hides issues.
 */
QIcon rowIcon(const QString& name, bool referenced, bool dim, bool slashed = false, const RowIssues& marks = {})
{
    static QHash<QString, QIcon> cache;
    QString key = name + (referenced ? "/r" : "") + (dim ? "/d" : "") + (slashed ? "/s" : "") + marks.key();
    if (auto it = cache.find(key); it != cache.end())
        return *it;
    QIcon base = icon(name);
    if (base.isNull())
        base = icon("point");
    QIcon::Mode mode = dim ? QIcon::Disabled : QIcon::Normal;
    QIcon out;
    for (int size : {16, 32})
    {
        QPixmap pm = base.pixmap(QSize(size, size), mode);
        if (slashed)
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            const double stroke = size / 12.0;
            const QLineF line(size * 0.125, size * 0.125, size * 0.875, size * 0.875);
            // A gap cut either side keeps the line readable where it crosses the glyph.
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.setPen(QPen(Qt::transparent, stroke * 3.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(line);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setPen(QPen(themeColor(dim ? "iconDisabled" : "iconNormal"), stroke, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(line);
        }
        if (referenced)
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            int b = size * 9 / 16;
            QRect badge(size - b, size - b, b, b);
            p.setPen(Qt::NoPen);
            p.setBrush(themeShade(40));
            p.drawEllipse(badge);
            p.drawPixmap(badge.adjusted(1, 1, -1, -1), icon("link").pixmap(QSize(b - 2, b - 2), QIcon::Active));
        }
        if (marks.own)
        {
            QPainter p(&pm);
            const int b = size * 10 / 16;
            p.drawPixmap(0, size - b, issues::badge(marks.own->severity, marks.own->blocking, b));
        }
        if (marks.below)
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            const int d = size * 6 / 16;
            p.setPen(QPen(themeShade(40), size / 16.0));
            p.setBrush(issues::color(*marks.below));
            p.drawEllipse(QRectF(size - d - 0.5, 0.5, d, d));
        }
        out.addPixmap(pm);
    }
    cache.insert(key, out);
    return out;
}

/** "|group1|pCube1", Maya's full DAG path. */
std::string dagPath(scene::Graph& g, scene::NodeId id)
{
    std::string out;
    for (scene::NodeId p = id; p != scene::kNoNode; p = g.parent(p))
        if (const scene::Node* n = g.node(p))
            out = "|" + n->name() + out;
    return out;
}

std::vector<scene::NodeId> selectedIds()
{
    return scene::Graph::current().selection();
}

json namesOf(const std::vector<scene::NodeId>& ids)
{
    json names = json::array();
    for (scene::NodeId id : ids)
        if (scene::Node* n = scene::Graph::current().node(id))
            names.push_back(n->path());
    return names;
}

constexpr const char* kShowSetsPref = "/outliner/showSets";

bool showSets()
{
    return PrefStore::get().value<bool>("general", kShowSetsPref, true);
}

constexpr const char* kFlagLightsPref = "/outliner/flagPlaceholderLights";

bool flagPlaceholderLights()
{
    return PrefStore::get().value<bool>("general", kFlagLightsPref, true);
}

void restyleOutliners()
{
    for (Outliner* o : gOutliners)
        o->scheduleRestyle();
}

void copyToClipboard(bool fullPath)
{
    scene::Graph& g = scene::Graph::current();
    QStringList lines;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
            lines << QString::fromStdString(fullPath ? dagPath(g, id) : n->name());
    if (!lines.isEmpty())
        QApplication::clipboard()->setText(lines.join('\n'));
}

void registerOutlinerActions()
{
    if (actions::find("outliner.revealSelected"))
        return;
    auto onOutliner = [](void (Outliner::*fn)()) {
        return [fn] {
            if (Outliner* o = activeOutliner())
                (o->*fn)();
        };
    };
    actions::add({"outliner.revealSelected", "Reveal Selected", "Outliner", onOutliner(&Outliner::revealSelected), nullptr,
                  "Expands the outliner down to the selected objects and scrolls to them (F over the outliner)."});
    actions::add({"outliner.rename", "Rename", "Outliner", onOutliner(&Outliner::renameSelected), nullptr,
                  "Edits the selected object's name in place in the outliner."});
    actions::add({"outliner.expandAll", "Expand All", "Outliner", onOutliner(&Outliner::expandEverything), nullptr,
                  "Opens every row of the outliner."});
    actions::add({"outliner.collapseAll", "Collapse All", "Outliner", onOutliner(&Outliner::collapseEverything), nullptr,
                  "Folds the outliner down to the top-level objects."});
    actions::add({.id = "outliner.toggleSets", .label = "Show Sets", .category = "Outliner",
                  .run =
                      [] {
                          PrefStore::get().set("general", kShowSetsPref, !showSets());
                          for (Outliner* o : gOutliners)
                              o->scheduleRebuild();
                      },
                  .doc = "Lists object sets after the hierarchy, each with its members under it.",
                  .python = "lila.prefs.set(\"general\", \"/outliner/showSets\", False)",
                  .checked = [] { return showSets(); }});
    actions::add({.id = "outliner.toggleFlagPlaceholderLights", .label = "Flag Placeholder Lights", .category = "Outliner",
                  .run = [] { PrefStore::get().set("general", kFlagLightsPref, !flagPlaceholderLights()); },
                  .doc = "Strikes through the icon of lights no light extension renders.",
                  .python = "lila.prefs.set(\"general\", \"/outliner/flagPlaceholderLights\", False)",
                  .checked = [] { return flagPlaceholderLights(); }});
    // Rows are keyed on the flag, so a restyle only touches what changed.
    PrefStore::get().onChanged([](const std::string& file) {
        if (file == "general")
            QTimer::singleShot(0, qApp, restyleOutliners);
    });
    scene::lights::providers().addListener([](RegistryChange, const std::string&, const std::string&) { restyleOutliners(); });
    scene::diagnostics::addListener(restyleOutliners);
    actions::add({"outliner.copyName", "Copy Name", "Outliner", [] { copyToClipboard(false); }, nullptr,
                  "Puts the selected objects' names on the clipboard, one per line."});
    actions::add({"outliner.copyPath", "Copy Path", "Outliner", [] { copyToClipboard(true); }, nullptr,
                  "Puts the selected objects' full DAG paths (|group|object) on the clipboard, one per line."});
    actions::add({"select.children", "Select Children", "Select", [] {
                      scene::Graph& g = scene::Graph::current();
                      std::vector<scene::NodeId> kids;
                      for (scene::NodeId id : g.selection())
                          for (scene::NodeId c : g.children(id))
                              if (std::find(kids.begin(), kids.end(), c) == kids.end())
                                  kids.push_back(c);
                      if (!kids.empty())
                          runUi("select", {{"args", namesOf(kids)}});
                  }, nullptr, "Replaces the selection with the direct children of the selected objects.",
                  "select(listRelatives(children=True))"});

    using iconBars::Kind;
    const std::string bar = iconBars::kOutliner;
    iconBars::add({.id = "outliner/rename", .bar = bar, .action = "window.rename", .icon = "forms", .label = "Batch Rename"});
    iconBars::add({.id = "outliner/naming", .bar = bar, .kind = Kind::Menu, .icon = "text-spellcheck", .label = "Naming Conventions",
                   .menuActions = {"edit.checkNames", "edit.conformNames", "", "naming.toggleEnforce"}});
    iconBars::add({.id = "outliner/-view", .bar = bar, .kind = Kind::Separator});
    iconBars::add({.id = "outliner/reveal", .bar = bar, .action = "outliner.revealSelected", .icon = "focus-2"});
    iconBars::add({.id = "outliner/expand", .bar = bar, .action = "outliner.expandAll", .icon = "fold-down"});
    iconBars::add({.id = "outliner/collapse", .bar = bar, .action = "outliner.collapseAll", .icon = "fold-up"});
    iconBars::add({.id = "outliner/-copy", .bar = bar, .kind = Kind::Separator});
    iconBars::add({.id = "outliner/copy", .bar = bar, .kind = Kind::Menu, .icon = "copy", .label = "Copy",
                   .menuActions = {"outliner.copyName", "outliner.copyPath"}});
}

/** Adds an action's menu item, labelled as the action (or label) with its hotkey, running it through run. */
QAction* addItem(QMenu* menu, const std::string& id, const std::function<void(const json&)>& run, const QString& label = {})
{
    const Action* a = actions::find(id);
    if (!a)
        return nullptr;
    QString text = label.isEmpty() ? QString::fromStdString(a->label) : label;
    QString key = Hotkeys::get().keyFor(id);
    QAction* item = menu->addAction(key.isEmpty() ? text : text + "\t" + key, [run, id] { run(json(id)); });
    item->setToolTip(QString::fromStdString(a->doc));
    return item;
}

/** The label that follows the cursor while rows are moved: the first name, and how many more. */
QPixmap movePixmap(const QStringList& names, const QFont& font, const QPalette& pal, qreal dpr)
{
    QString text = names.size() == 1 ? names.front() : QString("%1 +%2").arg(names.front()).arg(names.size() - 1);
    QFontMetrics fm(font);
    QSize size(fm.horizontalAdvance(text) + 12, fm.height() + 6);
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Highlight));
    p.drawRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), 3, 3);
    p.setFont(font);
    p.setPen(pal.color(QPalette::HighlightedText));
    p.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, text);
    return pm;
}

const io::Reference* referenceById(uint32_t id)
{
    for (const io::Reference& r : io::refs::all())
        if (r.id == id)
            return &r;
    return nullptr;
}

void addReferenceItems(QMenu* menu, const std::string& ns, bool loaded)
{
    json args = {{"args", {ns}}};
    auto add = [&](const char* label, const char* command) {
        menu->addAction(label, [command, args] { actions::run(json{{"command", command}, {"args", args}}); });
    };
    if (loaded)
    {
        add("Reload Reference", "reloadReference");
        add("Unload Reference", "unloadReference");
        add("Import Objects from Reference", "importReference");
    }
    else
        add("Load Reference", "loadReference");
    menu->addSeparator();
    if (const Action* a = actions::find("window.referenceEditor"))
        menu->addAction(QString::fromStdString(a->label), [] { actions::run(json("window.referenceEditor")); });
}
} // namespace

Outliner::Outliner(QWidget* parent) : QTreeWidget(parent)
{
    gOutliners.push_back(this);
    registerOutlinerActions();
    for (events::Event e : {events::Event::SceneNew, events::Event::SceneOpened})
        mEventSubs.push_back(events::subscribe(e, [this](const events::EventData&) { mExpanded.clear(); }));

    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setUniformRowHeights(true);
    setIconSize(QSize(16, 16));
    setItemDelegate(new OutlinerRowDelegate(this));
    setMouseTracking(true);
    setProperty("pickWalk", true);
    setProperty("hotkeyContext", "outliner");
    setProperty("hotkeyHover", true);
    deletion::claimScene(this);
    Hotkeys::passThrough(this);
    // Drops land through dropEvent; drags start only from MMB (startMove), so an LMB drag selects instead.
    setDragDropMode(QAbstractItemView::InternalMove);
    setDragEnabled(false);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &Outliner::showContextMenu);

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId id) {
        const scene::Node* n = scene::Graph::current().node(id);
        if (c == scene::Change::Value && n && scene::objectSets::isSet(*n))
            scheduleRebuild();
        else if (c == scene::Change::Value || c == scene::Change::Connection)
            scheduleRowRestyle(id);
        else if (c == scene::Change::Selection)
        {
            if (ops::currentArgs())
                mSelectionStale = true;
            else
                syncSelectionFromScene();
        }
        else
            scheduleRebuild();
    });
    mExecListener = ops::addExecuteListener([this](const ExecuteEvent& e) {
        if (e.done && std::exchange(mSelectionStale, false))
            syncSelectionFromScene();
    });

    connect(this, &QTreeWidget::itemSelectionChanged, this, &Outliner::pushSelectionToScene);
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (mSyncing)
            return;
        if (const outliner::RowSource* src = rowSource(item); src && item->data(0, kSourceRowRole).isValid())
        {
            if (src->rename && !item->text(0).trimmed().isEmpty())
                src->rename(item->data(0, kSourceRowRole).toString().toStdString(), item->text(0).trimmed().toStdString());
            return scheduleRebuild();
        }
        auto id = item->data(0, kIdRole).toULongLong();
        if (!id)
            id = item->data(0, kSetRole).toULongLong();
        scene::Node* n = scene::Graph::current().node(id);
        std::string to = item->text(0).trimmed().toStdString();
        if (!n || to == n->name())
            return;
        if (to.empty())
            return scheduleRebuild();
        try
        {
            ops::execute("rename", {{"args", {n->path(), to}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            scheduleRebuild();
        }
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (mSyncing)
            return;
        if (item->data(0, kSourceRole).isValid() && !item->data(0, kSourceRowRole).isValid())
            mOpenSources.erase(item->data(0, kSourceRole).toString().toStdString());
        else if (scene::NodeId set = item->data(0, kSetRole).toULongLong())
            mOpenSets.erase(set);
        else
            mExpanded.erase(item->data(0, kIdRole).toULongLong());
        scheduleRestyle();
    });
    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (mSyncing)
            return;
        if (item->data(0, kSourceRole).isValid() && !item->data(0, kSourceRowRole).isValid())
            mOpenSources.insert(item->data(0, kSourceRole).toString().toStdString());
        else if (scene::NodeId set = item->data(0, kSetRole).toULongLong())
            mOpenSets.insert(set);
        else
            mExpanded.insert(item->data(0, kIdRole).toULongLong());
        scheduleRestyle();
    });
    connect(this, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* item, int) {
        if (const outliner::RowSource* src = rowSource(item); src && src->activate && item->data(0, kSourceRowRole).isValid())
            return src->activate(item->data(0, kSourceRowRole).toString().toStdString());
        if (scene::Node* set = scene::Graph::current().node(item->data(0, kSetRole).toULongLong()))
            runUi("selectSetMembers", {{"args", {set->path()}}});
    });

    rebuild();
}

Outliner::~Outliner()
{
    std::erase(gOutliners, this);
    for (events::SubscriptionId id : mEventSubs)
        events::unsubscribe(id);
    scene::Graph::current().removeListener(mListener);
    ops::removeExecuteListener(mExecListener);
}

void Outliner::dropEvent(QDropEvent* e)
{
    e->setDropAction(Qt::IgnoreAction);
    e->accept();
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> moved = mMoving;
    std::erase_if(moved, [&](scene::NodeId id) { return !g.node(id); });
    if (moved.empty())
        return;

    QTreeWidgetItem* target = itemAt(e->position().toPoint());
    if (target && target->data(0, kSetRole).isValid())
    {
        if (scene::Node* set = g.node(target->data(0, kSetRole).toULongLong()))
        {
            json args = namesOf(moved);
            args.insert(args.begin(), set->path());
            runUi("addToSet", {{"args", args}});
        }
        return;
    }
    scene::NodeId targetId = target ? scene::NodeId(target->data(0, kIdRole).toULongLong()) : scene::kNoNode;
    DropIndicatorPosition where = targetId != scene::kNoNode ? dropIndicatorPosition() : OnViewport;
    scene::NodeId parent = scene::kNoNode;
    scene::NodeId anchor = scene::kNoNode; // the sibling the moved rows go in front of; none = the end
    bool place = true;
    if (where == OnItem)
    {
        if (std::find(moved.begin(), moved.end(), targetId) != moved.end())
            return;
        parent = targetId;
        place = false;
    }
    else if (where == AboveItem)
    {
        parent = g.parent(targetId);
        anchor = targetId;
    }
    else if (where == BelowItem && target->isExpanded() && !g.children(targetId).empty())
    {
        // The line sits between the row and its first child.
        parent = targetId;
        anchor = g.children(targetId).front();
    }
    else if (where == BelowItem)
    {
        parent = g.parent(targetId);
        const auto& sib = parent == scene::kNoNode ? g.roots() : g.children(parent);
        auto it = std::find(sib.begin(), sib.end(), targetId);
        if (it != sib.end() && it + 1 != sib.end())
            anchor = *(it + 1);
    }

    json call = {{"args", namesOf(moved)}};
    if (parent != scene::kNoNode)
        call["args"].push_back(g.node(parent)->path());
    else
        call["world"] = true;
    if (place)
        call["index"] = outliner::dropIndex(parent == scene::kNoNode ? g.roots() : g.children(parent), moved, anchor);
    runUi("parent", call);
    scheduleRebuild();
}

void Outliner::keyPressEvent(QKeyEvent* e)
{
    QWidget* editor = QApplication::focusWidget();
    // Enter commits the edit rather than cancelling it.
    bool renameKey = (Hotkeys::get().isKeyFor(e, "outliner.rename") || Hotkeys::get().isKeyFor(e, "edit.rename")) &&
                     e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter;
    if (state() == EditingState && renameKey && editor && viewport()->isAncestorOf(editor))
    {
        closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
        setFocus();
        return;
    }
    QTreeWidget::keyPressEvent(e);
}

bool Outliner::eyeAt(const QPoint& pos, bool toggle)
{
    QTreeWidgetItem* item = itemAt(pos);
    if (!item || item->data(0, kVisRole).toInt() == kNoEye || !outlinerEyeRect(visualItemRect(item)).contains(pos))
        return false;
    if (toggle)
        if (scene::Node* n = scene::Graph::current().node(item->data(0, kIdRole).toULongLong()))
            runUi("toggleVisibility", {{"args", {n->path()}}});
    return true;
}

void Outliner::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && eyeAt(e->position().toPoint(), false))
        return e->accept();
    QTreeWidget::mouseDoubleClickEvent(e);
}

void Outliner::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && eyeAt(e->position().toPoint(), true))
        return e->accept();
    if (e->button() == Qt::MiddleButton)
    {
        mMoving.clear();
        mMovePress = e->position().toPoint();
        if (QTreeWidgetItem* item = itemAt(mMovePress))
            if (scene::NodeId id = item->data(0, kIdRole).toULongLong())
                mMoving = outliner::menuTargets(id, selectedIds());
        e->accept();
        return;
    }
    QTreeWidget::mousePressEvent(e);
    mDragSelecting = e->button() == Qt::LeftButton;
}

void Outliner::mouseMoveEvent(QMouseEvent* e)
{
    if (!mMoving.empty() && (e->buttons() & Qt::MiddleButton))
    {
        if ((e->position().toPoint() - mMovePress).manhattanLength() >= QApplication::startDragDistance())
            startMove();
        e->accept();
        return;
    }
    QTreeWidget::mouseMoveEvent(e);
}

void Outliner::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton)
    {
        mMoving.clear();
        e->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton)
    {
        mDragSelecting = false;
        if (mSelectionPending)
        {
            mSelectionPending = false;
            pushSelectionToScene();
        }
    }
}

void Outliner::startMove()
{
    // Top-to-bottom row order, so the rows land in the order they were shown.
    std::vector<scene::NodeId> rows;
    QModelIndexList indexes;
    QStringList names;
    for (QTreeWidgetItemIterator it(this); *it; ++it)
        if (scene::NodeId id = (*it)->data(0, kIdRole).toULongLong();
            id && std::find(mMoving.begin(), mMoving.end(), id) != mMoving.end())
        {
            rows.push_back(id);
            indexes << indexFromItem(*it);
            names << (*it)->text(0);
        }
    mMoving = rows;
    if (rows.empty())
        return;
    auto* drag = new QDrag(this);
    QMimeData* mime = model()->mimeData(indexes);
    QStringList nodeNames;
    for (scene::NodeId id : rows)
        if (scene::Node* n = scene::Graph::current().node(id))
            nodeNames << QString::fromStdString(n->path());
    mime->setData(kNodeNamesMime, nodeNames.join(QChar('\n')).toUtf8());
    drag->setMimeData(mime);
    drag->setPixmap(movePixmap(names, font(), palette(), devicePixelRatioF()));
    drag->setHotSpot(QPoint(-12, -4));
    drag->exec(Qt::MoveAction | Qt::LinkAction, Qt::MoveAction);
    mMoving.clear();
}

void Outliner::scheduleRebuild()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        if (height() <= 0)
            mStale = true;
        else
            rebuild();
    });
}

void Outliner::resizeEvent(QResizeEvent* e)
{
    QTreeWidget::resizeEvent(e);
    if (mStale && height() > 0)
    {
        mStale = false;
        rebuild();
    }
}

void Outliner::scheduleRestyle()
{
    if (mRestyleQueued || mQueued)
        return;
    mRestyleQueued = true;
    QTimer::singleShot(0, this, [this] {
        mRestyleQueued = false;
        mDirtyRows.clear();
        restyle();
    });
}

void Outliner::scheduleRowRestyle(scene::NodeId id)
{
    if (mRestyleQueued || mQueued)
        return;
    // A layer or other node outside the hierarchy can change rows anywhere.
    if (const scene::Node* n = scene::Graph::current().node(id); !n || !n->type().dag)
        return scheduleRestyle();
    const bool first = mDirtyRows.empty();
    mDirtyRows.insert(id);
    if (!first)
        return;
    QTimer::singleShot(0, this, [this] {
        if (mDirtyRows.empty())
            return;
        std::unordered_set<scene::NodeId> rows = std::move(mDirtyRows);
        mDirtyRows.clear();
        restyle(&rows);
    });
}

void Outliner::rebuild()
{
    LILA_ZONE();

    mStale = false;
    mSyncing = true;
    scene::Graph& g = scene::Graph::current();
    const int scroll = verticalScrollBar()->value();
    // Selected rows that moved to another parent get revealed once the tree is back.
    std::vector<scene::NodeId> moved;
    for (scene::NodeId id : g.selection())
        if (auto it = mItems.find(id); it != mItems.end())
        {
            QTreeWidgetItem* was = it->second->parent();
            const scene::NodeId oldParent = was ? scene::NodeId(was->data(0, kIdRole).toULongLong()) : scene::kNoNode;
            if (g.node(id) && g.parent(id) != oldParent)
                moved.push_back(id);
        }
    clear();
    mItems.clear();
    std::function<void(scene::NodeId, QTreeWidgetItem*)> add = [&](scene::NodeId id, QTreeWidgetItem* parent) {
        scene::Node* n = g.node(id);
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
        item->setText(0, QString::fromStdString(n->name()));
        item->setData(0, kIdRole, QVariant::fromValue<qulonglong>(id));
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        // Referenced names can't be edited.
        if (n->referenceId())
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        const std::string& doc = n->type().doc;
        item->setToolTip(0, QString::fromStdString(g.fullPath(id) + "\n" + n->type().name + (doc.empty() ? "" : "\n" + doc)));
        mItems[id] = item;
        for (scene::NodeId c : g.children(id))
            add(c, item);
    };
    for (scene::NodeId id : g.roots())
        add(id, nullptr);
    if (showSets())
        addSetRows();
    addSourceRows();
    // An unloaded reference has no nodes, so it gets a placeholder row to load it back from.
    for (const io::Reference& r : io::refs::all())
        if (!r.loaded)
        {
            auto* item = new QTreeWidgetItem(this);
            item->setText(0, QString::fromStdString(r.ns + " (unloaded)"));
            item->setData(0, kUnloadedRefRole, QString::fromStdString(r.ns));
            item->setFlags(Qt::ItemIsEnabled);
            item->setIcon(0, rowIcon("link-off", false, true));
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, themeShade(110));
            item->setToolTip(0, QString::fromStdString("Unloaded reference\n" + r.path.generic_string()));
        }
    collapseAll();
    std::erase_if(mExpanded, [&](scene::NodeId id) { return !mItems.contains(id); });
    for (scene::NodeId id : mExpanded)
        mItems[id]->setExpanded(true);
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (QTreeWidgetItem* top = topLevelItem(i); top->data(0, kSetRole).isValid())
            top->setExpanded(mOpenSets.contains(top->data(0, kSetRole).toULongLong()));
        else if (top->data(0, kSourceRole).isValid())
            top->setExpanded(mOpenSources.contains(top->data(0, kSourceRole).toString().toStdString()));
    applyFilter();
    mSyncing = false;
    restyle();
    syncSelectionFromScene();
    verticalScrollBar()->setValue(scroll);
    if (!moved.empty())
        reveal(moved);
}

void Outliner::addSetRows()
{
    scene::Graph& g = scene::Graph::current();
    std::erase_if(mOpenSets, [&](scene::NodeId id) { return !g.node(id); });
    for (const scene::Node* s : scene::objectSets::all(g))
    {
        const bool referenced = s->referenceId() != 0;
        const bool quick = s->as<bool>(scene::ObjectSet::kQuickSelect);
        auto* item = new QTreeWidgetItem(this);
        item->setText(0, QString::fromStdString(s->name()));
        item->setData(0, kSetRole, QVariant::fromValue<qulonglong>(s->id()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled | (referenced ? Qt::ItemFlags() : Qt::ItemIsEditable));
        item->setIcon(0, rowIcon(quick ? "pointer" : "list-details", referenced, false));
        QFont f = item->font(0);
        f.setItalic(referenced);
        item->setFont(0, f);
        item->setToolTip(0, QString::fromStdString(std::string(quick ? "Quick select set" : "Object set") +
                                                   "\nClick to select its members; drop rows on it to add them."));
        for (const std::string& m : scene::objectSets::format(g, scene::objectSets::members(g, s->id())))
        {
            auto* row = new QTreeWidgetItem(item);
            row->setText(0, QString::fromStdString(m));
            row->setData(0, kSetMemberRole, QString::fromStdString(m));
            row->setFlags(Qt::ItemIsEnabled);
            row->setForeground(0, themeShade(150));
        }
    }
}

void Outliner::addSourceRows()
{
    for (const outliner::RowSource& src : rowSources())
    {
        const std::vector<outliner::ExtraRow> rows = src.rows ? src.rows() : std::vector<outliner::ExtraRow>{};
        if (rows.empty())
            continue;
        auto* group = new QTreeWidgetItem(this);
        group->setText(0, QString::fromStdString(src.title));
        group->setData(0, kSourceRole, QString::fromStdString(src.id));
        group->setFlags(Qt::ItemIsEnabled);
        group->setIcon(0, rowIcon(QString::fromStdString(src.icon), false, false));
        for (const outliner::ExtraRow& r : rows)
        {
            auto* row = new QTreeWidgetItem(group);
            row->setText(0, QString::fromStdString(r.label));
            row->setData(0, kSourceRole, QString::fromStdString(src.id));
            row->setData(0, kSourceRowRole, QString::fromStdString(r.id));
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (src.rename ? Qt::ItemIsEditable : Qt::ItemFlags()));
            row->setIcon(0, rowIcon(QString::fromStdString(r.icon.empty() ? src.icon : r.icon), false, r.hidden));
            row->setToolTip(0, QString::fromStdString(r.tooltip));
            if (r.hidden)
                row->setForeground(0, themeShade(100));
        }
    }
}

void Outliner::showSetMenu(QTreeWidgetItem* item, const QPoint& globalPos)
{
    scene::Graph& g = scene::Graph::current();
    QTreeWidgetItem* setItem = item->data(0, kSetRole).isValid() ? item : item->parent();
    scene::Node* set = setItem ? g.node(setItem->data(0, kSetRole).toULongLong()) : nullptr;
    if (!set)
        return;
    const std::string name = set->path();
    const bool editable = set->referenceId() == 0;
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    if (item->data(0, kSetMemberRole).isValid())
    {
        const std::string member = item->data(0, kSetMemberRole).toString().toStdString();
        menu.addAction(icon("pointer"), "Select Member", [member] { runUi("select", {{"args", {member.substr(0, member.find('.'))}}}); });
        menu.addAction(icon("minus"), "Remove from Set", [name, member] { runUi("removeFromSet", {{"args", {name, member}}}); });
        menu.exec(globalPos);
        return;
    }
    menu.addAction(icon("pointer"), "Select Set Members", [name] { runUi("selectSetMembers", {{"args", {name}}}); });
    menu.addAction("Add Selection to Set", [name] { runUi("addToSet", {{"args", {name}}}); });
    menu.addAction("Remove Selection from Set", [name] { runUi("removeFromSet", {{"args", {name}}}); });
    menu.addSeparator();
    QAction* quick = menu.addAction("Quick Select Set", [name, on = !set->as<bool>(scene::ObjectSet::kQuickSelect)] {
        runUi("setAttr", {{"args", {name + ".quickSelect", on}}});
    });
    quick->setCheckable(true);
    quick->setChecked(set->as<bool>(scene::ObjectSet::kQuickSelect));
    quick->setToolTip("List it under Edit > Quick Select Sets.");
    menu.addAction(icon("pencil"), "Rename", [this, setItem] { editItem(setItem, 0); })->setEnabled(editable);
    menu.addAction(icon("trash"), "Delete Set", [name] { runUi("deleteSet", {{"args", {name}}}); });
    menu.exec(globalPos);
}

void Outliner::restyle(const std::unordered_set<scene::NodeId>* only)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    const QColor normal = palette().color(QPalette::Text);
    const bool flagLights = flagPlaceholderLights();
    namespace dx = scene::diagnostics;
    const bool showInfo = issues::badgeInfo();
    const std::optional<dx::Severity> tint = issues::tintFrom();
    std::unordered_map<scene::NodeId, std::vector<const dx::Issue*>> own;
    std::unordered_map<scene::NodeId, dx::Severity> below;
    for (const dx::Issue& i : dx::issues())
    {
        if (i.node == scene::kNoNode || (!showInfo && i.severity == dx::Severity::Info))
            continue;
        own[i.node].push_back(&i);
        for (scene::NodeId p = g.parent(i.node); p != scene::kNoNode; p = g.parent(p))
            if (auto [it, added] = below.try_emplace(p, i.severity); !added)
                it->second = std::max(it->second, i.severity);
    }
    // The rows to style: those that can be seen, all of them or under the edited nodes.
    std::vector<std::pair<scene::NodeId, QTreeWidgetItem*>> rows;
    std::vector<QTreeWidgetItem*> stack;
    auto shownChildren = [&](QTreeWidgetItem* item) {
        if (item->isExpanded())
            for (int i = 0; i < item->childCount(); ++i)
                stack.push_back(item->child(i));
    };
    auto seen = [](QTreeWidgetItem* item) {
        for (QTreeWidgetItem* p = item->parent(); p; p = p->parent())
            if (!p->isExpanded())
                return false;
        return true;
    };
    if (only)
    {
        for (scene::NodeId id : *only)
            if (auto it = mItems.find(id); it != mItems.end() && seen(it->second))
                stack.push_back(it->second);
    }
    else
        for (int i = 0; i < topLevelItemCount(); ++i)
            stack.push_back(topLevelItem(i));
    while (!stack.empty())
    {
        QTreeWidgetItem* item = stack.back();
        stack.pop_back();
        if (const scene::NodeId id = item->data(0, kIdRole).toULongLong(); id && mItems.contains(id) && mItems.at(id) == item)
            rows.emplace_back(id, item);
        shownChildren(item);
    }
    for (auto& [id, item] : rows)
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        RowIssues marks;
        const std::vector<const dx::Issue*>* rowIssues = nullptr;
        if (auto it = own.find(id); it != own.end())
        {
            rowIssues = &it->second;
            marks.own = issues::worst(it->second);
        }
        if (auto it = below.find(id); it != below.end() && !item->isExpanded() && item->childCount())
            marks.below = it->second;
        bool referenced = n->referenceId() != 0;
        bool hidden = outliner::hiddenInHierarchy(g, id);
        bool kept = mKept.contains(id);
        const scene::instancing::Kind shares = scene::instancing::kind(g, id);
        // Instances and references show the shared-data glyph and an italic name; their source stays as it is.
        const bool instanced = shares == scene::instancing::Kind::Instance || shares == scene::instancing::Kind::Reference;
        QString iconName = instanced ? QString("components") : QString::fromStdString(outliner::iconName(*n));
        const bool flagged = flagLights && scene::lights::isPlaceholder(*n);
        const bool tinted = tint && marks.own && marks.own->severity >= *tint;
        QString style = iconName + (referenced ? "/r" : "") + (hidden ? "/h" : "") + (kept ? "/k" : "") + (instanced ? "/i" : "") +
                        (flagged ? "/f" : "") + marks.key() + (tinted ? "/t" : "") +
                        (rowIssues ? QString("/v%1").arg(dx::version()) : QString());
        const RowVis vis = n->type().attrIndex("visibility") < 0 ? kNoEye
                           : !outliner::visibleSelf(g, id)        ? kHiddenSelf
                           : hidden                               ? kHiddenAbove
                                                                  : kShown;
        if (vis == kHiddenSelf)
            style += "/o";
        if (item->data(0, kStyleRole).toString() == style)
            continue;
        item->setData(0, kStyleRole, style);
        item->setData(0, kVisRole, int(vis));
        item->setData(0, kIssueRole, marks.own ? QVariant(int(marks.own->severity) * 2 + (marks.own->blocking ? 1 : 0)) : QVariant());
        item->setData(0, kIssueCountRole, rowIssues ? int(rowIssues->size()) : 0);
        RowIssues iconMarks = marks;
        iconMarks.own.reset();
        item->setIcon(0, rowIcon(iconName, referenced, hidden, flagged, iconMarks));
        QFont f = item->font(0);
        f.setItalic(referenced || instanced);
        item->setFont(0, f);
        if (instanced)
        {
            const scene::NodeId src = scene::instancing::source(g, id);
            item->setToolTip(0, QString::fromStdString(g.fullPath(id) + "\n" + scene::instancing::kindName(shares) + " of " +
                                                       (src ? g.fullPath(src) : std::string("?"))));
        }
        else
        {
            const std::string& doc = n->type().doc;
            item->setToolTip(0, QString::fromStdString(g.fullPath(id) + "\n" + n->type().name + (doc.empty() ? "" : "\n" + doc) +
                                                       (flagged ? "\nPlaceholder: no light extension renders this light." : "")));
        }
        if (rowIssues)
            item->setToolTip(0, item->toolTip(0) + "\n\n" + issues::lines(*rowIssues));
        QColor c = hidden || kept ? themeShade(100) : referenced ? themeShade(150) : normal;
        if (tinted)
            c = issues::color(marks.own->severity);
        item->setForeground(0, c);
    }
}

void Outliner::setFilter(outliner::Filter filter)
{
    mFilter = std::move(filter);
    rebuild();
}

void Outliner::applyFilter()
{
    LILA_ZONE();

    mKept.clear();
    if (mFilter.empty() || !mFilter.valid())
        return;
    scene::Graph& g = scene::Graph::current();
    outliner::FilterRows rows = outliner::filterRows(g, mFilter);
    mKept = std::move(rows.kept);
    for (auto& [id, item] : mItems)
        item->setHidden(!rows.matched.contains(id) && !mKept.contains(id));
    // Opened without being remembered in mExpanded, so clearing the filter folds the rows back as they were.
    for (scene::NodeId id : rows.matched)
        for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
            if (auto it = mItems.find(p); it != mItems.end())
                it->second->setExpanded(true);
    for (int i = 0; i < topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* top = topLevelItem(i);
        if (top->data(0, kSetRole).isValid())
        {
            const scene::Node* set = g.node(top->data(0, kSetRole).toULongLong());
            top->setHidden(!set || !mFilter.matches(set->name(), g.fullPath(set->id()), &set->type()));
        }
        else if (top->data(0, kUnloadedRefRole).isValid())
            top->setHidden(true);
        else if (top->data(0, kSourceRole).isValid())
        {
            bool any = false;
            for (int r = 0; r < top->childCount(); ++r)
            {
                const std::string label = top->child(r)->text(0).toStdString();
                const bool match = mFilter.matches(label, label, nullptr);
                top->child(r)->setHidden(!match);
                any |= match;
            }
            top->setHidden(!any);
            if (any)
                top->setExpanded(true);
        }
    }
}

void Outliner::syncSelectionFromScene()
{
    mSyncing = true;
    const auto& sel = scene::Graph::current().selection();
    for (QTreeWidgetItemIterator it(this); *it; ++it)
    {
        if (const outliner::RowSource* src = rowSource(*it); src && (*it)->data(0, kSourceRowRole).isValid())
        {
            (*it)->setSelected(src->current && src->current() == (*it)->data(0, kSourceRowRole).toString().toStdString());
            continue;
        }
        auto id = (*it)->data(0, kIdRole).toULongLong();
        (*it)->setSelected(id && std::find(sel.begin(), sel.end(), id) != sel.end());
    }
    shadeAncestors();
    mSyncing = false;
}

void Outliner::shadeAncestors()
{
    scene::Graph& g = scene::Graph::current();
    const std::set<scene::NodeId> chain = outliner::selectionAncestors(g, g.selection());
    // Part way from the list toward the selection colour, so the chain reads as tied to the selection in every theme.
    const QColor base = themeColor("base"), sel = themeColor("highlight");
    constexpr double kToward = 0.45;
    const QBrush shade(QColor::fromRgbF(float(base.redF() + (sel.redF() - base.redF()) * kToward),
                                        float(base.greenF() + (sel.greenF() - base.greenF()) * kToward),
                                        float(base.blueF() + (sel.blueF() - base.blueF()) * kToward)));
    for (auto& [id, item] : mItems)
    {
        const QVariant want = chain.contains(id) ? QVariant(shade) : QVariant();
        if (item->data(0, Qt::BackgroundRole) != want)
            for (int c = 0; c < item->columnCount(); ++c)
                item->setData(c, Qt::BackgroundRole, want);
    }
}

void Outliner::pushSelectionToScene()
{
    if (mSyncing)
        return;
    if (mDragSelecting)
    {
        mSelectionPending = true;
        return;
    }
    // Selection order matters (parent, parent chain, constraints). Qt's selectedItems() isn't click order, so keep
    // the scene's existing order and append whatever was newly selected.
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> picked;
    for (QTreeWidgetItem* item : selectedItems())
        if (scene::NodeId id = item->data(0, kIdRole).toULongLong())
            picked.push_back(id);
    std::vector<scene::NodeId> ordered;
    for (scene::NodeId id : g.selection())
        if (std::find(picked.begin(), picked.end(), id) != picked.end())
            ordered.push_back(id);
    for (scene::NodeId id : picked)
        if (std::find(ordered.begin(), ordered.end(), id) == ordered.end())
            ordered.push_back(id);
    json names = namesOf(ordered);
    try
    {
        if (names.empty())
            ops::execute("select", {{"clear", true}}, CallSource::UI);
        else
            ops::execute("select", {{"args", names}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void Outliner::revealSelected()
{
    reveal(scene::Graph::current().selection(), QAbstractItemView::PositionAtCenter);
}

void Outliner::reveal(const std::vector<scene::NodeId>& ids, QAbstractItemView::ScrollHint hint)
{
    scene::Graph& g = scene::Graph::current();
    QTreeWidgetItem* first = nullptr;
    for (scene::NodeId id : ids)
    {
        auto it = mItems.find(id);
        if (it == mItems.end())
            continue;
        for (scene::NodeId p : outliner::ancestors(g, id))
            if (auto pit = mItems.find(p); pit != mItems.end())
            {
                pit->second->setExpanded(true);
                mExpanded.insert(p);
            }
        if (!first)
            first = it->second;
    }
    if (first)
        scrollToItem(first, hint);
}

void Outliner::renameSelected()
{
    scene::Graph& g = scene::Graph::current();
    if (g.selection().empty())
        return;
    auto it = mItems.find(g.selection().front());
    if (it == mItems.end() || !(it->second->flags() & Qt::ItemIsEditable))
        return;
    revealSelected();
    setCurrentItem(it->second, 0, QItemSelectionModel::NoUpdate);
    editItem(it->second, 0);
}

void Outliner::expandEverything()
{
    for (auto& [id, item] : mItems)
        if (item->childCount())
            mExpanded.insert(id);
    expandAll();
}

void Outliner::collapseEverything()
{
    collapseAll();
    mExpanded.clear();
}

void Outliner::showContextMenu(const QPoint& pos)
{
    scene::Graph& g = scene::Graph::current();
    QTreeWidgetItem* clickedItem = itemAt(pos);
    QMenu menu(this);
    menu.setToolTipsVisible(true);

    if (clickedItem && (clickedItem->data(0, kSetRole).isValid() || clickedItem->data(0, kSetMemberRole).isValid()))
    {
        showSetMenu(clickedItem, viewport()->mapToGlobal(pos));
        return;
    }
    if (const outliner::RowSource* src = rowSource(clickedItem))
    {
        if (src->menu && clickedItem->data(0, kSourceRowRole).isValid())
        {
            src->menu(&menu, clickedItem->data(0, kSourceRowRole).toString().toStdString());
            menu.exec(viewport()->mapToGlobal(pos));
        }
        return;
    }
    if (clickedItem && clickedItem->data(0, kUnloadedRefRole).isValid())
    {
        addReferenceItems(&menu, clickedItem->data(0, kUnloadedRefRole).toString().toStdString(), false);
        menu.exec(viewport()->mapToGlobal(pos));
        return;
    }

    scene::NodeId clicked = clickedItem ? scene::NodeId(clickedItem->data(0, kIdRole).toULongLong()) : scene::kNoNode;
    const std::vector<scene::NodeId> targets = outliner::menuTargets(clicked, selectedIds());
    // Actions work on the selection, so a click outside it selects the clicked row first, when the item runs.
    auto run = [targets](const json& target) {
        scene::Graph& g = scene::Graph::current();
        std::vector<scene::NodeId> sel = g.selection();
        std::vector<scene::NodeId> want = targets;
        std::sort(sel.begin(), sel.end());
        std::sort(want.begin(), want.end());
        if (!want.empty() && sel != want)
            runUi("select", {{"args", namesOf(targets)}});
        actions::run(target);
    };

    bool any = !targets.empty();
    bool anyParented = false, anyGroup = false, anyTransform = false, anyMesh = false, anyChildren = false, anyShared = false;
    std::vector<uint32_t> refIds;
    for (scene::NodeId id : targets)
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        anyParented |= g.parent(id) != scene::kNoNode;
        anyGroup |= n->type().name == "transform" && !g.children(id).empty();
        anyTransform |= n->type().isA("transform");
        anyMesh |= n->type().isA("mesh");
        anyShared |= scene::instancing::kind(g, id) != scene::instancing::Kind::None;
        anyChildren |= !g.children(id).empty();
        if (n->referenceId() && std::find(refIds.begin(), refIds.end(), n->referenceId()) == refIds.end())
            refIds.push_back(n->referenceId());
    }
    auto item = [&](const std::string& id, bool enabled = true, const QString& label = {}) {
        if (QAction* a = addItem(&menu, id, run, label))
            a->setEnabled(enabled);
    };

    item("select.hierarchy", any && anyChildren);
    item("select.children", any && anyChildren);
    item("viewport.frameSelected", any, "Frame in Viewport");
    item("outliner.revealSelected", any);
    menu.addSeparator();
    item("outliner.rename", targets.size() == 1 && clickedItem && (clickedItem->flags() & Qt::ItemIsEditable));
    item("edit.duplicate", any);
    item("edit.instance", anyMesh);
    if (anyShared)
    {
        item("edit.makeUnique");
        item("select.instances");
        item("select.instanceSource");
    }
    if (QAction* del = menu.addAction("Delete", [run] { run(json{{"command", "delete"}}); }))
        del->setEnabled(any);
    menu.addSeparator();
    item("edit.group", any);
    item("edit.ungroup", anyGroup);
    item("edit.unparent", anyParented);
    QAction* parentTo = menu.addAction("Parent To...", [this, targets] {
        scene::Graph& g = scene::Graph::current();
        std::set<scene::NodeId> excluded(targets.begin(), targets.end());
        for (scene::NodeId t : targets)
            for (scene::NodeId d : g.descendants(t))
                excluded.insert(d);
        QStringList candidates;
        for (QTreeWidgetItemIterator it(this); *it; ++it)
            if (scene::NodeId id = (*it)->data(0, kIdRole).toULongLong(); id && !excluded.contains(id))
                candidates << (*it)->text(0);
        bool ok = false;
        QString parentName = QInputDialog::getItem(this, "Parent To", "Parent", candidates, 0, true, &ok);
        if (!ok || parentName.isEmpty())
            return;
        json args = namesOf(targets);
        args.push_back(parentName.toStdString());
        actions::run(json{{"command", "parent"}, {"args", {{"args", args}}}});
    });
    parentTo->setEnabled(any);
    menu.addSeparator();
    item("display.hide", any);
    item("display.show", any);
    item("display.toggleVisibility", any);
    item("viewport.isolate", any);
    menu.addSeparator();
    if (anyTransform)
    {
        item("modify.freezeTransforms");
        item("modify.centerPivot");
    }
    if (anyMesh)
    {
        item("mesh.bakeModifierStack");
        QMenu* mats = nullptr;
        for (scene::NodeId id : g.nodes())
            if (scene::Node* m = g.node(id); m && m->type().name == "material")
            {
                if (!mats)
                    mats = menu.addMenu("Assign Material");
                std::string name = m->path();
                mats->addAction(QString::fromStdString(m->name()), [run, name, targets] {
                    json args = namesOf(targets);
                    args.insert(args.begin(), name);
                    run(json{{"command", "assignMaterial"}, {"args", {{"args", args}}}});
                });
            }
    }
    if (anyTransform || anyMesh)
        menu.addSeparator();
    for (uint32_t refId : refIds)
        if (const io::Reference* r = referenceById(refId))
        {
            QMenu* sub = menu.addMenu(icon("link"), QString::fromStdString(refIds.size() == 1 ? "Reference" : "Reference " + r->ns));
            addReferenceItems(sub, r->ns, true);
        }
    if (!refIds.empty())
        menu.addSeparator();
    auto plain = [](const json& target) { actions::run(target); };
    addItem(&menu, "outliner.expandAll", plain);
    addItem(&menu, "outliner.collapseAll", plain);
    menu.addSeparator();
    item("outliner.copyName", any);
    item("outliner.copyPath", any);
    menu.exec(viewport()->mapToGlobal(pos));
}

void revealSelectionInOutliners()
{
    for (Outliner* o : gOutliners)
        if (o->isVisible())
            o->revealSelected();
}

void revealInOutliners(const std::vector<scene::NodeId>& ids)
{
    for (Outliner* o : gOutliners)
        if (o->isVisible())
            o->reveal(ids);
}

namespace
{
constexpr const char* kSplitPref = "/outliner/split";

/** Clicking into it selects all of it, so typing replaces the filter; Esc clears it. */
class FilterEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

protected:
    void focusInEvent(QFocusEvent* e) override
    {
        QLineEdit::focusInEvent(e);
        // After the click that focused it has placed the cursor.
        if (e->reason() == Qt::MouseFocusReason)
            QTimer::singleShot(0, this, [this] { selectAll(); });
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape && !text().isEmpty())
            return clear();
        QLineEdit::keyPressEvent(e);
    }
};

const char* kFilterSyntax = "Filter the outliner by name. Matches keep their parents, dimmed.\n"
                            "  pCube*  ?  [a-c]  [!0-9]   wildcards over the whole name\n"
                            "  cube sphere  or  cube, sphere   any of the terms\n"
                            "  !*Shape   leaves out what matches\n"
                            "  type:mesh   that type or one derived from it\n"
                            "  |rig|*   a term with | matches the full path\n"
                            "Case is ignored. Esc clears.";

std::optional<bool> outlinerActionState(const std::string& id)
{
    return viewportActionState(id, nullptr);
}

/** The ... menu: every iconBars::kOutliner item as a labelled row, menus as submenus. */
void fillOutlinerMore(QMenu* menu)
{
    menu->clear();
    bool pendingSeparator = false;
    for (const iconBars::Item* item : iconBars::itemsIn(iconBars::kOutliner))
    {
        if (item->kind == iconBars::Kind::Separator)
        {
            pendingSeparator = !menu->isEmpty();
            continue;
        }
        if (pendingSeparator)
            menu->addSeparator();
        pendingSeparator = false;
        if (item->kind == iconBars::Kind::Menu)
        {
            const Action* a = item->action.empty() ? nullptr : actions::find(item->action);
            const QString title = QString::fromStdString(!item->label.empty() ? item->label : a ? a->label : item->id);
            QMenu* sub = menu->addMenu(icon(QString::fromStdString(item->icon)), title);
            if (item->fill)
                item->fill(sub);
            else
                iconBars::addActionRows(sub, item->menuActions, outlinerActionState);
            continue;
        }
        iconBars::addActionRows(menu, {item->action}, outlinerActionState);
        if (QAction* added = menu->actions().isEmpty() ? nullptr : menu->actions().back())
        {
            if (!item->label.empty())
                added->setText(QString::fromStdString(item->label));
            if (const QString key = Hotkeys::get().keyFor(item->action); !key.isEmpty())
                added->setShortcut(QKeySequence(key));
            if (!item->icon.empty())
                added->setIcon(icon(QString::fromStdString(item->icon)));
        }
    }
    menu->setToolTipsVisible(true);
}

QToolButton* outlinerMenuButton(QWidget* parent, const char* iconName, const QString& tip, const char* objectName)
{
    auto* b = new QToolButton(parent);
    b->setObjectName(objectName);
    b->setAutoRaise(true);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setMenu(new QMenu(b));
    b->menu()->setToolTipsVisible(true);
    return b;
}

/**
 * An outliner under its filter row: the filter field and its options; the first pane also carries + (new groups and
 * sets) and ... (the rarer outliner actions, extension items included).
 */
QWidget* filteredOutliner(QWidget* parent, bool primary)
{
    auto* pane = new QWidget(parent);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    auto* edit = new FilterEdit(pane);
    edit->setObjectName("outlinerFilter");
    edit->setPlaceholderText("Filter");
    edit->setClearButtonEnabled(true);
    edit->addAction(icon("search"), QLineEdit::LeadingPosition);
    edit->setToolTip(kFilterSyntax);
    completion::attachSceneFilter(edit);
    row->addWidget(edit, 1);

    auto* options = outlinerMenuButton(pane, "filter", "<b>Filter Options</b><br>Regular expressions and what the outliner lists.",
                                       "outlinerFilterOptions");
    QAction* regex = options->menu()->addAction("Regular Expressions");
    regex->setObjectName("outlinerRegex");
    regex->setCheckable(true);
    regex->setToolTip("Each space separated term is a regex searched in the name (in the full path when it holds \\|). "
                      "! and type: still apply.");
    options->menu()->addSeparator();
    QMenu* listMenu = options->menu();
    QList<QAction*> fixed = listMenu->actions();
    QObject::connect(listMenu, &QMenu::aboutToShow, listMenu, [listMenu, fixed] {
        for (QAction* a : listMenu->actions())
            if (!fixed.contains(a))
            {
                listMenu->removeAction(a);
                a->deleteLater();
            }
        iconBars::addActionRows(listMenu, {"#Show", "outliner.toggleSets", "outliner.toggleFlagPlaceholderLights"}, outlinerActionState);
    });
    row->addWidget(options);

    if (primary)
    {
        row->addSpacing(4);
        auto* add = outlinerMenuButton(pane, "plus", "<b>New</b><br>Group the selection, or make an empty group or a set.", "outlinerAdd");
        iconBars::addActionRows(add->menu(), {"edit.group", "create.emptyGroup", "", "sets.create", "sets.createQuickSelect"},
                                outlinerActionState);
        row->addWidget(add);
        auto* more = outlinerMenuButton(pane, "dots", "<b>More</b><br>Rename, naming, expand and collapse, copy names.", "outlinerMore");
        QObject::connect(more->menu(), &QMenu::aboutToShow, more, [m = more->menu()] { fillOutlinerMore(m); });
        row->addWidget(more);
    }
    v->addLayout(row);
    auto* tree = new Outliner(pane);
    v->addWidget(tree, 1);

    auto* debounce = new QTimer(pane);
    debounce->setSingleShot(true);
    debounce->setInterval(120);
    auto apply = [edit, regex, options, tree] {
        outliner::Filter f(edit->text().toStdString(), regex->isChecked());
        achievementRules::outlinerFilter(edit->text().toStdString());
        edit->setToolTip(f.valid() ? QString(kFilterSyntax) : QString::fromStdString(f.error()) + "\n\n" + kFilterSyntax);
        options->setIcon(icon(regex->isChecked() ? "regex" : "filter"));
        tree->setFilter(std::move(f));
    };
    QObject::connect(debounce, &QTimer::timeout, pane, apply);
    QObject::connect(edit, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    QObject::connect(regex, &QAction::toggled, pane, apply);
    return pane;
}

/** A splitter handle with a short grip line, so the collapsed second pane can still be found. */
class GripHandle : public QSplitterHandle
{
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), themeShade(underMouse() ? 70 : 52));
        p.setPen(themeShade(120));
        int y = height() / 2, half = std::min(14, width() / 4);
        p.drawLine(width() / 2 - half, y, width() / 2 + half, y);
    }
    void enterEvent(QEnterEvent* e) override
    {
        QSplitterHandle::enterEvent(e);
        update();
    }
    void leaveEvent(QEvent* e) override
    {
        QSplitterHandle::leaveEvent(e);
        update();
    }
};

class GripSplitter : public QSplitter
{
public:
    using QSplitter::QSplitter;

protected:
    QSplitterHandle* createHandle() override { return new GripHandle(orientation(), this); }
};
} // namespace

OutlinerPanel::OutlinerPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* split = new GripSplitter(Qt::Vertical, this);
    split->setHandleWidth(5);
    split->addWidget(filteredOutliner(split, true));
    split->addWidget(filteredOutliner(split, false));
    split->setCollapsible(0, false);
    split->setCollapsible(1, true);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->handle(1)->setToolTip("Drag up for a second outliner");
    layout->addWidget(split);

    QList<int> sizes{1, 0};
    json saved = PrefStore::get().value("general", kSplitPref, json());
    if (saved.is_array() && saved.size() == 2 && saved[0].is_number_integer() && saved[1].is_number_integer())
        sizes = {std::max(1, saved[0].get<int>()), std::max(0, saved[1].get<int>())};
    split->setSizes(sizes);
    connect(split, &QSplitter::splitterMoved, this, [split] {
        QList<int> s = split->sizes();
        if (s.size() == 2)
            PrefStore::get().set("general", kSplitPref, json::array({s[0], s[1]}));
    });
}
} // namespace lila::ui

namespace lila::ui::outliner
{
void addRowSource(RowSource source)
{
    std::erase_if(rowSources(), [&](const RowSource& s) { return s.id == source.id; });
    rowSources().push_back(std::move(source));
    refreshRowSources();
}

void refreshRowSources()
{
    for (Outliner* o : gOutliners)
        o->scheduleRebuild();
}
} // namespace lila::ui::outliner
