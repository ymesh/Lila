#include "ui/PreferencesDialog.h"

#include "ops/Command.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/TextMatch.h"
#include "io/FileTypes.h"
#include "io/SceneTemplate.h"
#include "scene/Anim.h"
#include "scene/Ghosts.h"
#include "scene/Naming.h"
#include "scene/ComponentPick.h"
#include "scene/Transform.h"
#include "stream/ChangeStream.h"
#include "ui/ColorPopup.h"
#include "ui/CommandPreview.h"
#include "ui/HotkeyChord.h"
#include "ui/Icons.h"
#include "ui/Issues.h"
#include "ui/LocateFlash.h"
#include "ui/SceneDialogs.h"
#include "ui/MenuSets.h"
#include "ui/SceneLayout.h"
#include "ui/Theme.h"
#include "ui/LodDisplay.h"
#include "ui/RigViewport.h"
#include "ui/SoftSelectPanel.h"
#include "ui/SpinScrub.h"
#include "ui/TearOff.h"
#include "ui/TexturePicker.h"
#include "ui/ThemeEditor.h"
#include "ui/Telemetry.h"
#include "ui/WelcomeNews.h"
#include "ui/Updater.h"
#include "ui/ThemeModel.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/EditorPalette.h"
#include "ui/editor/Format.h"
#include "ui/DocsUi.h"
#include "ui/FileDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>
#include <memory>
#include <functional>
#include <tuple>

namespace lila::ui
{
namespace
{

constexpr const char* kCollapsedPref = "/preferences/collapsed";
constexpr int kResetDelaySeconds = 3;
/** Consent and identity, not preferences: Reset All keeps them so it can't turn telemetry back on or break MCP clients. */
constexpr std::pair<const char*, const char*> kKeptOnReset[] = {{"general", "/telemetry"}, {"general", "/mcp/token"}};

QPointer<PreferencesDialog> gPreferencesDialog;

/** Where Reset All keeps the user's prefs it replaced, one object per prefs file. */
std::filesystem::path backupPath()
{
    return PrefStore::get().userDir().parent_path() / "prefsBackup.json";
}

json readBackup()
{
    std::ifstream in(backupPath(), std::ios::binary);
    json j = in ? json::parse(in, nullptr, false, true) : json();
    return j.is_object() ? j : json();
}

/**
 * Asks before resetting: Reset stays disabled for a few seconds so a stray double click can't confirm it. A popup, so a
 * click anywhere else dismisses it.
 */
class ResetConfirm : public QFrame
{
public:
    ResetConfirm(QWidget* anchor, std::function<void()> reset) : QFrame(anchor, Qt::Popup)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel("Reset every preference in this window to its default?\n"
                                "Your current ones are backed up first: Restore brings them back.",
                                this));
        auto* buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto* cancel = new QPushButton("Cancel", this);
        connect(cancel, &QPushButton::clicked, this, &QWidget::close);
        buttons->addWidget(cancel);
        mReset = new QPushButton(this);
        connect(mReset, &QPushButton::clicked, this, [this, reset] {
            close();
            reset();
        });
        buttons->addWidget(mReset);
        v->addLayout(buttons);
        cancel->setFocus();
        tick();
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            tick();
            if (mLeft < 0)
                timer->stop();
        });
        timer->start(1000);
        adjustSize();
        move(anchor->mapToGlobal(QPoint(0, -height() - 4)));
    }

private:
    void tick()
    {
        mReset->setText(mLeft > 0 ? QString("Reset (%1)").arg(mLeft) : QString("Reset"));
        mReset->setEnabled(mLeft <= 0);
        --mLeft;
    }

    QPushButton* mReset;
    int mLeft = kResetDelaySeconds;
};

/** A collapsible run of rows under a header; the rows stay in the page's form so labels line up across sections. */
struct Section
{
    QToolButton* header;
    std::vector<int> rows;
};

/** One pref's row in a page's form, which the search lifts into its flat results and puts back. */
struct PrefRow
{
    QFormLayout* form;
    int index;
    std::shared_ptr<Section> section;
    QString category;
    QWidget* label;
    QWidget* field;
    std::vector<std::string> searchText;
    bool lifted = false;
    /** Spans both columns on its page; the label only shows in search results. */
    bool wide = false;
};

struct Page
{
    QWidget* widget;
    QFormLayout* form;
    const char* file;
    std::string title;
    std::shared_ptr<Section> section;
    std::vector<PrefRow>* rows;
};

/**
 * Filters every page's rows into one flat list. The rows' own label and editor widgets move into the results and back,
 * so an edit there is the same edit as in the tab.
 */
class PrefSearch
{
public:
    PrefSearch(QTabWidget* tabs, QWidget* resultsView, QGridLayout* grid, QLabel* empty)
        : mTabs(tabs), mResultsView(resultsView), mGrid(grid), mEmpty(empty)
    {
    }

    std::vector<PrefRow> rows;

    void apply(const QString& text)
    {
        restore();
        textMatch::Query query(text.toStdString());
        mTabs->setVisible(query.empty());
        mResultsView->setVisible(!query.empty());
        if (query.empty())
            return;
        // Taken bottom-up so each row's recorded index still holds while the rows above it are in place.
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
        {
            if (!query.matches(it->searchText))
                continue;
            QFormLayout::TakeRowResult taken = it->form->takeRow(it->index);
            delete taken.labelItem;
            delete taken.fieldItem;
            it->lifted = true;
        }
        int line = 0;
        for (PrefRow& r : rows)
        {
            if (!r.lifted)
                continue;
            auto* category = new QLabel(r.category);
            QPalette pal = category->palette();
            pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
            category->setPalette(pal);
            mCategories.push_back(category);
            mGrid->addWidget(category, line, 0);
            if (r.wide)
                mGrid->addWidget(r.field, line, 1, 1, 2);
            else
            {
                mGrid->addWidget(r.label, line, 1, Qt::AlignRight | Qt::AlignVCenter);
                mGrid->addWidget(r.field, line, 2);
                r.label->show();
            }
            r.field->show();
            ++line;
        }
        mEmpty->setVisible(line == 0);
    }

private:
    void restore()
    {
        for (PrefRow& r : rows)
        {
            if (!r.lifted)
                continue;
            mGrid->removeWidget(r.label);
            mGrid->removeWidget(r.field);
            if (r.wide)
            {
                r.form->insertRow(r.index, r.field);
                r.label->hide();
            }
            else
                r.form->insertRow(r.index, r.label, r.field);
            r.form->setRowVisible(r.index, !r.section || r.section->header->isChecked());
            r.lifted = false;
        }
        for (QLabel* c : mCategories)
            delete c;
        mCategories.clear();
    }

    QTabWidget* mTabs;
    QWidget* mResultsView;
    QGridLayout* mGrid;
    QLabel* mEmpty;
    std::vector<QLabel*> mCategories;
};

/** Escape clears a non-empty search instead of closing the dialog. */
class ClearOnEscape : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        auto* edit = static_cast<QLineEdit*>(obj);
        bool escape = (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) &&
                      static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape;
        if (!escape || edit->text().isEmpty())
            return false;
        if (e->type() == QEvent::ShortcutOverride)
        {
            e->accept();
            return true;
        }
        edit->clear();
        return true;
    }
};

Page page(QTabWidget* tabs, std::vector<PrefRow>* rows, const char* title, const char* file)
{
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->setLabelAlignment(Qt::AlignRight);
    scroll->setWidget(w);
    tabs->addTab(scroll, title);
    return {w, form, file, title, nullptr, rows};
}

/** Greys out a field whose key a studio folder locks. */
void applyStudioLock(const Page& p, QWidget* field, const std::string& key)
{
    if (key.empty() || !PrefStore::get().isLocked(p.file, key))
        return;
    field->setEnabled(false);
    field->setToolTip("Set by your studio" + (field->toolTip().isEmpty() ? QString() : "\n" + field->toolTip()));
}

/**
 * Adds a row and records it for the search; `key` is what it edits, matched like the label and tooltips. `fallback` is
 * the value the row shows while the key is unset, which its label's Copy Value gives too.
 */
template <typename Label>
void addRow(Page& p, Label label, QWidget* field, const std::string& key, json fallback = nullptr)
{
    applyStudioLock(p, field, key);
    p.form->addRow(label, field);
    int row = p.form->rowCount() - 1;
    QWidget* labelWidget = p.form->itemAt(row, QFormLayout::LabelRole)->widget();
    if (!key.empty())
    {
        std::string file = p.file;
        onRightClickCopy(labelWidget, QString::fromStdString(file + (key.front() == '/' ? "" : "/") + key), [file, key, fallback] {
            json v = PrefStore::get().value(file, key);
            return v.is_null() ? fallback : v;
        });
    }
    QString labelText = labelWidget->property("text").toString();
    QString category = QString::fromStdString(p.title);
    if (p.section)
        category += " / " + p.section->header->text();
    p.rows->push_back({p.form, row, p.section, category, labelWidget, field,
                       {labelText.toStdString(), labelWidget->toolTip().toStdString(), field->toolTip().toStdString(),
                        std::string(p.file) + key, category.toStdString()}});
    if (!p.section)
        return;
    p.section->rows.push_back(row);
    if (!p.section->header->isChecked())
        p.form->setRowVisible(row, false);
}

/** A row spanning the page width, for an editor too big for the field column. */
void addWideRow(Page& p, const QString& label, QWidget* field, const std::string& key)
{
    applyStudioLock(p, field, key);
    p.form->addRow(field);
    int row = p.form->rowCount() - 1;
    auto* labelWidget = new QLabel(label, p.widget);
    labelWidget->hide();
    QString category = QString::fromStdString(p.title);
    if (p.section)
        category += " / " + p.section->header->text();
    p.rows->push_back({p.form, row, p.section, category, labelWidget, field,
                       {label.toStdString(), field->toolTip().toStdString(), std::string(p.file) + key, category.toStdString()}});
    p.rows->back().wide = true;
    if (!p.section)
        return;
    p.section->rows.push_back(row);
    if (!p.section->header->isChecked())
        p.form->setRowVisible(row, false);
}

/** Starts a section: rows added after it sit under its header. The collapsed state is remembered per page and title. */
void sectionHeader(Page& p, const char* title)
{
    auto* header = new QToolButton;
    header->setText(title);
    header->setCheckable(true);
    header->setAutoRaise(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setStyleSheet(QString("QToolButton { text-align: left; font-weight: 600; padding: 3px; margin-top: %1px; }")
                              .arg(p.form->rowCount() ? 8 : 0));
    std::string key = std::string(kCollapsedPref) + "/" + p.title + "/" + title;
    bool open = !PrefStore::get().value<bool>("general", key, false);
    header->setChecked(open);
    header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    p.form->addRow(header);
    auto s = std::make_shared<Section>(Section{header, {}});
    QFormLayout* form = p.form;
    QObject::connect(header, &QToolButton::toggled, header, [s, form, key](bool on) {
        s->header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        for (int row : s->rows)
            form->setRowVisible(row, on);
        PrefStore::get().set("general", key, !on);
    });
    p.section = std::move(s);
}

void choiceField(Page& p, const char* label, const char* key, std::vector<std::pair<QString, std::string>> options, const std::string& fallback)
{
    auto* box = new QComboBox;
    std::string current = PrefStore::get().value<std::string>(p.file, key, fallback);
    for (auto& [text, value] : options)
    {
        box->addItem(text, QString::fromStdString(value));
        if (value == current)
            box->setCurrentIndex(box->count() - 1);
    }
    std::string file = p.file, k = key;
    QObject::connect(box, &QComboBox::currentIndexChanged, box,
                     [box, file, k](int) { PrefStore::get().set(file, k, box->currentData().toString().toStdString()); });
    setComboDefault(box, box->findData(QString::fromStdString(fallback)));
    addRow(p, label, box, k, fallback);
}

/** A combo of whole-number values. A stored value that isn't among them (set from a script) joins the list. */
void intChoiceField(Page& p, const char* label, const char* key, std::vector<std::pair<QString, int>> options, int fallback)
{
    auto* box = new QComboBox;
    const int current = PrefStore::get().value<int>(p.file, key, fallback);
    if (std::none_of(options.begin(), options.end(), [&](const auto& o) { return o.second == current; }))
        options.emplace_back(QString::number(current), current);
    for (auto& [text, value] : options)
    {
        box->addItem(text, value);
        if (value == current)
            box->setCurrentIndex(box->count() - 1);
    }
    std::string file = p.file, k = key;
    QObject::connect(box, &QComboBox::currentIndexChanged, box, [box, file, k](int) { PrefStore::get().set(file, k, box->currentData().toInt()); });
    setComboDefault(box, box->findData(fallback));
    addRow(p, label, box, k, fallback);
}

void toggleField(Page& p, const char* label, const char* key, bool fallback)
{
    auto* c = new QCheckBox;
    c->setChecked(PrefStore::get().value<bool>(p.file, key, fallback));
    std::string file = p.file, k = key;
    QObject::connect(c, &QCheckBox::toggled, c, [file, k](bool on) { PrefStore::get().set(file, k, on); });
    // Toolbar buttons and hotkeys flip some of these prefs while the dialog is open.
    int listener = PrefStore::get().onChanged([c, file, k, fallback](const std::string& changed) {
        bool on = PrefStore::get().value<bool>(file, k, fallback);
        if (changed == file && c->isChecked() != on)
        {
            QSignalBlocker block(c);
            c->setChecked(on);
        }
    });
    QObject::connect(c, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
    setCheckDefault(c, fallback);
    addRow(p, label, c, k, fallback);
}

void numberField(Page& p, const char* label, const char* key, double fallback, double lo, double hi, double step, int decimals = 2)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setValue(PrefStore::get().value<double>(p.file, key, fallback));
    std::string file = p.file, k = key;
    QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [file, k](double v) { PrefStore::get().set(file, k, v); });
    setSpinDefault(s, fallback);
    addRow(p, label, s, k, fallback);
}

/** A swatch that opens a color picker; the pref holds sRGB "#rrggbb". */
void colorField(Page& p, const char* label, const std::string& key, const std::string& fallback)
{
    auto* b = new QToolButton;
    b->setFixedSize(40, 18);
    b->setProperty("lilaRawColors", true);
    auto show = [b](const QString& hex) { b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #262626; }").arg(hex)); };
    std::string file = p.file;
    auto current = [file, key, fallback] {
        QColor c(QString::fromStdString(PrefStore::get().value<std::string>(file, key, fallback)));
        return c.isValid() ? c : QColor(QString::fromStdString(fallback));
    };
    show(current().name());
    QObject::connect(b, &QToolButton::clicked, b, [b, show, current, file, key] {
        QColor c = colorPopup::getColor(current(), b, "Choose Color");
        if (!c.isValid())
            return;
        PrefStore::get().set(file, key, c.name().toStdString());
        show(c.name());
    });
    auto* name = new QLabel(label);
    onMiddleClickReset(name, [show, file, key, fallback] {
        PrefStore::get().set(file, key, fallback);
        show(QString::fromStdString(fallback));
    }, QString::fromStdString(fallback), b);
    QWidget* field = colorPopup::withEyedropper(b, [show, file, key](const glm::dvec3& c) {
        const QString hex = colorMath::display(c).name();
        PrefStore::get().set(file, key, hex.toStdString());
        show(hex);
    });
    addRow(p, name, field, key, fallback);
}

void text(Page& p, const char* label, const char* key, const std::string& fallback, bool fileButton = false)
{
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* e = new QLineEdit(QString::fromStdString(PrefStore::get().value<std::string>(p.file, key, fallback)));
    h->addWidget(e, 1);
    std::string file = p.file, k = key;
    QObject::connect(e, &QLineEdit::editingFinished, e, [e, file, k] { PrefStore::get().set(file, k, e->text().toStdString()); });
    if (fileButton)
    {
        auto* b = new QPushButton("...");
        b->setFixedWidth(28);
        QObject::connect(b, &QPushButton::clicked, e, [e, file, k] {
            QString f = ui::fileDialog::getOpenFileName(e, "Choose File", e->text());
            if (f.isEmpty())
                return;
            e->setText(f);
            PrefStore::get().set(file, k, f.toStdString());
        });
        h->addWidget(b);
    }
    auto* name = new QLabel(label);
    onMiddleClickReset(name, [e, file, k, fallback] {
        e->setText(QString::fromStdString(fallback));
        PrefStore::get().set(file, k, fallback);
    }, fallback.empty() ? "empty" : QString::fromStdString(fallback), e);
    addRow(p, name, row, k, fallback);
}

/** Shows the page's last row only while shown() holds (and its section is open), checked whenever the page's prefs change. */
void showLastRowWhen(Page& p, std::function<bool()> shown)
{
    QFormLayout* form = p.form;
    const int row = form->rowCount() - 1;
    std::shared_ptr<Section> section = p.section;
    auto update = [form, row, section, shown] { form->setRowVisible(row, (!section || section->header->isChecked()) && shown()); };
    update();
    QWidget* field = form->itemAt(row, QFormLayout::FieldRole)->widget();
    if (section)
        QObject::connect(section->header, &QToolButton::toggled, field, update);
    const std::string file = p.file;
    int listener = PrefStore::get().onChanged([update, file](const std::string& changed) {
        if (changed == file)
            update();
    });
    QObject::connect(field, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
}

/**
 * Disables a row while disabled() holds, with `reason` as its tooltip. Checked whenever the page's prefs or the general
 * prefs (theme, app-wide switches) change.
 */
void disableRowWhen(Page& p, int row, std::function<bool()> disabled, const QString& reason)
{
    QWidget* label = p.form->itemAt(row, QFormLayout::LabelRole)->widget();
    QWidget* field = p.form->itemAt(row, QFormLayout::FieldRole)->widget();
    QPointer<QWidget> labelPtr(label), fieldPtr(field);
    auto update = [labelPtr, fieldPtr, disabled, reason] {
        if (!labelPtr || !fieldPtr)
            return;
        bool off = disabled();
        for (QWidget* w : {labelPtr.data(), fieldPtr.data()})
        {
            w->setEnabled(!off);
            w->setToolTip(off ? reason : QString());
        }
    };
    update();
    const std::string file = p.file;
    int listener = PrefStore::get().onChanged([field, update, file](const std::string& changed) {
        // Deferred: the theme cache drops on this same change.
        if (changed == file || changed == "general")
            QTimer::singleShot(0, field, update);
    });
    QObject::connect(field, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
}

/** Whether a naming rule's case is one where underscores need allowing, and they are allowed. */
bool namingUnderscores(const std::string& type, const std::string& fallbackCase, bool needAllowed)
{
    PrefStore& prefs = PrefStore::get();
    return prefs.value<std::string>("naming", "/rules/" + type, fallbackCase) != "snake_case" &&
           (!needAllowed || prefs.value<bool>("naming", "/allowUnderscores/" + type, true));
}

Page& rowsOf(void* page)
{
    return *static_cast<Page*>(page);
}

void builtIn(Registry<prefPages::Page>& r, const char* id, const char* title, const char* file, std::function<void(Page&)> build)
{
    r.put(id, {id, title, file, [build](prefPages::PageBuilder& b) { build(*static_cast<Page*>(b.mPage)); }}, kLilaOwner);
}

/** Lila's own pages, in tab order. */
void addBuiltIns(Registry<prefPages::Page>& r)
{
    namespace vd = viewportDisplay;
    builtIn(r, "general", "General", "general", [](Page& general) {
        sectionHeader(general, "Startup");
        toggleField(general, "Show the welcome dialog (examples and recent scenes)", kShowWelcomePref, true);
        toggleField(general, "Show recent scenes when the welcome dialog is off", kShowRecentPref, true);
        toggleField(general, "Show news on the welcome dialog (downloaded from the Lila site)", news::kEnabledPref, true);
        numberField(general, "Recent scenes to keep", "/recentFilesMax", 20.0, 1.0, 200.0, 1.0, 0);
        sectionHeader(general, "Scene");
        text(general, "New Scene template", io::sceneTemplate::kPref, "", true);
        choiceField(general, "Up axis", "/scene/upAxis", {{"Y up", "y"}, {"Z up", "z"}}, "y");
        choiceField(general, "Save format", "/scene/defaultFormat", {{"Binary (.lila)", "binary"}, {"Text (.lilaa)", "ascii"}}, "binary");
        numberField(general, "Autosave every (seconds)", "/autosave/intervalSeconds", 30.0, 5.0, 3600.0, 5.0, 0);
        numberField(general, "Keep recovered scenes (days, 0 = forever)", "/recovery/keepDays", 30.0, 0.0, 3650.0, 1.0, 0);
        sectionHeader(general, "File types");
        toggleField(general, "Open .lila and .lilaa with Lila", io::fileTypes::kScenesPref, io::fileTypes::scenesByDefault(paths::executableDir()));
        {
            auto* grid = new QWidget;
            auto* layout = new QGridLayout(grid);
            layout->setContentsMargins(0, 0, 0, 0);
            int at = 0;
            for (const io::fileTypes::Format& f : io::fileTypes::formats())
            {
                if (f.scene)
                    continue;
                auto* c = new QCheckBox(QString::fromStdString(f.ext));
                c->setToolTip(QString::fromStdString(f.description));
                const json ticked = PrefStore::get().value("general", io::fileTypes::kImportsPref, json::array());
                c->setChecked(ticked.is_array() && std::ranges::find(ticked, json(f.ext)) != ticked.end());
                QObject::connect(c, &QCheckBox::toggled, c, [ext = f.ext](bool on) {
                    const json was = PrefStore::get().value("general", io::fileTypes::kImportsPref, json::array());
                    json list = json::array();
                    for (const json& e : was.is_array() ? was : json::array())
                        if (e != ext)
                            list.push_back(e);
                    if (on)
                        list.push_back(ext);
                    PrefStore::get().set("general", io::fileTypes::kImportsPref, std::move(list));
                });
                layout->addWidget(c, at / 5, at % 5);
                ++at;
            }
            grid->setToolTip("Ticked formats open with Lila on double-click. Unticking hands them back.");
            addRow(general, "Open with Lila", grid, io::fileTypes::kImportsPref, json::array());
        }
        sectionHeader(general, "Undo");
        toggleField(general, "Infinite undo", "/undo/infinite", true);
        numberField(general, "Steps (when not infinite)", "/undo/steps", 500.0, 1.0, 100000.0, 50.0, 0);
        numberField(general, "Memory budget (MB)", "/undo/memoryMB", 2048.0, 0.0, 1048576.0, 256.0, 0);
        numberField(general, "Held key repeats after (ms)", "/undo/repeatDelayMs", 180.0, 0.0, 2000.0, 10.0, 0);
        numberField(general, "Held key repeats every (ms, 0 = off)", "/undo/repeatIntervalMs", 100.0, 0.0, 2000.0, 10.0, 0);
        sectionHeader(general, "Textures");
        {
            auto* edit = new QPushButton("Texture Folders...");
            edit->setToolTip("Which folders of the project the material editor's texture lists show, per workspace and per scene");
            QObject::connect(edit, &QPushButton::clicked, edit, [edit] { texturePicker::showFoldersDialog(edit->window()); });
            addRow(general, "Project texture folders", edit, "/textures");
        }
        sectionHeader(general, "References");
        toggleField(general, "Reload changed references", "/references/autoReload", true);
        sectionHeader(general, "Terminal");
        text(general, "Shell", "/terminal/shell", "");
        numberField(general, "Scrollback (lines)", "/terminal/scrollback", 10000.0, 0.0, 1000000.0, 1000.0, 0);
        toggleField(general, "Typing scrolls to bottom", "/terminal/scrollOnInput", true);
        toggleField(general, "Output scrolls to bottom", "/terminal/scrollOnOutput", false);
        toggleField(general, "Offer agent setup per workspace", "/agents/promptOnTerminal", true);
        sectionHeader(general, "Docs");
        toggleField(general, "Download docs updates at startup", "/docs/autoUpdate", true);
        toggleField(general, "Check for Lila updates at startup", updater::kCheckPref, true);
        sectionHeader(general, "Telemetry");
        toggleField(general, "Send anonymous usage statistics", kTelemetryUsagePref, false);
        toggleField(general, "Send crash reports", kTelemetryCrashPref, false);
        toggleField(general, "Send crash reports without asking", telemetry::kCrashAlwaysPref, false);
        auto onboarding = [&general](const char* label, const char* key, const std::vector<std::pair<std::string, std::string>>& choices) {
            std::vector<std::pair<QString, std::string>> options;
            for (const auto& [shown, value] : choices)
                options.emplace_back(QString::fromStdString(shown), value);
            choiceField(general, label, key, std::move(options), "");
        };
        onboarding("Your role", telemetry::kRolePref, telemetry::roleChoices());
        onboarding("Team size", telemetry::kTeamSizePref, telemetry::teamSizeChoices());
        onboarding("Used before Lila", telemetry::kPreviousDccPref, telemetry::previousDccChoices());
        {
            auto* buttons = new QWidget;
            auto* row = new QHBoxLayout(buttons);
            row->setContentsMargins(0, 0, 0, 0);
            auto* sent = new QPushButton("What's Sent...");
            QObject::connect(sent, &QPushButton::clicked, sent, [sent] { telemetry::showPending(sent->window()); });
            auto* reset = new QPushButton("Reset ID");
            reset->setToolTip("A new anonymous id: what is sent from now on can't be linked to what was sent before.");
            QObject::connect(reset, &QPushButton::clicked, reset, [] { telemetry::resetInstallId(); });
            auto* details = new QPushButton("Privacy Details...");
            QObject::connect(details, &QPushButton::clicked, details, [] { docs::open("privacy"); });
            row->addWidget(sent);
            row->addWidget(reset);
            row->addWidget(details);
            row->addStretch(1);
            addRow(general, "Anonymous id", buttons, telemetry::kInstallIdPref);
        }
        sectionHeader(general, "Change Stream");
        toggleField(general, "Stream scene changes to other processes", "/changeStream/enabled", false);
        numberField(general, "Port", "/changeStream/port", double(stream::kDefaultPort), 1.0, 65535.0, 1.0, 0);
        toggleField(general, "Localhost only", "/changeStream/localOnly", true);
        general.form->itemAt(general.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("Off, anyone who can reach this machine on the port can read the scene.");
    });

    builtIn(r, "interface", "Interface", "general", [](Page& ui) {
        sectionHeader(ui, "Theme");
        addWideRow(ui, "Theme", makeThemeEditor(), themes::kThemePref);
        sectionHeader(ui, "Chrome");
        intChoiceField(ui, "UI scale (applies after restart)", kUiScalePref, {{"100%", 100}, {"125%", 125}, {"150%", 150}, {"175%", 175}, {"200%", 200}}, 100);
        toggleField(ui, "Show menu bar (the hotbox still has the menus)", "/ui/showMenuBar", true);
        toggleField(ui, "Show shelf (` still opens the floating shelf)", "/ui/showShelf", false);
        toggleField(ui, "Show all menus (otherwise the menu bar follows what you are doing)", menuSets::kAllMenusPref, false);
        toggleField(ui, "Open scenes in the mode and layout they were saved in", sceneLayout::kRestorePref, true);
        toggleField(ui, "Flash windows as they open", kLocateFlashPref, true);
        numberField(ui, "Flash pulses", kLocateFlashPulsesPref, 1.0, 1.0, 10.0, 1.0, 0);
        numberField(ui, "Flash pulse length (s)", kLocateFlashPulseSecondsPref, 0.4, 0.1, 5.0, 0.1, 2);
        numberField(ui, "Flash strength", kLocateFlashStrengthPref, 0.7, 0.05, 1.0, 0.05, 2);
        numberField(ui, "Flash reach from the edges", kLocateFlashReachPref, 0.05, 0.0, 1.0, 0.05, 2);
        colorField(ui, "Flash color", kLocateFlashColorPref, kLocateFlashDefaultColor);
        toggleField(ui, "Tear-offs drop at the cursor", tearoff::kDropAtCursorPref, false);
        toggleField(ui, "Badges at the viewport cursor for held keys", "/viewport/cursorBadges", true);
        choiceField(ui, "Hotkeys follow", hotkeys::kKeysFollowPref,
               {{"Key labels", "labels"}, {"Key positions (US layout)", "positions"}}, "labels");
        choiceField(ui, "Toggle Channel Box (Ctrl+A) switches to", "/ui/channelBoxToggle",
               {{"The tab shown before it", "lastTab"}, {"The attribute editor (Maya)", "attributeEditor"}}, "lastTab");
        sectionHeader(ui, "Issues");
        toggleField(ui, "Badge info in the outliner (warnings and errors always are)", issues::kBadgeInfoPref, false);
        choiceField(ui, "Color outliner names by their issues", issues::kTintRowsPref,
               {{"Off", "off"}, {"Errors", "error"}, {"Warnings and errors", "warning"}, {"Everything, info too", "info"}}, "off");
        sectionHeader(ui, "File Dialog");
        toggleField(ui, "Use the system file dialog instead of Lila's", fileDialog::kNativePref, false);
        toggleField(ui, "Show hidden files", fileDialog::kShowHiddenPref, false);
        toggleField(ui, "Allow rename and new folder", fileOps::kAllowEditPref, true);
        toggleField(ui, "Allow cut, copy, paste and drag-and-drop", fileOps::kAllowMovePref, true);
        toggleField(ui, "Allow delete (to the Recycle Bin, or source control)", fileOps::kAllowDeletePref, true);
        sectionHeader(ui, "Color");
        toggleField(ui, "Monotone (overrides the colors below)", vd::kMonotone, false);
        toggleField(ui, "View axis indicator in X/Y/Z colors", vd::kViewAxisColors, true);
        toggleField(ui, "Manipulator axes in X/Y/Z colors", vd::kManipulatorAxisColors, true);
        choiceField(ui, "Selected color with several selected", vd::kSelectionOrderColors,
               {{"Last selected, the rest white (Maya)", "last"},
                {"All but the first, which is white", "allButFirst"},
                {"First selected, the rest white", "first"},
                {"All selected", "all"}},
               "last");
        toggleField(ui, "Time slider keys in red", vd::kTimeKeyColors, true);
        toggleField(ui, "Graph editor X/Y/Z curves in axis colors", vd::kGraphAxisColors, true);
        toggleField(ui, "Animation ghosts tinted before and after", vd::kGhostColors, true);
        toggleField(ui, "Classic component colors (blue wire, pink vertices, red hover, yellow selection)", vd::kComponentColors, true);
        toggleField(ui, "Window flash in color", kLocateFlashColorsPref, true);
        sectionHeader(ui, "Component Thickness (px)");
        const vd::ComponentThickness thickness;
        numberField(ui, "Edge thickness", vd::kEdgeThickness, thickness.edge, 1.0, 16.0, 0.5, 1);
        numberField(ui, "Hovered edge thickness", vd::kEdgeThicknessHover, thickness.edgeHover, 0.5, 16.0, 0.5, 1);
        numberField(ui, "Selected edge thickness", vd::kEdgeThicknessSelected, thickness.edgeSelected, 1.0, 16.0, 0.5, 1);
        numberField(ui, "Vertex thickness", vd::kVertexThickness, thickness.vertex, 0.5, 32.0, 0.5, 1);
        numberField(ui, "Hovered vertex thickness", vd::kVertexThicknessHover, thickness.vertexHover, 0.5, 32.0, 0.5, 1);
        numberField(ui, "Selected vertex thickness", vd::kVertexThicknessSelected, thickness.vertexSelected, 0.5, 32.0, 0.5, 1);
        numberField(ui, "UV seam thickness", vd::kUvSeamThickness, thickness.uvSeam, 1.0, 16.0, 0.5, 1);
        toggleField(ui, "Selected vertices in edge and face modes", vd::kVerticesInEdgeFaceModes, false);
        toggleField(ui, "Edges on the symmetry plane in red", vd::kSymmetryEdges, true);
        toggleField(ui, "Symmetry plane edges on selected objects in object mode", vd::kSymmetryEdgesObjectMode, true);
        numberField(ui, "Symmetry plane edge thickness", vd::kSymmetryEdgeThickness, thickness.symmetryEdge, 1.0, 16.0, 0.5, 1);
        sectionHeader(ui, "Floating Shelf");
        choiceField(ui, "Opens at", "/floatingShelf/openAt",
               {{"Cursor", "cursor"}, {"Center of viewport", "viewport"}, {"Center of screen", "screen"}}, "viewport");
        numberField(ui, "Offset X (px)", "/floatingShelf/offsetX", 250.0, -2000.0, 2000.0, 10.0, 0);
        numberField(ui, "Offset Y (px)", "/floatingShelf/offsetY", 0.0, -2000.0, 2000.0, 10.0, 0);
        toggleField(ui, "Remember last position (overrides Opens at)", "/floatingShelf/rememberPosition", false);
        toggleField(ui, "Close when clicking elsewhere", "/floatingShelf/closeOnClickAway", false);
        toggleField(ui, "Close after running a button", "/floatingShelf/closeAfterRun", false);
    });

    builtIn(r, "viewport", "Viewport", "general", [](Page& viewport) {
        sectionHeader(viewport, "Environment");
        text(viewport, "HDRI", "/viewport/env/path", "", true);
        numberField(viewport, "Rotation", "/viewport/env/rotation", 0.0, -360.0, 360.0, 5.0, 0);
        numberField(viewport, "Intensity", "/viewport/env/intensity", 1.0, 0.0, 10.0, 0.1);
        numberField(viewport, "Light color", "/viewport/env/lightSaturation", 1.0, 0.0, 1.0, 0.05);
        toggleField(viewport, "Show environment", "/viewport/env/background", true);
        numberField(viewport, "Background blur", "/viewport/env/blur", 0.6, 0.0, 1.0, 0.05);
        numberField(viewport, "Background exposure", "/viewport/env/backgroundExposure", 0.25, 0.0, 4.0, 0.1);
        sectionHeader(viewport, "Lighting");
        toggleField(viewport, "Shadows", vd::kShadows, true);
        numberField(viewport, "Ground shadow", "/viewport/groundShadow", 0.5, 0.0, 1.0, 0.05);
        toggleField(viewport, "Ambient occlusion", "/viewport/ao/enabled", true);
        choiceField(viewport, "AO method", "/viewport/ao/mode",
               {{"Accurate: darkens ambient light only", "accurate"}, {"Fast: darkens the whole surface, no extra geometry pass", "fast"},
                {"Ray traced: against the scene itself, no halos (RTX-class GPUs)", "raytraced"}},
               "accurate");
        numberField(viewport, "AO radius (cm)", "/viewport/ao/radius", 30.0, 0.1, 10000.0, 1.0, 1);
        numberField(viewport, "AO intensity", "/viewport/ao/intensity", 1.0, 0.0, 4.0, 0.1);
        sectionHeader(viewport, "Color");
        numberField(viewport, "Exposure (stops)", vd::kExposure, 0.0, -10.0, 10.0, 0.1);
        numberField(viewport, "Gamma", vd::kGamma, 1.0, 0.1, 4.0, 0.05);
        sectionHeader(viewport, "Antialiasing");
        toggleField(viewport, "Enabled", vd::kAaEnabled, true);
        choiceField(viewport, "Mode", vd::kAaMode,
                    {{"Multisample", "multisample"}, {"Temporal", vd::kAaTemporal}},
                    "multisample");
        {
            auto* box = new QComboBox;
            int current = PrefStore::get().value<int>("general", vd::kAaSamples, vd::kDefaultSamples);
            for (int n : {2, 4, 8})
            {
                box->addItem(QString("%1x").arg(n), n);
                if (n == current)
                    box->setCurrentIndex(box->count() - 1);
            }
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [box](int) { PrefStore::get().set("general", vd::kAaSamples, box->currentData().toInt()); });
            setComboDefault(box, box->findData(vd::kDefaultSamples));
            addRow(viewport, "Multisample samples", box, vd::kAaSamples);
        }
        toggleField(viewport, "Temporal (smooths shading shimmer)", vd::kTaa, true);
        numberField(viewport, "Still frames averaged", vd::kAccumulate, 16.0, 1.0, 256.0, 1.0, 0);
        sectionHeader(viewport, "Frame Rate");
        intChoiceField(viewport, "Max frame rate", vd::kFrameCap,
                       {{"Display refresh", render::kFrameCapRefresh}, {"30", 30}, {"60", 60}, {"75", 75}, {"90", 90}, {"120", 120}, {"144", 144},
                        {"165", 165}, {"240", 240}, {"Unlimited", 0}},
                       render::kFrameCapRefresh);
        intChoiceField(viewport, "In background", vd::kBackgroundFrameCap,
                       {{"Same as above", 0}, {"10", 10}, {"15", 15}, {"30", 30}, {"60", 60}}, vd::kDefaultBackgroundFrameCap);
        toggleField(viewport, "VSync", vd::kVsync, false);
        sectionHeader(viewport, "Grid");
        toggleField(viewport, "Show grid", vd::kGrid, true);
        numberField(viewport, "Spacing (cm)", "/grid/spacing", 10.0, 0.1, 10000.0, 1.0, 1);
        sectionHeader(viewport, "Display");
        toggleField(viewport, "Poly count", "/viewport/polyCount", true);
        toggleField(viewport, "Panel toolbar", vd::kToolbar, true);
        toggleField(viewport, "Smooth preview shows the subdivided edges", vd::kSmoothSubdivisions, false);
        toggleField(viewport, "Smooth preview keeps hard edges", vd::kSmoothHardEdges, false);
        toggleField(viewport, "Hiding a parent hides its children", scene::kInheritVisibilityPref, true);
        sectionHeader(viewport, "Deformers");
        toggleField(viewport, "Evaluate deformers on the GPU (exports always use the CPU)", "/deformers/gpu", true);
        numberField(viewport, "GPU for meshes from (points)", "/deformers/gpuMinPoints", 20000.0, 0.0, 100000000.0, 1000.0, 0);
        sectionHeader(viewport, "Level of Detail");
        choiceField(viewport, "LOD groups show by default", lodDisplay::kMode,
               {{"The level for their screen size", "screenSize"}, {"A fixed level", "level"}, {"All levels", "all"}}, "screenSize");
        numberField(viewport, "Fixed level", lodDisplay::kLevel, 0.0, 0.0, double(scene::lod::kMaxLevels - 1), 1.0, 0);
        sectionHeader(viewport, "Selection");
        toggleField(viewport, "Marquee selects edges and faces it touches (off: only those whose centre it covers)", "/select/marqueeTouch", true);
        toggleField(viewport, "Camera-based selection (component marquees skip what is hidden behind surfaces)", "/select/cameraBased", false);
        toggleField(viewport, "Backface culling selection (skip components on faces turned away)", "/select/backfaceCulling", false);
        toggleField(viewport, "Click selects edges by their center", scene::componentPick::kEdgesByCenter, false);
        toggleField(viewport, "Click selects faces by their center", scene::componentPick::kFacesByCenter, false);
        sectionHeader(viewport, "Panel Bars");
        toggleField(viewport, "Hide toolbar items the scene has no use for (X-Ray Joints with no joints)", vd::kHideUnusedItems, true);
        sectionHeader(viewport, "Debug Drawing");
        toggleField(viewport, "Show debug drawing", "/debugDraw/show", true);
        numberField(viewport, "Default duration (s)", "/debugDraw/defaultDuration", 5.0, 0.0, 3600.0, 1.0, 1);
        numberField(viewport, "Longest duration without persistent (s)", "/debugDraw/maxDuration", 3600.0, 1.0, 86400.0, 60.0, 0);
        numberField(viewport, "Most lines and triangles", "/debugDraw/maxPrimitives", 1000000.0, 1000.0, 50000000.0, 100000.0, 0);
        numberField(viewport, "Most points", "/debugDraw/maxPoints", 100000.0, 10.0, 10000000.0, 10000.0, 0);
        numberField(viewport, "Most text labels", "/debugDraw/maxTexts", 10000.0, 10.0, 1000000.0, 1000.0, 0);
        sectionHeader(viewport, "Framing Zoom (2 = twice as close, below 1 = further out)");
        numberField(viewport, "Frame Selected zoom factor", "/viewport/frameSelected/zoomFactor", 1.0, 0.1, 100.0, 0.25, 2);
        numberField(viewport, "Frame All zoom factor", "/viewport/frameAll/zoomFactor", 1.0, 0.1, 100.0, 0.25, 2);
        toggleField(viewport, "Frame Under Cursor also zooms in", "/viewport/frameUnderCursor/zoom", true);
        numberField(viewport, "Frame Under Cursor zoom factor", "/viewport/frameUnderCursor/zoomFactor", 8.0, 0.1, 100.0, 0.25, 2);
        sectionHeader(viewport, "Brushes");
        toggleField(viewport, "Tumble around the last brush stroke", viewportTools::kOrbitStrokePref, true);
        toggleField(viewport, "Undoing a stroke moves the tumble point back", viewportTools::kOrbitUndoPref, true);
        sectionHeader(viewport, "View Cube");
        numberField(viewport, "Turn time (s, 0 snaps)", "/viewport/viewCube/turnTime", 0.15, 0.0, 2.0, 0.05, 2);
        toggleField(viewport, "Left-drag tumbles", "/viewport/viewCube/leftDrag", true);
        toggleField(viewport, "Middle-drag tracks", "/viewport/viewCube/middleDrag", true);
        toggleField(viewport, "Right-drag dollies", "/viewport/viewCube/rightDrag", true);
        sectionHeader(viewport, "Highlight Under Cursor");
        toggleField(viewport, "Components", "/viewport/preselect/components", true);
        toggleField(viewport, "Objects", "/viewport/preselect/objects", false);
        sectionHeader(viewport, "Topology Under Cursor");
        {
            namespace cp = scene::componentPick;
            for (auto [t, label, colorLabel] : {std::tuple{cp::Topology::Ngon, "N-gons (5+ sides)", "N-gon color"},
                                                {cp::Topology::Triangle, "Triangles", "Triangle color"},
                                                {cp::Topology::Pole, "Poles (5+ edges)", "Pole color"}})
            {
                toggleField(viewport, label, cp::topologyEnabledKey(t).c_str(), true);
                colorField(viewport, colorLabel, cp::topologyColorKey(t), cp::topologyDefaultColor(t));
            }
        }

        sectionHeader(viewport, "Animation Ghosts");
        {
            namespace gh = scene::ghosts;
            toggleField(viewport, "Show ghosts", gh::kEnabled, false);
            choiceField(viewport, "Ghost at", gh::kMode, {{"Previous and next keys", "keys"}, {"Every N frames", "frames"}}, "keys");
            choiceField(viewport, "Ghosts show", gh::kShow, {{"Meshes", "mesh"}, {"Skeletons", "skeleton"}, {"Both", "both"}}, "mesh");
            numberField(viewport, "Ghosts before", gh::kBefore, 2.0, 0.0, 50.0, 1.0, 0);
            numberField(viewport, "Ghosts after", gh::kAfter, 2.0, 0.0, 50.0, 1.0, 0);
            numberField(viewport, "Step (frames, or every Nth key)", gh::kStep, 1.0, 1.0, 1000.0, 1.0, 0);
            numberField(viewport, "Ghost opacity", gh::kOpacity, 0.3, 0.0, 1.0, 0.05);
            colorField(viewport, "Before color", gh::kBeforeColor, gh::kDefaultBeforeColor);
            colorField(viewport, "After color", gh::kAfterColor, gh::kDefaultAfterColor);
        }

        sectionHeader(viewport, "Joints");
        colorField(viewport, "Selected joint color", kJointSelectedColor, kJointDefaultSelectedColor);
        numberField(viewport, "Chain separation (ancestors' step from the selection)", kJointChainSeparation, 1.0, 0.0, 3.0, 0.1);
        toggleField(viewport, "Color ancestors' bones to joints off the chain", kJointChainBranches, false);
        sectionHeader(viewport, "Marking Menu Preview");
        toggleField(viewport, "Preview the command under the cursor", commandPreview::kEnabledPref, true);
        choiceField(viewport, "Shows", commandPreview::kCoveragePref,
               {{"Around the cursor", "cursor"}, {"Around the view's center", "center"}, {"Around the cursor and the view's center", "both"},
                {"The whole result", "all"}},
               commandPreview::kCoverageDefault);
        numberField(viewport, "Radius (px)", commandPreview::kRadiusPxPref, commandPreview::kRadiusPxDefault, 10.0, 4000.0, 10.0, 0);
        toggleField(viewport, "Show the result mesh outside the radius too", commandPreview::kResultMeshPref, false);
        numberField(viewport, "Edges fade as faces turn away (0 to 1)", commandPreview::kEdgeFadePref, commandPreview::kEdgeFadeDefault, 0.0, 1.0, 0.05, 2);
        numberField(viewport, "Edges pulled toward the camera (% of mesh size)", commandPreview::kPullPref, commandPreview::kPullDefault, 0.0, 50.0, 0.1, 2);
        numberField(viewport, "Edges fade out behind the surface over (% of mesh size)", commandPreview::kBehindPref, commandPreview::kBehindDefault, 0.0, 100.0, 0.5, 1);
        numberField(viewport, "Preview after resting on an item (ms)", commandPreview::kDelayMsPref, commandPreview::kDelayMsDefault, 0.0, 2000.0, 10.0, 0);
        numberField(viewport, "No preview above (faces selected, 0 = no limit)", commandPreview::kMaxFacesPref, commandPreview::kMaxFacesDefault, 0.0,
               1e9, 50000.0, 0);
        numberField(viewport, "Previews slower than this are skipped (ms)", commandPreview::kSlowMsPref, commandPreview::kSlowMsDefault, 10.0, 60000.0, 50.0, 0);
        numberField(viewport, "Retry a slow preview after (seconds)", commandPreview::kRetrySecondsPref, commandPreview::kRetrySecondsDefault, 0.0, 86400.0, 10.0, 0);
    });

    builtIn(r, "manipulators", "Manipulators", "general", [](Page& manip) {
        sectionHeader(manip, "Manipulators");
        numberField(manip, "Size (pixels)", "/manipulator/size", 100.0, 30.0, 400.0, 5.0, 0);
        choiceField(manip, "Move space", "/manipulator/moveSpace", {{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}, {"Component", "component"}, {"Normal", "normal"}}, "world");
        choiceField(manip, "Rotate space", "/manipulator/rotateSpace", {{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}, {"Gimbal", "gimbal"}, {"Component", "component"}}, "world");
        choiceField(manip, "Scale space (components)", "/manipulator/scaleSpace", {{"World", "world"}, {"Object", "object"}, {"Component", "component"}}, "world");
        numberField(manip, "Rotate snap (degrees)", "/manipulator/rotateStep", 15.0, 0.1, 180.0, 1.0, 1);
        sectionHeader(manip, "Soft Select");
        toggleField(manip, "Enabled", "/softSelect/enabled", false);
        numberField(manip, "Radius (cm)", "/softSelect/radius", 25.0, 0.01, 100000.0, 1.0);
        addRow(manip, "Falloff", new SoftSelectPanel, "/softSelect/falloff");
        numberField(manip, "Falloff display strength (0 to 1)", viewportDisplay::kSoftSelectEmphasis, viewportDisplay::kSoftSelectEmphasisDefault, 0.0,
               1.0, 0.1, 2);
    });

    builtIn(r, "measure", "Measure", "general", [](Page& measure) {
        sectionHeader(measure, "Units");
        choiceField(measure, "Display units", "/units/linear",
               {{"Millimeters", "mm"}, {"Centimeters", "cm"}, {"Meters", "m"}, {"Inches", "in"}, {"Feet", "ft"}}, "cm");
        numberField(measure, "Decimal places", "/measure/precision", 2.0, 0.0, 8.0, 1.0, 0);
        sectionHeader(measure, "Clipboard");
        choiceField(measure, "Measure Selection copies", "/measure/clipboard",
               {{"The main number", "value"}, {"The full report", "report"}, {"Nothing", "off"}}, "value");
        toggleField(measure, "Include units when copying", "/measure/clipboardUnits", false);
        sectionHeader(measure, "Measure Tool");
        toggleField(measure, "Attach measure points to the object clicked", "/measure/attach", false);
        numberField(measure, "Result stays on screen (seconds)", "/measure/hudSeconds", 5.0, 0.5, 60.0, 0.5, 1);
        toggleField(measure, "Show measurements", "/measure/show", true);
    });

    builtIn(r, "timeSlider", "Time Slider", "anim", [](Page& timeSlider) {
        sectionHeader(timeSlider, "Playback");
        {
            namespace anim = scene::anim;
            auto* box = new QComboBox;
            for (anim::PlaybackSpeed speed : anim::kPlaybackSpeeds)
            {
                box->addItem(QString::fromUtf8(anim::speedLabel(speed).data(), qsizetype(anim::speedLabel(speed).size())),
                             QString::fromUtf8(anim::speedName(speed).data(), qsizetype(anim::speedName(speed).size())));
                if (speed == anim::playback().speed)
                    box->setCurrentIndex(box->count() - 1);
            }
            box->setToolTip("Real-time drops frames to keep pace with fps. Play every frame shows each one, as fast as the "
                            "scene evaluates or capped at fps.");
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [box](int) {
                try
                {
                    ops::execute("playbackOptions", {{"playbackSpeed", box->currentData().toString().toStdString()}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
            int listener = anim::addPlaybackListener([box] {
                QSignalBlocker block(box);
                box->setCurrentIndex(box->findData(QString::fromUtf8(anim::speedName(anim::playback().speed).data())));
            });
            QObject::connect(box, &QObject::destroyed, [listener] { anim::removePlaybackListener(listener); });
            setComboDefault(box, box->findData("realTime"));
            addRow(timeSlider, "Playback speed", box, "/playback/speed");
        }
        {
            namespace anim = scene::anim;
            auto* spin = new QDoubleSpinBox;
            spin->setRange(anim::kMinCustomSpeed, anim::kMaxCustomSpeed);
            spin->setDecimals(2);
            spin->setSingleStep(0.25);
            spin->setSuffix("x");
            spin->setValue(anim::playback().customSpeed);
            spin->setEnabled(anim::playback().speed == anim::PlaybackSpeed::Custom);
            spin->setToolTip("Real-time multiplier used when Playback speed is Custom Speed, e.g. 0.25 or 1.5.");
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [](double v) {
                try
                {
                    ops::execute("playbackOptions", {{"playbackSpeed", "custom"}, {"speedMultiplier", v}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
            int listener = anim::addPlaybackListener([spin] {
                QSignalBlocker block(spin);
                spin->setValue(anim::playback().customSpeed);
                spin->setEnabled(anim::playback().speed == anim::PlaybackSpeed::Custom);
            });
            QObject::connect(spin, &QObject::destroyed, [listener] { anim::removePlaybackListener(listener); });
            setSpinDefault(spin, 1.0);
            addRow(timeSlider, "Custom speed multiplier", spin, "/playback/speedMultiplier");
        }
    });

    builtIn(r, "graphEditor", "Graph Editor", "graphEditor", [](Page& graph) {
        sectionHeader(graph, "Tool Keys");
        // A tap only switches the tool; the drag starts once the key is held and the mouse travels.
        toggleField(graph, "Hold W, E, R to drag keys", "/holdToDrag", true);
    });

    builtIn(r, "naming", "Naming", "naming", [](Page& naming) {
        sectionHeader(naming, "Conventions");
        toggleField(naming, "Enforce conventions", "/enabled", false);
        choiceField(naming, "When a name breaks them", "/mode", {{"Fix it", "fix"}, {"Warn", "warn"}, {"Refuse", "block"}}, "fix");
        sectionHeader(naming, "Uniqueness");
        // Off, names only need to be unique among siblings and paths (Parent|Child) tell the rest apart.
        toggleField(naming, "Unique node names", "/uniqueNames", false);
        sectionHeader(naming, "Case");
        choiceField(naming, "Joints", "/rules/joint", {{"snake_case", "snake_case"}, {"PascalCase", "PascalCase"}, {"camelCase", "camelCase"}}, "snake_case");
        choiceField(naming, "Everything else", "/rules/*", {{"PascalCase", "PascalCase"}, {"snake_case", "snake_case"}, {"camelCase", "camelCase"}}, "PascalCase");
        for (auto [label, type, fallback] : {std::tuple{"Allow underscores in joints", "joint", "snake_case"},
                                             {"Allow underscores in everything else", "*", "PascalCase"}})
        {
            toggleField(naming, label, ("/allowUnderscores/" + std::string(type)).c_str(), true);
            naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
                ->widget()
                ->setToolTip("PascalCase and camelCase keep the underscores typed in a name: Arm_L, not ArmL.");
            showLastRowWhen(naming, [type, fallback] { return namingUnderscores(type, fallback, false); });
        }
        auto anyUnderscores = [] { return namingUnderscores("joint", "snake_case", true) || namingUnderscores("*", "PascalCase", true); };
        toggleField(naming, "Underscores only before suffixes", "/underscoreSuffixesOnly", true);
        naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("An underscore must be followed by a listed suffix or a number: Arm_L and Wheel_LF stay, Dog_Leg_B becomes DogLeg_B.");
        showLastRowWhen(naming, anyUnderscores);
        sectionHeader(naming, "Prefixes and Suffixes");
        text(naming, "Prefixes", "/prefixes", scene::NamingPolicy::defaults()["prefixes"].get<std::string>());
        naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("Comma-separated. Lead the name before an underscore, matched ignoring case and written as listed: sm_rock becomes SM_Rock.");
        text(naming, "Suffixes", "/underscoreSuffixes", scene::NamingPolicy::defaults()["underscoreSuffixes"].get<std::string>());
        naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("Comma-separated. Matched ignoring case and written as listed, so arm_l becomes Arm_L.");
        showLastRowWhen(naming, [anyUnderscores] { return anyUnderscores() && PrefStore::get().value<bool>("naming", "/underscoreSuffixesOnly", true); });
    });

    builtIn(r, "sourceControl", "Source Control", "sourceControl", [](Page& sc) {
        sectionHeader(sc, "Provider");
        choiceField(sc, "Provider", "/provider", {{"Automatic", "auto"}, {"Perforce", "perforce"}, {"Git", "git"}, {"None", "none"}}, "auto");
        sectionHeader(sc, "Saving");
        choiceField(sc, "Saving a read-only file", "/autoCheckout", {{"Ask first", "prompt"}, {"Check out", "always"}, {"Do nothing", "never"}}, "prompt");
        toggleField(sc, "Mark new files for add", "/autoAdd", true);
        sectionHeader(sc, "Perforce");
        text(sc, "P4PORT", "/perforce/port", "");
        text(sc, "P4USER", "/perforce/user", "");
        text(sc, "P4CLIENT", "/perforce/client", "");
    });

    builtIn(r, "unreal", "Unreal", "unreal", [](Page& unreal) {
        sectionHeader(unreal, "Sync");
        toggleField(unreal, "Sync mapped assets on save", "/autoSync", true);
        text(unreal, "Export cache folder", "/cacheDir", "");
        sectionHeader(unreal, "Skeletal meshes");
        choiceField(unreal, "Character faces", "/facing", {{"+X (as modelled)", "x"}, {"+Y (Epic mannequin)", "y"}}, "x");
    });

    builtIn(r, "scripting", "Scripting", "scriptEditor", [](Page& script) {
        sectionHeader(script, "Script Editor");
        toggleField(script, "Relative line numbers", CodeEditor::kRelativeLineNumbersPref, CodeEditor::kRelativeLineNumbersDefault);
        toggleField(script, "Vim keybindings", "/vim", false);
        toggleField(script, "Monotone", palette::kMonotonePref, false);
        const int monotoneRow = script.form->rowCount() - 1;
        toggleField(script, "App Monotone overrides script editor", palette::kFollowAppMonotonePref, true);
        disableRowWhen(script, monotoneRow, palette::monotoneForcedByApp,
                       "The app is Monotone and \"App Monotone overrides script editor\" is on, so the script editor stays grey");

        sectionHeader(script, "Formatting");
        namespace fk = format::key;
        const format::Settings d;
        auto tip = [&script](const char* text) {
            script.form->itemAt(script.form->rowCount() - 1, QFormLayout::FieldRole)->widget()->setToolTip(text);
        };
        toggleField(script, "Format on save", fk::onSave, d.onSave);
        toggleField(script, "Organize imports on save", fk::organizeImportsOnSave, d.organizeImportsOnSave);
        tip("Sorts and groups imports with ruff's isort rules (Ctrl+Alt+O does it on demand).");
        toggleField(script, "Fix all on save", fk::fixAllOnSave, d.fixAllOnSave);
        tip("Applies ruff's safe fixes, such as removing unused imports.");
        toggleField(script, "Format on paste", fk::onPaste, d.onPaste);
        tip("Formats the statements the pasted text lands in.");
        toggleField(script, "Format on type", fk::onType, d.onType);
        tip("Formats a statement once it is complete: on Enter at the end of a line, or on typing the bracket or colon that ends it.");
        toggleField(script, "Format on focus loss", fk::onFocusLoss, d.onFocusLoss);
        toggleField(script, "Trim trailing whitespace on save", fk::trimTrailingWhitespace, d.trimTrailingWhitespace);
        tip("Leaves strings and the caret's line alone.");
        toggleField(script, "Ensure final newline on save", fk::finalNewline, d.finalNewline);
        toggleField(script, "Use project ruff config", fk::useProjectConfig, d.useProjectConfig);
        tip("A ruff.toml, .ruff.toml or pyproject.toml with [tool.ruff] beside the script or above it overrides the style below.");
        numberField(script, "Line length", fk::lineLength, d.lineLength, 40, 320, 1, 0);
        choiceField(script, "Quote style", fk::quoteStyle, {{"Double", "double"}, {"Single", "single"}, {"Preserve", "preserve"}},
               d.quoteStyle.toStdString());
        numberField(script, "Indent width", fk::indentWidth, d.indentWidth, 1, 8, 1, 0);
        tip("Indentation ruff formats with. Tab in the editor always indents by 4.");

        sectionHeader(script, "Scene Scripts");
        choiceField(script, "Scripts set to run on open", "/sceneScriptsOnOpen", {{"Ask first", "ask"}, {"Never run", "never"}}, "ask");
    });
}
} // namespace

namespace prefPages
{
Registry<Page>& registry()
{
    static Registry<Page> r;
    static bool builtIns = [] {
        // Lila's own, even when an extension's init is what first asks for the registry.
        owners::OwnerScope lila({});
        addBuiltIns(r);
        return true;
    }();
    (void)builtIns;
    return r;
}

void add(Page page, std::string owner)
{
    std::string id = page.id;
    registry().put(std::move(id), std::move(page), std::move(owner));
}

uint64_t add(ext::Context& ctx, Page page)
{
    if (const std::string* owner = registry().ownerOf(page.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "preferences page '" + page.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = page.id;
    add(std::move(page), ctx.owner());
    return ctx.track("preferencesPage:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool remove(const std::string& id)
{
    return registry().remove(id);
}

std::string PageBuilder::file() const
{
    return rowsOf(mPage).file;
}

void PageBuilder::section(const std::string& title)
{
    lila::ui::sectionHeader(rowsOf(mPage), title.c_str());
}

void PageBuilder::toggle(const std::string& label, const std::string& key, bool fallback)
{
    lila::ui::toggleField(rowsOf(mPage), label.c_str(), key.c_str(), fallback);
}

void PageBuilder::number(const std::string& label, const std::string& key, double fallback, double lo, double hi, double step, int decimals)
{
    lila::ui::numberField(rowsOf(mPage), label.c_str(), key.c_str(), fallback, lo, hi, step, decimals);
}

void PageBuilder::choice(const std::string& label, const std::string& key, const std::vector<std::pair<std::string, std::string>>& options,
                         const std::string& fallback)
{
    std::vector<std::pair<QString, std::string>> opts;
    for (auto& [text, value] : options)
        opts.emplace_back(QString::fromStdString(text), value);
    lila::ui::choiceField(rowsOf(mPage), label.c_str(), key.c_str(), std::move(opts), fallback);
}

void PageBuilder::text(const std::string& label, const std::string& key, const std::string& fallback, bool fileButton)
{
    lila::ui::text(rowsOf(mPage), label.c_str(), key.c_str(), fallback, fileButton);
}

void PageBuilder::color(const std::string& label, const std::string& key, const std::string& fallback)
{
    lila::ui::colorField(rowsOf(mPage), label.c_str(), key, fallback);
}

void PageBuilder::row(const std::string& label, QWidget* field, const std::string& key)
{
    addRow(rowsOf(mPage), QString::fromStdString(label), field, key);
}
} // namespace prefPages

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Preferences");
    resize(840, 520);
    auto* v = new QVBoxLayout(this);
    auto* search = new QLineEdit(this);
    search->setPlaceholderText("Search preferences");
    search->setClearButtonEnabled(true);
    search->addAction(icon("search"), QLineEdit::LeadingPosition);
    search->installEventFilter(new ClearOnEscape(search));
    v->addWidget(search);
    auto* tabs = new QTabWidget(this);
    v->addWidget(tabs, 1);

    auto* resultsView = new QScrollArea(this);
    resultsView->setWidgetResizable(true);
    resultsView->setFrameShape(QFrame::NoFrame);
    resultsView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* results = new QWidget;
    auto* resultsLayout = new QVBoxLayout(results);
    auto* grid = new QGridLayout;
    grid->setColumnStretch(2, 1);
    resultsLayout->addLayout(grid);
    auto* empty = new QLabel("No matching preferences");
    resultsLayout->addWidget(empty);
    resultsLayout->addStretch(1);
    resultsView->setWidget(results);
    resultsView->hide();
    v->addWidget(resultsView, 1);
    auto prefSearch = std::make_shared<PrefSearch>(tabs, resultsView, grid, empty);
    std::vector<PrefRow>& rows = prefSearch->rows;
    connect(search, &QLineEdit::textChanged, resultsView, [prefSearch](const QString& text) { prefSearch->apply(text); });

    for (const prefPages::Page* def : prefPages::registry().allInOrder())
    {
        if (!def->build)
            continue;
        mFiles.insert(def->file);
        Page p =page(tabs, &rows, def->title.c_str(), def->file.c_str());
        prefPages::PageBuilder builder(&p);
        try
        {
            def->build(builder);
        }
        catch (const std::exception& e)
        {
            log::error("preferences page {}: {}", def->id, e.what());
        }
    }

    auto* bottom = new QHBoxLayout;
    auto* folder = new QPushButton("Open Prefs Folder", this);
    connect(folder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(PrefStore::get().userDir().wstring())));
    });
    bottom->addWidget(new QLabel("Changes apply immediately.", this));
    auto* reset = new QToolButton(this);
    reset->setIcon(icon("refresh-alert"));
    reset->setAutoRaise(true);
    reset->setToolTip("Reset all preferences to their defaults, keeping a backup of the current ones");
    connect(reset, &QToolButton::clicked, this, [this, reset] { (new ResetConfirm(reset, [this] { resetAll(); }))->show(); });
    bottom->addWidget(reset);
    auto* restore = new QToolButton(this);
    restore->setIcon(icon("restore"));
    restore->setAutoRaise(true);
    restore->setEnabled(!readBackup().empty());
    restore->setToolTip(restore->isEnabled() ? "Restore the preferences as they were before the last reset"
                                             : "No preferences backed up yet: resetting makes a backup");
    connect(restore, &QToolButton::clicked, this, [this] { restoreBackup(); });
    bottom->addWidget(restore);
    bottom->addStretch(1);
    bottom->addWidget(folder);
    auto* close = new QPushButton("Close", this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(close);
    v->addLayout(bottom);
}

void PreferencesDialog::resetAll()
{
    PrefStore& prefs = PrefStore::get();
    json backup = json::object();
    for (const std::string& file : mFiles)
        backup[file] = prefs.userDoc(file);
    std::error_code ec;
    std::filesystem::create_directories(backupPath().parent_path(), ec);
    std::ofstream out(backupPath(), std::ios::binary | std::ios::trunc);
    out << backup.dump(2) << "\n";
    out.close();
    if (!out)
    {
        log::error("preferences: couldn't write the backup {}, nothing was reset", paths::toUtf8(backupPath()));
        return;
    }
    for (const std::string& file : mFiles)
    {
        json kept = json::object();
        for (const auto& [keptFile, key] : kKeptOnReset)
            if (const json::json_pointer ptr(key); file == keptFile && backup[file].contains(ptr))
                kept[ptr] = backup[file][ptr];
        prefs.setUserDoc(file, std::move(kept));
    }
    log::info("preferences reset, the previous ones are in {}", paths::toUtf8(backupPath()));
    rebuild();
}

void PreferencesDialog::restoreBackup()
{
    json backup = readBackup();
    for (auto& [file, doc] : backup.items())
        PrefStore::get().setUserDoc(file, doc);
    log::info("preferences restored from {}", paths::toUtf8(backupPath()));
    rebuild();
}

void PreferencesDialog::rebuild()
{
    QWidget* parent = parentWidget();
    QRect geo = geometry();
    auto* tabs = findChild<QTabWidget*>();
    int tab = tabs ? tabs->currentIndex() : 0;
    gPreferencesDialog = nullptr;
    close();
    open(parent);
    gPreferencesDialog->setGeometry(geo);
    if (auto* t = gPreferencesDialog->findChild<QTabWidget*>())
        t->setCurrentIndex(tab);
}

void PreferencesDialog::open(QWidget* parent)
{
    if (!gPreferencesDialog)
    {
        gPreferencesDialog = new PreferencesDialog(parent);
        gPreferencesDialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    gPreferencesDialog->show();
    gPreferencesDialog->raise();
    gPreferencesDialog->activateWindow();
    flashLocate(gPreferencesDialog);
}
} // namespace lila::ui
