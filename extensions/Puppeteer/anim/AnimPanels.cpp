// Animate-mode panels: Stacker, Mixer, Anim Bank and Cycle Maker, with their actions, menu and hotkeys. Every edit
// goes through a command (AnimCommands.cpp or the base layer and take commands), so it is undoable and echoed.

#include "AnimCommands.h"
#include "AnimOps.h"
#include "Common.h"
#include "PanelModels.h"
#include "Puppeteer.h"
#include "lila/FileDialog.h"

#include <lila/Events.h>
#include <lila/Ghosts.h>
#include <lila/Icons.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <cmath>
#include <memory>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer::anim
{
namespace
{
/** The character the Stacker and the key actions work on, and the keys picked in the Stacker. */
struct StackerState
{
    std::string node; // a node in the character (its root), kept while the selection moves off it
    std::vector<size_t> selected;
};

StackerState& state()
{
    static StackerState s;
    return s;
}

/** The selection's character root, else the last one shown while it still exists. */
std::string currentCharacter()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
        try
        {
            const Character ch = resolveCharacter(g, id);
            state().node = g.node(ch.root)->path();
            return state().node;
        }
        catch (const std::exception&)
        {
        }
    if (!state().node.empty() && !named(g, state().node))
        state().node.clear();
    return state().node;
}

json selectedKeysJson()
{
    json k = json::array();
    for (size_t i : state().selected)
        k.push_back(i);
    return k;
}

/** Runs a command on the current character. */
json onCharacter(const char* command, json call = json::object())
{
    const std::string ch = currentCharacter();
    if (ch.empty())
    {
        log::write(LogLevel::Warning, std::string(command) + ": select a character");
        return nullptr;
    }
    call["args"] = json::array({ch});
    return runCommand(command, call);
}

/** Runs a Stacker command on the current character and the keys selected in the Stacker. */
json onKeys(const char* command, json call = json::object())
{
    call["keys"] = selectedKeysJson();
    return onCharacter(command, std::move(call));
}

/** The call for an operation after the last selected key (the default key when none is selected). */
json afterSelected()
{
    return state().selected.empty() ? json::object() : json{{"index", state().selected.back()}};
}

/** Calls fn once per event-loop pass however many events asked for it. */
std::function<void()> coalesced(QWidget* owner, std::function<void()> fn)
{
    auto pending = std::make_shared<bool>(false);
    return [owner, fn = std::move(fn), pending] {
        if (*pending)
            return;
        *pending = true;
        QTimer::singleShot(0, owner, [fn, pending] {
            *pending = false;
            fn();
        });
    };
}

void subscribe(QWidget* w, std::initializer_list<events::Event> which, const std::function<void()>& fn)
{
    std::vector<events::SubscriptionId> subs;
    for (events::Event e : which)
        subs.push_back(events::subscribe(e, [fn](const events::EventData&) { fn(); }, "Puppeteer"));
    QObject::connect(w, &QObject::destroyed, [subs] {
        for (events::SubscriptionId s : subs)
            events::unsubscribe(s);
    });
}

QToolButton* toolButton(QWidget* parent, const char* icon, const QString& tip, std::function<void()> fn, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setIcon(ui::icon(icon));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCheckable(checkable);
    QObject::connect(b, &QToolButton::clicked, parent, [fn = std::move(fn)] { fn(); });
    return b;
}

QToolButton* textButton(QWidget* parent, const QString& label, const QString& tip, std::function<void()> fn)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(tip);
    b->setProperty("role", "push");
    QObject::connect(b, &QToolButton::clicked, parent, [fn = std::move(fn)] { fn(); });
    return b;
}

/** The action's tooltip: its label, hotkey and doc. */
QString actionTip(const char* id)
{
    return QString::fromStdString(actions::tooltip(id));
}

QLabel* groupLabel(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

struct MoreRow
{
    const char* label;
    QString tip;
    std::function<void()> run;
};

/** "...": a menu of rarer commands. */
QToolButton* moreButton(QWidget* parent, std::initializer_list<MoreRow> rows)
{
    auto* b = new QToolButton(parent);
    b->setIcon(ui::icon("dots"));
    b->setToolTip("More Commands");
    b->setPopupMode(QToolButton::InstantPopup);
    b->setProperty("role", "overflow");
    auto* menu = new QMenu(b);
    menu->setToolTipsVisible(true);
    for (const MoreRow& r : rows)
        menu->addAction(r.label, r.run)->setToolTip(r.tip);
    b->setMenu(menu);
    return b;
}

/** A number field: right-aligned, unit inside, no step buttons. */
void numberStyle(QAbstractSpinBox* s, const QString& unit)
{
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    if (auto* d = qobject_cast<QDoubleSpinBox*>(s); d && !unit.isEmpty())
        d->setSuffix(" " + unit);
}

QString frameText(double f)
{
    const double r = std::round(f);
    return std::abs(f - r) < 1e-3 ? QString::number(int64_t(r)) : QString::number(f, 'f', 2);
}

// Stacker

QWidget* makeStacker(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);

    auto* header = new QLabel(w);
    header->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(header);

    auto* tools = new QHBoxLayout;
    tools->setSpacing(1);
    tools->addWidget(toolButton(w, "square-plus", actionTip("puppeteer.stacker.createKey"), [] { onCharacter("stackerCreateKey", afterSelected()); }));
    tools->addWidget(toolButton(w, "trash", actionTip("puppeteer.stacker.deleteKey"), [] { onKeys("stackerDeleteKey"); }));
    tools->addWidget(toolButton(w, "copy", actionTip("puppeteer.stacker.copy"), [] { onKeys("stackerCopyKeys"); }));
    tools->addWidget(toolButton(w, "clipboard", actionTip("puppeteer.stacker.paste"), [] { onKeys("stackerPasteKeys"); }));
    tools->addWidget(toolButton(w, "row-insert-bottom", actionTip("puppeteer.stacker.insert"), [] { onCharacter("stackerInsertKeys", afterSelected()); }));
    tools->addWidget(toolButton(w, "flip-vertical", actionTip("puppeteer.stacker.mirror"), [] { onKeys("stackerMirrorKeys"); }));
    tools->addSpacing(10);
    QToolButton* prevGhost = toolButton(w, "arrow-bar-to-left", "Ghost Previous Key\nShow the pose of the key before the current frame", {}, true);
    QToolButton* nextGhost = toolButton(w, "arrow-bar-to-right", "Ghost Next Key\nShow the pose of the key after the current frame", {}, true);
    QToolButton* loop = toolButton(w, "repeat", "Loop Key\nThe last key mirrors the first and the cycle repeats", [] { onCharacter("stackerLoopKey"); }, true);
    for (QToolButton* b : {prevGhost, nextGhost, loop})
        tools->addWidget(b);
    tools->addStretch(1);
    v->addLayout(tools);

    auto ghostToggle = [](bool before, bool on) {
        const ghosts::Settings s = ghosts::Settings::fromPrefs();
        json call = {{"mode", "keys"}, {before ? "before" : "after", on ? 1 : 0}};
        call["enable"] = on || (before ? s.after : s.before) > 0;
        runCommand("ghosting", call);
    };
    QObject::connect(prevGhost, &QToolButton::toggled, w, [ghostToggle](bool on) { ghostToggle(true, on); });
    QObject::connect(nextGhost, &QToolButton::toggled, w, [ghostToggle](bool on) { ghostToggle(false, on); });

    auto* table = new QTableWidget(0, 3, w);
    table->setHorizontalHeaderLabels({"#", "Frame", "Gap"});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    table->setProperty("hotkeyPassThrough", "global");
    ui::deletion::claim(table, [] { onKeys("stackerDeleteKey"); });
    v->addWidget(table, 1);

    auto* quick = new QHBoxLayout;
    quick->setSpacing(2);
    auto select = [table](const char* which) {
        const json r = onCharacter("stackerSelect", {{"which", which}});
        if (!r.is_array())
            return;
        table->clearSelection();
        for (const json& i : r)
            for (int c = 0; c < table->columnCount(); ++c)
                if (QTableWidgetItem* it = table->item(int(i.get<int64_t>()), c))
                    it->setSelected(true);
    };
    quick->addWidget(groupLabel(w, "Select"));
    quick->addWidget(textButton(w, "All", "Select All\nEvery key but the loop key", [select] { select("all"); }));
    quick->addWidget(textButton(w, "1st Half", "Select First Half\nThe first half of the cycle", [select] { select("firstHalf"); }));
    quick->addWidget(textButton(w, "2nd Half", "Select Second Half\nThe second half of the cycle", [select] { select("secondHalf"); }));
    quick->addStretch(1);
    v->addLayout(quick);

    auto* marks = new QHBoxLayout;
    marks->setSpacing(2);
    marks->addWidget(groupLabel(w, "Mark"));
    marks->addWidget(textButton(w, "Breakdown", "Mark Breakdown\nMark the selected keys as breakdowns", [] { onKeys("stackerMarkKey", {{"mark", "breakdown"}}); }));
    marks->addWidget(textButton(w, "Extreme", "Mark Extreme\nMark the selected keys as extremes", [] { onKeys("stackerMarkKey", {{"mark", "extreme"}}); }));
    marks->addWidget(textButton(w, "Clear", "Clear Marks\nClear the selected keys' marks", [] { onKeys("stackerMarkKey", {{"mark", "none"}}); }));
    marks->addStretch(1);
    v->addLayout(marks);

    auto* reset = new QHBoxLayout;
    reset->setSpacing(2);
    auto* pose = new QComboBox(w);
    pose->addItem("Bind", "bind");
    pose->addItem("Ref", "ref");
    pose->setEditable(true);
    pose->setToolTip("Reset Pose\nThe library pose Reset goes back to; type any pose name");
    auto* tBox = new QCheckBox("T", w);
    auto* rBox = new QCheckBox("R", w);
    auto* sBox = new QCheckBox("S", w);
    for (QCheckBox* c : {tBox, rBox, sBox})
        c->setChecked(true);
    tBox->setToolTip("Translate\nReset translate");
    rBox->setToolTip("Rotate\nReset rotate");
    sBox->setToolTip("Scale\nReset scale");
    reset->addWidget(textButton(w, "Reset", "Reset Keys\nReset the selected keys to the pose, in the ticked channels", [pose, tBox, rBox, sBox] {
        json channels = json::array();
        if (tBox->isChecked())
            channels.push_back("translate");
        if (rBox->isChecked())
            channels.push_back("rotate");
        if (sBox->isChecked())
            channels.push_back("scale");
        if (channels.size() == 3)
            channels = json::array();
        // A listed pose sends its stored name; anything typed is a pose name as it stands.
        const int i = pose->currentIndex();
        const QString name = i >= 0 && pose->itemText(i) == pose->currentText() ? pose->itemData(i).toString() : pose->currentText();
        onKeys("stackerResetKeys", {{"pose", name.toStdString()}, {"channels", channels}});
    }));
    reset->addWidget(pose);
    for (QCheckBox* c : {tBox, rBox, sBox})
        reset->addWidget(c);
    reset->addSpacing(10);
    reset->addWidget(groupLabel(w, "Gap"));
    reset->addWidget(toolButton(w, "arrow-autofit-content", actionTip("puppeteer.stacker.gapDecrease"), [] { onKeys("stackerNudgeGap", {{"by", -1.0}}); }));
    reset->addWidget(toolButton(w, "arrow-autofit-width", actionTip("puppeteer.stacker.gapIncrease"), [] { onKeys("stackerNudgeGap", {{"by", 1.0}}); }));
    reset->addStretch(1);
    v->addLayout(reset);

    auto filling = std::make_shared<bool>(false);
    auto fill = [table, header, loop, prevGhost, nextGhost, filling] {
        const std::string ch = currentCharacter();
        json keys;
        if (!ch.empty())
            try
            {
                keys = ops::execute("stackerKeys", {{"args", {ch}}});
            }
            catch (const std::exception&)
            {
            }
        *filling = true;
        const std::vector<size_t> keep = state().selected;
        table->setRowCount(0);
        if (!keys.is_object())
        {
            header->setText("No character. Select a Puppeteer character to stack its keys.");
            *filling = false;
            return;
        }
        header->setText(QString("%1    %2 keys, %3 frames")
                            .arg(QString::fromStdString(keys.value("root", std::string())))
                            .arg(keys["rows"].size())
                            .arg(frameText(keys.value("total", 0.0))));
        const double now = Graph::current().time();
        const json& rowsJson = keys["rows"];
        table->setRowCount(int(rowsJson.size()));
        for (int r = 0; r < int(rowsJson.size()); ++r)
        {
            const json& row = rowsJson[size_t(r)];
            const std::string mark = row.value("mark", std::string("none"));
            const bool isLoop = row.value("loop", false);
            QString number = isLoop ? QString("L") : QString::number(r + 1);
            if (mark == "breakdown")
                number += "  b";
            else if (mark == "extreme")
                number += "  E";
            auto* num = new QTableWidgetItem(number);
            num->setFlags(num->flags() & ~Qt::ItemIsEditable);
            QFont f = num->font();
            f.setBold(mark == "extreme" || sameTime(row.value("time", 0.0), now));
            f.setItalic(mark == "breakdown" || isLoop);
            num->setFont(f);
            num->setToolTip(isLoop ? "Loop key: mirrors the first key" : QString::fromStdString(mark == "none" ? "Key" : mark));
            auto* frame = new QTableWidgetItem(frameText(row.value("frame", 0.0)));
            frame->setFlags(frame->flags() & ~Qt::ItemIsEditable);
            frame->setFont(f);
            const bool last = r + 1 == int(rowsJson.size());
            auto* gap = new QTableWidgetItem(last ? QString() : frameText(row.value("gap", 0.0)));
            if (last)
                gap->setFlags(gap->flags() & ~Qt::ItemIsEditable);
            else
                gap->setToolTip("Frames to the next key; edit to retime everything after");
            table->setItem(r, 0, num);
            table->setItem(r, 1, frame);
            table->setItem(r, 2, gap);
        }
        for (size_t i : keep)
            if (int(i) < table->rowCount())
                for (int c = 0; c < 3; ++c)
                    table->item(int(i), c)->setSelected(true);
        loop->setChecked(keys.value("loop", false));
        const ghosts::Settings gs = ghosts::Settings::fromPrefs();
        for (auto [b, on] : {std::pair{prevGhost, gs.enabled && gs.keys && gs.before > 0}, std::pair{nextGhost, gs.enabled && gs.keys && gs.after > 0}})
        {
            const QSignalBlocker block(b);
            b->setChecked(on);
        }
        *filling = false;
    };

    QObject::connect(table, &QTableWidget::itemSelectionChanged, w, [table, filling] {
        if (*filling)
            return;
        std::vector<size_t> sel;
        for (const QModelIndex& i : table->selectionModel()->selectedRows())
            sel.push_back(size_t(i.row()));
        std::sort(sel.begin(), sel.end());
        state().selected = sel;
    });
    QObject::connect(table, &QTableWidget::cellClicked, w, [table](int row, int) {
        const std::string ch = currentCharacter();
        if (ch.empty() || table->selectionModel()->selectedRows().size() != 1)
            return;
        try
        {
            const json keys = ops::execute("stackerKeys", {{"args", {ch}}});
            if (row < int(keys["rows"].size()))
                runCommand("currentTime", {{"args", {keys["rows"][size_t(row)]["time"]}}});
        }
        catch (const std::exception&)
        {
        }
    });
    QObject::connect(table, &QTableWidget::itemChanged, w, [filling](QTableWidgetItem* it) {
        if (*filling || it->column() != 2)
            return;
        bool ok = false;
        const double gap = it->text().toDouble(&ok);
        if (ok)
            onCharacter("stackerSetGap", {{"index", it->row()}, {"gap", gap}});
    });

    auto refresh = coalesced(w, fill);
    subscribe(w, {events::Event::UndoStackChanged, events::Event::SelectionChanged, events::Event::SceneNew, events::Event::SceneOpened,
                  events::Event::TimeChanged},
              refresh);
    fill();
    return w;
}

// Mixer

QWidget* makeMixer(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);

    auto* tree = new QTreeWidget(w);
    tree->setColumnCount(5);
    tree->setHeaderLabels({"", "Layer", "Weight", "Mode", ""});
    tree->setRootIsDecorated(false);
    // The name takes what the icon, weight and mode columns leave; those keep fixed widths so it is never squeezed.
    QHeaderView* header = tree->header();
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(24);
    for (int c : {0, 4})
    {
        header->setSectionResizeMode(c, QHeaderView::Fixed);
        header->resizeSection(c, 26);
    }
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Fixed);
    header->resizeSection(2, 64);
    header->setSectionResizeMode(3, QHeaderView::Fixed);
    header->resizeSection(3, 92);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->addWidget(tree, 1);

    auto selectedLayers = [tree] {
        json out = json::array();
        for (QTreeWidgetItem* it : tree->selectedItems())
            if (it->data(1, Qt::UserRole).toBool() == false)
                out.push_back(it->text(1).toStdString());
        return out;
    };
    auto current = [tree]() -> std::string {
        QTreeWidgetItem* it = tree->currentItem();
        return it && !it->data(1, Qt::UserRole).toBool() ? it->text(1).toStdString() : std::string();
    };

    auto* row1 = new QHBoxLayout;
    row1->setSpacing(2);
    row1->addWidget(textButton(w, "New", "New Layer\nA layer with the selected joints", [] { runCommand("createAnimLayer"); }));
    row1->addWidget(textButton(w, "Add Joints", "Add Joints\nAdd the selected joints to the layer and key into it", [current] {
        if (current().empty())
            return;
        runCommand("addAnimLayerMembers", {{"layer", current()}});
        runCommand("setActiveAnimLayer", {{"args", {current()}}});
    }));
    row1->addWidget(textButton(w, "Remove Joints", "Remove Joints\nTake the selected joints off the layer", [current] {
        if (!current().empty())
            runCommand("removeAnimLayerMembers", {{"layer", current()}});
    }));
    row1->addWidget(moreButton(w, {
        {"Extract to New Layer", "Move the selected joints' animation from the active layer to a new layer", [] { runCommand("extractAnimLayer"); }},
        {"Merge Down", "Merge the selected layers down", [selectedLayers] {
             if (!selectedLayers().empty())
                 runCommand("mergeAnimLayers", {{"args", selectedLayers()}});
         }},
        {"Duplicate", "Duplicate the layer", [current] {
             if (!current().empty())
                 runCommand("duplicateAnimLayer", {{"args", {current()}}});
         }},
        {"Move Up", "Move the layer up the stack", [current] {
             if (!current().empty())
                 runCommand("reorderAnimLayer", {{"args", {current()}}, {"by", 1}});
         }},
        {"Move Down", "Move the layer down the stack", [current] {
             if (!current().empty())
                 runCommand("reorderAnimLayer", {{"args", {current()}}, {"by", -1}});
         }},
    }));
    auto deleteLayers = [selectedLayers] {
        if (!selectedLayers().empty())
            runCommand("deleteAnimLayer", {{"args", selectedLayers()}});
    };
    ui::deletion::claim(tree, deleteLayers);
    row1->addStretch(1);
    row1->addWidget(toolButton(w, "trash", "Delete Layers\nDelete the selected layers", deleteLayers));
    v->insertLayout(0, row1);

    auto filling = std::make_shared<bool>(false);
    auto fill = [w, tree, filling] {
        json layers;
        try
        {
            layers = ops::execute("listAnimLayers", json::object());
        }
        catch (const std::exception&)
        {
            return;
        }
        *filling = true;
        const QString keep = tree->currentItem() ? tree->currentItem()->text(1) : QString();
        tree->clear();
        for (const LayerRow& r : mixerRows(layers))
        {
            auto* it = new QTreeWidgetItem(tree);
            const QString name = QString::fromStdString(r.name);
            it->setText(1, r.base ? QString("Base Animation") : name);
            it->setData(1, Qt::UserRole, r.base);
            QFont f = it->font(1);
            f.setBold(r.active);
            it->setFont(1, f);
            it->setToolTip(1, r.active ? "Active: keys go here" : "Click to key into this layer");
            if (r.base)
                continue;
            auto* eye = new QToolButton(tree);
            eye->setAutoRaise(true);
            eye->setIcon(ui::icon(r.mute ? "eye-off" : "eye"));
            eye->setToolTip(r.mute ? "Muted\nClick to unmute the layer" : "Mute\nClick to mute the layer");
            QObject::connect(eye, &QToolButton::clicked, w, [name, r] { runCommand("editAnimLayer", {{"args", {name.toStdString()}}, {"mute", !r.mute}}); });
            tree->setItemWidget(it, 0, eye);
            auto* weight = new QDoubleSpinBox(tree);
            numberStyle(weight, {});
            weight->setRange(0.0, 1.0);
            weight->setSingleStep(0.05);
            weight->setValue(r.weight);
            weight->setKeyboardTracking(false);
            QObject::connect(weight, &QDoubleSpinBox::valueChanged, w,
                             [name](double x) { runCommand("editAnimLayer", {{"args", {name.toStdString()}}, {"weight", x}}); });
            tree->setItemWidget(it, 2, weight);
            auto* mode = new QComboBox(tree);
            mode->addItem("Additive", "additive");
            mode->addItem("Override", "override");
            mode->setCurrentIndex(std::max(0, mode->findData(QString::fromStdString(r.mode))));
            QObject::connect(mode, &QComboBox::currentIndexChanged, w, [name, mode] {
                runCommand("editAnimLayer", {{"args", {name.toStdString()}}, {"mode", mode->currentData().toString().toStdString()}});
            });
            tree->setItemWidget(it, 3, mode);
            auto* lock = new QToolButton(tree);
            lock->setAutoRaise(true);
            lock->setIcon(ui::icon(r.lock ? "lock" : "lock-open"));
            lock->setToolTip(r.lock ? "Locked\nClick to unlock the layer" : "Lock\nNo keys or joint changes on the layer");
            QObject::connect(lock, &QToolButton::clicked, w, [name, r] { runCommand("editAnimLayer", {{"args", {name.toStdString()}}, {"lock", !r.lock}}); });
            tree->setItemWidget(it, 4, lock);
            if (name == keep)
                tree->setCurrentItem(it);
        }
        *filling = false;
    };
    QObject::connect(tree, &QTreeWidget::itemClicked, w, [filling](QTreeWidgetItem* it, int column) {
        if (*filling || column != 1)
            return;
        runCommand("setActiveAnimLayer", it->data(1, Qt::UserRole).toBool() ? json::object() : json{{"args", {it->text(1).toStdString()}}});
    });

    auto refresh = coalesced(w, fill);
    subscribe(w, {events::Event::UndoStackChanged, events::Event::SceneNew, events::Event::SceneOpened}, refresh);
    fill();
    return w;
}

// Anim Bank

void addFolder(QTreeWidgetItem* parent, QTreeWidget* tree, const Folder& f)
{
    for (const Folder& sub : f.folders)
    {
        auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        it->setText(0, QString::fromStdString(sub.name));
        it->setIcon(0, ui::icon("folder"));
        it->setData(0, Qt::UserRole, QString());
        it->setData(0, Qt::UserRole + 1, QString::fromStdString(sub.path));
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        it->setExpanded(true);
        addFolder(it, tree, sub);
    }
    for (const TakeRow& t : f.takes)
    {
        auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        it->setText(0, QString::fromStdString(t.name));
        it->setText(1, QString("%1-%2").arg(frameText(t.start * t.fps), frameText(t.end * t.fps)));
        it->setIcon(0, ui::icon(t.active ? "player-track-next" : "movie"));
        it->setData(0, Qt::UserRole, QString::fromStdString(t.name));
        it->setData(0, Qt::UserRole + 1, QString::fromStdString(t.folder));
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        QFont font = it->font(0);
        font.setBold(t.active);
        it->setFont(0, font);
        it->setToolTip(0, QString::fromStdString((t.active ? "Active take" : "Double-click to switch to it") +
                                                 std::string(t.loop ? ", loops" : "") + (t.root.empty() ? ", whole scene" : ", " + t.root)));
    }
}

QWidget* makeBank(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);

    auto* tree = new QTreeWidget(w);
    tree->setColumnCount(2);
    tree->setHeaderLabels({"Take", "Frames"});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    v->addWidget(tree, 1);

    auto currentTake = [tree]() -> std::string {
        QTreeWidgetItem* it = tree->currentItem();
        return it ? it->data(0, Qt::UserRole).toString().toStdString() : std::string();
    };

    auto* row = new QHBoxLayout;
    row->setSpacing(2);
    row->addWidget(textButton(w, "New", "New Take\nA take for the selected character", [] {
        json call = json::object();
        if (const std::string ch = currentCharacter(); !ch.empty())
            call["root"] = ch;
        runCommand("createTake", call);
    }));
    row->addWidget(textButton(w, "Duplicate", "Duplicate Take\nA copy named _01, _02 ...", [currentTake] {
        if (!currentTake().empty())
            runCommand("bankDuplicateTake", {{"args", {currentTake()}}});
    }));
    auto deleteTake = [currentTake] {
        if (!currentTake().empty())
            runCommand("deleteTake", {{"args", {currentTake()}}});
    };
    ui::deletion::claim(tree, deleteTake);
    row->addWidget(moreButton(w, {
        {"Move to Folder...", "File the take under a folder", [w, tree, currentTake] {
             if (currentTake().empty())
                 return;
             bool ok = false;
             const QString now = tree->currentItem()->data(0, Qt::UserRole + 1).toString();
             const QString f = QInputDialog::getText(w, "Take Folder", "Folder (a/b, empty for none):", QLineEdit::Normal, now, &ok);
             if (ok)
                 runCommand("editTake", {{"args", {currentTake()}}, {"folder", normalFolder(f.toStdString())}});
         }},
        {"Import Take...", "Import a take from a .lilatake file", [w] {
             const QString path = ui::fileDialog::getOpenFileName(w, "Import Take", QString(), "Takes (*.lilatake);;All files (*)");
             if (path.isEmpty())
                 return;
             json call = {{"args", {path.toStdString()}}};
             if (const std::string ch = currentCharacter(); !ch.empty())
                 call["root"] = ch;
             runCommand("importTake", call);
         }},
        {"Export Take...", "Export the take to a .lilatake file", [w, currentTake] {
             if (currentTake().empty())
                 return;
             const QString path = ui::fileDialog::getSaveFileName(w, "Export Take", QString::fromStdString(currentTake() + ".lilatake"),
                                                               "Takes (*.lilatake)");
             if (!path.isEmpty())
                 runCommand("exportTake", {{"args", {currentTake()}}, {"path", path.toStdString()}});
         }},
    }));
    row->addStretch(1);
    row->addWidget(toolButton(w, "trash", "Delete Take\nDelete the take and its animation", deleteTake));
    v->insertLayout(0, row);

    auto filling = std::make_shared<bool>(false);
    auto fill = [tree, filling] {
        json takes;
        try
        {
            takes = ops::execute("listTakes", json::object());
        }
        catch (const std::exception&)
        {
            return;
        }
        *filling = true;
        const QString keep = tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString() : QString();
        tree->clear();
        addFolder(nullptr, tree, bankTree(takes, currentCharacter()));
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if (!keep.isEmpty() && (*it)->data(0, Qt::UserRole).toString() == keep)
                tree->setCurrentItem(*it);
        *filling = false;
    };
    QObject::connect(tree, &QTreeWidget::itemDoubleClicked, w, [](QTreeWidgetItem* it, int) {
        const QString take = it->data(0, Qt::UserRole).toString();
        if (!take.isEmpty())
            runCommand("setActiveTake", {{"args", {take.toStdString()}}});
    });
    QObject::connect(tree, &QTreeWidget::itemChanged, w, [filling](QTreeWidgetItem* it, int column) {
        const QString take = it->data(0, Qt::UserRole).toString();
        if (*filling || column != 0 || take.isEmpty() || it->text(0) == take)
            return;
        runCommand("renameTake", {{"args", {take.toStdString(), it->text(0).toStdString()}}});
    });

    auto refresh = coalesced(w, fill);
    subscribe(w, {events::Event::UndoStackChanged, events::Event::SelectionChanged, events::Event::SceneNew, events::Event::SceneOpened,
                  events::Event::NodeRenamed},
              refresh);
    fill();
    return w;
}

// Cycle Maker

QWidget* makeCycleMaker(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(4, 4, 4, 4);

    auto* axis = new QComboBox(w);
    for (const char* a : {"x", "y", "z"})
        axis->addItem(QString(a).toUpper(), a);
    axis->setToolTip("Mirror Axis\nThe axis the mirror plane faces");
    auto axisName = [axis] { return axis->currentData().toString().toStdString(); };
    auto* cycle = new QHBoxLayout;
    cycle->setSpacing(2);
    cycle->addWidget(textButton(w, "Cycle Step", actionTip("puppeteer.cycle.step"), [axisName] { onCharacter("cycleStep", {{"axis", axisName()}}); }));
    cycle->addWidget(textButton(w, "Cycle Side", actionTip("puppeteer.cycle.side"), [axisName] { runCommand("cycleSide", {{"axis", axisName()}}); }));
    cycle->addWidget(textButton(w, "Loop Key", actionTip("puppeteer.stacker.loopKey"), [] { onCharacter("stackerLoopKey"); }));
    cycle->addSpacing(10);
    cycle->addWidget(groupLabel(w, "Mirror Axis"));
    cycle->addWidget(axis);
    cycle->addStretch(1);
    v->addLayout(cycle);

    auto* box = new QGroupBox("Walk / Run", w);
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto spin = [box, form](const char* label, double lo, double hi, double def, const char* unit, const char* tip, int decimals = 1) {
        auto* s = new QDoubleSpinBox(box);
        numberStyle(s, unit);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setValue(def);
        s->setToolTip(QString("%1\n%2").arg(label, tip));
        form->addRow(label, s);
        return s;
    };
    auto* run = new QCheckBox("Run", box);
    run->setToolTip("Shorter contact, a flight phase, the bounce inverted");
    form->addRow(run);
    QDoubleSpinBox* stride = spin("Stride", 0, 500, 50, "cm", "How far each foot travels under the body while planted");
    QDoubleSpinBox* speed = spin("Speed", 0, 2000, 0, "cm/s", "Above zero it sets the cycle length from the stride");
    QDoubleSpinBox* length = spin("Cycle Length", 4, 480, 32, "f", "Frames per cycle (two steps), when speed is zero", 0);
    QDoubleSpinBox* bounce = spin("Bounce", 0, 50, 3, "cm", "How far the pelvis rises and falls");
    QDoubleSpinBox* step = spin("Step Height", 0, 100, 10, "cm", "How high the swinging foot lifts");
    QDoubleSpinBox* arms = spin("Arm Swing", 0, 90, 20, "°", "How far each arm swings either way");
    QDoubleSpinBox* lean = spin("Lean", -45, 45, 4, "°", "How far the spine leans forward");
    QDoubleSpinBox* sway = spin("Sway", 0, 30, 2, "cm", "How far the pelvis shifts over the planted foot");
    QDoubleSpinBox* twist = spin("Twist", 0, 45, 6, "°", "How far the pelvis turns with the legs");
    auto* keysPerStep = new QSpinBox(box);
    numberStyle(keysPerStep, {});
    keysPerStep->setRange(1, 32);
    keysPerStep->setValue(4);
    form->addRow("Keys per Step", keysPerStep);
    QObject::connect(run, &QCheckBox::toggled, w, [stride, bounce, length](bool on) {
        stride->setValue(on ? 90 : 50);
        bounce->setValue(on ? 6 : 3);
        length->setValue(on ? 20 : 32);
    });
    form->addRow(textButton(box, "Generate", "Generate\nKey a looping cycle into a new take for the selected character", [=] {
        onCharacter("walkCycle", {{"run", run->isChecked()},
                             {"stride", stride->value()},
                             {"speed", speed->value()},
                             {"cycleLength", length->value()},
                             {"bounce", bounce->value()},
                             {"stepHeight", step->value()},
                             {"armSwing", arms->value()},
                             {"lean", lean->value()},
                             {"sway", sway->value()},
                             {"twist", twist->value()},
                             {"keysPerStep", keysPerStep->value()}});
    }));
    v->addWidget(box);
    v->addStretch(1);
    return w;
}

void registerUi(ext::Context& ctx)
{
    const char* cat = "Animation";
    auto add = [&](const char* id, const char* label, const char* doc, std::function<void()> fn) {
        actions::add(ctx, {.id = id, .label = label, .category = cat, .run = std::move(fn), .doc = doc});
    };
    add("puppeteer.stacker.createKey", "Create Key After", "Make a key after the selected Stacker key, holding its pose, one gap later.",
        [] { onCharacter("stackerCreateKey", afterSelected()); });
    add("puppeteer.stacker.deleteKey", "Delete Key With Timing", "Delete the selected Stacker keys, closing their gaps.",
        [] { onKeys("stackerDeleteKey"); });
    add("puppeteer.stacker.gapIncrease", "Increase Gap", "Lengthen the selected Stacker keys' gaps by a frame, retiming everything after.",
        [] { onKeys("stackerNudgeGap", {{"by", 1.0}}); });
    add("puppeteer.stacker.gapDecrease", "Decrease Gap", "Shorten the selected Stacker keys' gaps by a frame, retiming everything after.",
        [] { onKeys("stackerNudgeGap", {{"by", -1.0}}); });
    add("puppeteer.stacker.copy", "Copy Keys", "Copy the selected Stacker keys' poses.", [] { onKeys("stackerCopyKeys"); });
    add("puppeteer.stacker.paste", "Paste Keys", "Paste copied poses onto the selected Stacker keys.", [] { onKeys("stackerPasteKeys"); });
    add("puppeteer.stacker.insert", "Insert Keys After", "Insert the copied keys after the selected Stacker key.",
        [] { onCharacter("stackerInsertKeys", afterSelected()); });
    add("puppeteer.stacker.mirror", "Mirror Keys", "Mirror the pose at the selected Stacker keys.", [] { onKeys("stackerMirrorKeys"); });
    add("puppeteer.stacker.loopKey", "Toggle Loop Key", "Turn the character's loop key on or off.",
        [] { onCharacter("stackerLoopKey"); });
    add("puppeteer.cycle.step", "Cycle Step", "Build the second step of the cycle from the first half, mirrored.",
        [] { onCharacter("cycleStep"); });
    actions::add(ctx, {.id = "puppeteer.cycle.side", .label = "Cycle Side", .category = cat,
                       .doc = "Give the opposite side of the selected joints their motion half a cycle later, mirrored.", .command = "cycleSide"});
    add("puppeteer.cycle.walk", "Walk Cycle", "Key a looping walk into a new take for the selected character.",
        [] { onCharacter("walkCycle"); });
    add("puppeteer.cycle.run", "Run Cycle", "Key a looping run into a new take for the selected character.",
        [] { onCharacter("walkCycle", {{"run", true}, {"stride", 90.0}, {"bounce", 6.0}, {"cycleLength", 20.0}}); });

    panels::add(ctx, {.id = "puppeteerStacker", .title = "Stacker", .create = makeStacker, .area = Qt::LeftDockWidgetArea, .icon = "list-numbers", .open = false});
    panels::add(ctx, {.id = "puppeteerMixer", .title = "Mixer", .create = makeMixer, .area = Qt::LeftDockWidgetArea, .icon = "stack-2", .open = false});
    panels::add(ctx, {.id = "puppeteerAnimBank", .title = "Anim Bank", .create = makeBank, .area = Qt::LeftDockWidgetArea, .icon = "movie", .open = false});
    panels::add(ctx, {.id = "puppeteerCycleMaker", .title = "Cycle Maker", .create = makeCycleMaker, .icon = "walk", .open = false});

    const std::string menu = "Animation/Animate";
    menus::addItem(ctx, {.id = "puppeteer.anim.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Animate"});
    int sep = 0;
    for (const char* a : {"window.puppeteerStacker", "window.puppeteerMixer", "window.puppeteerAnimBank", "window.puppeteerCycleMaker", "",
                          "puppeteer.stacker.createKey", "puppeteer.stacker.deleteKey", "puppeteer.stacker.gapIncrease",
                          "puppeteer.stacker.gapDecrease", "puppeteer.stacker.loopKey", "", "puppeteer.cycle.step", "puppeteer.cycle.side",
                          "puppeteer.cycle.walk", "puppeteer.cycle.run"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("puppeteer.anim.menu.") + a, .menu = menu, .action = a});
        else
            menus::addItem(ctx, {.id = "puppeteer.anim.menu.sep" + std::to_string(sep++), .menu = menu, .kind = menus::Kind::Separator});
    }

    hotkeyLayers::add(ctx, {.id = "puppeteer.anim.keys",
                            .contexts = {{"global", {{"Ctrl+]", "puppeteer.stacker.gapIncrease"}, {"Ctrl+[", "puppeteer.stacker.gapDecrease"}}}},
                            .mode = kAnimateMode});
}
} // namespace
} // namespace puppeteer::anim

namespace puppeteer
{
void registerAnimPanels(lila::ext::Context& ctx)
{
    anim::registerAnimCommands(ctx);
    anim::registerUi(ctx);
}
} // namespace puppeteer
