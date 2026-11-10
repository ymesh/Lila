#include "ui/CommandPalette.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "python/Python.h"
#include "ui/Actions.h"
#include "ui/CommandHistory.h"
#include "ui/Hotkeys.h"
#include "ui/OutlinerFilter.h"
#include "ui/Theme.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportWidget.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/ScriptEditor.h"

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace lila::ui::commandPalette
{
namespace
{
constexpr const char* kPrefs = "commandPalette";
constexpr int kHistoryCap = 200;
constexpr int kMaxResults = 8;
constexpr int kDetailRole = Qt::UserRole + 1;

CommandHistory& history()
{
    static CommandHistory h = [] {
        CommandHistory loaded(kHistoryCap);
        auto saved = PrefStore::get().value<std::vector<std::string>>(kPrefs, "/history", {});
        if (saved.empty())
            saved = PrefStore::get().value<std::vector<std::string>>("commandLine", "/history", {});
        QStringList entries;
        for (const std::string& s : saved)
            entries.append(QString::fromStdString(s));
        loaded.setEntries(entries);
        return loaded;
    }();
    return h;
}

void saveHistory()
{
    std::vector<std::string> out;
    for (const QString& s : history().entries())
        out.push_back(s.toStdString());
    PrefStore::get().set(kPrefs, "/history", out);
}

void runLine(const QString& code)
{
    if (!python::initialized())
    {
        log::error("Python is not available");
        return;
    }
    history().add(code);
    saveHistory();
    actions::ViaScope via(actions::Via::Palette);
    actions::RunReport report(nlohmann::json{{"python", code.toStdString()}});
    report.ok = true;
    log::write(LogLevel::Echo, code.toStdString() + "\n");
    // A bare command name runs it rather than printing the function.
    static const QRegularExpression bareName(R"(^\s*([A-Za-z_][\w.]*)\s*$)");
    if (QRegularExpressionMatch m = bareName.match(code); m.hasMatch())
    {
        std::string name = m.captured(1).toStdString();
        python::run(name + "() if callable(" + name + ") else " + name, "<command>");
        return;
    }
    python::run(code.toStdString(), "<command>");
}

struct PaletteResult
{
    std::string action;  // an action id, or empty for a command
    std::string command; // a command name
    QString title;
    QString detail;
    int score = 0;
    bool bound = false; // has a hotkey
    QString insert;     // an argument row: what replaces the argument being typed
};

/** A doc string's first sentence. */
QString firstSentence(const std::string& doc)
{
    QString s = QString::fromStdString(doc);
    if (int end = s.indexOf(". "); end > 0)
        s.truncate(end + 1);
    return s;
}

/** Calls, assignments, literals and statements are Python, not a search. */
bool looksLikePython(const QString& text)
{
    static const QRegularExpression code(R"([()=\[\]'"]|^\s*lila\.|^\s*(import|from|for|if|print)\b)");
    return code.match(text).hasMatch();
}

/**
 * How well the query names the label: the whole label, then its start, then the start of one of its words (camelCase
 * humps count), then a scattered fuzzy match. Short queries need a word start, so one letter doesn't list everything.
 */
int nameScore(const QString& q, const QString& label)
{
    const int fuzzy = outliner::fuzzyScore(q.toStdString(), label.toStdString());
    if (fuzzy <= 0)
        return 0;
    if (label.compare(q, Qt::CaseInsensitive) == 0)
        return 400 + fuzzy;
    if (label.startsWith(q, Qt::CaseInsensitive))
        return 300 + fuzzy;
    for (int i = 1; i < label.size(); ++i)
    {
        const bool wordStart = !label[i - 1].isLetterOrNumber() || (label[i].isUpper() && label[i - 1].isLower());
        if (wordStart && QStringView(label).mid(i).startsWith(q, Qt::CaseInsensitive))
            return 200 + fuzzy;
    }
    return q.size() <= 2 ? 0 : fuzzy;
}

std::vector<PaletteResult> searchPalette(const QString& text)
{
    const QString q = text.trimmed();
    std::vector<PaletteResult> out;
    if (q.isEmpty())
        return out;
    for (const Action* a : actions::all())
    {
        if (!a->run || a->label.empty() || (a->enabled && !a->enabled()))
            continue;
        const QString label = QString::fromStdString(actions::displayLabel(a->id));
        const int score = nameScore(q, label);
        if (score <= 0)
            continue;
        QString detail = QString::fromStdString(a->category);
        const QString key = Hotkeys::get().keyFor(a->id);
        if (!key.isEmpty())
            detail += "  " + key;
        out.push_back({a->id, {}, label, detail, score, !key.isEmpty()});
    }
    // Actions lead on a partial name (mo: Move Tool); the whole name of a command (move) puts that command first.
    for (const CommandDef* c : ops::all())
    {
        const QString name = QString::fromStdString(c->name);
        int score = name == q ? 1000 : nameScore(q, name) - 100;
        if (score > 0)
            out.push_back({{}, c->name, name + "()", firstSentence(c->doc), score});
    }
    // Equal scores: a bound action (the everyday ones), then the shorter name.
    std::stable_sort(out.begin(), out.end(), [](const PaletteResult& a, const PaletteResult& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.bound != b.bound)
            return a.bound;
        return a.title.size() < b.title.size();
    });
    if (out.size() > size_t(kMaxResults))
        out.resize(kMaxResults);
    return out;
}

/** A result row: its name, and its category, hotkey or doc dim at the right. */
/** The caret inside a call of a registered command: in `move(by=(1, 0, 0), x` it is typing move's argument `x`. */
struct CallContext
{
    const CommandDef* command = nullptr;
    int argStart = 0; // where the argument being typed begins
    QString arg;      // its text up to the caret
};

std::optional<CallContext> callAt(const QString& before)
{
    static const QRegularExpression head(R"(^\s*(?:lila\.ops\.)?([A-Za-z_]\w*)\()");
    const QRegularExpressionMatch m = head.match(before);
    if (!m.hasMatch())
        return std::nullopt;
    const CommandDef* c = ops::find(m.captured(1).toStdString());
    if (!c)
        return std::nullopt;
    int depth = 0;
    int argStart = int(m.capturedEnd());
    QChar quote;
    for (int i = argStart; i < before.size(); ++i)
    {
        const QChar ch = before[i];
        if (!quote.isNull())
        {
            if (ch == quote && before[i - 1] != '\\')
                quote = QChar();
        }
        else if (ch == '\'' || ch == '"')
            quote = ch;
        else if (ch == '(' || ch == '[' || ch == '{')
            ++depth;
        else if ((ch == ')' || ch == ']' || ch == '}') && --depth < 0)
            return std::nullopt;
        else if (ch == ',' && depth == 0)
            argStart = i + 1;
    }
    return CallContext{c, argStart, before.mid(argStart)};
}

/** The flag the argument names: `x=0` and a bare `x` are both x. */
const FlagDef* activeFlag(const CallContext& call)
{
    const QString name = call.arg.section('=', 0, 0).trimmed();
    for (const FlagDef& f : call.command->flags)
        if (name == QString::fromStdString(f.name) || (!f.shortName.empty() && name == QString::fromStdString(f.shortName)))
            return &f;
    return nullptr;
}

/** The call's signature with the current argument underlined, and that argument's doc (the command's without one). */
QString callHint(const CallContext& call)
{
    const FlagDef* active = activeFlag(call);
    const QString dim = themeColor("dimText").name();
    auto dimmed = [&dim](const QString& text) { return QString("<span style=\"color:%1\">%2</span>").arg(dim, text.toHtmlEscaped()); };
    QStringList args;
    if (!call.command->positionalDoc.empty())
        args << dimmed(QString::fromStdString(call.command->positionalDoc));
    for (const FlagDef& f : call.command->flags)
    {
        const QString name = QString::fromStdString(f.name);
        args << (&f == active ? QString("<b><u>%1</u></b>").arg(name.toHtmlEscaped()) : dimmed(name));
    }
    const QString doc = active ? QString::fromStdString(active->doc) : firstSentence(call.command->doc);
    return QString("<b>%1</b>(%2)<br>%3").arg(QString::fromStdString(call.command->name).toHtmlEscaped(), args.join(", "), dimmed(doc));
}

/**
 * The flags whose names start with the argument being typed, in the command's order, leaving out ones the call already
 * gives; none once the argument has a value.
 */
std::vector<PaletteResult> flagRows(const CallContext& call, const QString& before)
{
    std::vector<PaletteResult> out;
    const QString typed = call.arg.trimmed();
    if (typed.contains('=') || (!typed.isEmpty() && !(typed[0].isLetter() || typed[0] == '_')))
        return out;
    static const QRegularExpression given(R"(([A-Za-z_]\w*)\s*=(?!=))");
    QStringList used;
    for (auto it = given.globalMatch(before.left(call.argStart)); it.hasNext();)
        used << it.next().captured(1);
    for (const FlagDef& f : call.command->flags)
    {
        const QString name = QString::fromStdString(f.name);
        if (used.contains(name) || (!f.shortName.empty() && used.contains(QString::fromStdString(f.shortName))))
            continue;
        if (!name.startsWith(typed, Qt::CaseInsensitive) && QString::fromStdString(f.shortName) != typed)
            continue;
        out.push_back({{}, {}, name + "=", firstSentence(f.doc), 0, false, name + "="});
        if (out.size() == size_t(kMaxResults))
            break;
    }
    return out;
}

class ResultDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString title = opt.text;
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);
        const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w).adjusted(4, 0, -6, 0);
        p->save();
        p->setFont(opt.font);
        const QFontMetrics fm = p->fontMetrics();
        const QString name = fm.elidedText(title, Qt::ElideRight, r.width());
        const int nameW = fm.horizontalAdvance(name);
        p->setPen(themeColor("brightText"));
        p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, name);
        const int gap = fm.horizontalAdvance("    ");
        const QString detail = fm.elidedText(index.data(kDetailRole).toString(), Qt::ElideRight, std::max(0, r.width() - nameW - gap));
        p->setPen(themeColor("dimText"));
        p->drawText(r, Qt::AlignVCenter | Qt::AlignRight, detail);
        p->restore();
    }
};

class PaletteBox : public QFrame
{
public:
    explicit PaletteBox(QWidget* over) : QFrame(over, Qt::Tool | Qt::FramelessWindowHint), mOver(over)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);
        mResults = new QListWidget(this);
        mResults->setFocusPolicy(Qt::NoFocus);
        mResults->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mResults->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mResults->setUniformItemSizes(true);
        mResults->setItemDelegate(new ResultDelegate(mResults));
        mResults->hide();
        connect(mResults, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
            mResults->setCurrentItem(it);
            runCurrent();
        });
        lay->addWidget(mResults);
        mHint = new QLabel(this);
        mHint->setTextFormat(Qt::RichText);
        mHint->setWordWrap(true);
        mHint->setContentsMargins(4, 2, 4, 2);
        mHint->hide();
        lay->addWidget(mHint);
        mEdit = new CodeEditor(this);
        mEdit->setGutterVisible(false);
        mEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        mEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mEdit->setUntitledName("Command");
        mEdit->autoPopups = false;
        mEdit->setPlaceholderText("Search actions and commands, or type Python");
        QFont font = monoFont();
        // No language server or linter: the palette's own results, call hint and argument list are its only popups.
        if (auto* editor = over->window()->findChild<ScriptEditor*>())
            font = editor->editorFont();
        mEdit->setEditorFont(font);
        mEdit->hostKeyHandler = [this](QKeyEvent* k) { return hostKey(k); };
        lay->addWidget(mEdit);
        setFocusProxy(mEdit);

        history().resetWalk();
        mEdit->setPlainText(history().last());
        mEdit->selectAll();
        mFromHistory = true;
        connect(mEdit, &QPlainTextEdit::textChanged, this, [this] {
            if (!mWalking)
                mFromHistory = false;
            updateResults();
        });
        connect(mEdit, &QPlainTextEdit::cursorPositionChanged, this, [this] { updateResults(); });

        mLine = QFontMetrics(font).lineSpacing() + 2 * int(mEdit->document()->documentMargin()) + 2 * mEdit->frameWidth();
        mEdit->setFixedHeight(mLine);
        resize(width(), mLine + 8);
        mLayout = over->window()->findChild<ViewportLayout*>();
        // Follows the viewport as the window moves or the layout, docks or quad view change its size.
        over->window()->installEventFilter(this);
        if (mLayout)
        {
            mLayout->installEventFilter(this);
            for (ViewportWidget* v : mLayout->views())
                v->installEventFilter(this);
        }
        place();

        // Closes like a popup when focus goes elsewhere; the completion and doc popups never take focus.
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (now && now != this && !isAncestorOf(now))
                close();
        });
        connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive)
                close();
        });
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            // After the event, once every widget in the layout has its new geometry.
            QTimer::singleShot(0, this, [this] { place(); });
            break;
        default: break;
        }
        return QFrame::eventFilter(obj, e);
    }

private:
    /** Centred just above the bottom edge of the active viewport (above its bottom bar), or of the window without one. */
    void place()
    {
        QWidget* anchor = mLayout && mLayout->active() && mLayout->active()->isVisible() ? mLayout->active() : mOver.data();
        if (!anchor)
            return;
        const int margin = anchor == mOver ? 48 : 8;
        const int w = std::max(160, std::min(640, anchor->width() - 40));
        const int rows = mResults->isVisibleTo(this) ? mResults->count() : 0;
        if (rows)
            mResults->setFixedHeight(rows * mResults->sizeHintForRow(0) + 2 * mResults->frameWidth());
        const int hint = mHint->isVisibleTo(this) ? mHint->heightForWidth(w - 8) : 0;
        if (hint)
            mHint->setFixedHeight(hint);
        const int h = mLine + 8 + (rows ? mResults->height() + 4 : 0) + (hint ? hint + 4 : 0);
        // One call, so the window never shows at its new size in the old place or the reverse.
        setGeometry(QRect(anchor->mapToGlobal(QPoint((anchor->width() - w) / 2, anchor->height() - h - margin)), QSize(w, h)));
    }

    std::optional<CallContext> currentCall() const
    {
        return mFromHistory ? std::nullopt : callAt(mEdit->toPlainText().left(mEdit->textCursor().position()));
    }

    /**
     * Matches for the typed text, the best right above the field; inside a command's call, its signature and the flags
     * that fit the argument being typed. None for a history recall or other Python.
     */
    void updateResults()
    {
        const QString text = mEdit->toPlainText();
        const std::optional<CallContext> call = currentCall();
        if (call)
        {
            mFound = flagRows(*call, text.left(mEdit->textCursor().position()));
            mHint->setText(callHint(*call));
        }
        else
            mFound = mFromHistory || looksLikePython(text) ? std::vector<PaletteResult>{} : searchPalette(text);
        mHint->setVisible(call.has_value());
        mResults->clear();
        for (auto it = mFound.rbegin(); it != mFound.rend(); ++it)
        {
            auto* item = new QListWidgetItem(it->title, mResults);
            item->setData(kDetailRole, it->detail);
            item->setToolTip(it->detail);
        }
        mResults->setVisible(!mFound.empty());
        // Flags offered right after `(` or `, ` wait for Up or Down, so Enter there still runs the call.
        const bool pick = !mFound.empty() && !(call && call->arg.trimmed().isEmpty());
        mResults->setCurrentRow(pick ? mResults->count() - 1 : -1);
        place();
    }

    void runCurrent()
    {
        const int row = mResults->currentRow();
        if (row < 0 || row >= int(mFound.size()))
            return;
        const PaletteResult r = mFound[mFound.size() - 1 - size_t(row)];
        if (!r.insert.isEmpty())
        {
            insertArgument(r.insert);
            return;
        }
        close();
        actions::ViaScope via(actions::Via::Palette);
        if (!r.action.empty())
            actions::run(nlohmann::json(r.action));
        else
            runLine(QString::fromStdString(r.command));
    }

    /** Replaces the text with a call of the command, the caret inside its parentheses. */
    void startCall(const std::string& command)
    {
        QTextCursor c = mEdit->textCursor();
        c.select(QTextCursor::Document);
        c.insertText(QString::fromStdString(command) + "()");
        c.setPosition(int(command.size()) + 1);
        mEdit->setTextCursor(c);
    }

    /** Replaces the argument being typed with `text`, keeping one space after the comma before it. */
    void insertArgument(const QString& text)
    {
        const std::optional<CallContext> call = currentCall();
        if (!call)
            return;
        QTextCursor c = mEdit->textCursor();
        const bool afterComma = call->argStart > 0 && mEdit->toPlainText()[call->argStart - 1] == ',';
        c.setPosition(call->argStart);
        c.setPosition(call->argStart + int(call->arg.size()), QTextCursor::KeepAnchor);
        c.insertText((afterComma ? " " : "") + text);
        mEdit->setTextCursor(c);
    }

    bool hostKey(QKeyEvent* k)
    {
        Qt::KeyboardModifiers mods = k->modifiers() & ~Qt::KeypadModifier;
        if (Hotkeys::get().isKeyFor(k, "window.commandPalette"))
        {
            close();
            return true;
        }
        switch (k->key())
        {
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (mods != Qt::NoModifier)
                return false;
            if (mResults->isVisibleTo(this))
            {
                const int n = mResults->count();
                const int at = mResults->currentRow();
                const bool up = k->key() == Qt::Key_Up;
                mResults->setCurrentRow(at < 0 ? (up ? n - 1 : 0) : (at + (up ? -1 : 1) + n) % n);
                return true;
            }
            mWalking = true;
            mEdit->setPlainText(k->key() == Qt::Key_Up ? history().older() : history().newer());
            mWalking = false;
            mFromHistory = true;
            updateResults();
            mEdit->moveCursor(QTextCursor::End);
            return true;
        case Qt::Key_Space:
            // The editor's own completion (Ctrl+Space) would open a second list.
            return mods == Qt::ControlModifier;
        case Qt::Key_Tab:
            // Completes the highlighted row: an argument's name, or a command's call to fill in. Never indents.
            if (mods != Qt::NoModifier)
                return false;
            if (const int row = mResults->isVisibleTo(this) ? mResults->currentRow() : -1; row >= 0)
            {
                // A copy: inserting rebuilds mFound.
                const PaletteResult r = mFound[mFound.size() - 1 - size_t(row)];
                if (!r.insert.isEmpty())
                    insertArgument(r.insert);
                else if (!r.command.empty())
                    startCall(r.command);
            }
            return true;
        case Qt::Key_Escape: close(); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        {
            // Shift+Enter runs the text as Python even when it matches something.
            if (mods != Qt::NoModifier && mods != Qt::ShiftModifier)
                return false;
            if (mods == Qt::NoModifier && mResults->isVisibleTo(this) && mResults->currentRow() >= 0)
            {
                runCurrent();
                return true;
            }
            QString code = mEdit->toPlainText();
            close();
            if (!code.trimmed().isEmpty())
                runLine(code);
            return true;
        }
        default: return false;
        }
    }

    CodeEditor* mEdit = nullptr;
    QListWidget* mResults = nullptr;
    QLabel* mHint = nullptr;
    std::vector<PaletteResult> mFound;
    int mLine = 0;
    bool mFromHistory = false;
    bool mWalking = false;
    QPointer<QWidget> mOver;
    QPointer<ViewportLayout> mLayout;
};

QPointer<PaletteBox>& instance()
{
    static QPointer<PaletteBox> box;
    return box;
}
} // namespace

void toggle(QWidget* over)
{
    QPointer<PaletteBox>& box = instance();
    if (box && box->isVisible())
    {
        box->close();
        return;
    }
    box = new PaletteBox(over);
    box->show();
    box->raise();
    box->activateWindow();
    box->setFocus();
}

void registerActions(QWidget* over)
{
    QPointer<QWidget> window(over);
    actions::add({.id = "window.commandPalette", .label = "Command Palette", .category = "Windows", .run = [window] {
                      if (window)
                          toggle(window);
                  },
                  .doc = "Type to find any action or command and press Enter to run it, or type Python and press Enter to run "
                         "it like the script editor does. Tab on a command starts its call; inside the call the palette lists "
                         "its arguments and Tab fills one in. Shift+Enter always runs the text as Python. Opens on the last "
                         "line; Up and Down walk the history.",
                  .python = "lila.ui.run_action(\"window.commandPalette\")"});
}
} // namespace lila::ui::commandPalette
