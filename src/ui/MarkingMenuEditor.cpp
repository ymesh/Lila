#include "ui/MarkingMenuEditor.h"
#include "ui/MarkingMenu.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/HotkeyBindings.h"
#include "ui/HotkeyEditor.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace lila::ui
{
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
const char* kSlots[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

fs::path defaultsDir()
{
    return paths::resourcesDir() / "defaults" / "markingMenus";
}

fs::path userMenusDir()
{
    return PrefStore::get().userDir() / "markingMenus";
}

json readJson(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return json();
    std::ostringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false, true);
    return j.is_discarded() ? json() : j;
}

json defaultMenu(const std::string& name)
{
    json j = readJson(defaultsDir() / (name + ".json"));
    return j.is_object() ? j : json::object();
}

bool hasDefault(const std::string& name)
{
    std::error_code ec;
    return fs::exists(defaultsDir() / (name + ".json"), ec);
}

/** A menu the user made and hasn't saved yet. Set menus ("<set>/<name>") open under their base name's key. */
bool isNewUserMenu(const std::string& name)
{
    if (name.find('/') != std::string::npos || hasDefault(name))
        return false;
    std::vector<std::string> contributed = markingMenus::contributedNames();
    if (std::find(contributed.begin(), contributed.end(), name) != contributed.end())
        return false;
    json saved = PrefStore::get().merged("markingMenus/" + name);
    return !saved.is_object() || saved.empty();
}

bool hasHotkey(const std::string& menu)
{
    const json target = {{"markingMenu", menu}};
    for (const hotkeys::Binding& b : hotkeys::snapshot().bindings)
        if (b.press == target || b.hold == target || b.release == target ||
            std::find(b.buttons.begin(), b.buttons.end(), target) != b.buttons.end())
            return true;
    return false;
}

/** The RFC 7386 patch that turns from into to, so the user layer only holds what differs from the defaults. */
json mergePatch(const json& from, const json& to)
{
    if (!from.is_object() || !to.is_object())
        return to;
    json patch = json::object();
    for (auto& [k, v] : from.items())
        if (!to.contains(k))
            patch[k] = nullptr;
    for (auto& [k, v] : to.items())
        if (!from.contains(k))
            patch[k] = v;
        else if (from[k] != v)
            patch[k] = mergePatch(from[k], v);
    return patch;
}

int slotIndex(const std::string& name)
{
    for (int i = 0; i < 8; ++i)
        if (name == kSlots[i])
            return i;
    return -1;
}

QString targetSummary(const json& item)
{
    if (!item.is_object())
        return {};
    if (item.contains("children") && item["children"].is_object())
        return "submenu";
    const json& a = item.value("action", json());
    if (a.is_string())
        return QString::fromStdString(a.get<std::string>());
    if (a.is_object() && a.contains("python"))
        return "Python";
    if (a.is_object())
        return QString::fromStdString(a.dump());
    return "-";
}

QString itemLabel(const json& item)
{
    if (!item.is_object())
        return {};
    if (item.value("separator", false))
        return "(separator)";
    if (item.value("column", false))
        return "(new column)";
    std::string label = item.value("label", std::string());
    if (label.empty())
        label = actions::labelFor(item.value("action", json()));
    return QString::fromStdString(label);
}
} // namespace

// ----------------------------------------------------------------- RadialPreview

RadialPreview::RadialPreview(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(360, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize RadialPreview::sizeHint() const
{
    return {460, 480};
}

void RadialPreview::setLevel(const json& level, int selected, QString crumb)
{
    mLevel = level.is_object() ? level : json::object();
    mSelected = selected;
    mCrumb = std::move(crumb);
    update();
}

std::vector<RadialPreview::Box> RadialPreview::layoutBoxes() const
{
    std::vector<Box> boxes;
    QFontMetricsF fm(font());
    double h = 24;
    int overflowCount = mLevel.contains("overflow") && mLevel["overflow"].is_array() ? int(mLevel["overflow"].size()) : 0;
    double sideWidth = 56;
    for (int i : {1, 2, 3, 5, 6, 7})
        if (mLevel.contains(kSlots[i]) && mLevel[kSlots[i]].is_object())
            sideWidth = std::max(sideWidth, fm.horizontalAdvance(itemLabel(mLevel[kSlots[i]])) + 34);
    // Keep the widest side label inside the widget.
    double radius = std::min({width() * 0.30, width() / 2.0 - sideWidth, (height() - overflowCount * h - 60) * 0.36});
    radius = std::max(radius, 70.0);
    QPointF center(width() / 2.0, 34 + radius + h);

    for (int i = 0; i < 8; ++i)
    {
        bool has = mLevel.contains(kSlots[i]) && mLevel[kSlots[i]].is_object();
        Box b{i, {}, has ? itemLabel(mLevel[kSlots[i]]) : QString(kSlots[i]), !has,
              has && mLevel[kSlots[i]].contains("children") && mLevel[kSlots[i]]["children"].is_object()};
        double w = std::max(56.0, fm.horizontalAdvance(b.label) + 22 + (b.submenu ? 12 : 0));
        QPointF p = center + kCompassDirs[i] * radius;
        double x = kCompassDirs[i].x() > 0.1 ? p.x() - 12 : kCompassDirs[i].x() < -0.1 ? p.x() - w + 12 : p.x() - w / 2;
        b.rect = QRectF(x, p.y() - h / 2, w, h);
        boxes.push_back(b);
    }
    const double top = boxes[4].rect.bottom() + 10;
    std::vector<double> widths{120};
    for (int i = 0; i < overflowCount; ++i)
        if (mLevel["overflow"][i].value("column", false))
            widths.push_back(120);
        else
            widths.back() = std::max(widths.back(), fm.horizontalAdvance(itemLabel(mLevel["overflow"][i])) + 28);
    double total = -8;
    for (double w : widths)
        total += w + 8;
    double left = center.x() - total / 2, y = top;
    size_t column = 0;
    for (int i = 0; i < overflowCount; ++i)
    {
        boxes.push_back({kOverflow + i, QRectF(left, y, widths[column], h - 2), itemLabel(mLevel["overflow"][i]), false, false});
        y += h;
        if (mLevel["overflow"][i].value("column", false))
        {
            left += widths[column++] + 8;
            y = top;
        }
    }
    return boxes;
}

void RadialPreview::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());

    p.setPen(palette().color(QPalette::PlaceholderText));
    p.drawText(QRectF(10, 6, width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, mCrumb);

    auto boxes = layoutBoxes();
    QPointF center = (boxes[0].rect.center() + boxes[4].rect.center()) / 2;
    center.setX(width() / 2.0);
    p.setPen(QPen(themeShade(200), 1.5));
    p.setBrush(themeShade(30));
    p.drawEllipse(center, 5, 5);
    p.setPen(QPen(themeShade(70), 1, Qt::DotLine));
    for (int i = 0; i < 8; ++i)
        p.drawLine(center + kCompassDirs[i] * 14, boxes[i].rect.center());

    for (const Box& b : boxes)
    {
        bool sel = b.slot == mSelected;
        QPainterPath path;
        path.addRoundedRect(b.rect, b.slot < kOverflow ? 4 : 2, b.slot < kOverflow ? 4 : 2);
        if (b.empty)
        {
            p.setPen(QPen(sel ? themeShade(200) : themeShade(85), 1, Qt::DashLine));
            p.setBrush(sel ? QBrush(themeShade(70)) : QBrush(Qt::NoBrush));
            p.drawPath(path);
            p.setPen(themeShade(110));
            p.drawText(b.rect, Qt::AlignCenter, b.label);
            continue;
        }
        p.setPen(QPen(sel ? themeShade(230) : themeShade(95), sel ? 1.5 : 1));
        p.setBrush(sel ? themeShade(150) : themeShade(44));
        p.drawPath(path);
        p.setPen(sel ? themeShade(15) : themeShade(220));
        QRectF tr = b.rect.adjusted(10, 0, -10, 0);
        p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, b.label);
        if (b.submenu)
            p.drawText(tr, Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("▸"));
    }
}

void RadialPreview::mousePressEvent(QMouseEvent* e)
{
    for (const Box& b : layoutBoxes())
        if (b.rect.adjusted(-3, -3, 3, 3).contains(e->position()))
        {
            Q_EMIT picked(b.slot);
            return;
        }
}

// ------------------------------------------------------------- MarkingMenuEditor

MarkingMenuEditor::MarkingMenuEditor(QWidget* parent) : QWidget(parent)
{
    setWindowTitle("Marking Menu Editor");
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    auto* split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split);

    // Menu list
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("Menus"));
    mMenus = new QListWidget;
    leftLayout->addWidget(mMenus, 1);
    auto* newMenu = new QPushButton("New Menu...");
    mResetDefault = new QPushButton("Reset to Default");
    mResetDefault->setToolTip("Drop your changes to this menu (user-only menus are cleared)");
    leftLayout->addWidget(newMenu);
    leftLayout->addWidget(mResetDefault);
    split->addWidget(left);

    // Preview
    auto* center = new QWidget;
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    mPreview = new RadialPreview;
    centerLayout->addWidget(mPreview, 1);
    auto* saveRow = new QHBoxLayout;
    mSave = new QPushButton("Save");
    mRevert = new QPushButton("Revert");
    saveRow->addStretch(1);
    saveRow->addWidget(mRevert);
    saveRow->addWidget(mSave);
    centerLayout->addLayout(saveRow);
    split->addWidget(center);

    // Structure and item editor
    auto* right = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    mTree = new QTreeWidget;
    mTree->setColumnCount(3);
    mTree->setHeaderLabels({"Slot", "Label", "Target"});
    mTree->setColumnWidth(0, 140);
    mTree->setColumnWidth(1, 130);
    mTree->setUniformRowHeights(true);
    rightLayout->addWidget(mTree, 1);

    auto* form = new QFormLayout;
    mSlotLabel = new QLabel;
    form->addRow("Slot", mSlotLabel);
    mLabel = new QLineEdit;
    mLabel->setPlaceholderText("defaults to the action's label");
    form->addRow("Label", mLabel);
    mKind = new QComboBox;
    mKind->addItem("Action", int(Kind::Action));
    mKind->addItem("Python", int(Kind::Python));
    mKind->addItem("Submenu", int(Kind::Submenu));
    mKind->addItem("Other (JSON)", int(Kind::Other));
    form->addRow("Target", mKind);
    rightLayout->addLayout(form);

    mPages = new QStackedWidget;
    auto* actionPage = new QWidget;
    auto* actionLayout = new QVBoxLayout(actionPage);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    mActionSearch = new QLineEdit;
    mActionSearch->setPlaceholderText("Search actions");
    mActionSearch->setClearButtonEnabled(true);
    actionLayout->addWidget(mActionSearch);
    mActionList = new QListWidget;
    for (const Action* a : actions::all())
    {
        auto* it = new QListWidgetItem(QString::fromStdString(a->label + "    " + a->id));
        it->setData(Qt::UserRole, QString::fromStdString(a->id));
        it->setToolTip(QString::fromStdString(a->category + ": " + a->id));
        mActionList->addItem(it);
    }
    actionLayout->addWidget(mActionList);
    mPages->addWidget(actionPage);

    mPython = new QPlainTextEdit;
    mPython->setFont(monoFont());
    mPython->setPlaceholderText("Python run when the item is picked");
    mPython->setTabStopDistance(QFontMetricsF(monoFont()).horizontalAdvance(' ') * 4);
    mPages->addWidget(mPython);

    auto* subPage = new QLabel("Submenu: its slots are listed under this item in the tree.\nPick one there or in the preview to edit it.");
    subPage->setWordWrap(true);
    subPage->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    mPages->addWidget(subPage);

    auto* otherPage = new QWidget;
    auto* otherLayout = new QVBoxLayout(otherPage);
    otherLayout->setContentsMargins(0, 0, 0, 0);
    mOther = new QPlainTextEdit;
    mOther->setFont(monoFont());
    mOther->setPlaceholderText("{\"command\": \"name\", \"args\": {}}");
    otherLayout->addWidget(mOther);
    mOtherError = new QLabel;
    otherLayout->addWidget(mOtherError);
    mPages->addWidget(otherPage);
    mPages->setMinimumHeight(170);
    rightLayout->addWidget(mPages);

    auto* itemButtons = new QHBoxLayout;
    mToPython = new QPushButton("Duplicate as Python Item");
    mToPython->setToolTip("Replace the target with the Python it runs, ready to edit");
    mRemove = new QPushButton("Remove");
    mUp = new QPushButton("Up");
    mDown = new QPushButton("Down");
    auto* addOverflowButton = new QPushButton("Add Overflow Item");
    auto* addSeparatorButton = new QPushButton("Add Separator");
    addSeparatorButton->setToolTip("A divider line between groups of overflow rows");
    auto* addColumnButton = new QPushButton("Add Column");
    addColumnButton->setToolTip("Starts a new overflow column beside the previous one");
    itemButtons->addWidget(mToPython);
    itemButtons->addWidget(mRemove);
    itemButtons->addWidget(mUp);
    itemButtons->addWidget(mDown);
    itemButtons->addStretch(1);
    itemButtons->addWidget(addOverflowButton);
    itemButtons->addWidget(addSeparatorButton);
    itemButtons->addWidget(addColumnButton);
    rightLayout->addLayout(itemButtons);
    split->addWidget(right);

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 3);
    split->setStretchFactor(2, 2);
    split->setSizes({150, 480, 430});

    connect(mMenus, &QListWidget::currentTextChanged, this, [this](const QString& name) {
        if (mLoading || name.isEmpty() || name.toStdString() == mName)
            return;
        if (mDirty && QMessageBox::question(this, "Marking Menu Editor", "Discard unsaved changes to " +
                                                                         QString::fromStdString(mName) + "?") != QMessageBox::Yes)
        {
            refreshMenuList();
            return;
        }
        openMenu(name.toStdString());
    });
    connect(newMenu, &QPushButton::clicked, this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, "New Marking Menu", "Name (letters, digits, _):", QLineEdit::Normal, "", &ok);
        if (!ok || !QRegularExpression("^[A-Za-z0-9_]+$").match(name).hasMatch())
            return;
        mName = name.toStdString();
        mDoc = PrefStore::get().merged("markingMenus/" + mName);
        if (mDoc.empty())
            mDoc = {{"N", {{"label", "New Item"}}}, {"overflow", json::array()}};
        mSel = "/N";
        setDirty(true);
        refreshMenuList();
        rebuildTree();
        showSelection();
    });
    connect(mResetDefault, &QPushButton::clicked, this, [this] {
        if (mName.empty())
            return;
        PrefStore::get().set("markingMenus/" + mName, "", json::object());
        setDirty(false);
        openMenu(mName);
    });
    connect(mSave, &QPushButton::clicked, this, &MarkingMenuEditor::save);
    // Delete removes the picked slot or item from the unsaved menu; the menu list and the action picker keep theirs.
    deletion::claim(this, [this] { removeSelected(); });
    deletion::claim(mMenus, nullptr);
    deletion::claim(mActionList, nullptr);
    connect(mRevert, &QPushButton::clicked, this, [this] {
        setDirty(false);
        openMenu(mName);
    });

    connect(mTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (mLoading || !it)
            return;
        std::string ptr = it->data(0, Qt::UserRole).toString().toStdString();
        if (ptr.empty())
            return;
        mSel = ptr == "/" ? "" : ptr;
        showSelection();
    });
    connect(mPreview, &RadialPreview::picked, this, [this](int slot) {
        std::string lp = levelPointer();
        selectPointer(slot >= RadialPreview::kOverflow ? lp + "/overflow/" + std::to_string(slot - RadialPreview::kOverflow)
                                                       : lp + "/" + kSlots[slot]);
    });

    connect(mLabel, &QLineEdit::textEdited, this, [this](const QString& text) {
        editItem([&](json& it) {
            if (text.isEmpty())
                it.erase("label");
            else
                it["label"] = text.toStdString();
        });
    });
    connect(mKind, &QComboBox::activated, this, [this] {
        auto kind = Kind(mKind->currentData().toInt());
        editItem([&](json& it) {
            json action = it.value("action", json());
            switch (kind)
            {
            case Kind::Action:
                it.erase("children");
                if (!action.is_string())
                    it.erase("action");
                break;
            case Kind::Python:
                it.erase("children");
                if (!(action.is_object() && action.contains("python")))
                    it["action"] = {{"python", actions::pythonFor(action)}};
                break;
            case Kind::Submenu:
                it.erase("action");
                if (!it.contains("children") || !it["children"].is_object())
                    it["children"] = json::object();
                break;
            case Kind::Other:
                it.erase("children");
                if (!action.is_object() || action.contains("python"))
                    it["action"] = {{"command", ""}, {"args", json::object()}};
                break;
            }
        });
        showSelection();
    });
    connect(mActionSearch, &QLineEdit::textChanged, this, [this](const QString& q) {
        for (int i = 0; i < mActionList->count(); ++i)
        {
            QListWidgetItem* it = mActionList->item(i);
            it->setHidden(!q.isEmpty() && !it->text().contains(q, Qt::CaseInsensitive) && !it->toolTip().contains(q, Qt::CaseInsensitive));
        }
    });
    connect(mActionList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* li) {
        if (mLoading || !li)
            return;
        std::string id = li->data(Qt::UserRole).toString().toStdString();
        editItem([&](json& it) {
            json old = it.value("action", json());
            std::string label = it.value("label", std::string());
            // Keep a custom label, but follow the action when the label was just the old action's.
            if (label.empty() || label == actions::labelFor(old))
            {
                const Action* a = actions::find(id);
                it["label"] = a ? a->label : id;
            }
            it["action"] = id;
            it.erase("children");
        });
        QSignalBlocker block(mLabel);
        if (json* it = item())
            mLabel->setText(QString::fromStdString(it->value("label", std::string())));
    });
    connect(mPython, &QPlainTextEdit::textChanged, this, [this] {
        if (mLoading)
            return;
        editItem([&](json& it) { it["action"] = {{"python", mPython->toPlainText().toStdString()}}; });
    });
    connect(mOther, &QPlainTextEdit::textChanged, this, [this] {
        if (mLoading)
            return;
        json j = json::parse(mOther->toPlainText().toStdString(), nullptr, false);
        if (j.is_discarded())
        {
            mOtherError->setText("not valid JSON yet");
            return;
        }
        mOtherError->clear();
        editItem([&](json& it) { it["action"] = j; });
    });
    connect(mToPython, &QPushButton::clicked, this, [this] {
        editItem([&](json& it) { it["action"] = {{"python", actions::pythonFor(it.value("action", json()))}}; });
        showSelection();
    });
    connect(mRemove, &QPushButton::clicked, this, &MarkingMenuEditor::removeSelected);
    connect(mUp, &QPushButton::clicked, this, [this] { moveOverflow(-1); });
    connect(mDown, &QPushButton::clicked, this, [this] { moveOverflow(1); });
    connect(addOverflowButton, &QPushButton::clicked, this, [this] { addOverflow({{"label", "New Item"}}); });
    connect(addSeparatorButton, &QPushButton::clicked, this, [this] { addOverflow({{"separator", true}}); });
    connect(addColumnButton, &QPushButton::clicked, this, [this] { addOverflow({{"column", true}}); });

    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file.rfind("markingMenus/", 0) != 0)
            return;
        refreshMenuList();
        if (file == "markingMenus/" + mName && !mDirty)
            openMenu(mName);
    });

    refreshMenuList();
    if (mMenus->count() > 0)
        openMenu(mMenus->item(0)->text().toStdString());
    setDirty(false);
    resize(1180, 700);
}

MarkingMenuEditor::~MarkingMenuEditor()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

void MarkingMenuEditor::refreshMenuList()
{
    std::set<std::string> names;
    for (const fs::path& dir : {defaultsDir(), userMenusDir()})
    {
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".json")
                names.insert(fs::relative(e.path(), dir, ec).replace_extension().generic_string());
    }
    for (const std::string& n : markingMenus::contributedNames())
        names.insert(n);
    if (!mName.empty())
        names.insert(mName);

    mLoading = true;
    mMenus->clear();
    for (const std::string& n : names)
    {
        bool builtIn = hasDefault(n);
        json user = PrefStore::get().merged("markingMenus/" + n);
        if (!builtIn && user.empty() && n != mName)
            continue;
        auto* it = new QListWidgetItem(QString::fromStdString(n));
        it->setToolTip(builtIn ? "built-in" : "user menu");
        mMenus->addItem(it);
        if (n == mName)
            mMenus->setCurrentItem(it);
    }
    mLoading = false;
}

void MarkingMenuEditor::openMenu(const std::string& name)
{
    mName = name;
    mDoc = PrefStore::get().merged("markingMenus/" + name);
    if (!mDoc.is_object())
        mDoc = json::object();
    if (mSel.empty() || !item())
        mSel = "/N";
    setDirty(false);
    refreshMenuList();
    rebuildTree();
    showSelection();
}

void MarkingMenuEditor::selectPointer(const std::string& ptr)
{
    mSel = ptr;
    rebuildTree();
    showSelection();
}

void MarkingMenuEditor::showMenu(const std::string& name, const std::string& ptr)
{
    if (name != mName)
    {
        if (mDirty && QMessageBox::question(this, "Marking Menu Editor",
                                            "Discard unsaved changes to " + QString::fromStdString(mName) + "?") != QMessageBox::Yes)
            return;
        mSel.clear();
        openMenu(name);
    }
    if (!ptr.empty() && mDoc.contains(json::json_pointer(ptr)))
        selectPointer(ptr);
}

void MarkingMenuEditor::setDirty(bool dirty)
{
    mDirty = dirty;
    mSave->setEnabled(dirty);
    mRevert->setEnabled(dirty);
    mResetDefault->setEnabled(!mName.empty());
    setWindowTitle(QString("Marking Menu Editor - %1%2").arg(QString::fromStdString(mName), dirty ? " *" : ""));
}

void MarkingMenuEditor::save()
{
    if (mName.empty())
        return;
    json patch = mergePatch(defaultMenu(mName), mDoc);
    const bool firstSave = isNewUserMenu(mName);
    setDirty(false);
    PrefStore::get().set("markingMenus/" + mName, "", patch);
    if (firstSave && !hasHotkey(mName))
        assignMarkingMenuHotkeyDialog(mName, this);
}

json* MarkingMenuEditor::item()
{
    if (mSel.empty())
        return nullptr;
    json::json_pointer p(mSel);
    if (!mDoc.contains(p) || !mDoc[p].is_object())
        return nullptr;
    return &mDoc[p];
}

json& MarkingMenuEditor::ensureItem()
{
    if (json* it = item())
        return *it;
    json& it = mDoc[json::json_pointer(mSel)];
    it = json::object();
    return it;
}

std::string MarkingMenuEditor::levelPointer() const
{
    if (mSel.empty())
        return "";
    json::json_pointer p(mSel);
    json::json_pointer parent = p.parent_pointer();
    if (!parent.empty() && parent.back() == "overflow")
        parent = parent.parent_pointer();
    return parent.to_string();
}

int MarkingMenuEditor::selectedSlot() const
{
    if (mSel.empty())
        return -1;
    json::json_pointer p(mSel);
    json::json_pointer parent = p.parent_pointer();
    if (!parent.empty() && parent.back() == "overflow")
        return RadialPreview::kOverflow + std::stoi(p.back());
    return slotIndex(p.back());
}

void MarkingMenuEditor::editItem(const std::function<void(json&)>& fn)
{
    if (mLoading || mSel.empty())
        return;
    fn(ensureItem());
    setDirty(true);
    rebuildTree();
    refreshPreview();
}

void MarkingMenuEditor::removeSelected()
{
    if (!item())
        return;
    json::json_pointer p(mSel);
    json& parent = mDoc[p.parent_pointer()];
    if (parent.is_array())
    {
        int i = std::stoi(p.back());
        parent.erase(size_t(i));
        if (parent.empty())
            mSel = levelPointer() + "/N";
        else
            mSel = p.parent_pointer().to_string() + "/" + std::to_string(std::min(i, int(parent.size()) - 1));
    }
    else
        parent.erase(p.back());
    setDirty(true);
    rebuildTree();
    showSelection();
}

void MarkingMenuEditor::moveOverflow(int delta)
{
    int slot = selectedSlot();
    if (slot < RadialPreview::kOverflow)
        return;
    int i = slot - RadialPreview::kOverflow;
    json::json_pointer p(mSel);
    json& arr = mDoc[p.parent_pointer()];
    int j = i + delta;
    if (j < 0 || j >= int(arr.size()))
        return;
    std::swap(arr[size_t(i)], arr[size_t(j)]);
    mSel = p.parent_pointer().to_string() + "/" + std::to_string(j);
    setDirty(true);
    rebuildTree();
    showSelection();
}

void MarkingMenuEditor::addOverflow(const json& item)
{
    if (mName.empty())
        return;
    std::string lp = levelPointer();
    json& level = lp.empty() ? mDoc : mDoc[json::json_pointer(lp)];
    if (!level.contains("overflow") || !level["overflow"].is_array())
        level["overflow"] = json::array();
    level["overflow"].push_back(item);
    mSel = lp + "/overflow/" + std::to_string(level["overflow"].size() - 1);
    setDirty(true);
    rebuildTree();
    showSelection();
}

void MarkingMenuEditor::rebuildTree()
{
    mLoading = true;
    mTree->clear();
    auto* root = new QTreeWidgetItem(mTree);
    root->setText(0, QString::fromStdString(mName));
    root->setFirstColumnSpanned(true);
    root->setFlags(Qt::ItemIsEnabled);
    addLevel(root, mDoc, "");
    mTree->expandAll();
    mLoading = false;
}

void MarkingMenuEditor::addLevel(QTreeWidgetItem* parent, const json& level, const std::string& base)
{
    QColor dim = palette().color(QPalette::PlaceholderText);
    for (int i = 0; i < 8; ++i)
    {
        std::string ptr = base + "/" + kSlots[i];
        bool has = level.contains(kSlots[i]) && level[kSlots[i]].is_object();
        auto* it = new QTreeWidgetItem(parent);
        it->setText(0, kSlots[i]);
        it->setData(0, Qt::UserRole, QString::fromStdString(ptr));
        if (has)
        {
            const json& item = level[kSlots[i]];
            it->setText(1, itemLabel(item));
            it->setText(2, targetSummary(item));
            if (item.contains("children") && item["children"].is_object())
                addLevel(it, item["children"], ptr + "/children");
        }
        else
        {
            it->setText(1, "(empty)");
            for (int c = 0; c < 3; ++c)
                it->setForeground(c, dim);
        }
        if (ptr == mSel)
            mTree->setCurrentItem(it);
    }
    auto* overflow = new QTreeWidgetItem(parent);
    overflow->setText(0, "overflow");
    overflow->setFlags(Qt::ItemIsEnabled);
    if (level.contains("overflow") && level["overflow"].is_array())
        for (size_t i = 0; i < level["overflow"].size(); ++i)
        {
            std::string ptr = base + "/overflow/" + std::to_string(i);
            const json& item = level["overflow"][i];
            auto* it = new QTreeWidgetItem(overflow);
            it->setText(0, QString::number(i + 1));
            it->setText(1, itemLabel(item));
            it->setText(2, targetSummary(item));
            it->setData(0, Qt::UserRole, QString::fromStdString(ptr));
            if (ptr == mSel)
                mTree->setCurrentItem(it);
        }
}

void MarkingMenuEditor::showSelection()
{
    mLoading = true;
    json* it = item();
    int slot = selectedSlot();
    mSlotLabel->setText(slot < 0 ? QString("-")
                        : slot >= RadialPreview::kOverflow ? QString("overflow %1").arg(slot - RadialPreview::kOverflow + 1)
                                                           : QString(kSlots[slot]) + (it ? "" : "  (empty: edit to create)"));
    mLabel->setText(it ? QString::fromStdString(it->value("label", std::string())) : QString());

    json action = it ? it->value("action", json()) : json();
    Kind kind = Kind::Action;
    if (it && it->contains("children") && (*it)["children"].is_object())
        kind = Kind::Submenu;
    else if (action.is_object() && action.contains("python"))
        kind = Kind::Python;
    else if (action.is_object())
        kind = Kind::Other;
    mKind->setCurrentIndex(mKind->findData(int(kind)));
    mPages->setCurrentIndex(int(kind));

    mActionList->setCurrentItem(nullptr);
    if (action.is_string())
        for (int i = 0; i < mActionList->count(); ++i)
            if (mActionList->item(i)->data(Qt::UserRole).toString().toStdString() == action.get<std::string>())
            {
                mActionList->setCurrentRow(i);
                mActionList->scrollToItem(mActionList->item(i));
            }
    mPython->setPlainText(kind == Kind::Python ? QString::fromStdString(action.value("python", std::string())) : QString());
    mOther->setPlainText(kind == Kind::Other ? QString::fromStdString(action.dump(2)) : QString());
    mOtherError->clear();

    mToPython->setEnabled(action.is_string() || (action.is_object() && action.contains("command")));
    mRemove->setEnabled(it != nullptr);
    mUp->setEnabled(slot > RadialPreview::kOverflow);
    bool lastOverflow = true;
    if (slot >= RadialPreview::kOverflow)
    {
        json::json_pointer p(mSel);
        lastOverflow = slot - RadialPreview::kOverflow + 1 >= int(mDoc[p.parent_pointer()].size());
    }
    mDown->setEnabled(slot >= RadialPreview::kOverflow && !lastOverflow);
    mLoading = false;
    refreshPreview();
}

void MarkingMenuEditor::refreshPreview()
{
    std::string lp = levelPointer();
    json level = lp.empty() ? mDoc : mDoc.value(json::json_pointer(lp), json::object());

    // Breadcrumb from the labels of the submenu items on the way down.
    QString crumb = QString::fromStdString(mName);
    std::string walk;
    json::json_pointer p(lp);
    std::vector<std::string> tokens;
    for (json::json_pointer q = p; !q.empty(); q = q.parent_pointer())
        tokens.insert(tokens.begin(), q.back());
    for (const std::string& t : tokens)
    {
        walk += "/" + t;
        if (t != "children")
            crumb += QString::fromUtf8("  ›  ") + itemLabel(mDoc.value(json::json_pointer(walk), json()));
    }
    mPreview->setLevel(level, selectedSlot(), crumb);
}
} // namespace lila::ui
