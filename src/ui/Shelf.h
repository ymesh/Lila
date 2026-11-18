#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <nlohmann/json.hpp>

#include <functional>

#include <QTimer>
#include <QWidget>

class QComboBox;
class QDockWidget;
class QLabel;
class QLayout;
class QMenu;
class QTabBar;
class QStackedWidget;

namespace lila::ui
{
/**
 * Shelves live in prefs as "shelves/<Name>" ({"items": [{"label", "icon", "target", "tooltip"}]}), ordered by the general
 * pref /shelves/order. "icon" names a Tabler icon (see ui::icon). A target is anything actions::run accepts: an action
 * id, {"command": ...} or {"python": ...}.
 */
namespace shelves
{
LILA_UI_API std::vector<std::string> names();
LILA_UI_API nlohmann::json items(const std::string& shelf);
LILA_UI_API void setItems(const std::string& shelf, const nlohmann::json& items);
LILA_UI_API void addItem(const std::string& shelf, const nlohmann::json& item);
LILA_UI_API void create(const std::string& shelf);
LILA_UI_API void remove(const std::string& shelf);
LILA_UI_API bool rename(const std::string& shelf, const std::string& to);
/** A studio shelf locked from its network folder: shown to everyone, never edited, renamed or deleted here. */
LILA_UI_API bool isLocked(const std::string& shelf);

/**
 * A shelf contributed in code or from an extension's resources/shelves: its items are defaults under the user's prefs,
 * so the user can still edit it, and it is listed after the user's own order until they place it.
 */
struct Shelf
{
    std::string name;
    nlohmann::json items = nlohmann::json::array();
};
LILA_UI_API Registry<Shelf>& registry();
LILA_UI_API void add(Shelf s, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t add(ext::Context& ctx, Shelf s);

/**
 * Adds a button for target (anything actions::run accepts), labelled like the action, to the shelf the floating shelf
 * shows while it is open, else to the current shelf.
 */
LILA_UI_API void addToCurrent(const nlohmann::json& target);
/** Ctrl+Shift is held: clicking a menu item or releasing on a marking menu item adds it to the shelf instead of running it. */
LILA_UI_API bool addModifierHeld();

/** The shelf the docked shelf shows; the first when there is no docked shelf. */
LILA_UI_API std::string current();
/** Shows that shelf in the docked shelf. False when there is no such shelf. */
LILA_UI_API bool setCurrent(const std::string& shelf);
/** Calls fn with the shelf's name whenever the docked shelf switches tab. Returns an id for removeOnCurrentChanged. */
LILA_UI_API int onCurrentChanged(std::function<void(const std::string&)> fn);
LILA_UI_API void removeOnCurrentChanged(int id);

/** Icon size of the floating shelf's buttons, the general pref /floatingShelf/iconSize. Ctrl+wheel over it scales it. */
constexpr int kFloatingIconDefault = 24;
LILA_UI_API int floatingIconSize();
/** size moved by notches wheel steps, clamped to the sizes the floating shelf allows. */
LILA_UI_API int stepFloatingIconSize(int size, int notches);
} // namespace shelves

/**
 * One shelf's buttons: a single row when docked, a wrapping grid of square icon buttons when floating. Buttons drag to
 * reorder (also onto another shelf) and right-click to edit.
 */
class LILA_UI_API ShelfPage : public QWidget
{
    Q_OBJECT

public:
    ShelfPage(std::string shelf, bool floating, QWidget* parent = nullptr);
    ~ShelfPage() override;
    void setShelf(std::string shelf);
    void rebuild();
    const std::string& shelf() const { return mShelf; }

    /** Opens the button editor for a new button and appends it. */
    void addButton();
    /** The ways to add a button: new Python or command button, paste, and every registered action. */
    void fillAddMenu(QMenu* menu);

Q_SIGNALS:
    void triggered();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void itemMenu(int index, QPoint global);
    int dropIndex(QPoint pos) const;

    std::string mShelf;
    bool mFloating;
    QLayout* mLayout = nullptr;
    QPoint mPressPos;
    int mPrefListener = 0;
};

/** The docked shelf strip with a tab per shelf. */
class LILA_UI_API ShelfPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ShelfPanel(QWidget* parent = nullptr);
    ~ShelfPanel() override;

    /** A thin grip in place of the dock's title bar, so the shelf keeps its vertical space but can still be dragged. */
    static QWidget* dockTitleBar(QDockWidget* dock);

    std::string currentShelf() const;
    bool showShelf(const std::string& name);

private:
    void rebuild();

    QTabBar* mTabs = nullptr;
    QStackedWidget* mPages = nullptr;
    std::vector<std::string> mNames;
    int mPrefListener = 0;
};

/**
 * The floating shelf window. toggle() opens it pinned, staying up until toggled again or closed. popup() opens it
 * without taking focus: hold its hotkey and release over a button to run it, or tap and click; it stays until the
 * hotkey is pressed again unless /floatingShelf/closeOnClickAway or /floatingShelf/closeAfterRun is set. Where it
 * opens is the general pref /floatingShelf/openAt (cursor, viewport or screen).
 */
class LILA_UI_API FloatingShelf : public QWidget
{
    Q_OBJECT

public:
    static void toggle();
    static void popup();
    static void releaseKey();
    /** Once per install: opens the shelf with a line naming its hotkey, for people who never read the docs. */
    static void introduce();
    static FloatingShelf* active();
    /** Whether the shelf is on screen. */
    static bool isOpen();
    /** The shelf it shows. */
    std::string shelf() const;

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void moveEvent(QMoveEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    FloatingShelf(QWidget* parent, bool pinned);
    ~FloatingShelf() override;
    static void open(bool pinned);
    void build();
    void showIntro();
    void reloadPicker();
    void showShelf(const std::string& name);
    void place();
    void settingsMenu(QMenu* menu);
    /** With the remember-position pref on, stores where the shelf is so the next one opens there. */
    void rememberPosition();
    void setIconSize(int size);
    /** The window sides within the frame band at p, in local coordinates; the shelf has no native frame. */
    Qt::Edges frameEdges(QPoint p) const;
    static Qt::CursorShape edgeCursor(Qt::Edges edges);

    QLabel* mGrip = nullptr;
    QComboBox* mPicker = nullptr;
    ShelfPage* mPage = nullptr;
    bool mPinned = false;
    qint64 mOpenedAt = 0;
    int mPrefListener = 0;
    int mWheelAccum = 0;
    QTimer mPositionTimer;
};

LILA_UI_API void registerShelfActions();
} // namespace lila::ui
