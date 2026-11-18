#include "ui/SetsUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <optional>

namespace lila::ui::sets
{
namespace
{
json run(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

void createQuickSelect(QWidget* parent)
{
    bool ok = false;
    QString name = QInputDialog::getText(parent, "Create Quick Select Set", "Name", QLineEdit::Normal, "quickSelectSet1", &ok);
    if (!ok)
        return;
    run("createSet", {{"name", name.trimmed().toStdString()}, {"quickSelect", true}});
}
} // namespace
} // namespace lila::ui::sets

namespace lila::ui
{
namespace
{
constexpr int kSetPathRole = Qt::UserRole;
} // namespace

QuickSelectSetsPanel::QuickSelectSetsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    auto add = [&](const char* iconName, const QString& tip, auto fn, const QString& text = {}) {
        QToolButton* b = toolButton(this, iconName, tip, text);
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    add("plus", "<b>New Set</b><br>Create a quick select set from the selection.", [this] {
        json r = sets::run("createSet", {{"name", "quickSelectSet1"}, {"quickSelect", true}});
        if (!r.is_string())
            return;
        refresh();
        for (int i = 0; i < mTree->topLevelItemCount(); ++i)
            if (QTreeWidgetItem* it = mTree->topLevelItem(i); it->data(0, kSetPathRole).toString().toStdString() == r.get<std::string>())
            {
                mTree->setCurrentItem(it);
                rename(it);
            }
    });
    bar->addSpacing(10);
    mSelect = add("pointer", "<b>Select Members</b><br>Select the members of the current set.", [this] {
        if (std::string set = currentSet(); !set.empty())
            sets::run("selectSetMembers", {{"args", {set}}});
    }, "Select");
    mSelect->setEnabled(false);
    add("square-plus", "<b>Add Selected</b><br>Add the selection to the current set.", [this] {
        if (std::string set = currentSet(); !set.empty())
            sets::run("addToSet", {{"args", {set}}});
    }, "Add");
    add("square-minus", "<b>Remove Selected</b><br>Remove the selection from the current set.", [this] {
        if (std::string set = currentSet(); !set.empty())
            sets::run("removeFromSet", {{"args", {set}}});
    }, "Remove");
    bar->addStretch(1);
    mAllSets = add("filter", "<b>All Sets</b><br>List every object set, not only quick select sets.", [this] { refresh(); }, "All Sets");
    mAllSets->setCheckable(true);
    bar->addSpacing(10);
    add("trash", "<b>Delete Set</b><br>Delete the current set. Its members stay.", [this] {
        if (std::string set = currentSet(); !set.empty())
            sets::run("deleteSet", {{"args", {set}}});
    });
    v->addLayout(bar);

    mTree = new QTreeWidget(this);
    mTree->setColumnCount(2);
    mTree->setHeaderHidden(true);
    mTree->setRootIsDecorated(false);
    mTree->setIndentation(0);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    mTree->setIconSize(QSize(14, 14));
    Hotkeys::passThrough(mTree);
    deletion::claim(mTree, [this] {
        if (std::string set = currentSet(); !set.empty())
            sets::run("deleteSet", {{"args", {set}}});
    });
    QHeaderView* header = mTree->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    v->addWidget(mTree, 1);
    mEmpty = emptyStateLabel("No quick select sets. Click + to make one from the selection.", this);
    mEmpty->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    mEmpty->setContentsMargins(6, 8, 6, 8);
    v->addWidget(mEmpty);
    // Takes the spare height only while the tree is hidden, so the empty panel shrinks to its line.
    v->addStretch(0);

    connect(mTree, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* item, int) {
        const std::string set = item->data(0, kSetPathRole).toString().toStdString();
        if (set.empty())
            return;
        const bool add = QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        sets::run("selectSetMembers", {{"args", {set}}, {"add", add}});
    });
    connect(mTree, &QTreeWidget::currentItemChanged, this, [this] { mSelect->setEnabled(!currentSet().empty()); });
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) { rename(item); });
    connect(mTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (mFilling || column != 0)
            return;
        const std::string old = item->data(0, kSetPathRole).toString().toStdString();
        const std::string name = item->text(0).trimmed().toStdString();
        if (!name.empty() && name != old)
            sets::run("rename", {{"args", {old, name}}});
        scheduleRefresh();
    });
    connect(mTree->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this] { mEditing = false; });
    connect(mTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) { contextMenu(at); });

    QPointer<QuickSelectSetsPanel> self(this);
    mGraphListener = scene::Graph::current().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self || c == scene::Change::Selection)
            return;
        // Dragging an object sends a stream of value changes that never touch a set row.
        if (c == scene::Change::Value)
            if (const scene::Node* n = scene::Graph::current().node(id); !n || !scene::objectSets::isSet(*n))
                return;
        self->scheduleRefresh();
    });
    refresh();
}

QuickSelectSetsPanel::~QuickSelectSetsPanel() { scene::Graph::current().removeListener(mGraphListener); }

void QuickSelectSetsPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void QuickSelectSetsPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        // A rebuild would end a rename in progress; it runs when the edit closes.
        if (mEditing)
        {
            QTimer::singleShot(200, this, [this] { scheduleRefresh(); });
            return;
        }
        refresh();
    });
}

void QuickSelectSetsPanel::refresh()
{
    scene::Graph& g = scene::Graph::current();
    const std::string current = currentSet();
    std::optional<bool> filter;
    if (!mAllSets->isChecked())
        filter = true;

    mFilling = true;
    mTree->setUpdatesEnabled(false);
    mTree->clear();
    for (const scene::Node* set : scene::objectSets::all(g, filter))
    {
        const QString name = QString::fromStdString(set->name());
        const size_t count = scene::objectSets::members(g, set->id()).size();
        auto* item = new QTreeWidgetItem(mTree);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(0, kSetPathRole, QString::fromStdString(set->path()));
        item->setText(0, name);
        item->setIcon(0, icon("list-details"));
        item->setText(1, QString::number(count));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, "Click to select the members (Ctrl or Shift adds them), double-click to rename.");
        item->setToolTip(1, count == 1 ? QString("1 member") : QString("%1 members").arg(count));
        if (set->path() == current)
            mTree->setCurrentItem(item);
    }
    const bool empty = mTree->topLevelItemCount() == 0;
    mTree->setVisible(!empty);
    mEmpty->setText(mAllSets->isChecked() ? "No sets. Click + to make a quick select set from the selection."
                                          : "No quick select sets. Click + to make one from the selection.");
    mEmpty->setVisible(empty);
    mTree->setUpdatesEnabled(true);
    mFilling = false;
}

void QuickSelectSetsPanel::rename(QTreeWidgetItem* item)
{
    if (!(item->flags() & Qt::ItemIsEditable))
        return;
    mEditing = true;
    mTree->editItem(item, 0);
}

std::string QuickSelectSetsPanel::currentSet() const
{
    if (QTreeWidgetItem* it = mTree ? mTree->currentItem() : nullptr)
        return it->data(0, kSetPathRole).toString().toStdString();
    return {};
}

void QuickSelectSetsPanel::contextMenu(const QPoint& at)
{
    QTreeWidgetItem* item = mTree->itemAt(at);
    QMenu menu(this);
    const std::string set = item ? item->data(0, kSetPathRole).toString().toStdString() : std::string();
    if (set.empty())
    {
        menu.addAction(icon("plus"), "Create Quick Select Set from Selection",
                       [] { sets::run("createSet", {{"name", "quickSelectSet1"}, {"quickSelect", true}}); });
        menu.exec(mTree->viewport()->mapToGlobal(at));
        return;
    }
    menu.addAction("Select Members", [set] { sets::run("selectSetMembers", {{"args", {set}}}); });
    menu.addAction("Add Members to Selection", [set] { sets::run("selectSetMembers", {{"args", {set}}, {"add", true}}); });
    menu.addSeparator();
    menu.addAction(icon("square-plus"), "Add Selection to Set", [set] { sets::run("addToSet", {{"args", {set}}}); });
    menu.addAction(icon("square-minus"), "Remove Selection from Set", [set] { sets::run("removeFromSet", {{"args", {set}}}); });
    menu.addSeparator();
    menu.addAction(icon("pencil"), "Rename", [this, item] { rename(item); });
    menu.addAction(icon("trash"), "Delete Set", [set] { sets::run("deleteSet", {{"args", {set}}}); });
    menu.exec(mTree->viewport()->mapToGlobal(at));
}
} // namespace lila::ui

namespace lila::ui::sets
{
void registerActions(QWidget* dialogParent)
{
    if (actions::find("sets.create"))
        return;
    actions::add({.id = "sets.create", .label = "Set", .category = "Create",
                  .doc = "A new object set holding the selected objects, or the selected components in component mode.",
                  .python = "lila.ops.createSet(name=\"set1\")", .command = "createSet", .call = json::object()});
    QPointer<QWidget> parent(dialogParent);
    actions::add({.id = "sets.createQuickSelect", .label = "Quick Select Set...", .category = "Create",
                  .run = [parent] { createQuickSelect(parent); },
                  .doc = "Names a quick select set holding the selection (objects or components); Edit > Quick Select Sets "
                         "selects it again in one click.",
                  .python = "lila.ops.createSet(name=\"pickMe\", quickSelect=True)"});
    actions::add({.id = "window.quickSelectSets", .label = "Quick Select Sets", .category = "Windows",
                  .run = [] { panels::toggle(kQuickSelectSetsDock); },
                  .doc = "Lists the quick select sets: click one to select its members; create, edit, rename or delete them.",
                  .python = "lila.ui.run_action(\"window.quickSelectSets\")"});
}

void attachQuickSelectMenu(QMenu* menu)
{
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] {
        menu->clear();
        scene::Graph& g = scene::Graph::current();
        for (const scene::Node* s : scene::objectSets::all(g, true))
        {
            const std::string name = s->path();
            QAction* a = menu->addAction(icon("list-details"), QString::fromStdString(s->name()),
                                         [name] { run("selectSetMembers", {{"args", {name}}}); });
            a->setToolTip(QString::fromStdString("Select the members of " + name));
        }
        if (menu->isEmpty())
            menu->addAction("(no quick select sets)")->setEnabled(false);
        menu->addSeparator();
        if (const Action* create = actions::find("sets.createQuickSelect"))
            menu->addAction(QString::fromStdString(create->label), [] { actions::run(json("sets.createQuickSelect")); });
        menu->addAction("Quick Select Sets Panel", [] { panels::show(kQuickSelectSetsDock); });
    });
}
} // namespace lila::ui::sets
