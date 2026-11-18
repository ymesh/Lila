#include "ui/Shelf.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/FeatureStatus.h"
#include "ui/FlowLayout.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/TokenCompleter.h"
#include "ui/UiCommon.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportWidget.h"

#include <map>

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStringListModel>
#include <QTabBar>
#include <QTimer>
#include <QWindow>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <initializer_list>

namespace lila::ui
{
using json = nlohmann::json;

namespace shelves
{
std::vector<std::string> names()
{
    json order = PrefStore::get().value("general", "/shelves/order", json::array({"General", "Custom"}));
    std::vector<std::string> out;
    for (const json& n : order)
        if (n.is_string())
            out.push_back(n.get<std::string>());
    json hidden = PrefStore::get().value("general", "/shelves/hidden", json::array());
    for (const std::string& name : registry().idsInOrder())
        if (std::find(out.begin(), out.end(), name) == out.end() && !(hidden.is_array() && std::find(hidden.begin(), hidden.end(), json(name)) != hidden.end()))
            out.push_back(name);
    for (const std::string& file : PrefStore::get().studioFiles("shelves/"))
    {
        std::string name = file.substr(8);
        const bool isHidden = hidden.is_array() && std::find(hidden.begin(), hidden.end(), json(name)) != hidden.end();
        if (std::find(out.begin(), out.end(), name) == out.end() && (!isHidden || isLocked(name)))
            out.push_back(name);
    }
    return out;
}

bool isLocked(const std::string& shelf) { return PrefStore::get().isLocked("shelves/" + shelf, "/items"); }

Registry<Shelf>& registry()
{
    static Registry<Shelf> r;
    static int listener = r.addListener([](RegistryChange change, const std::string& name, const std::string&) {
        std::string file = "shelves/" + name;
        if (change == RegistryChange::Removed)
            PrefStore::get().removeDefaults(file, "shelf");
        else if (const Shelf* s = r.find(name))
            PrefStore::get().setDefaults(file, "shelf", {{"items", s->items}});
    });
    (void)listener;
    return r;
}

void add(Shelf s, std::string owner)
{
    std::string name = s.name;
    if (!s.items.is_array())
        s.items = json::array();
    registry().put(std::move(name), std::move(s), std::move(owner));
}

uint64_t add(ext::Context& ctx, Shelf s)
{
    if (const std::string* owner = registry().ownerOf(s.name); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "shelf '" + s.name + "' already belongs to " + *owner);
        return 0;
    }
    std::string name = s.name;
    add(std::move(s), ctx.owner());
    return ctx.track("shelf:" + name, [name, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(name); o && *o == own)
            registry().remove(name);
    });
}

std::string current()
{
    for (QWidget* w : QApplication::allWidgets())
        if (auto* panel = qobject_cast<ShelfPanel*>(w))
            if (std::string s = panel->currentShelf(); !s.empty())
                return s;
    std::vector<std::string> all = names();
    return all.empty() ? std::string() : all.front();
}

namespace
{
std::map<int, std::function<void(const std::string&)>>& currentListeners()
{
    static std::map<int, std::function<void(const std::string&)>> listeners;
    return listeners;
}
} // namespace

int onCurrentChanged(std::function<void(const std::string&)> fn)
{
    static int next = 0;
    currentListeners()[++next] = std::move(fn);
    return next;
}

void removeOnCurrentChanged(int id)
{
    currentListeners().erase(id);
}

void notifyCurrentChanged(const std::string& shelf)
{
    // Copied, so a listener may add or remove listeners.
    auto listeners = currentListeners();
    for (auto& [id, fn] : listeners)
        fn(shelf);
}

bool setCurrent(const std::string& shelf)
{
    std::vector<std::string> all = names();
    if (std::find(all.begin(), all.end(), shelf) == all.end())
        return false;
    for (QWidget* w : QApplication::allWidgets())
        if (auto* panel = qobject_cast<ShelfPanel*>(w))
            panel->showShelf(shelf);
    return true;
}

json items(const std::string& shelf)
{
    json v = PrefStore::get().value("shelves/" + shelf, "/items", json::array());
    return v.is_array() ? v : json::array();
}

void setItems(const std::string& shelf, const json& items)
{
    if (isLocked(shelf))
        return;
    PrefStore::get().set("shelves/" + shelf, "/items", items);
}

void addItem(const std::string& shelf, const json& item)
{
    json all = items(shelf);
    all.push_back(item);
    setItems(shelf, all);
}

void create(const std::string& shelf)
{
    auto all = names();
    if (std::find(all.begin(), all.end(), shelf) != all.end())
        return;
    json hidden = PrefStore::get().value("general", "/shelves/hidden", json::array());
    if (hidden.is_array() && std::erase(hidden.get_ref<json::array_t&>(), json(shelf)))
        PrefStore::get().set("general", "/shelves/hidden", hidden);
    all.push_back(shelf);
    PrefStore::get().set("general", "/shelves/order", all);
    setItems(shelf, json::array());
}

void remove(const std::string& shelf)
{
    if (isLocked(shelf))
        return;
    auto all = names();
    all.erase(std::remove(all.begin(), all.end(), shelf), all.end());
    PrefStore::get().set("general", "/shelves/order", all);
    // A contributed shelf would come back on its own; remember the user deleted it.
    if (registry().contains(shelf))
    {
        json hidden = PrefStore::get().value("general", "/shelves/hidden", json::array());
        if (!hidden.is_array())
            hidden = json::array();
        hidden.push_back(shelf);
        PrefStore::get().set("general", "/shelves/hidden", hidden);
    }
}

bool rename(const std::string& shelf, const std::string& to)
{
    if (isLocked(shelf))
        return false;
    auto all = names();
    if (to.empty() || std::find(all.begin(), all.end(), to) != all.end())
        return false;
    auto it = std::find(all.begin(), all.end(), shelf);
    if (it == all.end())
        return false;
    setItems(to, items(shelf));
    *it = to;
    PrefStore::get().set("general", "/shelves/order", all);
    return true;
}
} // namespace shelves

namespace
{
constexpr const char* kItemMime = "application/x-lila-shelf-item";

/** The item's label; an action's label cut to 8 characters when it was added gets its full name back. */
QString displayLabel(const json& item)
{
    std::string label = item.value("label", "?");
    if (json target = item.value("target", json()); label.size() == 8 && target.is_string())
        if (std::string full = actions::labelFor(target); full.size() > 8 && full.starts_with(label))
            label = full;
    return QString::fromStdString(label).trimmed();
}

/** Word-wraps a label into at most maxLines lines of width, eliding the last line when the rest doesn't fit. */
QString wrapLabel(const QFontMetrics& fm, const QString& label, int width, int maxLines)
{
    QStringList words = label.split(' ', Qt::SkipEmptyParts);
    QStringList lines;
    QString line;
    int i = 0;
    for (; i < words.size() && int(lines.size()) < maxLines - 1; ++i)
    {
        QString next = line.isEmpty() ? words[i] : line + ' ' + words[i];
        if (fm.horizontalAdvance(next) <= width || line.isEmpty())
        {
            line = next;
            continue;
        }
        lines << fm.elidedText(line, Qt::ElideRight, width);
        line = words[i];
    }
    QStringList rest = words.mid(i);
    if (!rest.isEmpty())
        line = line.isEmpty() ? rest.join(' ') : line + ' ' + rest.join(' ');
    lines << fm.elidedText(line, Qt::ElideRight, width);
    return lines.join('\n');
}

QString tooltipFor(const json& item)
{
    json target = item.value("target", json());
    std::string custom = item.value("tooltip", "");
    const Action* a = target.is_string() ? actions::find(target.get<std::string>()) : nullptr;
    std::string text = custom.empty() ? actions::labelFor(target) : custom;
    if (a)
    {
        QString key = Hotkeys::get().keyFor(a->id);
        text += key.isEmpty() ? "" : "  (" + key.toStdString() + ")";
        if (!a->doc.empty() && a->doc != custom)
            text += "\n" + a->doc;
        if (std::string status = featureStatus::label(a->id); !status.empty())
            text += "\n" + status;
    }
    QString tip = QString::fromStdString(text);
    std::string code = actions::pythonFor(target);
    if (!code.empty())
        tip += "\n\n" + QString::fromStdString(code);
    return tip;
}

QIcon itemIcon(const json& item)
{
    QIcon i = icon(QString::fromStdString(item.value("icon", "")));
    if (!i.isNull())
        return i;
    json target = item.value("target", json());
    if (target.is_object() && target.contains("python"))
        return icon("brand-python");
    if (target.is_object() && target.contains("command"))
        return icon("terminal-2");
    return icon("bolt");
}

/** Icons for the picker, with the name kept out of the grid (tooltip only) so the cells stay small. */
class IconModel : public QStringListModel
{
public:
    using QStringListModel::QStringListModel;
    QVariant data(const QModelIndex& index, int role) const override
    {
        QString name = QStringListModel::data(index, Qt::DisplayRole).toString();
        switch (role)
        {
        case Qt::DecorationRole: return icon(name);
        case Qt::ToolTipRole:
        case Qt::UserRole: return name;
        case Qt::DisplayRole: return QVariant();
        default: return QStringListModel::data(index, role);
        }
    }
};

/** Searchable grid of the bundled icons. Returns the chosen name, "" for none, or nullopt on cancel. */
std::optional<QString> pickIcon(QWidget* parent, const QString& current)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Choose Icon");
    dlg.resize(520, 460);
    auto* v = new QVBoxLayout(&dlg);
    auto* search = new QLineEdit;
    search->setPlaceholderText(QString("Search %1 icons").arg(iconNames().size()));
    search->setClearButtonEnabled(true);
    v->addWidget(search);

    auto* model = new IconModel(iconNames(), &dlg);
    auto* proxy = new QSortFilterProxyModel(&dlg);
    proxy->setSourceModel(model);
    proxy->setFilterRole(Qt::UserRole);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto* view = new QListView;
    view->setViewMode(QListView::IconMode);
    view->setResizeMode(QListView::Adjust);
    view->setMovement(QListView::Static);
    view->setUniformItemSizes(true);
    view->setIconSize({24, 24});
    view->setGridSize({40, 40});
    view->setModel(proxy);
    v->addWidget(view, 1);

    auto* name = new QLabel(current.isEmpty() ? "No icon" : current);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* none = buttons->addButton("No Icon", QDialogButtonBox::ResetRole);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(name, 1);
    bottom->addWidget(buttons);
    v->addLayout(bottom);

    QString chosen = current;
    QObject::connect(search, &QLineEdit::textChanged, proxy, &QSortFilterProxyModel::setFilterFixedString);
    QObject::connect(view->selectionModel(), &QItemSelectionModel::currentChanged, &dlg, [&](const QModelIndex& i) {
        chosen = i.data(Qt::UserRole).toString();
        name->setText(chosen);
    });
    QObject::connect(view, &QListView::doubleClicked, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(none, &QPushButton::clicked, &dlg, [&] {
        chosen.clear();
        dlg.accept();
    });
    if (int row = int(iconNames().indexOf(current)); row >= 0)
        view->setCurrentIndex(proxy->mapFromSource(model->index(row)));
    search->setFocus();
    if (dlg.exec() != QDialog::Accepted)
        return std::nullopt;
    return chosen;
}

/** Editor for a shelf button: label, tooltip, icon, and what it runs (Python, a command, or an action). */
bool editItem(QWidget* parent, json& item, const QString& title = "Edit Shelf Button")
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    auto* form = new QFormLayout(&dlg);
    auto* label = new QLineEdit(QString::fromStdString(item.value("label", "")));
    auto* tooltip = new QLineEdit(QString::fromStdString(item.value("tooltip", "")));
    QString iconName = QString::fromStdString(item.value("icon", ""));
    auto* iconButton = new QPushButton;
    auto showIcon = [&] {
        iconButton->setIcon(icon(iconName));
        iconButton->setText(iconName.isEmpty() ? "Choose..." : iconName);
    };
    showIcon();
    QObject::connect(iconButton, &QPushButton::clicked, &dlg, [&] {
        if (auto picked = pickIcon(&dlg, iconName))
        {
            iconName = *picked;
            showIcon();
        }
    });

    json target = item.value("target", json());
    auto* kind = new QComboBox;
    kind->addItems({"Python", "Command", "Action"});
    auto* pages = new QStackedWidget;
    auto* code = new QPlainTextEdit(QString::fromStdString(actions::pythonFor(target)));
    code->setFont(monoFont());
    code->setMinimumSize(420, 160);
    pages->addWidget(code);

    auto* commandPage = new QWidget;
    auto* commandForm = new QFormLayout(commandPage);
    commandForm->setContentsMargins(0, 0, 0, 0);
    bool isCommand = target.is_object() && target.contains("command");
    auto* command = new QLineEdit(isCommand ? QString::fromStdString(target.value("command", "")) : QString());
    auto* args = new QPlainTextEdit(QString::fromStdString(isCommand ? target.value("args", json::object()).dump(2) : "{}"));
    args->setFont(monoFont());
    command->setPlaceholderText("resetPivot");
    completion::attachValues(command, [] {
        QStringList names;
        for (const CommandDef* c : ops::all())
            names << QString::fromStdString(c->name);
        names.sort(Qt::CaseInsensitive);
        return names;
    });
    commandForm->addRow("Command", command);
    commandForm->addRow("Args (JSON)", args);
    pages->addWidget(commandPage);

    auto* action = new QComboBox;
    action->setEditable(true);
    for (const Action* a : actions::all())
        action->addItem(QString::fromStdString(a->category + ": " + a->label), QString::fromStdString(a->id));
    action->completer()->setCompletionMode(QCompleter::PopupCompletion);
    action->completer()->setFilterMode(Qt::MatchContains);
    pages->addWidget(action);

    if (target.is_string())
    {
        kind->setCurrentIndex(2);
        action->setCurrentIndex(action->findData(QString::fromStdString(target.get<std::string>())));
    }
    else if (isCommand)
        kind->setCurrentIndex(1);
    pages->setCurrentIndex(kind->currentIndex());
    QObject::connect(kind, &QComboBox::currentIndexChanged, pages, &QStackedWidget::setCurrentIndex);

    form->addRow("Label", label);
    form->addRow("Tooltip", tooltip);
    form->addRow("Icon", iconButton);
    form->addRow("Runs", kind);
    form->addRow(pages);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);

    json newTarget;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        switch (kind->currentIndex())
        {
        case 0: newTarget = {{"python", code->toPlainText().toStdString()}}; break;
        case 1:
        {
            json parsed = json::parse(args->toPlainText().toStdString(), nullptr, false);
            if (command->text().trimmed().isEmpty() || !parsed.is_object())
            {
                QMessageBox::warning(&dlg, title, "Give a command name and its args as a JSON object.");
                return;
            }
            newTarget = {{"command", command->text().trimmed().toStdString()}, {"args", parsed}};
            break;
        }
        default:
        {
            QString id = action->currentData().toString();
            if (id.isEmpty() || action->currentText() != action->itemText(action->currentIndex()))
                id = action->currentText().trimmed();
            newTarget = id.toStdString();
        }
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    // Python is prefilled with the action's equivalent, so an untouched action item stays an action.
    if (kind->currentIndex() != 0 || code->toPlainText().toStdString() != actions::pythonFor(target))
        item["target"] = newTarget;
    item["label"] = label->text().toStdString();
    item["tooltip"] = tooltip->text().toStdString();
    if (iconName.isEmpty())
        item.erase("icon");
    else
        item["icon"] = iconName.toStdString();
    return true;
}

/** Rename a shelf and reorder, edit or delete its buttons in one list. Returns the shelf's name afterwards. */
std::string editShelf(QWidget* parent, const std::string& shelf)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Edit Shelf");
    dlg.resize(360, 420);
    auto* v = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(QString::fromStdString(shelf));
    form->addRow("Name", name);
    v->addLayout(form);
    auto* list = new QListWidget;
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setIconSize({16, 16});
    v->addWidget(list, 1);

    json all = shelves::items(shelf);
    auto refreshRow = [&](QListWidgetItem* row) {
        const json& item = all[row->data(Qt::UserRole).toInt()];
        row->setIcon(itemIcon(item));
        row->setText(QString::fromStdString(item.value("label", "?")));
    };
    for (int i = 0; i < int(all.size()); ++i)
    {
        auto* row = new QListWidgetItem(list);
        row->setData(Qt::UserRole, i);
        refreshRow(row);
    }

    auto* rowButtons = new QHBoxLayout;
    auto* edit = new QPushButton("Edit...");
    auto* changeIcon = new QPushButton("Icon...");
    auto* del = new QPushButton("Delete");
    rowButtons->addWidget(edit);
    rowButtons->addWidget(changeIcon);
    rowButtons->addWidget(del);
    rowButtons->addStretch(1);
    v->addLayout(rowButtons);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(buttons);

    auto editRow = [&] {
        if (QListWidgetItem* row = list->currentItem())
            if (editItem(&dlg, all[row->data(Qt::UserRole).toInt()]))
                refreshRow(row);
    };
    QObject::connect(edit, &QPushButton::clicked, &dlg, editRow);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, editRow);
    QObject::connect(changeIcon, &QPushButton::clicked, &dlg, [&] {
        QListWidgetItem* row = list->currentItem();
        if (!row)
            return;
        json& item = all[row->data(Qt::UserRole).toInt()];
        if (auto picked = pickIcon(&dlg, QString::fromStdString(item.value("icon", ""))))
        {
            item["icon"] = picked->toStdString();
            refreshRow(row);
        }
    });
    QObject::connect(del, &QPushButton::clicked, &dlg, [&] { delete list->currentItem(); });
    deletion::claim(list, [list] { delete list->currentItem(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return shelf;

    json reordered = json::array();
    for (int r = 0; r < list->count(); ++r)
        reordered.push_back(all[list->item(r)->data(Qt::UserRole).toInt()]);
    shelves::setItems(shelf, reordered);
    std::string to = name->text().trimmed().toStdString();
    if (to != shelf && shelves::rename(shelf, to))
        return to;
    return shelf;
}

void moveItem(const std::string& from, int index, const std::string& to, int at)
{
    json src = shelves::items(from);
    if (index < 0 || index >= int(src.size()))
        return;
    json item = src[index];
    src.erase(src.begin() + index);
    if (from == to)
    {
        if (at > index)
            --at;
        src.insert(src.begin() + std::clamp(at, 0, int(src.size())), item);
        shelves::setItems(from, src);
        return;
    }
    json dst = shelves::items(to);
    dst.insert(dst.begin() + std::clamp(at, 0, int(dst.size())), item);
    shelves::setItems(to, dst);
    shelves::setItems(from, src);
}

json pythonItem(const QString& code)
{
    return {{"label", "Py"}, {"tooltip", code.trimmed().split('\n').first().toStdString()}, {"target", {{"python", code.toStdString()}}}};
}
} // namespace

// ------------------------------------------------------------------- ShelfPage

ShelfPage::ShelfPage(std::string shelf, bool floating, QWidget* parent) : QWidget(parent), mShelf(std::move(shelf)), mFloating(floating)
{
    setAcceptDrops(true);
    if (mFloating)
    {
        auto* flow = new FlowLayout(2);
        flow->setContentsMargins(4, 4, 4, 4);
        mLayout = flow;
    }
    else
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(4, 2, 4, 2);
        row->setSpacing(2);
        mLayout = row;
    }
    setLayout(mLayout);
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "shelves/" + mShelf)
            rebuild();
    });
    rebuild();
}

ShelfPage::~ShelfPage()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

void ShelfPage::setShelf(std::string shelf)
{
    mShelf = std::move(shelf);
    rebuild();
}

void ShelfPage::rebuild()
{
    // deleteLater: a rebuild can run inside one of these buttons' own signals.
    while (QLayoutItem* item = mLayout->takeAt(0))
    {
        if (QWidget* w = item->widget())
        {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    json all = shelves::items(mShelf);
    const int iconSize = mFloating ? shelves::floatingIconSize() : 16;
    QFont smallFont = font();
    smallFont.setPointSizeF(smallFont.pointSizeF() - 1.0);
    QFontMetrics fm(smallFont);
    // Labels get two lines and a width that grows with the icons, so bigger icons show more of each name.
    const int cell = std::max(iconSize + 24, iconSize * 3 / 2 + 16);
    const int cellHeight = iconSize + 10 + 2 * fm.lineSpacing();
    for (int i = 0; i < int(all.size()); ++i)
    {
        const json& item = all[i];
        QString label = displayLabel(item);
        auto* b = new QToolButton(this);
        b->setIcon(itemIcon(item));
        b->setToolTip(tooltipFor(item));
        b->setAutoRaise(true);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        b->setProperty("shelfIndex", i);
        if (mFloating)
        {
            b->setFont(smallFont);
            b->setText(wrapLabel(fm, label, cell - 6, 2));
            b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            b->setIconSize({iconSize, iconSize});
            b->setFixedSize(cell, cellHeight);
        }
        else
        {
            b->setText(QFontMetrics(b->font()).elidedText(label, Qt::ElideRight, 120));
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setIconSize({16, 16});
            b->setMinimumHeight(24);
        }
        b->installEventFilter(this);
        json target = item.value("target", json());
        connect(b, &QToolButton::clicked, this, [this, target] {
            Q_EMIT triggered();
            actions::ViaScope via(actions::Via::Shelf);
            if (!actions::run(target))
                log::warn("shelf: '{}' failed", actions::labelFor(target));
        });
        connect(b, &QToolButton::customContextMenuRequested, this, [this, i, b](QPoint p) { itemMenu(i, b->mapToGlobal(p)); });
        mLayout->addWidget(b);
    }
    if (mFloating)
    {
        auto* plus = new QToolButton(this);
        plus->setIcon(icon("plus"));
        plus->setIconSize({iconSize * 3 / 4, iconSize * 3 / 4});
        plus->setAutoRaise(true);
        plus->setFixedSize(cell, cellHeight);
        plus->setToolTip("Add a button");
        connect(plus, &QToolButton::clicked, this, &ShelfPage::addButton);
        mLayout->addWidget(plus);
        return;
    }
    if (all.empty())
    {
        auto* hint = new QToolButton(this);
        hint->setText("Drop Python here, or right-click to add");
        hint->setEnabled(false);
        // Clicks fall through to the shelf so right-clicking the hint opens the shelf menu.
        hint->setAttribute(Qt::WA_TransparentForMouseEvents);
        hint->setAutoRaise(true);
        mLayout->addWidget(hint);
    }
    static_cast<QHBoxLayout*>(mLayout)->addStretch(1);
}

void ShelfPage::addButton()
{
    json item = {{"label", "Button"}, {"target", {{"python", "# your code\n"}}}};
    if (editItem(window(), item, "New Shelf Button"))
        shelves::addItem(mShelf, item);
}

void ShelfPage::fillAddMenu(QMenu* menu)
{
    menu->addAction(icon("brand-python"), "New Python Button...", this, &ShelfPage::addButton);
    menu->addAction(icon("terminal-2"), "New Command Button...", this, [this] {
        json item = {{"label", "Cmd"}, {"target", {{"command", ""}, {"args", json::object()}}}};
        if (editItem(window(), item, "New Shelf Button"))
            shelves::addItem(mShelf, item);
    });
    menu->addAction(icon("clipboard"), "Paste Python as Button", this, [this] {
        QString code = QApplication::clipboard()->text();
        if (!code.trimmed().isEmpty())
            shelves::addItem(mShelf, pythonItem(code));
    });
    QMenu* add = menu->addMenu(icon("bolt"), "Add Action");
    std::map<std::string, QMenu*> categories;
    for (const Action* a : actions::all())
    {
        QMenu*& sub = categories[a->category];
        if (!sub)
            sub = add->addMenu(QString::fromStdString(a->category));
        std::string id = a->id, label = a->label;
        sub->addAction(QString::fromStdString(label), this, [this, id, label] {
            shelves::addItem(mShelf, {{"label", label}, {"tooltip", label}, {"target", id}});
        });
    }
}

void shelves::addToCurrent(const json& target)
{
    FloatingShelf* floating = FloatingShelf::active();
    std::string shelf = floating && !floating->shelf().empty() ? floating->shelf() : current();
    if (shelf.empty())
    {
        log::warn("no shelf to add to");
        return;
    }
    std::string label = actions::labelFor(target);
    addItem(shelf, {{"label", label}, {"tooltip", label}, {"target", target}});
    log::info("Added {} to the {} shelf", label, shelf);
}

int shelves::floatingIconSize()
{
    return stepFloatingIconSize(PrefStore::get().value<int>("general", "/floatingShelf/iconSize", kFloatingIconDefault), 0);
}

int shelves::stepFloatingIconSize(int size, int notches)
{
    return std::clamp(size + notches * 4, 16, 96);
}

bool shelves::addModifierHeld()
{
    return (QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) == (Qt::ControlModifier | Qt::ShiftModifier);
}

void ShelfPage::itemMenu(int index, QPoint global)
{
    // Changes wait until the menu is gone, so the rebuild they cause never deletes a widget mid-signal.
    std::function<void()> after;
    QMenu menu;
    if (shelves::isLocked(mShelf))
    {
        menu.addAction(icon("copy"), "Copy Python", [this, index] {
            json all = shelves::items(mShelf);
            if (index < int(all.size()))
                QApplication::clipboard()->setText(QString::fromStdString(actions::pythonFor(all[index].value("target", json()))));
        });
        menu.addAction(icon("lock"), "Studio shelf, read-only")->setEnabled(false);
        menu.exec(global);
        return;
    }
    menu.addAction(icon("pencil"), "Edit...", [&] {
        after = [this, index] {
            json all = shelves::items(mShelf);
            if (index < int(all.size()) && editItem(window(), all[index]))
                shelves::setItems(mShelf, all);
        };
    });
    menu.addAction(icon("photo"), "Change Icon...", [&] {
        after = [this, index] {
            json all = shelves::items(mShelf);
            if (index >= int(all.size()))
                return;
            if (auto picked = pickIcon(window(), QString::fromStdString(all[index].value("icon", ""))))
            {
                all[index]["icon"] = picked->toStdString();
                shelves::setItems(mShelf, all);
            }
        };
    });
    menu.addAction(icon("copy"), "Copy Python", [&] {
        json all = shelves::items(mShelf);
        if (index < int(all.size()))
            QApplication::clipboard()->setText(QString::fromStdString(actions::pythonFor(all[index].value("target", json()))));
    });
    menu.addSeparator();
    menu.addAction(icon("trash"), "Delete", [&] {
        after = [this, index] {
            json all = shelves::items(mShelf);
            if (index < int(all.size()))
            {
                all.erase(all.begin() + index);
                shelves::setItems(mShelf, all);
            }
        };
    });
    menu.exec(global);
    if (after)
        after();
}

void ShelfPage::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu;
    if (shelves::isLocked(mShelf))
        menu.addAction(icon("lock"), "Studio shelf, read-only")->setEnabled(false);
    else
        fillAddMenu(&menu);
    menu.exec(e->globalPos());
}

bool ShelfPage::eventFilter(QObject* obj, QEvent* e)
{
    auto* b = qobject_cast<QToolButton*>(obj);
    if (!b || !b->property("shelfIndex").isValid())
        return QWidget::eventFilter(obj, e);
    if (e->type() == QEvent::MouseButtonPress)
        mPressPos = static_cast<QMouseEvent*>(e)->position().toPoint();
    else if (e->type() == QEvent::MouseMove)
    {
        auto* m = static_cast<QMouseEvent*>(e);
        if ((m->buttons() & Qt::LeftButton) && !shelves::isLocked(mShelf) &&
            (m->position().toPoint() - mPressPos).manhattanLength() >= QApplication::startDragDistance())
        {
            auto* mime = new QMimeData;
            mime->setData(kItemMime, QByteArray::fromStdString(json{{"shelf", mShelf}, {"index", b->property("shelfIndex").toInt()}}.dump()));
            auto* drag = new QDrag(b);
            drag->setMimeData(mime);
            drag->setPixmap(b->grab());
            drag->setHotSpot(mPressPos);
            b->setDown(false);
            drag->exec(Qt::MoveAction);
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

int ShelfPage::dropIndex(QPoint pos) const
{
    int best = -1;
    int bestDist = std::numeric_limits<int>::max();
    QRect bestRect;
    for (QObject* child : children())
    {
        auto* b = qobject_cast<QToolButton*>(child);
        if (!b || !b->isVisible() || !b->property("shelfIndex").isValid())
            continue;
        QRect r = b->geometry();
        int dx = std::max({r.left() - pos.x(), 0, pos.x() - r.right()});
        int dy = std::max({r.top() - pos.y(), 0, pos.y() - r.bottom()});
        if (dx + dy < bestDist)
        {
            bestDist = dx + dy;
            best = b->property("shelfIndex").toInt();
            bestRect = r;
        }
    }
    if (best < 0)
        return 0;
    return pos.x() > bestRect.center().x() ? best + 1 : best;
}

void ShelfPage::dragEnterEvent(QDragEnterEvent* e)
{
    if (shelves::isLocked(mShelf))
        return;
    if (e->mimeData()->hasFormat(kItemMime) || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void ShelfPage::dragMoveEvent(QDragMoveEvent* e)
{
    if (shelves::isLocked(mShelf))
        return;
    if (e->mimeData()->hasFormat(kItemMime) || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void ShelfPage::dropEvent(QDropEvent* e)
{
    if (e->mimeData()->hasFormat(kItemMime))
    {
        json src = json::parse(e->mimeData()->data(kItemMime).toStdString(), nullptr, false);
        if (!src.is_object())
            return;
        std::string from = src.value("shelf", ""), to = mShelf;
        int index = src.value("index", -1), at = dropIndex(e->position().toPoint());
        e->acceptProposedAction();
        // After the drag's own event loop has unwound: the move rebuilds the button that started it.
        QTimer::singleShot(0, qApp, [from, index, to, at] { moveItem(from, index, to, at); });
        return;
    }
    QString code = e->mimeData()->text();
    if (code.trimmed().isEmpty())
        return;
    shelves::addItem(mShelf, pythonItem(code));
    e->acceptProposedAction();
}

// ------------------------------------------------------------------- ShelfPanel

ShelfPanel::ShelfPanel(QWidget* parent) : QWidget(parent)
{
    // The app style sheet draws the shelf's tabs and buttons by these names.
    setObjectName("shelfPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    mTabs = new QTabBar(this);
    mTabs->setObjectName("shelfTabs");
    mTabs->setExpanding(false);
    mTabs->setDrawBase(false);
    mTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    mPages = new QStackedWidget(this);
    v->addWidget(mTabs);
    v->addWidget(mPages);
    connect(mTabs, &QTabBar::currentChanged, mPages, &QStackedWidget::setCurrentIndex);
    connect(mTabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i >= 0)
            shelves::notifyCurrentChanged(mTabs->tabText(i).toStdString());
    });
    connect(mTabs, &QTabBar::customContextMenuRequested, this, [this](QPoint p) {
        int tab = mTabs->tabAt(p);
        QMenu menu;
        menu.addAction(icon("folder-plus"), "New Shelf...", [this] {
            bool ok = false;
            QString name = QInputDialog::getText(this, "New Shelf", "Name", QLineEdit::Normal, "", &ok);
            if (ok && !name.trimmed().isEmpty())
                shelves::create(name.trimmed().toStdString());
        });
        if (tab >= 0 && shelves::isLocked(mTabs->tabText(tab).toStdString()))
            menu.addAction(icon("lock"), "Studio shelf, read-only")->setEnabled(false);
        else if (tab >= 0)
        {
            std::string name = mTabs->tabText(tab).toStdString();
            menu.addAction(icon("pencil"), "Edit Shelf...", [this, name] { editShelf(this, name); });
            menu.addAction(icon("trash"), "Delete Shelf", [name] { shelves::remove(name); });
        }
        menu.exec(mTabs->mapToGlobal(p));
    });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if ((file == "general" || file.starts_with("shelves/")) && shelves::names() != mNames)
            rebuild();
    });
    rebuild();
}

ShelfPanel::~ShelfPanel()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

std::string ShelfPanel::currentShelf() const
{
    return mTabs->currentIndex() >= 0 ? mTabs->tabText(mTabs->currentIndex()).toStdString() : std::string();
}

bool ShelfPanel::showShelf(const std::string& name)
{
    for (int i = 0; i < mTabs->count(); ++i)
        if (mTabs->tabText(i).toStdString() == name)
        {
            mTabs->setCurrentIndex(i);
            return true;
        }
    return false;
}

QWidget* ShelfPanel::dockTitleBar(QDockWidget* dock)
{
    // A plain widget ignores mouse presses, so they fall through to the dock and still drag it.
    auto* grip = new QWidget(dock);
    grip->setFixedHeight(4);
    grip->setAttribute(Qt::WA_StyledBackground);
    grip->setStyleSheet("background: #3a3a3a;");
    grip->setCursor(Qt::SizeAllCursor);
    grip->setToolTip("Drag to move the shelf");
    return grip;
}

void ShelfPanel::rebuild()
{
    QString current = mTabs->currentIndex() >= 0 ? mTabs->tabText(mTabs->currentIndex()) : QString();
    while (mTabs->count())
        mTabs->removeTab(0);
    while (mPages->count())
    {
        QWidget* w = mPages->widget(0);
        mPages->removeWidget(w);
        w->deleteLater();
    }
    mNames = shelves::names();
    for (const std::string& name : mNames)
    {
        const int tab = mTabs->addTab(QString::fromStdString(name));
        if (shelves::isLocked(name))
        {
            mTabs->setTabIcon(tab, icon("lock"));
            mTabs->setTabToolTip(tab, "Studio shelf, shared from the network and read-only");
        }
        mPages->addWidget(new ShelfPage(name, false, mPages));
    }
    for (int i = 0; i < mTabs->count(); ++i)
        if (mTabs->tabText(i) == current)
            mTabs->setCurrentIndex(i);
}

// ------------------------------------------------------------------- FloatingShelf

namespace
{
QPointer<FloatingShelf> gFloating;
}

FloatingShelf* FloatingShelf::active() { return gFloating; }

std::string FloatingShelf::shelf() const
{
    return mPage ? mPage->shelf() : std::string();
}

void FloatingShelf::setIconSize(int size)
{
    size = shelves::stepFloatingIconSize(size, 0);
    if (size == shelves::floatingIconSize())
        return;
    PrefStore::get().set("general", "/floatingShelf/iconSize", size);
    mPage->rebuild();
}

FloatingShelf::FloatingShelf(QWidget* parent, bool pinned) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint), mPinned(pinned)
{
    setWindowTitle("Floating Shelf");
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    if (!pinned)
        setAttribute(Qt::WA_ShowWithoutActivating);
    // Qt::Popup would close the moment another app has focus, so click-away is handled here instead.
    qApp->installEventFilter(this);
    mPositionTimer.setSingleShot(true);
    mPositionTimer.setInterval(300);
    connect(&mPositionTimer, &QTimer::timeout, this, &FloatingShelf::rememberPosition);
    build();
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            reloadPicker();
    });
}

FloatingShelf::~FloatingShelf()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

void FloatingShelf::build()
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(2);
    auto* top = new QHBoxLayout;
    top->setSpacing(1);
    mGrip = new QLabel(this);
    mGrip->setPixmap(icon("grip-vertical").pixmap(16, 16));
    mGrip->setCursor(Qt::SizeAllCursor);
    mGrip->setToolTip("Drag to move the shelf");
    top->addWidget(mGrip);
    mPicker = new QComboBox(this);
    mPicker->setMinimumWidth(110);
    mPicker->setToolTip("Shelf");
    top->addWidget(mPicker, 1);

    auto tool = [&](const char* iconName, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(icon(iconName));
        b->setIconSize({16, 16});
        b->setAutoRaise(true);
        b->setToolTip(tip);
        top->addWidget(b);
        return b;
    };
    QToolButton* newShelf = tool("folder-plus", "New shelf");
    QToolButton* add = tool("code-plus", "Add a button from Python, a command or an action");
    QToolButton* edit = tool("pencil", "Edit this shelf: rename, reorder, edit buttons");
    QToolButton* del = tool("trash", "Delete this shelf");
    QToolButton* settings = tool("settings", "Floating shelf settings");
    QToolButton* help = tool("help", "How the floating shelf works");
    top->addSpacing(6);
    QToolButton* closeButton = tool("x", "Close the floating shelf (`)");
    connect(closeButton, &QToolButton::clicked, this, &QWidget::close);
    v->addLayout(top);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    mPage = new ShelfPage(PrefStore::get().value<std::string>("general", "/shelves/floatingTab", ""), true, scroll);
    scroll->setWidget(mPage);
    v->addWidget(scroll, 1);
    connect(mPage, &ShelfPage::triggered, this, [this] {
        if (!mPinned && PrefStore::get().value<bool>("general", "/floatingShelf/closeAfterRun", false))
            close();
    });
    reloadPicker();
    connect(mPicker, &QComboBox::currentIndexChanged, this, [this](int) { showShelf(mPicker->currentText().toStdString()); });

    connect(newShelf, &QToolButton::clicked, this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, "New Shelf", "Name", QLineEdit::Normal, "", &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        shelves::create(name.trimmed().toStdString());
        showShelf(name.trimmed().toStdString());
        reloadPicker();
    });
    auto* addMenu = new QMenu(this);
    connect(addMenu, &QMenu::aboutToShow, this, [this, addMenu] {
        addMenu->clear();
        mPage->fillAddMenu(addMenu);
    });
    add->setMenu(addMenu);
    add->setPopupMode(QToolButton::InstantPopup);
    connect(edit, &QToolButton::clicked, this, [this] {
        std::string name = editShelf(this, mPage->shelf());
        if (name != mPage->shelf())
        {
            showShelf(name);
            reloadPicker();
        }
    });
    connect(del, &QToolButton::clicked, this, [this] {
        std::string name = mPage->shelf();
        if (QMessageBox::question(this, "Delete Shelf", QString("Delete the shelf \"%1\"?").arg(QString::fromStdString(name))) ==
            QMessageBox::Yes)
            shelves::remove(name);
    });
    auto* settingsMenuWidget = new QMenu(this);
    connect(settingsMenuWidget, &QMenu::aboutToShow, this, [this, settingsMenuWidget] {
        settingsMenuWidget->clear();
        settingsMenu(settingsMenuWidget);
    });
    settings->setMenu(settingsMenuWidget);
    settings->setPopupMode(QToolButton::InstantPopup);
    connect(help, &QToolButton::clicked, this, [this] {
        QMessageBox::information(this, "Floating Shelf",
                                 "`: open or close the floating shelf. Hold `, then release over a button to run it.\n"
                                 "Hold F1 for the hotkey cheat sheet, or tap it to search hotkeys.\n\n"
                                 "Click a button to run it. Drag a button to reorder it, or onto another shelf.\n"
                                 "Right-click a button to edit it, change its icon or delete it.\n"
                                 "+ adds a button. Ctrl+Shift+click a menu item to add it to this shelf.\n"
                                 "Ctrl+wheel scales the icons, Ctrl+middle-click resets their size.\n"
                                 "Where the shelf opens is under Settings.");
    });
}

void FloatingShelf::settingsMenu(QMenu* menu)
{
    QMenu* openAt = menu->addMenu("Open At");
    auto* group = new QActionGroup(openAt);
    std::string current = PrefStore::get().value<std::string>("general", "/floatingShelf/openAt", "viewport");
    for (auto [label, value] : {std::pair{"Cursor", "cursor"}, {"Center of Viewport", "viewport"}, {"Center of Screen", "screen"}})
    {
        QAction* a = openAt->addAction(label);
        a->setCheckable(true);
        a->setChecked(current == value);
        group->addAction(a);
        std::string v = value;
        connect(a, &QAction::triggered, this, [v] { PrefStore::get().set("general", "/floatingShelf/openAt", v); });
    }
    for (auto [label, key] : {std::pair{"Close When Clicking Elsewhere", "/floatingShelf/closeOnClickAway"},
                              {"Close After Running a Button", "/floatingShelf/closeAfterRun"},
                              {"Remember Last Position", "/floatingShelf/rememberPosition"}})
    {
        QAction* a = menu->addAction(label);
        a->setCheckable(true);
        a->setChecked(PrefStore::get().value<bool>("general", key, false));
        std::string k = key;
        connect(a, &QAction::toggled, this, [k](bool on) { PrefStore::get().set("general", k, on); });
    }
    menu->addSeparator();
    menu->addAction(icon("settings"), "Preferences...", [] { actions::run(json("window.preferences")); });
}

void FloatingShelf::reloadPicker()
{
    std::vector<std::string> names = shelves::names();
    QStringList wanted;
    for (const std::string& n : names)
        wanted.push_back(QString::fromStdString(n));
    QStringList have;
    for (int i = 0; i < mPicker->count(); ++i)
        have.push_back(mPicker->itemText(i));
    std::string shown = mPage->shelf();
    if (std::find(names.begin(), names.end(), shown) == names.end())
        shown = names.empty() ? std::string() : names.front();
    if (have != wanted)
    {
        QSignalBlocker block(mPicker);
        mPicker->clear();
        mPicker->addItems(wanted);
    }
    {
        QSignalBlocker block(mPicker);
        mPicker->setCurrentText(QString::fromStdString(shown));
    }
    if (shown != mPage->shelf())
        mPage->setShelf(shown);
}

void FloatingShelf::showShelf(const std::string& name)
{
    if (name == mPage->shelf())
        return;
    mPage->setShelf(name);
    PrefStore::get().set("general", "/shelves/floatingTab", name);
}

void FloatingShelf::place()
{
    QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    QRect avail = screen->availableGeometry();
    std::string at = PrefStore::get().value<std::string>("general", "/floatingShelf/openAt", "viewport");
    QSize s = size();
    QPoint topLeft = cursor - QPoint(s.width() / 2, 40);
    if (at == "screen")
        topLeft = avail.center() - QPoint(s.width() / 2, s.height() / 2);
    else if (at == "viewport")
    {
        QWidget* view = nullptr;
        if (QMainWindow* mw = mainWindow())
            if (auto* layout = mw->findChild<ViewportLayout*>())
                view = layout->active() ? static_cast<QWidget*>(layout->active()) : layout;
        if (view)
            topLeft = view->mapToGlobal(view->rect().center()) - QPoint(s.width() / 2, s.height() / 2);
    }
    topLeft += QPoint(PrefStore::get().value<int>("general", "/floatingShelf/offsetX", 250), PrefStore::get().value<int>("general", "/floatingShelf/offsetY", 0));
    if (json last = PrefStore::get().value("general", "/floatingShelf/position", json());
        PrefStore::get().value<bool>("general", "/floatingShelf/rememberPosition", false) && last.is_array() && last.size() == 2 &&
        last[0].is_number_integer() && last[1].is_number_integer())
    {
        topLeft = QPoint(last[0].get<int>(), last[1].get<int>());
        // A monitor unplugged since then: back onto the one under the cursor.
        if (QScreen* there = QGuiApplication::screenAt(topLeft + QPoint(s.width() / 2, 16)))
            avail = there->availableGeometry();
    }
    topLeft.setX(std::clamp(topLeft.x(), avail.left(), std::max(avail.left(), avail.right() - s.width())));
    topLeft.setY(std::clamp(topLeft.y(), avail.top(), std::max(avail.top(), avail.bottom() - s.height())));
    move(topLeft);
}

void FloatingShelf::open(bool pinned)
{
    auto* s = new FloatingShelf(mainWindow(), pinned);
    gFloating = s;
    json size = PrefStore::get().value("general", "/floatingShelf/size", json::array({380, 210}));
    if (size.is_array() && size.size() == 2 && size[0].is_number() && size[1].is_number())
        s->resize(std::max(160, size[0].get<int>()), std::max(90, size[1].get<int>()));
    s->place();
    s->mOpenedAt = QDateTime::currentMSecsSinceEpoch();
    s->show();
    if (pinned)
    {
        s->raise();
        s->activateWindow();
    }
}

void FloatingShelf::toggle()
{
    if (gFloating)
        gFloating->close();
    else
        open(true);
}

void FloatingShelf::popup()
{
    if (gFloating)
        gFloating->close();
    else
        open(false);
}

void FloatingShelf::releaseKey()
{
    FloatingShelf* s = gFloating;
    if (!s || s->mPinned)
        return;
    // A quick tap leaves the shelf open for clicking; after a hold, releasing over a button runs it.
    if (QDateTime::currentMSecsSinceEpoch() - s->mOpenedAt < 250)
        return;
    QWidget* under = QApplication::widgetAt(QCursor::pos());
    if (auto* b = qobject_cast<QToolButton*>(under); b && s->isAncestorOf(b) && b->property("shelfIndex").isValid())
        b->click();
}

bool FloatingShelf::eventFilter(QObject* obj, QEvent* e)
{
    auto* inside = qobject_cast<QWidget*>(obj);
    if (inside && inside->window() == this)
    {
        if (e->type() == QEvent::Enter && obj != this)
            unsetCursor();
        if (e->type() == QEvent::MouseMove)
        {
            Qt::Edges edges = obj == this ? frameEdges(static_cast<QMouseEvent*>(e)->position().toPoint()) : Qt::Edges();
            if (edges)
                setCursor(edgeCursor(edges));
            else
                unsetCursor();
        }
        if (e->type() == QEvent::MouseButtonPress && (obj == mGrip || obj == this) &&
            static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton && windowHandle())
        {
            Qt::Edges edges = obj == this ? frameEdges(static_cast<QMouseEvent*>(e)->position().toPoint()) : Qt::Edges();
            if (edges)
                windowHandle()->startSystemResize(edges);
            else
                windowHandle()->startSystemMove();
            return true;
        }
        if (e->type() == QEvent::Wheel)
            if (auto* we = static_cast<QWheelEvent*>(e); we->modifiers() & Qt::ControlModifier)
            {
                // Touchpads send fractions of a notch; they add up over the gesture.
                mWheelAccum += we->angleDelta().y();
                if (int notches = mWheelAccum / 120; notches != 0)
                {
                    mWheelAccum -= notches * 120;
                    setIconSize(shelves::stepFloatingIconSize(shelves::floatingIconSize(), notches));
                }
                return true;
            }
        if (e->type() == QEvent::MouseButtonPress)
            if (auto* me = static_cast<QMouseEvent*>(e); me->button() == Qt::MiddleButton && (me->modifiers() & Qt::ControlModifier))
            {
                setIconSize(shelves::kFloatingIconDefault);
                return true;
            }
    }
    // Menus and dialogs opened from the shelf are other windows; clicks in them are not click-away.
    // Ctrl+Shift clicks add menu items to this shelf, so they keep it open.
    if (!mPinned && e->type() == QEvent::MouseButtonPress && !QApplication::activePopupWidget() && !QApplication::activeModalWidget() &&
        !shelves::addModifierHeld() && PrefStore::get().value<bool>("general", "/floatingShelf/closeOnClickAway", false))
    {
        auto* w = qobject_cast<QWidget*>(obj);
        if (w && w->window() != this)
            close();
    }
    return QWidget::eventFilter(obj, e);
}

Qt::Edges FloatingShelf::frameEdges(QPoint p) const
{
    // The layout margin is the frame: a press there resizes from that side.
    constexpr int kBand = 5;
    Qt::Edges edges;
    if (p.x() < kBand)
        edges |= Qt::LeftEdge;
    if (p.x() >= width() - kBand)
        edges |= Qt::RightEdge;
    if (p.y() < kBand)
        edges |= Qt::TopEdge;
    if (p.y() >= height() - kBand)
        edges |= Qt::BottomEdge;
    return edges;
}

Qt::CursorShape FloatingShelf::edgeCursor(Qt::Edges edges)
{
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
        return Qt::SizeFDiagCursor;
    if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
        return Qt::SizeBDiagCursor;
    if (edges & (Qt::LeftEdge | Qt::RightEdge))
        return Qt::SizeHorCursor;
    if (edges & (Qt::TopEdge | Qt::BottomEdge))
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

void FloatingShelf::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    p.setPen(palette().mid().color());
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

void FloatingShelf::rememberPosition()
{
    if (PrefStore::get().value<bool>("general", "/floatingShelf/rememberPosition", false))
        PrefStore::get().set("general", "/floatingShelf/position", json::array({pos().x(), pos().y()}));
}

void FloatingShelf::moveEvent(QMoveEvent* e)
{
    QWidget::moveEvent(e);
    if (isVisible())
        mPositionTimer.start();
}

void FloatingShelf::closeEvent(QCloseEvent* e)
{
    PrefStore::get().set("general", "/floatingShelf/size", json::array({width(), height()}));
    mPositionTimer.stop();
    rememberPosition();
    QWidget::closeEvent(e);
}

void FloatingShelf::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape)
    {
        close();
        return;
    }
    QWidget::keyPressEvent(e);
}

namespace
{
nlohmann::json shelfItems(std::initializer_list<std::array<const char*, 4>> items)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [label, iconName, tooltip, target] : items)
        out.push_back({{"label", label}, {"icon", iconName}, {"tooltip", tooltip}, {"target", target}});
    return out;
}

/** Modeling and Rigging: listed after the user's own shelves, editable like any other. */
void registerBuiltInShelves()
{
    shelves::add({"Modeling", shelfItems({
                                  {"Cube", "cube", "Create a cube", "create.cube"},
                                  {"Quad Sphere", "sphere", "Create an all-quad sphere", "create.quadSphere"},
                                  {"Cylinder", "cylinder", "Create a cylinder", "create.cylinder"},
                                  {"Plane", "plane", "Create a plane", "create.plane"},
                                  {"Cone", "cone", "Create a cone", "create.cone"},
                                  {"Combine", "arrows-join", "Combine the selected meshes into one", "mesh.combine"},
                                  {"Separate", "arrows-split-2", "Separate a mesh into its loose parts", "mesh.separate"},
                                  {"Mirror", "flip-horizontal", "Mirror the mesh across an axis", "mesh.mirror"},
                                  {"Union", "layers-union", "Boolean union of the selected meshes", "mesh.booleanUnion"},
                                  {"Difference", "layers-subtract", "Boolean difference: the first minus the rest", "mesh.booleanDifference"},
                                  {"Symmetry", "columns-2", "Toggle modeling symmetry", "mesh.toggleSymmetry"},
                                  {"Freeze", "snowflake", "Freeze transforms", "modify.freezeTransforms"},
                                  {"Center Piv", "square-dot", "Center pivots on each object's bounds", "modify.centerPivot"},
                                  {"Reset Piv", "focus-centered", "Move the pivot back to each object's origin", "modify.resetPivot"},
                                  {"Zero Piv", "focus-2", "Move the pivot to the world origin", "modify.zeroPivot"},
                                  {"Match Piv", "replace", "Copy the first selected object's pivot onto the others", "modify.matchPivot"},
                                  {"Stack", "stack-2", "Show or hide the Modifier Stack", "window.modifierStack"},
                                  {"Toolkit", "tools", "Show or hide the Modeling Toolkit", "window.modelingToolkit"},
                              })});
    shelves::add({"Rigging", shelfItems({
                                 {"Joint", "bone", "Joint tool: click to place joints", "rig.jointTool"},
                                 {"Orient", "compass", "Orient the selected joints", "rig.orientJoint"},
                                 {"Mirror", "flip-horizontal", "Mirror the selected joint chains", "rig.mirrorJoints"},
                                 {"Bind", "link-plus", "Bind the selected meshes to the selected joints", "rig.bindSkin"},
                                 {"Unbind", "unlink", "Unbind the selected skinned meshes", "rig.unbindSkin"},
                                 {"Paint", "brush", "Paint skin weights", "rig.paintWeights"},
                                 {"Bind Pose", "user", "Go to the bind pose", "rig.goToBindPose"},
                                 {"Parent", "link", "Parent constraint: the last selected follows the others", "rig.parentConstraint"},
                                 {"Point", "point", "Point constraint", "rig.pointConstraint"},
                                 {"Orient C", "rotate", "Orient constraint", "rig.orientConstraint"},
                                 {"Aim", "target", "Aim constraint", "rig.aimConstraint"},
                                 {"IK", "vector", "IK handle from the start joint to the end joint", "rig.ikHandle"},
                                 {"X-Ray", "eye", "Show joints through meshes", "rig.xrayJoints"},
                             })});
}
} // namespace

void registerShelfActions()
{
    registerBuiltInShelves();
    actions::add({.id = "shelf.floatingToggle", .label = "Toggle Floating Shelf", .category = "Windows", .run = [] { FloatingShelf::toggle(); },
                  .doc = "Open the floating shelf, pinned until toggled again."});
    actions::add({.id = "shelf.floatingOpen", .label = "Floating Shelf", .category = "Windows", .run = [] { FloatingShelf::popup(); },
                  .doc = "Open the shelf at the cursor."});
    actions::add({.id = "shelf.floatingRelease", .label = "Floating Shelf (release)", .category = "Windows",
                  .run = [] { FloatingShelf::releaseKey(); },
                  .doc = "Release half of the floating shelf key: after a hold, runs the button under the cursor and closes the shelf."});
}
} // namespace lila::ui
