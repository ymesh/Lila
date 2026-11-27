#include "ui/HotkeyEditor.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/FileDialog.h"
#include "ui/Hotkeys.h"
#include "ui/Keymaps.h"
#include "ui/MayaHotkeys.h"
#include "ui/Theme.h"
#include "ui/editor/CodeEditor.h"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace lila::ui
{
using json = nlohmann::json;
using hotkeys::Binding;

namespace
{
enum HotkeyRole
{
    KindRole = Qt::UserRole,
    ContextRole,
    KeyRole,
    ActionRole,
};

enum RowKind
{
    BindingRow = 1,
    UnboundDefaultRow,
    ActionRowKind,
};

enum HotkeyColumn
{
    ColKey,
    ColAction,
    ColMode,
    ColCategory,
    ColStatus,
    ColConflict,
    ColCount
};

QWidget* hostWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (qobject_cast<QMainWindow*>(w) && w->isVisible())
            return w;
    return QApplication::activeWindow();
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string categoryOf(const Binding& b)
{
    for (const std::string& id : hotkeys::actionIds(b))
        if (const Action* a = actions::find(id))
            return a->category;
    return "";
}

const char* originText(Binding::Origin o)
{
    switch (o)
    {
    case Binding::Origin::Default: return "default";
    case Binding::Origin::Modified: return "modified";
    case Binding::Origin::User: return "user";
    }
    return "";
}
} // namespace

// ------------------------------------------------------------------ ChordGrabber

ChordGrabber::ChordGrabber(QObject* parent) : QObject(parent) {}

ChordGrabber::~ChordGrabber()
{
    disarm();
}

void ChordGrabber::arm()
{
    if (mArmed)
        return;
    mArmed = true;
    qApp->installEventFilter(this);
}

void ChordGrabber::disarm()
{
    if (!mArmed)
        return;
    mArmed = false;
    qApp->removeEventFilter(this);
}

bool ChordGrabber::eventFilter(QObject*, QEvent* e)
{
    if (!mArmed)
        return false;
    switch (e->type())
    {
    case QEvent::ShortcutOverride:
        // Accepting it keeps QAction shortcuts from firing, so the key arrives as a plain press.
        e->accept();
        return true;
    case QEvent::KeyPress:
    {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->isAutoRepeat())
            return true;
        if (auto chord = hotkeys::chordFromEvent(k))
        {
            disarm();
            Q_EMIT captured(*chord);
        }
        return true;
    }
    case QEvent::KeyRelease:
        return true;
    default:
        return false;
    }
}

// ------------------------------------------------------------ ChordCaptureDialog

ChordCaptureDialog::ChordCaptureDialog(json target, std::string context, QWidget* parent, hotkeys::Slot slot)
    : QDialog(parent), mTarget(std::move(target))
{
    setWindowTitle("Assign Hotkey");
    setMinimumWidth(380);
    const std::string id = mTarget.is_string() ? mTarget.get<std::string>() : std::string();

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QString("<b>%1</b>&nbsp;&nbsp;<span>%2</span>")
                                 .arg(QString::fromStdString(actions::labelFor(mTarget)).toHtmlEscaped(),
                                      QString::fromStdString(id).toHtmlEscaped()));
    layout->addWidget(title);

    mChordButton = new QPushButton("Press a key...");
    mChordButton->setMinimumHeight(44);
    QFont big = mChordButton->font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    mChordButton->setFont(big);
    mChordButton->setToolTip("Click, then press the key chord to bind");
    layout->addWidget(mChordButton);

    auto* form = new QFormLayout;
    mMode = new QComboBox;
    // A marking menu opens only from key + mouse button.
    if (!(mTarget.is_object() && mTarget.contains("markingMenu")))
    {
        mMode->addItem("Press (tap)", int(hotkeys::Slot::Press));
        mMode->addItem("Hold", int(hotkeys::Slot::Hold));
        mMode->addItem("Release", int(hotkeys::Slot::Release));
    }
    mMode->addItem("Key + Left Click", int(hotkeys::Slot::Lmb));
    mMode->addItem("Key + Middle Click", int(hotkeys::Slot::Mmb));
    mMode->addItem("Key + Right Click", int(hotkeys::Slot::Rmb));
    mMode->setToolTip("Press runs on key down (on key up instead when the key also has a hold or a click).\n"
                      "Hold runs after holding past the hold delay. Release runs on key up.\n"
                      "Key + Click runs when that mouse button goes down while the key is held; the key's own\n"
                      "press and release then don't run.");
    mMode->setCurrentIndex(std::max(0, mMode->findData(int(slot))));
    form->addRow("Mode", mMode);
    mContext = new QComboBox;
    for (const std::string& c : hotkeys::snapshot().contexts)
        mContext->addItem(QString::fromStdString(c));
    int ci = mContext->findText(QString::fromStdString(context));
    mContext->setCurrentIndex(ci < 0 ? 0 : ci);
    form->addRow("Context", mContext);
    layout->addLayout(form);

    mInfo = new QLabel;
    mInfo->setWordWrap(true);
    mInfo->setMinimumHeight(36);
    layout->addWidget(mInfo);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    mOk = buttons->button(QDialogButtonBox::Ok);
    mOk->setText("Assign");
    mOk->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    mGrabber = new ChordGrabber(this);
    connect(mGrabber, &ChordGrabber::captured, this, &ChordCaptureDialog::onCaptured);
    connect(mChordButton, &QPushButton::clicked, this, [this] {
        mChordButton->setText("Press a key...");
        mGrabber->arm();
    });
    connect(mMode, &QComboBox::currentIndexChanged, this, &ChordCaptureDialog::refreshInfo);
    connect(mContext, &QComboBox::currentIndexChanged, this, &ChordCaptureDialog::refreshInfo);
    mGrabber->arm();
    refreshInfo();
}

void ChordCaptureDialog::onCaptured(QKeyCombination chord)
{
    mChord = chord;
    mChordButton->setText(hotkeys::displayText(chord));
    mOk->setEnabled(true);
    mOk->setFocus();
    refreshInfo();
}

void ChordCaptureDialog::refreshInfo()
{
    if (!mChord)
    {
        mInfo->setText("Any key works, including Esc and Enter. Click the box to capture again.");
        return;
    }
    hotkeys::Snapshot s = hotkeys::snapshot();
    std::string ctx = mContext->currentText().toStdString();
    auto slot = hotkeys::Slot(mMode->currentData().toInt());
    QStringList lines;
    if (const Binding* b = hotkeys::find(s, ctx, *mChord))
    {
        const json& cur = hotkeys::slotTarget(*b, slot);
        if (!cur.is_null())
            lines << QString("Replaces %1 in %2.").arg(QString::fromStdString(actions::labelFor(cur)), QString::fromStdString(ctx));
        else
            lines << QString("Joins the existing binding: %1.").arg(QString::fromStdString(hotkeys::describe(*b)));
    }
    for (const Binding& b : s.bindings)
    {
        if (b.chord != *mChord || b.context == ctx)
            continue;
        if (ctx != "global" && b.context == "global")
            lines << QString("Shadows global %1 here.").arg(QString::fromStdString(hotkeys::describe(b)));
        else if (ctx == "global")
            lines << QString("Shadowed in %1 by %2.").arg(QString::fromStdString(b.context), QString::fromStdString(hotkeys::describe(b)));
    }
    if (lines.isEmpty())
        lines << "Free in this context.";
    mInfo->setText(lines.join("\n"));
}

void ChordCaptureDialog::apply() const
{
    if (!mChord)
        return;
    hotkeys::assign(mContext->currentText().toStdString(), *mChord, hotkeys::Slot(mMode->currentData().toInt()), mTarget);
}

void importMayaHotkeysDialog(QWidget* parent)
{
    parent = parent ? parent : hostWindow();
    QString path;
    {
        const fileDialog::ContextScope scope("mayaHotkeys", "Hotkey Sets");
        path = fileDialog::getOpenFileName(parent, "Import Maya Hotkeys",
                                           QString::fromStdWString(mayaHotkeys::defaultFolder().wstring()),
                                           "Maya hotkeys (*.mhk *.mel);;All files (*)");
    }
    if (path.isEmpty())
        return;
    mayaHotkeys::Result r;
    try
    {
        r = mayaHotkeys::importFile(path.toStdWString());
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(parent, "Import Maya Hotkeys", QString::fromStdString(e.what()));
        return;
    }

    QMessageBox box(parent);
    box.setWindowTitle("Import Maya Hotkeys");
    box.setText(QString("%1 hotkeys imported into the keymap %2").arg(r.imported).arg(QString::fromStdString(r.keymap)));
    if (!r.skipped.empty())
    {
        box.setInformativeText(QString("%1 skipped").arg(r.skipped.size()));
        QStringList lines;
        for (const mayaHotkeys::Skipped& s : r.skipped)
            lines << QString::fromStdString(s.key + "    " + s.command + "    " + s.reason);
        box.setDetailedText(lines.join(QChar('\n')));
    }
    box.exec();
}

namespace
{
/** Name and source for a Python hotkey; returns the {"python", "label"} target, or nothing when cancelled or empty. */
std::optional<json> pythonTargetDialog(const json& initial, QWidget* parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Python Hotkey");
    dlg.setMinimumSize(520, 320);
    auto* layout = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(QString::fromStdString(initial.value("label", std::string())));
    name->setPlaceholderText("Python");
    name->setToolTip("Shown in the Hotkey Editor and the hotkey cheat sheet");
    form->addRow("Name", name);
    layout->addLayout(form);

    auto* code = new CodeEditor(&dlg);
    code->setUntitledName("Hotkey");
    code->setEditorFont(monoFont());
    code->setPlainText(QString::fromStdString(initial.value("python", std::string())));
    code->setPlaceholderText("setAttr(\"persp.visibility\", False)");
    code->hostKeyHandler = [&dlg](QKeyEvent* k) {
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier))
        {
            dlg.accept();
            return true;
        }
        return false;
    };
    layout->addWidget(code, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(initial.contains("python") ? "Save" : "Next...");
    ok->setToolTip("Ctrl+Enter");
    auto refresh = [code, ok] { ok->setEnabled(!code->toPlainText().trimmed().isEmpty()); };
    QObject::connect(code, &QPlainTextEdit::textChanged, &dlg, refresh);
    refresh();
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    code->setFocus();
    if (dlg.exec() != QDialog::Accepted || code->toPlainText().trimmed().isEmpty())
        return std::nullopt;
    json target = {{"python", code->toPlainText().toStdString()}};
    if (!name->text().trimmed().isEmpty())
        target["label"] = name->text().trimmed().toStdString();
    return target;
}

/** The first slot of b bound to Python, if any. */
std::optional<hotkeys::Slot> pythonSlot(const Binding& b)
{
    for (hotkeys::Slot slot : {hotkeys::Slot::Press, hotkeys::Slot::Hold, hotkeys::Slot::Release, hotkeys::Slot::Lmb,
                               hotkeys::Slot::Mmb, hotkeys::Slot::Rmb})
        if (const json& t = hotkeys::slotTarget(b, slot); t.is_object() && t.contains("python"))
            return slot;
    return std::nullopt;
}
} // namespace

void assignHotkeyDialog(const std::string& actionId, QWidget* parent)
{
    ChordCaptureDialog dlg(actionId, "global", parent ? parent : hostWindow());
    if (dlg.exec() == QDialog::Accepted)
        dlg.apply();
}

bool assignMarkingMenuHotkeyDialog(const std::string& menu, QWidget* parent)
{
    ChordCaptureDialog dlg(json{{"markingMenu", menu}}, "global", parent ? parent : hostWindow(), hotkeys::Slot::Lmb);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    dlg.apply();
    return true;
}

// --------------------------------------------------------------- KeyboardHeatMap

KeyboardHeatMap::KeyboardHeatMap(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto add = [this](double x, double y, double w, QString label, int key) {
        mCaps.push_back({std::move(label), key, QRectF(x, y, w, 1.0)});
    };
    auto row = [&](double x, double y, std::initializer_list<std::pair<const char*, int>> keys, double w = 1.0) {
        for (auto& [label, key] : keys)
        {
            add(x, y, w, label, key);
            x += w;
        }
        return x;
    };

    add(0, 0, 1, "Esc", Qt::Key_Escape);
    for (int i = 0; i < 12; ++i)
        add(2 + i + (i / 4) * 0.5, 0, 1, QString("F%1").arg(i + 1), Qt::Key_F1 + i);

    double y = 1.4;
    double x = row(0, y, {{"`", Qt::Key_QuoteLeft}, {"1", Qt::Key_1}, {"2", Qt::Key_2}, {"3", Qt::Key_3}, {"4", Qt::Key_4},
                          {"5", Qt::Key_5}, {"6", Qt::Key_6}, {"7", Qt::Key_7}, {"8", Qt::Key_8}, {"9", Qt::Key_9},
                          {"0", Qt::Key_0}, {"-", Qt::Key_Minus}, {"=", Qt::Key_Equal}});
    add(x, y, 2, "Backspace", Qt::Key_Backspace);

    y += 1;
    add(0, y, 1.5, "Tab", Qt::Key_Tab);
    x = row(1.5, y, {{"Q", Qt::Key_Q}, {"W", Qt::Key_W}, {"E", Qt::Key_E}, {"R", Qt::Key_R}, {"T", Qt::Key_T},
                     {"Y", Qt::Key_Y}, {"U", Qt::Key_U}, {"I", Qt::Key_I}, {"O", Qt::Key_O}, {"P", Qt::Key_P},
                     {"[", Qt::Key_BracketLeft}, {"]", Qt::Key_BracketRight}});
    add(x, y, 1.5, "\\", Qt::Key_Backslash);

    y += 1;
    add(0, y, 1.75, "Caps", Qt::Key_CapsLock);
    x = row(1.75, y, {{"A", Qt::Key_A}, {"S", Qt::Key_S}, {"D", Qt::Key_D}, {"F", Qt::Key_F}, {"G", Qt::Key_G},
                      {"H", Qt::Key_H}, {"J", Qt::Key_J}, {"K", Qt::Key_K}, {"L", Qt::Key_L},
                      {";", Qt::Key_Semicolon}, {"'", Qt::Key_Apostrophe}});
    add(x, y, 2.25, "Enter", Qt::Key_Return);

    y += 1;
    add(0, y, 2.25, "Shift", Qt::Key_Shift);
    x = row(2.25, y, {{"Z", Qt::Key_Z}, {"X", Qt::Key_X}, {"C", Qt::Key_C}, {"V", Qt::Key_V}, {"B", Qt::Key_B},
                      {"N", Qt::Key_N}, {"M", Qt::Key_M}, {",", Qt::Key_Comma}, {".", Qt::Key_Period},
                      {"/", Qt::Key_Slash}});
    add(x, y, 2.75, "Shift", Qt::Key_Shift);

    y += 1;
    x = row(0, y, {{"Ctrl", Qt::Key_Control}, {"Win", Qt::Key_Meta}, {"Alt", Qt::Key_Alt}}, 1.25);
    add(x, y, 6.25, "Space", Qt::Key_Space);
    row(x + 6.25, y, {{"Alt", Qt::Key_Alt}, {"Win", Qt::Key_Meta}, {"Menu", Qt::Key_Menu}, {"Ctrl", Qt::Key_Control}}, 1.25);

    row(15.5, 0, {{"PrtSc", Qt::Key_Print}, {"ScrLk", Qt::Key_ScrollLock}, {"Pause", Qt::Key_Pause}});
    row(15.5, 1.4, {{"Ins", Qt::Key_Insert}, {"Home", Qt::Key_Home}, {"PgUp", Qt::Key_PageUp}});
    row(15.5, 2.4, {{"Del", Qt::Key_Delete}, {"End", Qt::Key_End}, {"PgDn", Qt::Key_PageDown}});
    add(16.5, 4.4, 1, QString::fromUtf8("↑"), Qt::Key_Up);
    row(15.5, 5.4, {{"←", Qt::Key_Left}, {"↓", Qt::Key_Down}, {"→", Qt::Key_Right}});
}

void KeyboardHeatMap::setBindings(std::vector<Binding> bindings)
{
    mBindings = std::move(bindings);
    update();
}

void KeyboardHeatMap::setSelectedKey(int qtKey)
{
    mSelected = qtKey;
    update();
}

QSize KeyboardHeatMap::sizeHint() const
{
    return {18 * 38 + 8, int(6.4 * 38) + 8};
}

QSize KeyboardHeatMap::minimumSizeHint() const
{
    return {18 * 22 + 8, int(6.4 * 22) + 8};
}

QRectF KeyboardHeatMap::capRect(const Cap& c) const
{
    double unit = std::min((width() - 8) / 18.5, (height() - 8) / 6.4);
    QPointF origin((width() - unit * 18.5) / 2, (height() - unit * 6.4) / 2);
    QRectF r(origin + c.unitRect.topLeft() * unit, c.unitRect.size() * unit);
    return r.adjusted(1.5, 1.5, -1.5, -1.5);
}

const KeyboardHeatMap::Cap* KeyboardHeatMap::capAt(QPointF pos) const
{
    for (const Cap& c : mCaps)
        if (capRect(c).contains(pos))
            return &c;
    return nullptr;
}

int KeyboardHeatMap::countFor(int key) const
{
    int n = 0;
    for (const Binding& b : mBindings)
        if (int(b.chord.key()) == key)
            ++n;
    return n;
}

void KeyboardHeatMap::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().window());

    int maxCount = 1;
    for (const Cap& c : mCaps)
        maxCount = std::max(maxCount, countFor(c.key));

    QFont f = font();
    double unit = std::min((width() - 8) / 18.5, (height() - 8) / 6.4);
    f.setPixelSize(std::max(8, int(unit * 0.3)));
    QFont smallFont = f;
    smallFont.setPixelSize(std::max(7, int(unit * 0.22)));

    for (const Cap& c : mCaps)
    {
        QRectF r = capRect(c);
        int n = countFor(c.key);
        bool modifier = c.key == Qt::Key_Control || c.key == Qt::Key_Shift || c.key == Qt::Key_Alt ||
                        c.key == Qt::Key_Meta || c.key == Qt::Key_CapsLock;
        int level = modifier ? 30 : n == 0 ? 44 : 70 + int(130.0 * n / maxCount);
        QColor fill(level, level, level);
        QColor text = level > 135 ? themeShade(18) : modifier ? themeShade(110) : themeShade(205);

        QPen pen(themeShade(26), 1);
        if (c.key == mSelected && !modifier)
            pen = QPen(themeShade(245), 2);
        else if (c.key == mHover && !modifier)
            pen = QPen(themeShade(160), 1.5);
        p.setPen(pen);
        p.setBrush(fill);
        p.drawRoundedRect(r, 3, 3);

        p.setPen(text);
        p.setFont(f);
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop, c.label);
        if (n > 0)
        {
            p.setFont(smallFont);
            p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignRight | Qt::AlignBottom, QString::number(n));
        }
    }
}

void KeyboardHeatMap::mouseMoveEvent(QMouseEvent* e)
{
    const Cap* c = capAt(e->position());
    int key = c ? c->key : 0;
    if (key != mHover)
    {
        mHover = key;
        update();
    }
    if (!c)
    {
        QToolTip::hideText();
        return;
    }
    QStringList lines;
    for (const Binding& b : mBindings)
        if (int(b.chord.key()) == key)
            lines << QString("%1\t%2  [%3]")
                         .arg(hotkeys::displayText(b.chord), QString::fromStdString(hotkeys::describe(b)),
                              QString::fromStdString(b.context));
    QString tip = "<b>" + c->label.toHtmlEscaped() + "</b>";
    if (lines.isEmpty())
        tip += "<br>unbound";
    for (const QString& l : lines)
        tip += "<br><span style='white-space:pre'>" + l.toHtmlEscaped() + "</span>";
    QToolTip::showText(e->globalPosition().toPoint(), tip, this);
}

void KeyboardHeatMap::mousePressEvent(QMouseEvent* e)
{
    if (const Cap* c = capAt(e->position()))
        Q_EMIT keyClicked(c->key);
}

void KeyboardHeatMap::leaveEvent(QEvent*)
{
    mHover = 0;
    update();
}

// ------------------------------------------------------------------ HotkeyEditor

HotkeyEditor::HotkeyEditor(QWidget* parent) : QWidget(parent)
{
    setWindowTitle("Hotkey Editor");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    auto* keymapBar = new QHBoxLayout;
    keymapBar->addWidget(new QLabel("Keymap"));
    mKeymap = new QComboBox;
    mKeymap->setMinimumContentsLength(16);
    mKeymap->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    keymapBar->addWidget(mKeymap);
    auto* keymapMenuButton = new QToolButton;
    keymapMenuButton->setText("...");
    keymapMenuButton->setToolTip("New, rename, delete, import and export keymaps");
    keymapMenuButton->setPopupMode(QToolButton::InstantPopup);
    auto* keymapMenu = new QMenu(keymapMenuButton);
    keymapMenuButton->setMenu(keymapMenu);
    keymapBar->addWidget(keymapMenuButton);
    keymapBar->addStretch(1);
    layout->addLayout(keymapBar);

    auto askName = [this](const QString& title, const QString& current) {
        bool ok = false;
        QString name = QInputDialog::getText(this, title, "Name", QLineEdit::Normal, current, &ok).trimmed();
        return ok ? name.toStdString() : std::string();
    };
    keymapMenu->addAction("New...", this, [this, askName] {
        if (std::string name = askName("New Keymap", {}); !name.empty())
            keymapChange([&] { keymaps::create(name); });
    })->setToolTip("A copy of the current keymap");
    keymapMenu->addAction("New Empty...", this, [this, askName] {
        if (std::string name = askName("New Keymap", {}); !name.empty())
            keymapChange([&] { keymaps::create(name, true); });
    })->setToolTip("Lila's default hotkeys");
    keymapMenu->addAction("Rename...", this, [this, askName] {
        const std::string from = keymaps::active();
        if (std::string to = askName("Rename Keymap", QString::fromStdString(from)); !to.empty() && to != from)
            keymapChange([&] { keymaps::rename(from, to); });
    });
    keymapMenu->addAction("Reset to Lila Defaults", this, [this] {
        const std::string name = keymaps::active();
        if (QMessageBox::question(this, "Reset Keymap", QString("Drop every change in %1?").arg(QString::fromStdString(name))) ==
            QMessageBox::Yes)
            keymapChange([&] { keymaps::reset(name); });
    });
    keymapMenu->addAction("Delete", this, [this] {
        const std::string name = keymaps::active();
        if (QMessageBox::question(this, "Delete Keymap", QString("Delete %1?").arg(QString::fromStdString(name))) == QMessageBox::Yes)
            keymapChange([&] { keymaps::remove(name); });
    });
    keymapMenu->addSeparator();
    keymapMenu->addAction("Import...", this, [this] {
        QString path;
        {
            const fileDialog::ContextScope scope("keymaps", "Keymaps");
            path = fileDialog::getOpenFileName(this, "Import Keymap", {}, "Lila keymaps (*.lilakeys *.json);;All files (*)");
        }
        if (!path.isEmpty())
            keymapChange([&] { keymaps::use(keymaps::importFrom(path.toStdWString())); });
    });
    keymapMenu->addAction("Export...", this, [this] {
        const std::string name = keymaps::active();
        QString path;
        {
            const fileDialog::ContextScope scope("keymaps", "Keymaps");
            path = fileDialog::getSaveFileName(this, "Export Keymap", QString::fromStdString(name) + keymaps::kFileSuffix,
                                               "Lila keymaps (*.lilakeys)");
        }
        if (!path.isEmpty())
            keymapChange([&] { keymaps::exportTo(name, path.toStdWString()); });
    });
    connect(mKeymap, &QComboBox::activated, this, [this](int index) {
        const std::string name = mKeymap->itemData(index).toString().toStdString();
        keymapChange([&] { keymaps::use(name); });
    });

    auto* bar = new QHBoxLayout;
    mSearch = new QLineEdit;
    mSearch->setPlaceholderText("Search actions, ids, categories, keys");
    mSearch->setClearButtonEnabled(true);
    bar->addWidget(mSearch, 1);
    mKeySearch = new QToolButton;
    mKeySearch->setText("Press Key to Search");
    mKeySearch->setCheckable(true);
    mKeySearch->setToolTip("Capture the next key chord and show what it does");
    bar->addWidget(mKeySearch);
    mClearKey = new QToolButton;
    mClearKey->setVisible(false);
    mClearKey->setToolTip("Clear the key filter");
    bar->addWidget(mClearKey);
    bar->addWidget(new QLabel("Context"));
    mContextFilter = new QComboBox;
    bar->addWidget(mContextFilter);
    layout->addLayout(bar);

    auto* split = new QSplitter(Qt::Vertical);
    mTree = new QTreeWidget;
    mTree->setColumnCount(ColCount);
    mTree->setHeaderLabels({"Key", "Action", "Mode", "Category", "Status", "Conflict"});
    mTree->setRootIsDecorated(true);
    mTree->setUniformRowHeights(true);
    mTree->setAlternatingRowColors(true);
    mTree->header()->setStretchLastSection(true);
    mTree->setColumnWidth(ColKey, 160);
    mTree->setColumnWidth(ColAction, 260);
    mTree->setColumnWidth(ColMode, 100);
    mTree->setColumnWidth(ColCategory, 90);
    mTree->setColumnWidth(ColStatus, 70);
    split->addWidget(mTree);
    mMap = new KeyboardHeatMap;
    split->addWidget(mMap);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    layout->addWidget(split, 1);

    mDetail = new QLabel;
    mDetail->setWordWrap(true);
    mDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(mDetail);

    auto* buttons = new QHBoxLayout;
    mAssign = new QPushButton("Assign...");
    mAssign->setToolTip("Bind a new key chord to the selected action");
    mUnbind = new QPushButton("Unbind");
    mUnbind->setToolTip("Remove this binding (a default is overridden with null)");
    mReset = new QPushButton("Reset to Default");
    mReset->setToolTip("Drop your override for this key");
    mPython = new QPushButton("Python...");
    mPython->setToolTip("Bind a key to a Python script, or edit the selected Python hotkey");
    buttons->addWidget(mAssign);
    buttons->addWidget(mPython);
    buttons->addWidget(mUnbind);
    buttons->addWidget(mReset);
    buttons->addStretch(1);
    auto* importMaya = new QPushButton("Import from Maya...");
    importMaya->setToolTip("A new keymap from a Maya hotkey set (.mhk) or userHotkeys.mel");
    connect(importMaya, &QPushButton::clicked, this, [this] { importMayaHotkeysDialog(window()); });
    buttons->addWidget(importMaya);
    layout->addLayout(buttons);

    mGrabber = new ChordGrabber(this);

    connect(mSearch, &QLineEdit::textChanged, this, &HotkeyEditor::rebuild);
    connect(mContextFilter, &QComboBox::currentIndexChanged, this, &HotkeyEditor::rebuild);
    connect(mTree, &QTreeWidget::currentItemChanged, this, &HotkeyEditor::updateButtons);
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (const Binding* b = selectedBinding(); b && pythonSlot(*b))
            pythonHotkey();
        else
            assignSelected();
    });
    connect(mAssign, &QPushButton::clicked, this, &HotkeyEditor::assignSelected);
    connect(mPython, &QPushButton::clicked, this, &HotkeyEditor::pythonHotkey);
    auto unbind = [this] {
        if (const Binding* b = selectedBinding())
            hotkeys::unbind(b->context, b->keyText);
    };
    connect(mUnbind, &QPushButton::clicked, this, unbind);
    deletion::claim(mTree, unbind);
    deletion::claim(mMap, nullptr);
    connect(mReset, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem* it = mTree->currentItem())
            hotkeys::resetToDefault(it->data(0, ContextRole).toString().toStdString(),
                                    it->data(0, KeyRole).toString().toStdString());
    });
    connect(mKeySearch, &QToolButton::toggled, this, [this](bool on) {
        if (on)
        {
            mKeySearch->setText("Press a key...");
            mGrabber->arm();
        }
        else
        {
            mKeySearch->setText("Press Key to Search");
            mGrabber->disarm();
        }
    });
    connect(mGrabber, &ChordGrabber::captured, this, [this](QKeyCombination chord) {
        mKeySearch->setChecked(false);
        setKeyFilter(KeyFilter{int(chord.key()), int(chord.keyboardModifiers())});
    });
    connect(mClearKey, &QToolButton::clicked, this, [this] { setKeyFilter(std::nullopt); });
    connect(mMap, &KeyboardHeatMap::keyClicked, this, [this](int key) {
        if (mKeyFilter && mKeyFilter->key == key && !mKeyFilter->mods)
            setKeyFilter(std::nullopt);
        else
            setKeyFilter(KeyFilter{key, std::nullopt});
    });

    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "hotkeys")
            rebuild();
        else if (file == "general" || file.starts_with("keymaps/"))
            refreshKeymaps();
    });
    refreshKeymaps();
    rebuild();
    resize(860, 720);
}

HotkeyEditor::~HotkeyEditor()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

void HotkeyEditor::refreshKeymaps()
{
    const QSignalBlocker block(mKeymap);
    mKeymap->clear();
    const std::string current = keymaps::active();
    for (const keymaps::Info& k : keymaps::list())
    {
        const QString name = QString::fromStdString(k.name);
        mKeymap->addItem(k.studio ? name + " (studio)" : name, name);
        if (k.name == current)
            mKeymap->setCurrentIndex(mKeymap->count() - 1);
    }
}

void HotkeyEditor::keymapChange(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        QMessageBox::warning(this, "Keymap", QString::fromStdString(e.what()));
    }
    refreshKeymaps();
}

void HotkeyEditor::setKeyFilter(std::optional<KeyFilter> f)
{
    mKeyFilter = f;
    if (f)
    {
        QString text = f->mods ? hotkeys::displayText(QKeyCombination(Qt::KeyboardModifiers(*f->mods), Qt::Key(f->key)))
                               : hotkeys::keyName(f->key) + " (any modifiers)";
        mClearKey->setText(QString::fromUtf8("Key: %1  ✕").arg(text));
    }
    mClearKey->setVisible(bool(f));
    mMap->setSelectedKey(f ? f->key : 0);
    rebuild();
}

bool HotkeyEditor::matchesText(const std::string& actionId, const std::string& label, const std::string& category,
                               const std::string& extra) const
{
    std::string q = toLower(mSearch->text().trimmed().toStdString());
    if (q.empty())
        return true;
    for (const std::string* s : {&actionId, &label, &category, &extra})
        if (toLower(*s).find(q) != std::string::npos)
            return true;
    return false;
}

bool HotkeyEditor::matches(const Binding& b) const
{
    if (mKeyFilter)
    {
        if (int(b.chord.key()) != mKeyFilter->key)
            return false;
        if (mKeyFilter->mods && int(b.chord.keyboardModifiers()) != *mKeyFilter->mods)
            return false;
    }
    std::string ids;
    for (const std::string& id : hotkeys::actionIds(b))
        ids += id + " ";
    std::string keys = b.keyText + " " + hotkeys::displayText(b.chord).toStdString();
    return matchesText(ids, hotkeys::describe(b), categoryOf(b), keys);
}

QString HotkeyEditor::conflictFor(const Binding& b, const Binding** other) const
{
    for (const Binding& o : mSnap.bindings)
    {
        if (&o == &b || o.chord != b.chord)
            continue;
        *other = &o;
        if (o.context == b.context)
            return QString("same key as %1: %2").arg(QString::fromStdString(o.keyText), QString::fromStdString(hotkeys::describe(o)));
        if (b.context != "global" && o.context == "global")
            return QString("shadows global %1").arg(QString::fromStdString(hotkeys::describe(o)));
        if (b.context == "global")
            return QString("shadowed in %1 by %2").arg(QString::fromStdString(o.context), QString::fromStdString(hotkeys::describe(o)));
    }
    *other = nullptr;
    return {};
}

void HotkeyEditor::rebuild()
{
    QString keepKind, keepCtx, keepKey, keepAction;
    if (QTreeWidgetItem* it = mTree->currentItem())
    {
        keepKind = it->data(0, KindRole).toString();
        keepCtx = it->data(0, ContextRole).toString();
        keepKey = it->data(0, KeyRole).toString();
        keepAction = it->data(0, ActionRole).toString();
    }

    mSnap = hotkeys::snapshot();

    QString ctxSel = mContextFilter->currentText();
    {
        QSignalBlocker block(mContextFilter);
        mContextFilter->clear();
        mContextFilter->addItem("All");
        for (const std::string& c : mSnap.contexts)
            mContextFilter->addItem(QString::fromStdString(c));
        int i = mContextFilter->findText(ctxSel);
        mContextFilter->setCurrentIndex(i < 0 ? 0 : i);
    }
    std::string ctxFilter = mContextFilter->currentIndex() > 0 ? mContextFilter->currentText().toStdString() : "";

    std::vector<Binding> mapBindings;
    for (const Binding& b : mSnap.bindings)
        if (ctxFilter.empty() || b.context == ctxFilter)
            mapBindings.push_back(b);
    mMap->setBindings(std::move(mapBindings));

    QSignalBlocker block(mTree);
    mTree->clear();
    QTreeWidgetItem* restore = nullptr;
    QColor dim = palette().color(QPalette::PlaceholderText);
    QFont mono = monoFont();
    QFont groupFont = font();
    groupFont.setBold(true);

    auto tag = [&](QTreeWidgetItem* it, int kind, const std::string& ctx, const std::string& key, const std::string& action) {
        it->setData(0, KindRole, QString::number(kind));
        it->setData(0, ContextRole, QString::fromStdString(ctx));
        it->setData(0, KeyRole, QString::fromStdString(key));
        it->setData(0, ActionRole, QString::fromStdString(action));
        if (QString::number(kind) == keepKind && it->data(0, ContextRole).toString() == keepCtx &&
            it->data(0, KeyRole).toString() == keepKey && it->data(0, ActionRole).toString() == keepAction)
            restore = it;
    };

    std::vector<std::pair<QWidget*, QTreeWidgetItem*>> resolvers;
    for (const std::string& ctx : mSnap.contexts)
    {
        if (!ctxFilter.empty() && ctx != ctxFilter)
            continue;
        auto* group = new QTreeWidgetItem;
        group->setFont(0, groupFont);
        group->setFlags(Qt::ItemIsEnabled);

        std::vector<const Binding*> rows;
        for (const Binding& b : mSnap.bindings)
            if (b.context == ctx && matches(b))
                rows.push_back(&b);
        std::sort(rows.begin(), rows.end(), [](const Binding* a, const Binding* b) {
            return std::pair(int(a->chord.key()), int(a->chord.keyboardModifiers())) <
                   std::pair(int(b->chord.key()), int(b->chord.keyboardModifiers()));
        });
        for (const Binding* b : rows)
        {
            auto* it = new QTreeWidgetItem(group);
            it->setText(ColKey, hotkeys::displayText(b->chord));
            it->setFont(ColKey, mono);
            it->setText(ColAction, QString::fromStdString(hotkeys::describe(*b)));
            it->setText(ColMode, QString::fromStdString(hotkeys::modeText(*b)));
            it->setText(ColCategory, QString::fromStdString(categoryOf(*b)));
            it->setText(ColStatus, originText(b->origin));
            auto ids = hotkeys::actionIds(*b);
            it->setToolTip(ColAction, QString::fromStdString(ids.empty() ? "" : ids.front()));
            tag(it, BindingRow, ctx, b->keyText, ids.empty() ? "" : ids.front());

            const Binding* other = nullptr;
            QString conflict = conflictFor(*b, &other);
            if (other)
            {
                auto* cell = new QWidget;
                auto* h = new QHBoxLayout(cell);
                h->setContentsMargins(2, 0, 2, 0);
                auto* label = new QLabel(conflict);
                label->setToolTip(conflict);
                h->addWidget(label, 1);
                auto* fix = new QToolButton;
                fix->setText("Unbind Other");
                fix->setAutoRaise(true);
                fix->setToolTip(QString("Unbind %1 in %2").arg(QString::fromStdString(other->keyText), QString::fromStdString(other->context)));
                std::string oc = other->context, ok = other->keyText;
                connect(fix, &QToolButton::clicked, this, [oc, ok] { hotkeys::unbind(oc, ok); });
                h->addWidget(fix);
                resolvers.emplace_back(cell, it);
            }
        }
        for (const hotkeys::Unbound& u : mSnap.unbound)
        {
            if (u.context != ctx)
                continue;
            auto chord = hotkeys::parse(u.keyText);
            if (!chord)
                continue;
            Binding probe;
            probe.context = u.context;
            probe.keyText = u.keyText;
            probe.chord = *chord;
            hotkeys::split(u.defaultValue, probe);
            if (!matches(probe))
                continue;
            auto* it = new QTreeWidgetItem(group);
            it->setText(ColKey, hotkeys::displayText(*chord));
            it->setFont(ColKey, mono);
            it->setText(ColAction, QString::fromStdString(hotkeys::describe(probe)));
            it->setText(ColMode, QString::fromStdString(hotkeys::modeText(probe)));
            it->setText(ColCategory, QString::fromStdString(categoryOf(probe)));
            it->setText(ColStatus, "unbound");
            for (int c = 0; c < ColCount; ++c)
                it->setForeground(c, dim);
            auto ids = hotkeys::actionIds(probe);
            tag(it, UnboundDefaultRow, ctx, u.keyText, ids.empty() ? "" : ids.front());
        }
        group->setText(0, QString("%1  (%2)").arg(QString::fromStdString(ctx)).arg(group->childCount()));
        if (group->childCount() == 0 && (mKeyFilter || !mSearch->text().isEmpty()))
        {
            delete group;
            continue;
        }
        mTree->addTopLevelItem(group);
        group->setFirstColumnSpanned(true);
        group->setExpanded(true);
    }

    if (!mKeyFilter)
    {
        std::set<std::string> bound;
        for (const Binding& b : mSnap.bindings)
            for (const std::string& id : hotkeys::actionIds(b))
                bound.insert(id);
        auto* group = new QTreeWidgetItem;
        group->setFont(0, groupFont);
        group->setFlags(Qt::ItemIsEnabled);
        for (const Action* a : actions::all())
        {
            if (bound.count(a->id) || !matchesText(a->id, a->label, a->category, ""))
                continue;
            auto* it = new QTreeWidgetItem(group);
            it->setText(ColKey, "-");
            it->setForeground(ColKey, dim);
            it->setText(ColAction, QString::fromStdString(a->label));
            it->setToolTip(ColAction, QString::fromStdString(a->id));
            it->setText(ColCategory, QString::fromStdString(a->category));
            tag(it, ActionRowKind, "", "", a->id);
        }
        group->setText(0, QString("Unbound actions  (%1)").arg(group->childCount()));
        if (group->childCount() > 0)
        {
            mTree->addTopLevelItem(group);
            group->setFirstColumnSpanned(true);
            group->setExpanded(true);
        }
        else
            delete group;
    }

    for (auto& [cell, it] : resolvers)
        mTree->setItemWidget(it, ColConflict, cell);
    if (restore)
        mTree->setCurrentItem(restore);
    updateButtons();
}

const Binding* HotkeyEditor::selectedBinding() const
{
    QTreeWidgetItem* it = mTree->currentItem();
    if (!it || it->data(0, KindRole).toInt() != BindingRow)
        return nullptr;
    std::string ctx = it->data(0, ContextRole).toString().toStdString();
    std::string key = it->data(0, KeyRole).toString().toStdString();
    for (const Binding& b : mSnap.bindings)
        if (b.context == ctx && b.keyText == key)
            return &b;
    return nullptr;
}

std::string HotkeyEditor::selectedAction() const
{
    QTreeWidgetItem* it = mTree->currentItem();
    return it ? it->data(0, ActionRole).toString().toStdString() : "";
}

std::string HotkeyEditor::selectedContext() const
{
    QTreeWidgetItem* it = mTree->currentItem();
    std::string ctx = it ? it->data(0, ContextRole).toString().toStdString() : "";
    if (ctx.empty() && mContextFilter->currentIndex() > 0)
        ctx = mContextFilter->currentText().toStdString();
    return ctx.empty() ? "global" : ctx;
}

void HotkeyEditor::updateButtons()
{
    QTreeWidgetItem* it = mTree->currentItem();
    int kind = it ? it->data(0, KindRole).toInt() : 0;
    const Binding* b = selectedBinding();
    std::string action = selectedAction();
    mAssign->setEnabled(!action.empty() && actions::find(action));
    mUnbind->setEnabled(b != nullptr);
    mReset->setEnabled((b && b->origin != Binding::Origin::Default) || kind == UnboundDefaultRow);
    mPython->setText(b && pythonSlot(*b) ? "Edit Python..." : "Python...");

    QString detail;
    if (const Action* a = actions::find(action))
    {
        detail = QString("<b>%1</b>&nbsp;&nbsp;%2&nbsp;&nbsp;(%3)")
                     .arg(QString::fromStdString(a->label).toHtmlEscaped(), QString::fromStdString(a->id).toHtmlEscaped(),
                          QString::fromStdString(a->category).toHtmlEscaped());
        if (!a->doc.empty())
            detail += "<br>" + QString::fromStdString(a->doc).toHtmlEscaped();
        std::string py = actions::pythonFor(json(a->id));
        if (!py.empty())
            detail += QString("<br><code>%1</code>").arg(QString::fromStdString(py).toHtmlEscaped());
    }
    else if (kind == BindingRow || kind == UnboundDefaultRow)
    {
        detail = it->text(ColAction).toHtmlEscaped();
        if (const std::optional<hotkeys::Slot> slot = b ? pythonSlot(*b) : std::nullopt)
            detail += QString("<br><code>%1</code>")
                          .arg(QString::fromStdString(hotkeys::slotTarget(*b, *slot)["python"].get<std::string>()).toHtmlEscaped().replace("\n", "<br>"));
    }
    mDetail->setText(detail);
}

void HotkeyEditor::assignSelected()
{
    std::string action = selectedAction();
    if (action.empty() || !actions::find(action))
        return;
    ChordCaptureDialog dlg(action, selectedContext(), this);
    if (dlg.exec() == QDialog::Accepted)
        dlg.apply();
}

void HotkeyEditor::pythonHotkey()
{
    if (const Binding* b = selectedBinding())
        if (const std::optional<hotkeys::Slot> slot = pythonSlot(*b))
        {
            const Binding keep = *b;
            if (std::optional<json> target = pythonTargetDialog(hotkeys::slotTarget(keep, *slot), this))
                hotkeys::assign(keep.context, keep.chord, *slot, *target);
            return;
        }
    std::optional<json> target = pythonTargetDialog(json::object(), this);
    if (!target)
        return;
    ChordCaptureDialog dlg(*target, selectedContext(), this);
    if (dlg.exec() == QDialog::Accepted)
        dlg.apply();
}

// ---------------------------------------------------------------- menu hook

namespace
{
std::string actionIdFor(const QAction* qa)
{
    if (!qa || qa->menu() || qa->isSeparator())
        return "";
    std::string id = qa->data().toString().toStdString();
    if (!id.empty() && actions::find(id))
        return id;
    id = qa->objectName().toStdString();
    return !id.empty() && actions::find(id) ? id : "";
}

class MenuHotkeyHook : public QObject
{
public:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        QEvent::Type t = e->type();
        if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonRelease && t != QEvent::ContextMenu)
            return false;
        auto* menu = qobject_cast<QMenu*>(obj);
        if (!menu || menu == mPopup)
            return false;
        QPoint pos, global;
        if (t == QEvent::ContextMenu)
        {
            auto* ce = static_cast<QContextMenuEvent*>(e);
            pos = ce->pos();
            global = ce->globalPos();
        }
        else
        {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() != Qt::RightButton)
                return false;
            pos = me->position().toPoint();
            global = me->globalPosition().toPoint();
        }
        std::string id = actionIdFor(menu->actionAt(pos));
        if (id.empty())
            return false;
        // Eat the press and release too, so the right click doesn't also trigger the item.
        if (t == QEvent::MouseButtonPress || (mPopup && mPopup->isVisible()))
            return true;
        show(menu, id, global);
        return true;
    }

private:
    void show(QMenu* parent, const std::string& id, QPoint global)
    {
        auto* pop = new QMenu(parent);
        pop->setAttribute(Qt::WA_DeleteOnClose);
        QString key = Hotkeys::get().keyFor(id);
        pop->addAction(key.isEmpty() ? QString("Assign Hotkey...") : QString("Assign Hotkey...  (%1)").arg(key), [id] {
            QTimer::singleShot(0, [id] {
                for (int i = 0; i < 16 && QApplication::activePopupWidget(); ++i)
                    QApplication::activePopupWidget()->close();
                assignHotkeyDialog(id, nullptr);
            });
        });
        mPopup = pop;
        pop->popup(global);
    }

    QPointer<QMenu> mPopup;
};
} // namespace

void installMenuHotkeyHook()
{
    static MenuHotkeyHook* hook = nullptr;
    if (hook)
        return;
    hook = new MenuHotkeyHook;
    hook->setParent(qApp);
    qApp->installEventFilter(hook);
}
} // namespace lila::ui
