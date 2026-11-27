#pragma once

#include "core/Export.h"

#include "ui/HotkeyBindings.h"

#include <QDialog>
#include <QWidget>

#include <functional>
#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
/**
 * Swallows the next key chord app-wide (ahead of Hotkeys, which would otherwise run it) and reports it.
 * Installed on qApp while armed; app event filters run newest first.
 */
class LILA_UI_API ChordGrabber : public QObject
{
    Q_OBJECT

public:
    explicit ChordGrabber(QObject* parent = nullptr);
    ~ChordGrabber() override;
    void arm();
    void disarm();
    bool armed() const { return mArmed; }

Q_SIGNALS:
    void captured(QKeyCombination chord);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    bool mArmed = false;
};

/** Press a chord, pick a mode and context, and write it for one target (an action id, a {"markingMenu": name} or a
 * {"python": source}). */
class LILA_UI_API ChordCaptureDialog : public QDialog
{
    Q_OBJECT

public:
    ChordCaptureDialog(nlohmann::json target, std::string context, QWidget* parent = nullptr,
                       hotkeys::Slot slot = hotkeys::Slot::Press);

    /** Writes the binding. Call after exec() returns Accepted. */
    void apply() const;

private:
    void onCaptured(QKeyCombination chord);
    void refreshInfo();

    nlohmann::json mTarget;
    std::optional<QKeyCombination> mChord;
    ChordGrabber* mGrabber = nullptr;
    QPushButton* mChordButton = nullptr;
    QComboBox* mMode = nullptr;
    QComboBox* mContext = nullptr;
    QLabel* mInfo = nullptr;
    QPushButton* mOk = nullptr;
};

/** US keyboard drawn in grays, each key shaded by how many bindings use it. */
class LILA_UI_API KeyboardHeatMap : public QWidget
{
    Q_OBJECT

public:
    explicit KeyboardHeatMap(QWidget* parent = nullptr);
    void setBindings(std::vector<hotkeys::Binding> bindings);
    void setSelectedKey(int qtKey);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void keyClicked(int qtKey);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    struct Cap
    {
        QString label;
        int key = 0;
        QRectF unitRect; // in key units
    };
    QRectF capRect(const Cap& c) const;
    const Cap* capAt(QPointF pos) const;
    int countFor(int key) const;

    std::vector<Cap> mCaps;
    std::vector<hotkeys::Binding> mBindings;
    int mHover = 0;
    int mSelected = 0;
};

/** Lists every action and binding by context, with search, conflicts, assign / unbind / reset and a key map. */
class LILA_UI_API HotkeyEditor : public QWidget
{
    Q_OBJECT

public:
    explicit HotkeyEditor(QWidget* parent = nullptr);
    ~HotkeyEditor() override;

private:
    struct KeyFilter
    {
        int key = 0;
        std::optional<int> mods; // unset matches any modifiers
    };

    void rebuild();
    bool matches(const hotkeys::Binding& b) const;
    bool matchesText(const std::string& actionId, const std::string& label, const std::string& category,
                     const std::string& extra) const;
    QString conflictFor(const hotkeys::Binding& b, const hotkeys::Binding** other) const;
    void setKeyFilter(std::optional<KeyFilter> f);
    void updateButtons();
    std::string selectedAction() const;
    std::string selectedContext() const;
    const hotkeys::Binding* selectedBinding() const;
    void assignSelected();
    void pythonHotkey();
    void refreshKeymaps();
    /** Runs a keymap change, showing why it failed. */
    void keymapChange(const std::function<void()>& fn);

    hotkeys::Snapshot mSnap;
    std::optional<KeyFilter> mKeyFilter;
    QLineEdit* mSearch = nullptr;
    QToolButton* mKeySearch = nullptr;
    QToolButton* mClearKey = nullptr;
    QComboBox* mContextFilter = nullptr;
    QComboBox* mKeymap = nullptr;
    QTreeWidget* mTree = nullptr;
    KeyboardHeatMap* mMap = nullptr;
    QLabel* mDetail = nullptr;
    QPushButton* mAssign = nullptr;
    QPushButton* mPython = nullptr;
    QPushButton* mUnbind = nullptr;
    QPushButton* mReset = nullptr;
    ChordGrabber* mGrabber = nullptr;
    int mPrefListener = 0;
};

/** Capture dialog that binds a chord to actionId; usable from menus, shelves and marking menus. */
LILA_UI_API void assignHotkeyDialog(const std::string& actionId, QWidget* parent);

/** Capture dialog that binds key + mouse button to open the named marking menu. False when cancelled. */
LILA_UI_API bool assignMarkingMenuHotkeyDialog(const std::string& menu, QWidget* parent);

/** Picks a Maya hotkey set and imports it into a new keymap, then lists what didn't map. */
LILA_UI_API void importMayaHotkeysDialog(QWidget* parent);

/** Adds "Assign Hotkey..." to the right-click of any QMenu entry whose QAction data or objectName is an action id. */
LILA_UI_API void installMenuHotkeyHook();
} // namespace lila::ui
