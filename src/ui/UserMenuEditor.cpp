#include "ui/UserMenuEditor.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/Icons.h"
#include "ui/LocateFlash.h"
#include "ui/Menus.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/UserMenus.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
constexpr int kPointerRole = Qt::UserRole;

std::string fieldText(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

QString plain(const std::string& s)
{
    return qs(tearoff::itemLabel(s));
}

/** The menu titles along a registered path: "Mesh/Symmetry" is {"Mesh", "Symmetry"}, with the menu's own title first. */
std::vector<std::string> titlesOf(const std::string& path)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t slash = path.find('/'); start <= path.size(); slash = path.find('/', start))
    {
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (out.empty())
            if (const menus::Menu* m = menus::menuRegistry().find(part))
                part = m->title;
        out.push_back(tearoff::itemLabel(part));
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return out;
}

bool isUserOwned(const std::string& itemId)
{
    const std::string* owner = menus::itemRegistry().ownerOf(itemId);
    return owner && *owner == userMenus::kOwner;
}

/** Every registered menu and submenu path, shown as "Mesh > Symmetry", leaving out what the open menu itself adds. */
void collectPaths(const std::string& path, const QString& shown, const std::string& skip, std::vector<std::pair<std::string, QString>>& out)
{
    if (!skip.empty() && (path == skip || path.starts_with(skip + "/")))
        return;
    out.emplace_back(path, shown);
    for (const menus::Item* i : menus::itemsIn(path))
        if (i->kind == menus::Kind::Submenu)
            collectPaths(path + "/" + i->label, shown + "  >  " + plain(i->label), skip, out);
}

QString rowText(const menus::Item* i)
{
    switch (i->kind)
    {
    case menus::Kind::Action: return qs(actions::displayLabel(i->action));
    case menus::Kind::Separator: return "Separator";
    case menus::Kind::Section: return "Section " + plain(i->label);
    case menus::Kind::Submenu: return plain(i->label) + "  >";
    case menus::Kind::Custom: return {};
    }
    return {};
}

QString targetText(const json& target)
{
    if (target.is_string())
        return qs(target.get<std::string>());
    if (target.contains("python"))
        return "Python";
    if (target.contains("command"))
        return qs(fieldText(target, "command"));
    return {};
}

UserMenuEditor::Target targetKind(const json& target)
{
    if (target.is_object() && target.contains("python"))
        return UserMenuEditor::Target::Python;
    if (target.is_object())
        return UserMenuEditor::Target::Command;
    return UserMenuEditor::Target::Action;
}

QPointer<UserMenuEditor> gEditor;
} // namespace

UserMenuEditor::UserMenuEditor(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    auto* split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split, 1);

    // Menus
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("Menus"));
    mMenus = new QListWidget;
    leftLayout->addWidget(mMenus, 1);
    auto* menuButtons = new QHBoxLayout;
    auto* newMenu = new QPushButton("New");
    newMenu->setToolTip("Add a menu on the menu bar");
    mDelete = new QPushButton("Delete");
    menuButtons->addWidget(newMenu);
    menuButtons->addWidget(mDelete);
    leftLayout->addLayout(menuButtons);
    split->addWidget(left);

    // Placement and rows
    auto* center = new QWidget;
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    auto* form = mPlaceForm = new QFormLayout;
    mTitle = new QLineEdit;
    form->addRow("Title", mTitle);
    mPlace = new QComboBox;
    mPlace->addItem("Menu bar", "bar");
    mPlace->addItem("Submenu", "submenu");
    mPlace->addItem("Section", "section");
    mPlace->setItemData(1, "A cascade inside another menu", Qt::ToolTipRole);
    mPlace->setItemData(2, "Rows inline in another menu, under the title as a heading", Qt::ToolTipRole);
    form->addRow("Place", mPlace);
    mParent = new QComboBox;
    mParent->setEditable(true);
    mParent->setInsertPolicy(QComboBox::NoInsert);
    mParent->setMaxVisibleItems(24);
    form->addRow("In", mParent);
    mPosition = new QComboBox;
    mPosition->setMaxVisibleItems(24);
    form->addRow("Position", mPosition);
    centerLayout->addLayout(form);

    mTree = new QTreeWidget;
    mTree->setColumnCount(2);
    mTree->setHeaderLabels({"Item", "Target"});
    mTree->setColumnWidth(0, 220);
    mTree->setUniformRowHeights(true);
    centerLayout->addWidget(mTree, 1);

    auto* addRowButtons = new QHBoxLayout;
    auto addButton = [&](const char* text, const char* tip, json r) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, r] {
            json add = r;
            // A new action row starts on whatever the picker has selected.
            if (add.contains("action") && mActionList->currentItem())
                add["action"] = mActionList->currentItem()->data(Qt::UserRole).toString().toStdString();
            addRow(add);
        });
        addRowButtons->addWidget(b);
    };
    addButton("Action", "Add a row after the selected one; into a selected submenu", {{"action", ""}});
    addButton("Python", "Add a row that runs Python", {{"action", {{"python", ""}}}, {"label", "Script"}});
    addButton("Separator", "Add a divider line", {{"separator", true}});
    addButton("Section", "Add a titled divider; Shift+T can tear off its rows", {{"section", "Section"}});
    addButton("Submenu", "Add a cascade", {{"submenu", "Submenu"}, {"items", json::array()}});
    addRowButtons->addStretch(1);
    mUp = new QPushButton("Up");
    mDown = new QPushButton("Down");
    mRemove = new QPushButton("Remove");
    addRowButtons->addWidget(mUp);
    addRowButtons->addWidget(mDown);
    addRowButtons->addWidget(mRemove);
    centerLayout->addLayout(addRowButtons);
    split->addWidget(center);

    // The selected row
    mRowEditor = new QWidget;
    auto* rightLayout = new QVBoxLayout(mRowEditor);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    mRowTitle = new QLabel;
    rightLayout->addWidget(mRowTitle);
    auto* rowForm = mRowForm = new QFormLayout;
    mLabel = new QLineEdit;
    rowForm->addRow("Label", mLabel);
    mTarget = new QComboBox;
    mTarget->addItem("Action", int(Target::Action));
    mTarget->addItem("Python", int(Target::Python));
    mTarget->addItem("Command", int(Target::Command));
    rowForm->addRow("Runs", mTarget);
    mTooltip = new QLineEdit;
    rowForm->addRow("Tooltip", mTooltip);
    rightLayout->addLayout(rowForm);

    mPages = new QStackedWidget;
    auto* actionPage = new QWidget;
    auto* actionLayout = new QVBoxLayout(actionPage);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    mActionSearch = new QLineEdit;
    mActionSearch->setPlaceholderText("Search actions");
    mActionSearch->setClearButtonEnabled(true);
    actionLayout->addWidget(mActionSearch);
    mActionList = new QListWidget;
    mActionList->setUniformItemSizes(true);
    mActionList->setIconSize(QSize(16, 16));
    // Rows without an icon keep their text in line with the rest.
    QPixmap clear(16, 16);
    clear.fill(Qt::transparent);
    const QIcon blank(clear);
    for (const Action* a : actions::all())
    {
        if (a->id.starts_with("user."))
            continue;
        QIcon rowIcon = icon(qs(actions::iconFor(a->id)));
        auto* it = new QListWidgetItem(rowIcon.isNull() ? blank : rowIcon, qs(a->label + "    " + a->category));
        it->setData(Qt::UserRole, qs(a->id));
        it->setToolTip(qs(a->id + "\n" + a->doc));
        mActionList->addItem(it);
    }
    mActionList->sortItems();
    actionLayout->addWidget(mActionList);
    mPages->addWidget(actionPage);
    mPython = new QPlainTextEdit;
    mPython->setFont(monoFont());
    mPython->setPlaceholderText("Python run when the row is picked");
    mPython->setTabStopDistance(QFontMetricsF(monoFont()).horizontalAdvance(' ') * 4);
    mPages->addWidget(mPython);
    mCommand = new QPlainTextEdit;
    mCommand->setFont(monoFont());
    mCommand->setPlaceholderText("{\"command\": \"polyBevel\", \"args\": {\"offset\": 0.2}}");
    mPages->addWidget(mCommand);
    rightLayout->addWidget(mPages, 100);
    rightLayout->addStretch(1);
    split->addWidget(mRowEditor);

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 2);
    split->setSizes({160, 460, 380});

    auto* bottom = new QHBoxLayout;
    mProblem = new QLabel;
    mProblem->setStyleSheet(QString("color: %1").arg(themeColor("statusError").name()));
    bottom->addWidget(mProblem, 1);
    mTearOff = new QPushButton("Tear Off");
    mTearOff->setToolTip("Tear the saved menu off under the cursor; click to drop it");
    mRevert = new QPushButton("Revert");
    mSave = new QPushButton("Save");
    bottom->addWidget(mTearOff);
    bottom->addWidget(mRevert);
    bottom->addWidget(mSave);
    outer->addLayout(bottom);

    connect(mMenus, &QListWidget::currentRowChanged, this, [this](int i) {
        if (mLoading)
            return;
        mCur = i;
        mSel.clear();
        showMenu();
    });
    connect(newMenu, &QPushButton::clicked, this, [this] {
        std::string title = "Menu";
        for (int n = 2; !userMenus::problem({{"title", title}}, mDoc).empty(); ++n)
            title = "Menu " + std::to_string(n);
        mDoc.push_back({{"title", title}, {"parent", ""}, {"items", json::array()}});
        mCur = int(mDoc.size()) - 1;
        mSel.clear();
        setDirty(true);
        showMenu();
        mTitle->setFocus();
        mTitle->selectAll();
    });
    connect(mDelete, &QPushButton::clicked, this, [this] {
        if (!menu())
            return;
        mDoc.erase(mDoc.begin() + mCur);
        mCur = std::min(mCur, int(mDoc.size()) - 1);
        mSel.clear();
        setDirty(true);
        showMenu();
    });

    connect(mTitle, &QLineEdit::textEdited, this, [this](const QString& t) {
        editMenu([&](json& m) { m["title"] = t.toStdString(); });
        if (QListWidgetItem* it = mMenus->item(mCur))
            it->setText(t);
    });
    connect(mPlace, &QComboBox::activated, this, [this] {
        std::string place = currentChoice(mPlace);
        editMenu([&](json& m) {
            if (place == "bar")
            {
                m["parent"] = "";
                m.erase("as");
                m.erase("after");
            }
            else
            {
                if (fieldText(m, "parent").empty())
                    m["parent"] = mParent->count() ? mParent->itemData(0).toString().toStdString() : "File";
                m["as"] = place;
                m.erase("before");
            }
        });
        refreshPlacement();
    });
    connect(mParent, &QComboBox::activated, this, [this](int i) {
        std::string path = mParent->itemData(i).toString().toStdString();
        editMenu([&](json& m) {
            m["parent"] = path;
            m.erase("after");
        });
        refreshPositions();
    });
    connect(mPosition, &QComboBox::activated, this, [this](int i) {
        std::string anchor = mPosition->itemData(i).toString().toStdString();
        bool bar = currentChoice(mPlace) == "bar";
        editMenu([&](json& m) {
            const char* key = bar ? "before" : "after";
            if (anchor.empty())
                m.erase(key);
            else
                m[key] = anchor;
        });
    });

    connect(mTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (mLoading)
            return;
        mSel = it ? it->data(0, kPointerRole).toString().toStdString() : std::string();
        showRow();
    });
    connect(mUp, &QPushButton::clicked, this, [this] { moveRow(-1); });
    connect(mDown, &QPushButton::clicked, this, [this] { moveRow(1); });
    connect(mRemove, &QPushButton::clicked, this, [this] { removeRow(); });
    deletion::claim(mTree, [this] { removeRow(); });
    deletion::claim(mMenus, nullptr);
    deletion::claim(mActionList, nullptr);

    connect(mLabel, &QLineEdit::textEdited, this, [this](const QString& t) {
        editRow([&](json& r) {
            std::string label = t.toStdString();
            if (r.contains("section"))
                r["section"] = label;
            else if (r.contains("submenu"))
                r["submenu"] = label;
            else if (label.empty())
                r.erase("label");
            else
                r["label"] = label;
        });
    });
    connect(mTooltip, &QLineEdit::textEdited, this, [this](const QString& t) {
        editRow([&](json& r) {
            if (t.isEmpty())
                r.erase("doc");
            else
                r["doc"] = t.toStdString();
        });
    });
    connect(mTarget, &QComboBox::activated, this, [this] {
        auto kind = Target(mTarget->currentData().toInt());
        editRow([&](json& r) {
            json old = r.value("action", json());
            if (kind == targetKind(old))
                return;
            if (kind == Target::Python)
                r["action"] = {{"python", old.is_string() && !old.get<std::string>().empty() ? actions::pythonFor(old) : fieldText(old, "python")}};
            else if (kind == Target::Command)
                r["action"] = {{"command", old.is_string() && actions::find(old.get<std::string>()) ? actions::find(old.get<std::string>())->command : ""},
                               {"args", old.is_string() && actions::find(old.get<std::string>()) ? actions::find(old.get<std::string>())->call : json::object()}};
            else
                r["action"] = "";
        });
        showRow();
    });
    connect(mActionSearch, &QLineEdit::textChanged, this, [this](const QString& q) {
        QStringList words = q.split(' ', Qt::SkipEmptyParts);
        for (int i = 0; i < mActionList->count(); ++i)
        {
            QListWidgetItem* it = mActionList->item(i);
            QString hay = it->text() + " " + it->data(Qt::UserRole).toString();
            bool match = std::ranges::all_of(words, [&](const QString& w) { return hay.contains(w, Qt::CaseInsensitive); });
            it->setHidden(!match);
        }
    });
    connect(mActionList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (!row() || !row()->contains("action") || !row()->at("action").is_string())
            return;
        editRow([&](json& r) { r["action"] = it->data(Qt::UserRole).toString().toStdString(); });
        showRow();
    });
    connect(mActionList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        addRow({{"action", it->data(Qt::UserRole).toString().toStdString()}});
    });
    connect(mPython, &QPlainTextEdit::textChanged, this, [this] {
        if (!mLoading)
            editRow([&](json& r) { r["action"] = {{"python", mPython->toPlainText().toStdString()}}; });
    });
    connect(mCommand, &QPlainTextEdit::textChanged, this, [this] {
        if (mLoading)
            return;
        json j = json::parse(mCommand->toPlainText().toStdString(), nullptr, false);
        if (j.is_object() && j.contains("command"))
            editRow([&](json& r) { r["action"] = j; });
        else
            mProblem->setText("Command: give {\"command\": name, \"args\": {...}}");
    });

    connect(mSave, &QPushButton::clicked, this, &UserMenuEditor::save);
    connect(mRevert, &QPushButton::clicked, this, &UserMenuEditor::load);
    connect(mTearOff, &QPushButton::clicked, this, &UserMenuEditor::tearOffMenu);

    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == userMenus::kPrefFile && !mDirty)
            load();
    });
    load();
    resize(1080, 620);
}

UserMenuEditor::~UserMenuEditor()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

void UserMenuEditor::closeEvent(QCloseEvent* e)
{
    if (mDirty)
    {
        auto answer = QMessageBox::question(this, "Menu Editor", "Save changes to your menus?",
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && (save(), mDirty)))
        {
            e->ignore();
            return;
        }
    }
    QWidget::closeEvent(e);
}

void UserMenuEditor::load()
{
    std::string title = menu() ? fieldText(*menu(), "title") : std::string();
    mDoc = userMenus::all();
    mCur = mDoc.empty() ? -1 : 0;
    for (size_t i = 0; i < mDoc.size(); ++i)
        if (fieldText(mDoc[i], "title") == title)
            mCur = int(i);
    setDirty(false);
    showMenu();
}

void UserMenuEditor::openMenu(const std::string& title)
{
    for (size_t i = 0; i < mDoc.size(); ++i)
        if (fieldText(mDoc[i], "title") == title)
        {
            mCur = int(i);
            mSel.clear();
            showMenu();
            return;
        }
}

void UserMenuEditor::selectRow(const std::string& pointer)
{
    mSel = pointer;
    rebuildTree();
    showRow();
}

void UserMenuEditor::save()
{
    for (size_t i = 0; i < mDoc.size(); ++i)
    {
        json others = mDoc;
        others.erase(others.begin() + i);
        if (std::string p = userMenus::problem(mDoc[i], others); !p.empty())
        {
            mCur = int(i);
            showMenu();
            mProblem->setText(qs(p));
            return;
        }
    }
    setDirty(false);
    userMenus::setAll(mDoc);
    refreshPlacement();
}

void UserMenuEditor::setDirty(bool dirty)
{
    mDirty = dirty;
    mSave->setEnabled(dirty);
    mRevert->setEnabled(dirty);
    mTearOff->setEnabled(!dirty && menu());
    setWindowTitle(dirty ? "Menu Editor *" : "Menu Editor");
}

void UserMenuEditor::refreshMenuList()
{
    mLoading = true;
    mMenus->clear();
    for (const json& m : mDoc)
        mMenus->addItem(qs(fieldText(m, "title")));
    mMenus->setCurrentRow(mCur);
    mLoading = false;
}

void UserMenuEditor::showMenu()
{
    refreshMenuList();
    json* m = menu();
    mDelete->setEnabled(m);
    for (QWidget* w : std::initializer_list<QWidget*>{mTitle, mPlace, mParent, mPosition, mTree})
        w->setEnabled(m);
    mLoading = true;
    mTitle->setText(m ? qs(fieldText(*m, "title")) : QString());
    mLoading = false;
    refreshPlacement();
    rebuildTree();
    showRow();
}

void UserMenuEditor::refreshPlacement()
{
    json* m = menu();
    std::string parent = m ? fieldText(*m, "parent") : std::string();
    std::string place = parent.empty() ? "bar" : fieldText(*m, "as") == "section" ? "section" : "submenu";
    setChoice(mPlace, place);
    mPlaceForm->setRowVisible(mParent, place != "bar");

    mParent->clear();
    std::string skip = m && place != "section" ? userMenus::rowsPath(*m) : std::string();
    std::vector<std::pair<std::string, QString>> paths;
    for (menus::Area area : {menus::Area::MenuBar, menus::Area::Hotbox})
        for (const menus::Menu* menu : menus::menusIn(area))
            collectPaths(menu->id, (area == menus::Area::Hotbox ? "Hotbox  >  " : "") + plain(menu->title), skip, paths);
    for (const auto& [path, shown] : paths)
        mParent->addItem(shown, qs(path));
    int at = mParent->findData(qs(parent));
    if (at < 0 && !parent.empty())
    {
        mParent->addItem(qs(parent) + "  (not loaded)", qs(parent));
        at = mParent->count() - 1;
    }
    mParent->setCurrentIndex(at);
    refreshPositions();
}

void UserMenuEditor::refreshPositions()
{
    json* m = menu();
    mPosition->clear();
    if (!m)
        return;
    std::string parent = fieldText(*m, "parent");
    std::string self = "user:" + fieldText(*m, "title");
    if (parent.empty())
    {
        for (const menus::Menu* bar : menus::menusIn(menus::Area::MenuBar))
            if (bar->id != self)
                mPosition->addItem("Before " + plain(bar->title), qs(bar->id));
        setChoice(mPosition, fieldText(*m, "before").empty() ? "Help" : fieldText(*m, "before"));
        return;
    }
    for (const menus::Item* i : menus::itemsIn(parent))
    {
        // The user menus' own rows move with them; only their headings are places to anchor to.
        if (i->id == self || (isUserOwned(i->id) && !i->id.starts_with("user:")) || i->kind == menus::Kind::Custom)
            continue;
        mPosition->addItem("After " + rowText(i), qs(i->id));
    }
    mPosition->addItem("End", "");
    std::string after = fieldText(*m, "after");
    mPosition->setCurrentIndex(after.empty() ? mPosition->count() - 1 : std::max(0, mPosition->findData(qs(after))));
}

void UserMenuEditor::rebuildTree()
{
    mLoading = true;
    mTree->clear();
    QTreeWidgetItem* selected = nullptr;
    std::function<void(QTreeWidgetItem*, const json&, const std::string&)> add = [&](QTreeWidgetItem* parent, const json& rows,
                                                                                   const std::string& base) {
        if (!rows.is_array())
            return;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const json& r = rows[i];
            std::string ptr = base + "/" + std::to_string(i);
            auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(mTree);
            it->setData(0, kPointerRole, qs(ptr));
            if (r.contains("separator"))
            {
                it->setText(0, "Separator");
                it->setForeground(0, themeShade(120));
            }
            else if (r.contains("section"))
            {
                it->setText(0, qs(fieldText(r, "section")));
                QFont f = it->font(0);
                f.setBold(true);
                it->setFont(0, f);
                it->setText(1, "Section");
            }
            else if (r.contains("submenu"))
            {
                it->setText(0, qs(fieldText(r, "submenu")));
                it->setText(1, "Submenu");
                add(it, r.value("items", json::array()), ptr + "/items");
                it->setExpanded(true);
            }
            else
            {
                const json& target = r.value("action", json());
                std::string label = fieldText(r, "label");
                if (label.empty())
                    label = target.is_string() && target.get<std::string>().empty() ? "" : actions::labelFor(target);
                it->setText(0, label.empty() ? "Pick an action" : qs(label));
                it->setText(1, targetText(target));
                if (target.is_string())
                    it->setIcon(0, icon(qs(actions::iconFor(target.get<std::string>()))));
                if (label.empty())
                    it->setForeground(0, themeShade(120));
            }
            if (ptr == mSel)
                selected = it;
        }
    };
    if (json* m = menu())
        add(nullptr, m->value("items", json::array()), "/items");
    mTree->setCurrentItem(selected);
    mLoading = false;
    refreshProblem();
}

void UserMenuEditor::showRow()
{
    json* r = row();
    bool action = r && r->contains("action");
    bool named = r && (r->contains("section") || r->contains("submenu"));
    mRowEditor->setEnabled(action || named);
    mUp->setEnabled(r);
    mDown->setEnabled(r);
    mRemove->setEnabled(r);
    mLoading = true;
    mRowTitle->setText(!r ? "No row selected" : r->contains("separator") ? "Separator" : r->contains("section") ? "Section"
                                                                            : r->contains("submenu")        ? "Submenu"
                                                                                                            : "Row");
    mLabel->setText(!r ? QString() : r->contains("section") ? qs(fieldText(*r, "section")) : r->contains("submenu") ? qs(fieldText(*r, "submenu")) : qs(fieldText(*r, "label")));
    json target = action ? r->at("action") : json();
    mLabel->setPlaceholderText(action ? qs(actions::labelFor(target)) : QString());
    mTooltip->setText(action ? qs(fieldText(*r, "doc")) : QString());
    if (json* m = menu(); m && action)
        mTooltip->setPlaceholderText(qs(fieldText(*m, "title")) + " > " + (mLabel->text().isEmpty() ? mLabel->placeholderText() : mLabel->text()));
    mRowForm->setRowVisible(mTooltip, action);
    mRowForm->setRowVisible(mTarget, action);
    mRowForm->setRowVisible(mLabel, action || named);
    mPages->setVisible(action);
    Target kind = targetKind(target);
    mTarget->setCurrentIndex(mTarget->findData(int(kind)));
    mPages->setCurrentIndex(int(kind));
    if (kind == Target::Python)
        mPython->setPlainText(qs(fieldText(target, "python")));
    if (kind == Target::Command)
        mCommand->setPlainText(qs(target.dump(2)));
    mActionList->clearSelection();
    if (kind == Target::Action && target.is_string())
        for (int i = 0; i < mActionList->count(); ++i)
            if (mActionList->item(i)->data(Qt::UserRole).toString().toStdString() == target.get<std::string>())
            {
                mActionList->setCurrentRow(i);
                mActionList->scrollToItem(mActionList->item(i));
                break;
            }
    mLoading = false;
}

void UserMenuEditor::refreshProblem()
{
    json* m = menu();
    if (!m)
    {
        mProblem->setText(mDoc.empty() ? "No menus yet: New adds one" : QString());
        return;
    }
    json others = mDoc;
    others.erase(others.begin() + mCur);
    mProblem->setText(qs(userMenus::problem(*m, others)));
}

json* UserMenuEditor::menu()
{
    return mCur >= 0 && mCur < int(mDoc.size()) ? &mDoc[size_t(mCur)] : nullptr;
}

json* UserMenuEditor::row()
{
    json* m = menu();
    if (!m || mSel.empty())
        return nullptr;
    json::json_pointer p(mSel);
    return m->contains(p) ? &m->at(p) : nullptr;
}

json* UserMenuEditor::rowList(size_t* index)
{
    json* m = menu();
    if (!m)
        return nullptr;
    if (!row())
    {
        *index = (*m)["items"].size();
        return &(*m)["items"];
    }
    json::json_pointer p(mSel);
    *index = std::stoul(p.back());
    return &m->at(p.parent_pointer());
}

void UserMenuEditor::editMenu(const std::function<void(json&)>& fn)
{
    if (mLoading)
        return;
    if (json* m = menu())
    {
        fn(*m);
        setDirty(true);
        refreshProblem();
    }
}

void UserMenuEditor::editRow(const std::function<void(json&)>& fn)
{
    if (mLoading)
        return;
    if (json* r = row())
    {
        fn(*r);
        setDirty(true);
        std::string keep = mSel;
        rebuildTree();
        mSel = keep;
    }
}

void UserMenuEditor::addRow(json r)
{
    json* m = menu();
    if (!m)
        return;
    if (!m->contains("items"))
        (*m)["items"] = json::array();
    std::string base;
    size_t at = 0;
    json* list = nullptr;
    if (json* sel = row(); sel && sel->contains("submenu"))
    {
        base = mSel + "/items";
        list = &(*sel)["items"];
        at = list->size();
    }
    else
    {
        list = rowList(&at);
        base = row() ? json::json_pointer(mSel).parent_pointer().to_string() : "/items";
        if (row())
            ++at;
    }
    list->insert(list->begin() + std::min(at, list->size()), std::move(r));
    mSel = base + "/" + std::to_string(at);
    setDirty(true);
    rebuildTree();
    showRow();
}

void UserMenuEditor::removeRow()
{
    size_t at = 0;
    json* list = row() ? rowList(&at) : nullptr;
    if (!list)
        return;
    list->erase(list->begin() + at);
    json::json_pointer p(mSel);
    mSel = list->empty() ? std::string() : p.parent_pointer().to_string() + "/" + std::to_string(std::min(at, list->size() - 1));
    setDirty(true);
    rebuildTree();
    showRow();
}

void UserMenuEditor::moveRow(int delta)
{
    size_t at = 0;
    json* list = row() ? rowList(&at) : nullptr;
    if (!list || (delta < 0 && at == 0) || at + delta >= list->size())
        return;
    std::swap((*list)[at], (*list)[at + delta]);
    mSel = json::json_pointer(mSel).parent_pointer().to_string() + "/" + std::to_string(at + delta);
    setDirty(true);
    rebuildTree();
    showRow();
}

void UserMenuEditor::tearOffMenu()
{
    json* m = menu();
    if (!m)
        return;
    std::string parent = fieldText(*m, "parent");
    std::string title = fieldText(*m, "title");
    bool section = !parent.empty() && fieldText(*m, "as") == "section";
    std::vector<std::string> path = parent.empty() ? std::vector<std::string>{} : titlesOf(parent);
    if (!section)
        path.push_back(tearoff::itemLabel(title));
    QMenu* q = tearoff::findMenu(mainWindow(), path);
    if (!q)
    {
        mProblem->setText("Not on the menu bar in the current menu set");
        return;
    }
    tearoff::tearOffToPlace(q, section ? qs(title) : QString());
}

void openUserMenuEditor()
{
    if (!gEditor)
    {
        gEditor = new UserMenuEditor(mainWindow());
        gEditor->setWindowFlag(Qt::Window);
        gEditor->setAttribute(Qt::WA_DeleteOnClose);
    }
    gEditor->show();
    gEditor->raise();
    gEditor->activateWindow();
    flashLocate(gEditor);
}
} // namespace lila::ui
