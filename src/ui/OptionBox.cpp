#include "ui/OptionBox.h"
#include "ui/Theme.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/OptionPresets.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Workspace.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/LocateFlash.h"
#include "ui/DocsUi.h"
#include "ui/FileDialog.h"
#include "ui/FlagFields.h"
#include "ui/Icons.h"
#include "ui/Shelf.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <map>

namespace lila::ui::optionBox
{
namespace opt = ops::options;

namespace
{
constexpr const char* kBoxProperty = "lilaOptionBox";
constexpr const char* kDecorated = "lilaOptionBoxMenu";

/** Commands whose flags are switches of a mode or a selection, not settings worth a dialog. */
bool settingless(const std::string& command)
{
    static const QSet<QString> skip = {"undo", "redo", "select", "selectMode", "newScene", "setSymmetry", "smoothPreview", "show", "hide",
                                       "toggleVisibility", "convertSelection", "pickWalk", "symmetrize", "symmetryRebuild"};
    return skip.contains(QString::fromStdString(command));
}

/** Trailing space in the shortcut column that the square is painted over. */
QString padding()
{
    return QString(3, QChar(0x2003));
}

QWidget* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->inherits("QMainWindow") && w->isVisible())
            return w;
    return nullptr;
}

/** Secondary text: the doc line, notes and the Preset label. */
void optionDim(QWidget* w)
{
    QPalette pal = w->palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
        pal.setColor(group, QPalette::WindowText, themeColor("dimText"));
    w->setPalette(pal);
}

/** A text header over a group of fields, with the section rule above it. */
QWidget* optionSectionHeader(const QString& title, QWidget* parent)
{
    auto* w = new QWidget(parent);
    w->setAttribute(Qt::WA_StyledBackground);
    controls::setRole(w, "section");
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 8, 0, 2);
    auto* l = new QLabel(title, w);
    QFont f = l->font();
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    h->addWidget(l);
    return w;
}

class OptionsDialog : public QDialog
{
public:
    OptionsDialog(const Action& a, QWidget* parent)
        : QDialog(parent), mAction(a.id), mCommand(a.command), mCall(a.call.is_object() ? a.call : json::object())
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QString::fromStdString(title(a)) + " Options");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 8, 0, 10);
        root->setSpacing(6);

        // The doc line, then help and a ... menu with the settings, presets and Copy Python.
        const opt::Override* over = opt::findOverride(mCommand);
        std::string docsPage = over ? over->docs : std::string();
        auto* header = new QHBoxLayout();
        header->setContentsMargins(12, 0, 8, 0);
        header->setSpacing(4);
        auto* doc = new QLabel(QString::fromStdString(a.doc), this);
        doc->setWordWrap(true);
        doc->setVisible(!a.doc.empty());
        optionDim(doc);
        header->addWidget(doc, 1, Qt::AlignTop);
        if (a.doc.empty())
            header->addStretch(1);
        if (!docsPage.empty())
            header->addWidget(docs::helpButton(docsPage, this), 0, Qt::AlignTop);
        auto* more = new QToolButton(this);
        more->setObjectName("optionMore");
        more->setIcon(icon("dots"));
        more->setAutoRaise(true);
        more->setPopupMode(QToolButton::InstantPopup);
        more->setToolTip("Save or reset these settings, presets, Copy Python");
        controls::setRole(more, "overflow");
        auto* menu = new QMenu(more);
        menu->addAction("Save Settings", this, [this] { saveSettings(); })
            ->setToolTip("Remembers these values for the menu item, hotkeys and shelf buttons without running the command.");
        menu->addAction("Reset Settings", this, [this] { resetSettings(); });
        menu->addSeparator();
        menu->addAction("Save Preset...", this, [this] { savePreset("user"); })
            ->setToolTip("Keeps these values under a name in the Preset menu, for you in every project.");
        QAction* saveProject = menu->addAction("Save Project Preset...", this, [this] { savePreset("project"); });
        saveProject->setToolTip("Keeps these values in the project's .lila folder, so everyone working in it gets them.");
        QAction* remove = menu->addAction("Delete Preset", this, [this] { deletePreset(); });
        menu->addAction("Import Preset...", this, [this] { importPreset(); });
        menu->addAction("Show Presets Folder", this, [this] {
            const std::filesystem::path dir = opt::presets::dir(mCommand, "user");
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(paths::toUtf8(dir))));
        });
        menu->addSeparator();
        menu->addAction("Copy Python", this, [this] {
            QGuiApplication::clipboard()->setText(QString::fromStdString(ops::toPython(mCommand, pendingCall())));
        });
        connect(menu, &QMenu::aboutToShow, this, [this, saveProject, remove] {
            saveProject->setEnabled(Workspace::get().isOpen());
            remove->setEnabled(chosenPreset() && chosenPreset()->writable);
        });
        menu->setToolTipsVisible(true);
        more->setMenu(menu);
        header->addWidget(more, 0, Qt::AlignTop);
        root->addLayout(header);

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* body = new QWidget(scroll);
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(12, 4, 12, 4);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        scroll->setWidget(body);
        root->addWidget(scroll, 1);

        mValues = opt::values(mCommand, mCall);
        const json& values = mValues;
        std::vector<opt::Field> fields = opt::fields(mCommand, mCall);
        // Sections keep the order they first appear in; a field without one joins the untitled group at the top.
        std::vector<std::string> sections = {""};
        for (const opt::Field& f : fields)
            if (std::find(sections.begin(), sections.end(), f.section) == sections.end())
                sections.push_back(f.section);
        // Presets head the box: picking one is the first decision, the fields refine it.
        mPresetLabel = new QLabel("Preset", body);
        optionDim(mPresetLabel);
        mPresetHost = new QWidget(body);
        auto* presetRow = new QHBoxLayout(mPresetHost);
        presetRow->setContentsMargins(0, 0, 0, 6);
        presetRow->setSpacing(6);
        mPresetLabel->setContentsMargins(0, 3, 0, 0);
        grid->addWidget(mPresetLabel, 0, 0, Qt::AlignRight | Qt::AlignTop);
        grid->addWidget(mPresetHost, 0, 1);
        int row = 1;
        for (const std::string& section : sections)
        {
            bool any = std::any_of(fields.begin(), fields.end(), [&](const opt::Field& f) { return f.section == section; });
            if (!any)
                continue;
            if (!section.empty())
                grid->addWidget(optionSectionHeader(QString::fromStdString(section), body), row++, 0, 1, 2);
            for (const opt::Field& f : fields)
            {
                if (f.section != section)
                    continue;
                std::string flag = f.flag;
                FlagFieldWidgets w = makeFlagField(f, values.value(flag, f.defaultValue), [this, flag](const json& v) {
                    mValues[flag] = v;
                    if (!mFilling && mPresetMenu)
                        mPresetMenu->setCurrentIndex(-1);
                }, body);
                mSetters[flag] = w.set;
                if (w.label)
                {
                    grid->addWidget(w.label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
                    grid->addWidget(w.field, row, 1);
                }
                else
                    grid->addWidget(w.field, row, 1);
                ++row;
                if (QLabel* note = noteFor(flag, body))
                    grid->addWidget(note, row++, 1);
            }
        }
        grid->setRowStretch(row, 1);
        rebuildPresets();

        // Apply is the primary action and Enter's default; Esc or the window's close button closes.
        auto* buttons = new QHBoxLayout();
        buttons->setContentsMargins(12, 4, 12, 0);
        buttons->setSpacing(6);
        buttons->addStretch(1);
        auto* apply = new QPushButton("Apply", this);
        auto* applyClose = new QPushButton("Apply and Close", this);
        apply->setDefault(true);
        applyClose->setAutoDefault(false);
        buttons->addWidget(apply);
        buttons->addWidget(applyClose);
        root->addLayout(buttons);
        connect(applyClose, &QPushButton::clicked, this, [this] {
            run();
            close();
        });
        connect(apply, &QPushButton::clicked, this, [this] { run(); });

        body->adjustSize();
        resize(std::max(420, body->sizeHint().width() + 40), std::min(720, body->sizeHint().height() + 140));
    }

private:
    static std::string title(const Action& a)
    {
        std::string t = a.label;
        if (t.size() > 3 && t.ends_with("..."))
            t.resize(t.size() - 3);
        return t;
    }

    void run()
    {
        saveSettings();
        // An action that gathers its own arguments (the selection, a pick order) runs as itself, with these settings.
        if (const Action* a = actions::find(mAction); a && a->run)
            actions::run(json(mAction));
        else
            actions::runWithOptions(mCommand, mCall);
    }

    QLabel* noteFor(const std::string& flag, QWidget* parent) const
    {
        const opt::Override* o = opt::findOverride(mCommand);
        if (!o)
            return nullptr;
        auto it = o->notes.find(flag);
        if (it == o->notes.end() || !it->second)
            return nullptr;
        std::string text;
        try
        {
            text = it->second();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
        if (text.empty())
            return nullptr;
        auto* label = new QLabel(QString::fromStdString(text), parent);
        label->setWordWrap(true);
        optionDim(label);
        return label;
    }

    /** Stores the fields' values where Apply does, so the menu item, hotkeys and shelf run with them. */
    void saveSettings()
    {
        // A line edit commits on focus loss, which a button click or menu item may not have caused yet.
        if (QWidget* f = focusWidget())
            f->clearFocus();
        for (auto& [flag, v] : mValues.items())
            opt::setValue(mCommand, flag, v);
    }

    /** Sets the fields a fill button gives values for; the rest keep theirs. */
    void fillFrom(const std::function<json()>& values)
    {
        json given;
        try
        {
            given = values();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
            return;
        }
        if (!given.is_object())
            return;
        mFilling = true;
        for (auto& [flag, v] : given.items())
        {
            auto it = mSetters.find(flag);
            if (it == mSetters.end())
                continue;
            mValues[flag] = v;
            if (it->second)
                it->second(v);
        }
        mFilling = false;
    }

    void resetSettings()
    {
        opt::reset(mCommand);
        mValues = opt::values(mCommand, mCall);
        mFilling = true;
        for (auto& [flag, set] : mSetters)
            if (set && mValues.contains(flag))
                set(mValues[flag]);
        mFilling = false;
        if (mPresetMenu)
            mPresetMenu->setCurrentIndex(-1);
    }

    /**
     * The Preset row: a few fills and no saved presets stay buttons; otherwise one menu of the command's own presets,
     * then the studio's, the user's and the project's. The menu keeps showing the pick until a field changes.
     */
    void rebuildPresets()
    {
        const QString keep = mPresetMenu && mPresetMenu->currentIndex() >= 0 ? mPresetMenu->currentText() : QString();
        while (QLayoutItem* item = mPresetHost->layout()->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        mPresetMenu = nullptr;
        const opt::Override* o = opt::findOverride(mCommand);
        const std::vector<opt::Override::Fill> fills = o ? o->fills : std::vector<opt::Override::Fill>();
        mSaved = opt::presets::list(mCommand);
        auto* row = static_cast<QHBoxLayout*>(mPresetHost->layout());
        mPresetLabel->setVisible(!mSaved.empty() || fills.size() > 3);
        mPresetHost->setVisible(!fills.empty() || !mSaved.empty());
        if (mSaved.empty() && fills.size() <= 3)
        {
            for (const opt::Override::Fill& fill : fills)
            {
                auto* b = new QPushButton(QString::fromStdString(fill.label), mPresetHost);
                b->setToolTip(QString::fromStdString(fill.tip));
                connect(b, &QPushButton::clicked, this, [this, values = fill.values] { fillFrom(values); });
                row->addWidget(b);
            }
            row->addStretch(1);
            return;
        }
        auto* menu = new QComboBox(mPresetHost);
        menu->setObjectName("optionPresets");
        menu->setPlaceholderText("Custom");
        for (size_t i = 0; i < fills.size(); ++i)
        {
            menu->addItem(QString::fromStdString(fills[i].label), int(i));
            menu->setItemData(menu->count() - 1, QString::fromStdString(fills[i].tip), Qt::ToolTipRole);
        }
        static const std::map<std::string, QString> from = {{"studio", "Studio preset"}, {"user", "Your preset"}, {"project", "Project preset"}};
        for (size_t i = 0; i < mSaved.size(); ++i)
        {
            if (i == 0 && !fills.empty())
                menu->insertSeparator(menu->count());
            menu->addItem(QString::fromStdString(mSaved[i].name), -1 - int(i));
            menu->setItemData(menu->count() - 1, from.at(mSaved[i].source), Qt::ToolTipRole);
        }
        menu->setCurrentIndex(keep.isEmpty() ? -1 : menu->findText(keep));
        connect(menu, &QComboBox::activated, this, [this, menu, fills](int index) {
            const int at = menu->itemData(index).toInt();
            if (at >= 0)
                fillFrom(fills[size_t(at)].values);
            else
                fillFrom([values = mSaved[size_t(-1 - at)].values] { return values; });
        });
        row->addWidget(menu, 1);
        mPresetMenu = menu;
    }

    /** The saved preset the menu shows, or null for a built-in one or edited values. */
    const opt::presets::Preset* chosenPreset() const
    {
        if (!mPresetMenu || mPresetMenu->currentIndex() < 0)
            return nullptr;
        const int at = mPresetMenu->currentData().toInt();
        return at < 0 && size_t(-1 - at) < mSaved.size() ? &mSaved[size_t(-1 - at)] : nullptr;
    }

    /** The fields worth sharing: node pickers and lists name things in one scene, so they stay out. */
    json presetValues() const
    {
        json out = json::object();
        for (const opt::Field& f : opt::fields(mCommand, mCall))
            if (f.kind != opt::FieldKind::List && f.nodeType.empty() && mValues.contains(f.flag))
                out[f.flag] = mValues[f.flag];
        return out;
    }

    void savePreset(const std::string& source)
    {
        if (QWidget* f = focusWidget())
            f->clearFocus();
        const opt::presets::Preset* chosen = chosenPreset();
        bool ok = false;
        const QString name = QInputDialog::getText(this, source == "project" ? "Save Project Preset" : "Save Preset", "Name", QLineEdit::Normal,
                                                   chosen ? QString::fromStdString(chosen->name) : QString(), &ok)
                                 .trimmed();
        if (ok && !name.isEmpty())
            savePresetAs(name.toStdString(), source);
    }

    /** Saves the fields as a preset and shows it picked. */
    bool savePresetAs(const std::string& name, const std::string& source)
    {
        std::string error;
        if (!opt::presets::save(mCommand, name, presetValues(), source, &error))
        {
            log::error("Save preset: {}", error);
            return false;
        }
        rebuildPresets();
        mPresetMenu->setCurrentIndex(mPresetMenu->findText(QString::fromStdString(name)));
        return true;
    }

    void deletePreset()
    {
        const opt::presets::Preset* chosen = chosenPreset();
        std::string error;
        if (!chosen || !opt::presets::remove(*chosen, &error))
        {
            if (!error.empty())
                log::error("Delete preset: {}", error);
            return;
        }
        mPresetMenu->setCurrentIndex(-1);
        rebuildPresets();
    }

    void importPreset()
    {
        const QString file = fileDialog::getOpenFileName(this, "Import Preset", {}, "Option presets (*.json)");
        if (file.isEmpty())
            return;
        std::string error;
        std::optional<opt::presets::Preset> p = opt::presets::install(paths::fromUtf8(file.toStdString()), &error);
        if (!p)
        {
            log::error("Import preset: {}", error);
            return;
        }
        rebuildPresets();
        if (mPresetMenu)
            mPresetMenu->setCurrentIndex(mPresetMenu->findText(QString::fromStdString(p->name)));
    }

    /** The call Apply would make with the fields as they stand. */
    json pendingCall() const
    {
        json call = mCall;
        for (const opt::Field& f : opt::fields(mCommand, mCall))
            if (mValues.contains(f.flag) && mValues[f.flag] != f.defaultValue && !call.contains(f.flag))
                call[f.flag] = mValues[f.flag];
        return call;
    }

    std::string mAction;
    std::string mCommand;
    json mCall;
    /** The fields' values, stored only by Apply or Save Settings so Close leaves the remembered ones alone. */
    json mValues;
    std::map<std::string, std::function<void(const json&)>> mSetters;
    QLabel* mPresetLabel = nullptr;
    QWidget* mPresetHost = nullptr;
    QPointer<QComboBox> mPresetMenu;
    std::vector<opt::presets::Preset> mSaved;
    /** Set while a preset or reset writes the fields, so their change doesn't read as an edit. */
    bool mFilling = false;
};

std::map<std::string, QPointer<OptionsDialog>>& dialogs()
{
    static std::map<std::string, QPointer<OptionsDialog>> d;
    return d;
}

Registry<std::function<void()>>& customs()
{
    static Registry<std::function<void()>> c;
    return c;
}

/** Where the square sits on a row: the right end of the shortcut column, clear of the submenu arrow margin. */
QRect boxRect(const QMenu* menu, QAction* a)
{
    QRect r = menu->actionGeometry(a);
    int side = std::min(r.height() - 6, menu->fontMetrics().height());
    int padW = menu->fontMetrics().horizontalAdvance(padding());
    int rightMargin = 15;
    QRect box(0, 0, side, side);
    box.moveCenter(QPoint(r.right() - rightMargin - padW / 2, r.center().y()));
    return box;
}

void closeMenus()
{
    while (QWidget* p = QApplication::activePopupWidget())
        p->close();
}

class MenuFilter : public QObject
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
        case QEvent::Paint:
        {
            obj->event(e);
            QPainter p(menu);
            QPoint mouse = menu->mapFromGlobal(QCursor::pos());
            for (QAction* a : menu->actions())
                if (a->property(kBoxProperty).toBool() && a->isVisible())
                {
                    QRect box = boxRect(menu, a);
                    bool hot = box.adjusted(-4, -4, 4, 4).contains(mouse);
                    icon("app-window").paint(&p, box, Qt::AlignCenter, !a->isEnabled() ? QIcon::Disabled : hot ? QIcon::Active : QIcon::Normal);
                }
            return true;
        }
        case QEvent::MouseMove:
            menu->update();
            return false;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        {
            auto* me = static_cast<QMouseEvent*>(e);
            QAction* a = menu->actionAt(me->position().toPoint());
            // Any button: hotbox menus are opened and released with whichever button pressed the menu name.
            if (!a || !a->property(kBoxProperty).toBool() || !(me->button() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)))
                return false;
            std::string id = optionsAt(menu, a, int(me->globalPosition().x()), me->modifiers() & Qt::ShiftModifier);
            if (id.empty())
                return false;
            if (e->type() == QEvent::MouseButtonRelease)
            {
                closeMenus();
                if (shelves::addModifierHeld())
                    shelves::addToCurrent(json{{"optionBox", id}});
                else
                    QTimer::singleShot(0, [id] { open(id); });
            }
            return true;
        }
        default: return false;
        }
    }
};

MenuFilter* menuFilter()
{
    static QPointer<MenuFilter> f;
    if (!f)
        f = new MenuFilter(qApp);
    return f;
}

void markRows(QMenu* menu)
{
    QString pad = padding();
    for (QAction* a : menu->actions())
    {
        if (QMenu* sub = a->menu())
        {
            decorateMenu(sub);
            continue;
        }
        std::string id = a->data().toString().toStdString();
        bool box = !id.empty() && available(id);
        a->setProperty(kBoxProperty, box);
        if (!box)
            continue;
        QString text = a->text();
        if (!text.endsWith(pad))
            a->setText(text + (text.contains('\t') ? "" : "\t") + pad);
        QString hint = "Click the square at the right, or Shift+click, for options.";
        if (!a->toolTip().contains(hint))
            a->setToolTip(a->toolTip() + "\n" + hint);
    }
}
} // namespace

bool available(const std::string& actionId)
{
    if (customs().contains(actionId))
        return true;
    const Action* a = actions::find(actionId);
    return a && !a->command.empty() && !settingless(a->command) && opt::hasOptions(a->command, a->call);
}

std::string optionsAt(const QMenu* menu, QAction* a, int globalX, bool shift)
{
    if (!menu || !a || !a->property(kBoxProperty).toBool())
        return {};
    bool onBox = menu->mapFromGlobal(QPoint(globalX, 0)).x() >= boxRect(menu, a).left() - 6;
    // Ctrl+Shift on the item itself adds it to the shelf rather than opening its options.
    if (!onBox && (!shift || shelves::addModifierHeld()))
        return {};
    return a->data().toString().toStdString();
}

Registry<std::function<void()>>& customRegistry()
{
    return customs();
}

void addCustom(const std::string& actionId, std::function<void()> open, std::string owner)
{
    customs().put(actionId, std::move(open), std::move(owner));
}

uint64_t addCustom(ext::Context& ctx, const std::string& actionId, std::function<void()> open)
{
    if (const std::string* owner = customs().ownerOf(actionId); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "the option box of '" + actionId + "' already belongs to " + *owner);
        return 0;
    }
    addCustom(actionId, std::move(open), ctx.owner());
    return ctx.track("optionBox:" + actionId, [actionId, own = ctx.owner()] {
        if (const std::string* o = customs().ownerOf(actionId); o && *o == own)
            customs().remove(actionId);
    });
}

bool removeCustom(const std::string& actionId)
{
    return customs().remove(actionId);
}

void open(const std::string& actionId)
{
    if (const std::function<void()>* custom = customs().find(actionId))
    {
        std::function<void()> fn = *custom;
        fn();
        return;
    }
    const Action* a = actions::find(actionId);
    if (!a || a->command.empty())
        return;
    QPointer<OptionsDialog>& d = dialogs()[actionId];
    if (!d)
        d = new OptionsDialog(*a, mainWindow());
    d->show();
    d->raise();
    d->activateWindow();
    flashLocate(d);
}

/** The label of an action making this call (or running this command), else the command's name in words. */
std::string titleFor(const std::string& command, const nlohmann::json& call)
{
    std::string any;
    for (const Action* a : actions::all())
        if (a->command == command)
        {
            if (a->call == call)
                return a->label;
            if (any.empty())
                any = a->label;
        }
    if (!any.empty())
        return any;
    std::string words;
    for (char c : command)
    {
        if (std::isupper(static_cast<unsigned char>(c)) && !words.empty())
            words += ' ';
        words += words.empty() ? char(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    return words;
}

bool shiftOpens(const std::string& command, const nlohmann::json& call, const std::string& title)
{
    if (!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier))
        return false;
    return openCommand(title.empty() ? titleFor(command, call) : title, command, call);
}

bool openCommand(const std::string& title, const std::string& command, const nlohmann::json& call)
{
    const nlohmann::json fixed = call.is_object() ? call : nlohmann::json::object();
    if (settingless(command) || !opt::hasOptions(command, fixed))
        return false;
    const std::string key = "command:" + command + fixed.dump();
    QPointer<OptionsDialog>& d = dialogs()[key];
    if (!d)
    {
        const CommandDef* def = ops::find(command);
        d = new OptionsDialog(Action{.id = key, .label = title, .doc = def ? def->doc : std::string(), .command = command, .call = fixed},
                              mainWindow());
    }
    d->show();
    d->raise();
    d->activateWindow();
    flashLocate(d);
    return true;
}

void decorateMenu(QMenu* menu)
{
    if (!menu || menu->property(kDecorated).toBool())
        return;
    menu->setProperty(kDecorated, true);
    menu->installEventFilter(menuFilter());
    // Connected after the hotkey refresh that rewrites each row's text, so the padding goes on last.
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { markRows(menu); });
}

namespace
{
/**
 * The option box folded into a panel button: a small triangle in its bottom-right corner. Clicking that corner, a
 * right-click or a Shift+click opens the options instead of running the button; the corner brightens under the pointer.
 */
class OptionCorner : public QObject
{
public:
    OptionCorner(QToolButton* button, std::string actionId) : QObject(button), mButton(button), mActionId(std::move(actionId))
    {
        button->setMouseTracking(true);
        button->installEventFilter(this);
        button->setProperty("lilaOptionBox", true);
        const QString hint = "Click corner, right-click or Shift+click for options";
        if (!button->toolTip().contains(hint))
            button->setToolTip(button->toolTip().isEmpty() ? hint : button->toolTip() + "\n" + hint);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != mButton)
            return false;
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            const bool corner = m->button() == Qt::LeftButton && inCorner(m->position().toPoint());
            const bool shift = m->button() == Qt::LeftButton && (m->modifiers() & Qt::ShiftModifier);
            if (corner || shift || m->button() == Qt::RightButton)
            {
                open(mActionId);
                return true;
            }
            return false;
        }
        case QEvent::MouseButtonRelease:
            return static_cast<QMouseEvent*>(e)->button() == Qt::RightButton;
        case QEvent::ContextMenu:
            return true;
        case QEvent::MouseMove:
        {
            const bool hot = inCorner(static_cast<QMouseEvent*>(e)->position().toPoint());
            if (hot != mHot)
            {
                mHot = hot;
                mButton->update();
            }
            return false;
        }
        case QEvent::Leave:
            if (mHot)
            {
                mHot = false;
                mButton->update();
            }
            return false;
        case QEvent::Paint:
        {
            // Drawn after the button paints itself, on top of its fill.
            mButton->removeEventFilter(this);
            QCoreApplication::sendEvent(mButton, e);
            mButton->installEventFilter(this);
            QPainter p(mButton);
            p.setRenderHint(QPainter::Antialiasing);
            const QRect r = mButton->rect();
            const int size = mHot ? 7 : 5;
            QPolygon tri;
            tri << QPoint(r.right() - 2, r.bottom() - 2 - size) << QPoint(r.right() - 2, r.bottom() - 2) << QPoint(r.right() - 2 - size, r.bottom() - 2);
            p.setPen(Qt::NoPen);
            p.setBrush(mHot ? themeColor("brightText") : themeColor("dimText"));
            p.drawPolygon(tri);
            return true;
        }
        default:
            return false;
        }
    }

private:
    bool inCorner(QPoint pos) const
    {
        const QRect r = mButton->rect();
        return pos.x() >= r.right() - kZone && pos.y() >= r.bottom() - kZone;
    }

    static constexpr int kZone = 12;
    QToolButton* mButton;
    std::string mActionId;
    bool mHot = false;
};
} // namespace

QWidget* withOptionBox(QToolButton* button, const std::string& actionId, QWidget* parent)
{
    (void)parent;
    if (!button || !available(actionId) || button->property("lilaOptionBox").toBool())
        return button;
    new OptionCorner(button, actionId);
    return button;
}

void install()
{
    for (auto [id, command] : {std::pair{"rig.orientJoint", "orientJoint"}, {"rig.mirrorJoints", "mirrorJoints"}, {"rig.insertJoint", "insertJoint"},
                               {"rig.bindSkin", "skinCluster"}, {"rig.mirrorSkinWeights", "mirrorSkinWeights"},
                               {"rig.copySkinWeights", "copySkinWeights"}, {"rig.pruneWeights", "pruneWeights"},
                               {"rig.parentConstraint", "parentConstraint"}, {"rig.pointConstraint", "pointConstraint"},
                               {"rig.orientConstraint", "orientConstraint"}, {"rig.aimConstraint", "aimConstraint"}, {"rig.pin", "pin"},
                               {"rig.bakePin", "bakePin"}, {"rig.splineChain", "splineChain"}, {"rig.hoseLimb", "hoseLimb"},
                               {"rig.hoseFingers", "hoseFingers"}, {"rig.softNet", "softNet"}, {"rig.softNetLimbStations", "softNetLimbStations"},
                               {"rig.mirrorSoftNetKnots", "mirrorSoftNetKnots"}, {"rig.softNetUnreal", "softNetUnreal"}})
        actions::bindCommand(id, command);
    actions::bindCommand("rig.bindSkinHeatmap", "skinCluster", {{"method", "heatmap"}});
}
} // namespace lila::ui::optionBox
