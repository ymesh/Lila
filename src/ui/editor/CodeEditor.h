#pragma once

#include "core/Export.h"

#include "ui/editor/EditorTypes.h"

#include <nlohmann/json.hpp>

#include <QHash>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTimer>

#include <array>
#include <functional>

class QMenu;
class QMimeData;

namespace lila::ui
{
class CompletionPopup;
class DocPopup;
class LspClient;
class PythonHighlighter;

namespace ruff
{
class Checker;
class Runner;
}

/**
 * Python source editor: gutter, highlighting, PyCharm-style editing keys, and language features from an optional
 * LspClient plus the live interpreter. Knows nothing about tabs or running code; it asks through signals.
 */
class LILA_UI_API CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class Layer
    {
        CurrentLine,
        Occurrences,
        Search,
        Brackets,
        Diagnostics,
        Link,
        Flash,
        Count,
    };

    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    void setEditorFont(const QFont& font);

    /** Empty for an untitled buffer, which then goes by its untitled name. */
    void setFilePath(const QString& path);
    QString filePath() const { return mPath; }
    void setUntitledName(const QString& name);
    QString untitledName() const { return mUntitledName; }
    QString displayName() const;

    /** What the language server and ruff know this buffer as; untitled buffers get a path under the temp dir. */
    QString uri() const;
    QString analysisPath() const;
    /** The filename tracebacks will show: the path, or `<Untitled-1>`. */
    QString runFilename() const;

    void setLanguageClient(LspClient* lsp);
    /** Enables lint-as-you-type and formatting. */
    void setRuffExe(const QString& exe);

    /** Inserts code at the cursor; a call ending in `(` gets its `)`, signature help and the argument list. */
    void insertCall(const QString& code);
    /** Opens completion at the cursor, as Ctrl+Space does. */
    void showCompletion();

    /** 0-based line and column. */
    void goToLine(int line, int col = 0);
    /** Selects the 0-based range, cursor at its end, and centres it. */
    void selectRange(int line, int col, int endLine, int endCol);
    void goToLineDialog();
    /** Formats the selection, or the whole buffer without one, in the background. */
    void formatDocument();
    void formatSelection();
    /** Sorts and groups the imports (ruff's I rules). */
    void organizeImports();
    /** Applies every safe fix ruff has. */
    void fixAll();
    /** Runs the save steps the formatting prefs ask for, blocking, as one undo step. */
    void prepareForSave();
    /** The fixes, suppressions and code actions for the caret's line, as a menu at the caret (Alt+Enter). */
    void showQuickFixes();
    /** Replaces the whole text as one undo step, editing only what changed so the caret and scroll stay put. */
    void replaceAllText(const QString& text);

    /** The code Ctrl+Enter or the run cell key would run, and the 0-based document line it starts on. */
    QString codeToRun(bool cell, int* firstLine) const;
    /** The caret's line, for running it alone. */
    QString lineToRun(int* line) const;
    /** Briefly highlights the 0-based lines, to show what just ran. */
    void flashLines(int first, int last);
    /** Set when the host routes the run keys through its rebindable hotkeys, so the editor leaves them alone. */
    bool hostOwnsRunKeys = false;

    void setLayer(Layer layer, const QList<QTextEdit::ExtraSelection>& selections);
    void setDiagnostics(const QString& source, const QList<Diagnostic>& list);
    QList<Diagnostic> diagnostics() const;

    /** The "scriptEditor" /relativeLineNumbers pref, shared with the toolbar toggle and the Preferences page. */
    static constexpr const char* kRelativeLineNumbersPref = "/relativeLineNumbers";
    static constexpr bool kRelativeLineNumbersDefault = false;
    /** Repaints the line numbers, after their pref changed. */
    void refreshGutter();
    /** Shows or hides the line numbers, for one-line uses such as the command line. */
    void setGutterVisible(bool visible);

    int lineNumberAreaWidth() const;
    void paintGutter(QPaintEvent* e);
    bool gutterEvent(QEvent* e);

    /** Tried before the editor's own keys, so the host can claim chords like Ctrl+S. */
    std::function<bool(QKeyEvent*)> hostKeyHandler;
    /**
     * Off: typing opens no completion list or signature help on its own, for hosts that show their own (the command
     * palette). Tab and Ctrl+Space still complete.
     */
    bool autoPopups = true;
    /** Tried before everything but the completion popup: a modal keymap such as VimBinding. */
    std::function<bool(QKeyEvent*)> modalKeyHandler;

    /** Key presses arrive here directly, ahead of the app's global hotkeys, while the editor has focus. */
    void routeKeyPress(QKeyEvent* e) { keyPressEvent(e); }

Q_SIGNALS:
    void runRequested();
    void runCellRequested();
    void runLineRequested();
    void findRequested(bool replace);
    void showDocs(const QString& symbol);
    void openLocation(const QString& path, int line, int col);
    /** Go to definition landed outside this buffer: the definition's 0-based range there. */
    void openDefinition(const QString& path, int line, int col, int endLine, int endCol);
    void statusMessage(const QString& text);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    bool viewportEvent(QEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void insertFromMimeData(const QMimeData* source) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    using json = nlohmann::json;

    bool handleCompletionKey(QKeyEvent* e);
    bool handleEditingKey(QKeyEvent* e);
    void afterTyping(const QString& typed);

    void newlineAndIndent();
    /** Smart dedent of else/elif/except/finally, then on-type formatting of the statement `typed` completed. */
    void formatOnType(QChar typed);
    /** Range-formats the complete statement ending on `line`, if there is one. */
    void formatStatementAt(int line);
    /** Range-formats the statements covering [start, end) in the background. */
    void formatRange(int start, int end, bool quiet);
    /**
     * Runs ruff on `source` in the background and applies the result through `finish`, unless the text changed
     * meanwhile. `quiet` keeps failures (a syntax error, usually) out of the status bar; `done` goes there on a change.
     */
    void runFormatter(const QStringList& args, const QString& source, std::function<QString(const QString&)> finish, bool quiet,
                      const QString& done);
    /** ruff style overrides for this buffer: none when a project config governs it. */
    QStringList styleArgs(bool forCheck) const;
    /** Applies `text` as minimal edits in one undo step, mapping the caret, selection and scroll through them. */
    bool applyFormatted(const QString& text);
    void applyFix(const Diagnostic& d);
    /** Appends `# noqa: CODE` (ruff) or `# pyright: ignore[rule]` to the diagnostic's line. */
    void suppress(const Diagnostic& d);
    /** Adds the formatting and fix actions to `menu`. */
    void addFormatActions(QMenu* menu);
    bool smartBackspace();
    void indentSelection(bool dedent);
    void toggleComment();
    void duplicate();
    void moveLines(int direction);
    void deleteLines();
    void smartHome(bool select);
    bool autoPair(QChar c);

    void updateGutterWidth();
    void updateGutter(const QRect& rect, int dy);
    void onCursorMoved();
    void matchBrackets();
    void rebuildExtraSelections();
    void rebuildDiagnosticSelections();

    QTextCursor cursorAt(int line, int col) const;
    json position(const QTextCursor& c) const;
    json textDocumentPosition(const QTextCursor& c) const;
    bool inStringOrComment(const QTextCursor& c) const;
    void applyTextEdits(const json& edits);

    void syncDocument();
    void openInLsp();
    void scheduleLint();

    void requestCompletion(bool manual);
    /** `arguments`: opened by `(` or `,` in a call, shown only when the server offers keyword arguments.
        `provisional`: the interpreter's names shown while the server's answer is still on its way; never hides the popup. */
    void showCompletions(const json& result, bool manual, bool arguments, int generation, bool provisional = false);
    void acceptCompletion();
    void resolveCompletion(int entryIndex);
    void hideCompletion();

    void requestSignatureHelp();
    void showSignature(const json& result);
    void hideSignature();
    /** Fills the docs of the popup's `name=` items from the Args section of the last signature shown. */
    void documentKeywordArguments();

    void requestHover(const QPoint& viewportPos);
    /** Shows `html` in the hover popup just below `global`, the point the mouse rested on. */
    void showHover(const QString& html, const QPoint& global);
    void hideHover();
    /** The doc popups' maximum width: about 90 columns of the editor font, within the screen. */
    int docPopupWidth() const;
    void goToDefinition(const QTextCursor& at);
    void renameSymbol();
    void requestOccurrences();
    /** Asks the language server for semantic tokens and lays them over the lexical colours. */
    void requestSemanticTokens();
    void emitShowDocs(const QTextCursor& at);
    /** The dotted name at `at`, fully qualified when the interpreter can resolve it. */
    QString docsSymbolAt(const QTextCursor& at) const;
    /** Ctrl held over a name: underline it as a link and peek at its definition. */
    void updateLinkHover(const QPoint& viewportPos);
    void clearLinkHover();
    void requestPeek();

    /** The diagnostics covering a position, hints only when they mark unused code. */
    QList<Diagnostic> diagnosticsAt(int line, int col) const;
    QList<Diagnostic> filterInterpreterKnown(QList<Diagnostic> list) const;

    QWidget* mGutter;
    bool mGutterVisible = true;
    PythonHighlighter* mHighlighter;
    CompletionPopup* mPopup;
    DocPopup* mSignature;
    DocPopup* mHover;
    ruff::Checker* mRuff;
    ruff::Runner* mFormatter;
    QPointer<LspClient> mLsp;

    QString mPath;
    QString mUntitledName = "Untitled";
    QString mOpenUri;
    QString mRuffExe;

    std::array<QList<QTextEdit::ExtraSelection>, size_t(Layer::Count)> mLayers;
    QHash<QString, QList<Diagnostic>> mDiagnostics;

    QTimer mSyncTimer;
    QTimer mLintTimer;
    QTimer mCompletionTimer;
    QTimer mSignatureTimer;
    QTimer mOccurrenceTimer;
    QTimer mSemanticTimer;

    bool mDocDirty = false;
    /** The document revision the last format left, so focus loss formats only edited text. */
    int mFormattedRevision = -1;
    QTimer mFlashTimer;
    int mSemanticRequest = -1;
    int mCompletionRequest = -1;
    int mCompletionGeneration = 0;
    int mCompletionStart = -1;
    bool mCompletionIncomplete = false;
    bool mCompletionManual = false;
    QString mCompletionBase;
    /** The live interpreter's names for mCompletionBase, read once per request. */
    QList<CompletionEntry> mInterpreterEntries;
    int mSignatureRequest = -1;
    json mLastSignature;
    int mHoverRequest = -1;
    QPoint mHoverPos;
    QTimer mPeekTimer;
    int mPeekRequest = -1;
    int mLinkStart = -1;
    int mLinkEnd = -1;
    QPoint mPeekPos;
};
} // namespace lila::ui
