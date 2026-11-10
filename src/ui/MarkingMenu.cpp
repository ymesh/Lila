#include "ui/MarkingMenu.h"

#include "scene/Graph.h"
#include "scene/Materials.h"

#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "ui/AchievementRules.h"
#include "ui/Actions.h"
#include "ui/CameraBookmarks.h"
#include "ui/CommandPreview.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Shelf.h"
#include "ui/Theme.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
// Far enough out that the taller items of neighbouring slots (N and NE) don't overlap.
constexpr double kMenuRadius = 110.0;
constexpr double kDeadZone = 18.0;
// How long the cursor must rest on a compass item's option box before releasing there opens it rather than the item.
constexpr qint64 kBoxDwellMs = 250;
// A submenu row or a submenu in the slot above the list opens only after the cursor rests this long, so moving past
// it to reach something else never pops it open.
constexpr qint64 kSubmenuDwellMs = 450;
constexpr double kItemHeight = 32.0;
// Text inset from an item's left and right edges.
constexpr double kPadX = 13.0;
constexpr double kSeparatorHeight = 9.0;
constexpr double kBoxWidth = 18.0;
const char* kSlotNames[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

bool hasOptionBox(const json& action, const json& children, const json& boxAction)
{
    if (children.is_object())
        return false;
    return !boxAction.is_null() || (action.is_string() && optionBox::available(action.get<std::string>()));
}

MarkingMenu* gActive = nullptr;

void showStatus(const QString& text)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
            if (mw->statusBar())
            {
                text.isEmpty() ? mw->statusBar()->clearMessage() : mw->statusBar()->showMessage(text);
                return;
            }
}
} // namespace

MarkingMenu* MarkingMenu::active()
{
    return gActive;
}

namespace markingMenus
{
namespace
{
constexpr const char* kLayer = "contributed";

std::string& activeSetName()
{
    static std::string s;
    return s;
}
} // namespace

Registry<json>& registry()
{
    static Registry<json> r;
    static int listener = r.addListener([](RegistryChange change, const std::string& name, const std::string&) {
        if (change == RegistryChange::Removed)
            PrefStore::get().removeDefaults("markingMenus/" + name, kLayer);
        else if (const json* def = r.find(name))
            PrefStore::get().setDefaults("markingMenus/" + name, kLayer, *def);
    });
    (void)listener;
    return r;
}

void add(const std::string& name, json def, std::string owner)
{
    registry().put(name, def.is_object() ? std::move(def) : json::object(), std::move(owner));
}

uint64_t add(ext::Context& ctx, const std::string& name, json def)
{
    if (const std::string* owner = registry().ownerOf(name); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "marking menu '" + name + "' already belongs to " + *owner);
        return 0;
    }
    add(name, std::move(def), ctx.owner());
    return ctx.track("markingMenu:" + name, [name, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(name); o && *o == own)
            registry().remove(name);
    });
}

bool remove(const std::string& name)
{
    return registry().remove(name);
}

void addSet(const std::string& set, const json& menus, std::string owner)
{
    if (!menus.is_object())
        return;
    for (auto& [name, def] : menus.items())
        add(set + "/" + name, def, owner);
}

uint64_t addSet(ext::Context& ctx, const std::string& set, const json& menus)
{
    addSet(set, menus, ctx.owner());
    return ctx.track("markingMenuSet:" + set, [set, own = ctx.owner()] {
        for (const std::string& name : registry().owned(own))
            if (name.starts_with(set + "/"))
                registry().remove(name);
    });
}

size_t removeSet(const std::string& set)
{
    size_t n = 0;
    for (const std::string& name : registry().ids())
        if (name.starts_with(set + "/"))
            n += registry().remove(name) ? 1 : 0;
    return n;
}

Registry<ItemAdd>& itemRegistry()
{
    static Registry<ItemAdd> r;
    return r;
}

std::string addItem(ItemAdd add, std::string owner)
{
    std::string base = add.menu + "/" + add.slot + "/" + add.item.value("label", std::string());
    std::string id = base;
    for (int n = 2; itemRegistry().contains(id); ++n)
        id = base + "#" + std::to_string(n);
    itemRegistry().put(id, std::move(add), std::move(owner));
    return id;
}

uint64_t addItem(ext::Context& ctx, ItemAdd add)
{
    std::string id = addItem(std::move(add), ctx.owner());
    return ctx.track("markingMenuItem:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = itemRegistry().ownerOf(id); o && *o == own)
            itemRegistry().remove(id);
    });
}

bool removeItem(const std::string& id)
{
    return itemRegistry().remove(id);
}

void setActiveSet(std::string set)
{
    activeSetName() = std::move(set);
}

const std::string& activeSet()
{
    return activeSetName();
}

json resolve(const std::string& name)
{
    std::string used = name;
    json def;
    if (!activeSet().empty())
    {
        used = activeSet() + "/" + name;
        def = PrefStore::get().merged("markingMenus/" + used);
    }
    if (!def.is_object() || def.empty())
    {
        used = name;
        def = PrefStore::get().merged("markingMenus/" + name);
    }
    if (!def.is_object())
        def = json::object();
    for (ItemAdd* add : itemRegistry().allInOrder())
    {
        if (add->menu != used && add->menu != name)
            continue;
        if (add->slot == "overflow")
        {
            if (!def["overflow"].is_array())
                def["overflow"] = json::array();
            def["overflow"].push_back(add->item);
        }
        else if (std::find(std::begin(kSlotNames), std::end(kSlotNames), add->slot) != std::end(kSlotNames))
            def[add->slot] = add->item;
    }
    return def;
}

std::vector<std::string> contributedNames()
{
    return registry().ids();
}

std::string viewportMenuName(const std::string& base, bool overObject, bool componentMode)
{
    return base + (overObject || componentMode ? "Object" : "Empty");
}
} // namespace markingMenus

MarkingMenu* MarkingMenu::popup(const std::string& name, Trigger trigger, Qt::MouseButton button, std::optional<QPoint> at)
{
    return popupDefinition(markingMenus::resolve(name), trigger, button, at);
}

MarkingMenu* MarkingMenu::popupDefinition(const json& def, Trigger trigger, Qt::MouseButton button, std::optional<QPoint> at)
{
    if (!def.is_object() || def.empty())
        return nullptr;
    if (gActive)
        gActive->close();
    auto* mm = new MarkingMenu(def, trigger, button, at.value_or(QCursor::pos()));
    mm->show();
    mm->activateWindow();
    mm->grabMouse();
    mm->grabKeyboard();
    return mm;
}

MarkingMenu::~MarkingMenu()
{
    commandPreview::clear();
    if (!mStatus.isEmpty())
        showStatus({});
    if (gActive == this)
        gActive = nullptr;
}

MarkingMenu::MarkingMenu(const json& def, Trigger trigger, Qt::MouseButton button, QPoint at)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint),
      mTrigger(trigger), mButton(button)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    setMouseTracking(true);

    QPoint cursor = at;
    QScreen* screen = QGuiApplication::screenAt(cursor);
    setGeometry(screen ? screen->geometry() : QRect(cursor - QPoint(600, 600), QSize(1200, 1200)));

    mCursor = mapFromGlobal(cursor);
    mLevels.push_back(buildLevel(def, mCursor));
    // Before the menu shows, so the widget under the cursor is still the viewport's.
    for (QWidget* w = QApplication::widgetAt(cursor); w; w = w->parentWidget())
        if (auto* view = qobject_cast<ViewportWidget*>(w))
        {
            mPreviewFocus = commandPreview::Focus{view->viewInfo(), view->mapFromGlobal(cursor).toPointF()};
            break;
        }
    gActive = this;
    // Losing focus to another app means the button release never arrives (alt-tab sends a key the menu sees instead).
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
            dismiss();
    });
}

void MarkingMenu::dismiss()
{
    commandPreview::clear();
    if (gActive == this)
        gActive = nullptr;
    releaseMouse();
    releaseKeyboard();
    close();
}

MarkingMenu::Level MarkingMenu::buildLevel(const json& def, QPointF center, std::optional<QPointF> cascade) const
{
    Level level;
    level.center = center;
    level.cascade = cascade;
    auto toItem = [](const json& j) {
        Item it;
        it.label = QString::fromStdString(j.value("label", actions::labelFor(j.value("action", json()))));
        it.action = j.value("action", json());
        it.children = j.value("children", json());
        it.boxAction = j.value("optionAction", json());
        it.enabled = j.value("enabled", true) && actions::isEnabled(it.action);
        if (const Action* a = it.action.is_string() ? actions::find(it.action.get<std::string>()) : nullptr; a && a->checked)
            it.checked = a->checked();
        return it;
    };
    for (int i = 0; i < 8; ++i)
        if (def.contains(kSlotNames[i]) && def[kSlotNames[i]].is_object())
            level.compass[i] = toItem(def[kSlotNames[i]]);
    if (def.contains("overflow") && def["overflow"].is_array())
        for (const json& j : def["overflow"])
        {
            if (!j.is_object())
                continue;
            if (j.value("column", false))
            {
                if (!level.overflow.empty() && level.overflow.back().separator)
                    level.overflow.pop_back();
                if (!level.overflow.empty() && !level.overflow.back().column)
                {
                    Item col;
                    col.column = true;
                    col.enabled = false;
                    level.overflow.push_back(std::move(col));
                }
                continue;
            }
            if (j.value("separator", false))
            {
                // Never leading or doubled; a trailing one is dropped after the loop.
                if (!level.overflow.empty() && !level.overflow.back().separator && !level.overflow.back().column)
                {
                    Item sep;
                    sep.separator = true;
                    sep.enabled = false;
                    level.overflow.push_back(std::move(sep));
                }
                continue;
            }
            // A placeholder for the scene's camera nodes, each looked through from the active view.
            if (j.value("insert", std::string()) == "cameraNodes")
            {
                scene::Graph& g = scene::Graph::current();
                for (scene::NodeId id : g.nodes())
                    if (const scene::Node* n = g.node(id); n->type().isA("camera"))
                        level.overflow.push_back(toItem({{"label", n->name()},
                                                         {"action", {{"command", "lookThru"}, {"args", {{"args", {n->path()}}}}}}}));
                continue;
            }
            // A placeholder for the active view's camera bookmarks: a jump per saved slot, then Set and Clear.
            if (j.value("insert", std::string()) == "cameraBookmarks")
            {
                const ViewportWidget* view = cameraBookmarks::activeView();
                std::vector<int> saved = view ? view->bookmarks() : std::vector<int>{};
                for (int slot : saved)
                    level.overflow.push_back(toItem({{"label", "Bookmark " + std::to_string(slot)},
                                                     {"action", "viewport.bookmarkGo" + std::to_string(slot)}}));
                json set = json::array();
                for (int slot = 1; slot <= cameraBookmarks::kSlots; ++slot)
                    set.push_back({{"label", std::to_string(slot) + (std::ranges::find(saved, slot) != saved.end() ?"  (replace)" : "")},
                                   {"action", "viewport.bookmarkSet" + std::to_string(slot)}});
                level.overflow.push_back(toItem({{"label", "Set Bookmark"}, {"children", {{"overflow", set}}}}));
                level.overflow.push_back(toItem({{"label", "Clear Bookmarks"}, {"action", "viewport.bookmarkClear"}}));
                continue;
            }
            // A placeholder for the scene's materials: each assigns, its option box opens it in the Material Editor.
            if (j.value("insert", std::string()) == "materials")
            {
                std::vector<std::string> names;
                for (const scene::Node* n : scene::materials::all(scene::Graph::current()))
                    names.push_back(n->path());
                std::ranges::sort(names);
                for (const std::string& name : names)
                    level.overflow.push_back(toItem({{"label", name},
                                                     {"action", {{"command", "assignMaterial"}, {"args", {{"args", {name}}}}}},
                                                     {"optionAction", {{"command", "editMaterial"}, {"args", {{"args", {name}}}}}}}));
                if (names.empty())
                    level.overflow.push_back(toItem({{"label", "No Materials"}, {"enabled", false}}));
                continue;
            }
            level.overflow.push_back(toItem(j));
        }
    while (!level.overflow.empty() && (level.overflow.back().separator || level.overflow.back().column))
        level.overflow.pop_back();
    // Outer ring items share the overflow rows' picking and drawing, so they go after them.
    if (const json& outer = def.value("outer", json()); outer.is_object() && !cascade)
        for (int i = 0; i < 8; ++i)
        {
            if (!outer.contains(kSlotNames[i]))
                continue;
            const json& items = outer[kSlotNames[i]];
            for (const json& j : items.is_array() ? items : json::array({items}))
                if (j.is_object())
                {
                    Item it = toItem(j);
                    it.outerSlot = i;
                    level.overflow.push_back(std::move(it));
                }
        }
    layout(level);
    return level;
}

void MarkingMenu::layout(Level& level) const
{
    QFontMetricsF fm(font());
    for (int i = 0; i < 8; ++i)
    {
        if (!level.compass[i])
            continue;
        Item& it = *level.compass[i];
        bool box = hasOptionBox(it.action, it.children, it.boxAction);
        double w = fm.horizontalAdvance(it.label) + 2 * kPadX + 2 + (it.children.is_object() ? 12 : 0) + (box ? kBoxWidth : 0);
        QPointF p = level.center + kCompassDirs[i] * kMenuRadius;
        double x = kCompassDirs[i].x() > 0.1 ? p.x() - 12 : kCompassDirs[i].x() < -0.1 ? p.x() - w + 12 : p.x() - w / 2;
        it.rect = QRectF(x, p.y() - kItemHeight / 2, w, kItemHeight);
        it.box = box ? QRectF(it.rect.right() - kBoxWidth, it.rect.top(), kBoxWidth, it.rect.height()) : QRectF();
    }

    // Each outer item stacks beyond the one before it in its slot, starting from the compass item (or where it would be).
    std::array<QRectF, 8> last;
    for (int i = 0; i < 8; ++i)
    {
        QPointF p = level.center + kCompassDirs[i] * kMenuRadius;
        last[i] = level.compass[i] ? level.compass[i]->rect : QRectF(p.x() - 30, p.y() - kItemHeight / 2, 60, kItemHeight);
    }
    constexpr double kGap = 6.0;
    const size_t rows = std::ranges::find_if(level.overflow, [](const Item& it) { return it.outerSlot >= 0; }) - level.overflow.begin();
    for (size_t i = rows; i < level.overflow.size(); ++i)
    {
        Item& it = level.overflow[i];
        const QPointF d = kCompassDirs[it.outerSlot];
        const QRectF& prev = last[it.outerSlot];
        const bool box = hasOptionBox(it.action, it.children, it.boxAction);
        const double w = fm.horizontalAdvance(it.label) + 2 * kPadX + 2 + (box ? kBoxWidth : 0);
        double x = prev.center().x() - w / 2, y = prev.top();
        if (std::abs(d.y()) < 0.1)
            x = d.x() > 0 ? prev.right() + kGap : prev.left() - kGap - w;
        else
        {
            y = d.y() < 0 ? prev.top() - kGap - kItemHeight : prev.bottom() + kGap;
            if (d.x() > 0.1)
                x = prev.left() + 16;
            else if (d.x() < -0.1)
                x = prev.right() - 16 - w;
        }
        it.rect = QRectF(x, y, w, kItemHeight);
        it.box = box ? QRectF(it.rect.right() - kBoxWidth, it.rect.top(), kBoxWidth, it.rect.height()) : QRectF();
        last[it.outerSlot] = it.rect;
    }

    // Each column is as wide as its own widest row, and the columns sit side by side centred under the menu.
    struct Column
    {
        size_t begin = 0, end = 0;
        double width = 0;
    };
    std::vector<Column> columns(1);
    bool box = false;
    for (size_t i = 0; i < rows; ++i)
    {
        Item& it = level.overflow[i];
        if (it.column)
        {
            columns.back().end = i;
            columns.back().width += box ? kBoxWidth : 0;
            columns.push_back({i + 1});
            box = false;
            continue;
        }
        columns.back().width = std::max(columns.back().width, fm.horizontalAdvance(it.label) + 2 * kPadX + 9 + (it.children.is_object() ? 12 : 0));
        box |= hasOptionBox(it.action, it.children, it.boxAction);
    }
    columns.back().end = rows;
    columns.back().width += box ? kBoxWidth : 0;
    constexpr double kColumnGap = 8.0;
    double total = -kColumnGap;
    for (const Column& c : columns)
        total += c.width + kColumnGap;

    double left = level.center.x() - total / 2;
    // Below the compass and the outer ring, which can stack down past the south item.
    double top = level.center.y() + kMenuRadius + kItemHeight / 2;
    for (const QRectF& r : last)
        top = std::max(top, r.bottom());
    top += 10;
    if (level.cascade)
    {
        left = level.cascade->x();
        // Kept on screen: a long list near the bottom moves up.
        top = std::max(0.0, std::min(level.cascade->y(), height() - kItemHeight * double(level.overflow.size()) - 4));
    }
    for (const Column& c : columns)
    {
        double y = top;
        for (size_t i = c.begin; i < c.end; ++i)
        {
            Item& it = level.overflow[i];
            if (it.separator)
            {
                it.rect = QRectF(left, y - 1, c.width, kSeparatorHeight);
                y += kSeparatorHeight;
                continue;
            }
            it.rect = QRectF(left, y, c.width, kItemHeight - 2);
            it.box = hasOptionBox(it.action, it.children, it.boxAction) ? QRectF(it.rect.right() - kBoxWidth, y, kBoxWidth, it.rect.height())
                                                                         : QRectF();
            y += kItemHeight;
        }
        left += c.width + kColumnGap;
    }
}

MarkingMenu::Item* MarkingMenu::highlighted()
{
    Level& l = mLevels.back();
    if (mOverflow >= 0)
        return &l.overflow[mOverflow];
    if (mSlot >= 0 && l.compass[mSlot])
        return &*l.compass[mSlot];
    return nullptr;
}

void MarkingMenu::updateHighlight(QPointF pos)
{
    mCursor = pos;
    mCopied = false;
    // Back at a parent's centre, or on one of its rows, the submenus above it close and the parent picks again.
    while (mLevels.size() > 1)
    {
        const Level& parent = mLevels[mLevels.size() - 2];
        const QPointF back = pos - parent.center;
        bool onParent = !parent.cascade && std::hypot(back.x(), back.y()) < kDeadZone;
        for (const Item& row : parent.overflow)
            onParent |= row.rect.adjusted(-4, -1, -16, 1).contains(pos);
        if (!onParent)
            break;
        mLevels.pop_back();
        mSticky = false;
        update();
    }
    Level& l = mLevels.back();
    const int wasOverflow = mOverflow;
    mOverflow = -1;
    for (int i = 0; i < int(l.overflow.size()); ++i)
        if (l.overflow[i].enabled && l.overflow[i].rect.adjusted(-4, -1, 4, 1).contains(pos))
            mOverflow = i;
    if (mOverflow >= 0 && mOverflow != wasOverflow && l.overflow[mOverflow].children.is_object())
    {
        mRowSince.start();
        QTimer::singleShot(kSubmenuDwellMs, this, [this] {
            if (gActive == this)
                updateHighlight(mapFromGlobal(QCursor::pos()).toPointF());
        });
    }

    const bool wasOnBox = mOnBox;
    const int wasSlot = mSlot;
    mSlot = -1;
    mOnBox = false;
    // An option box is picked by position, not direction, so a square in a neighbouring sector still wins.
    for (int i = 0; i < 8 && mOverflow < 0; ++i)
        if (l.compass[i] && l.compass[i]->enabled && l.compass[i]->box.adjusted(-2, -2, 2, 2).contains(pos))
        {
            mSlot = i;
            mOnBox = true;
        }
    if (mOnBox && (!wasOnBox || wasSlot != mSlot))
    {
        mOnBoxSince.start();
        QTimer::singleShot(kBoxDwellMs, this, qOverload<>(&QWidget::update));
    }
    if (mOverflow >= 0)
        mOnBox = l.overflow[mOverflow].box.adjusted(0, -1, 4, 1).contains(pos);
    QPointF d = pos - l.center;
    double dist = std::hypot(d.x(), d.y());
    if (mOverflow < 0 && !mOnBox && dist > kDeadZone)
    {
        double angle = std::atan2(d.x(), -d.y()) * 180.0 / 3.14159265358979;
        int sector = int(std::lround((angle < 0 ? angle + 360.0 : angle) / 45.0)) % 8;
        if (l.compass[sector] && l.compass[sector]->enabled)
            mSlot = sector;
    }

    if (mSlot != wasSlot && mSlot >= 0 && l.compass[mSlot] && l.compass[mSlot]->children.is_object())
    {
        mSlotSince.start();
        QTimer::singleShot(kSubmenuDwellMs, this, [this] {
            if (gActive == this)
                updateHighlight(mapFromGlobal(QCursor::pos()).toPointF());
        });
    }
    // Moving past a submenu item opens it, centered on that item. The south slot sits on the way down to the list,
    // so there it opens only once the cursor rests on it.
    const bool southOverList = mSlot == 4 && !l.overflow.empty();
    const bool rested = mSlotSince.isValid() && mSlotSince.elapsed() >= kSubmenuDwellMs;
    if (Item* it = highlighted(); it && mOverflow < 0 && it->children.is_object() &&
                                  ((dist > kMenuRadius + 35 && !southOverList) || (southOverList && rested && dist > kMenuRadius - 10)))
    {
        mLevels.push_back(buildLevel(it->children, it->rect.center()));
        mSlot = -1;
        mOverflow = -1;
    }
    // An overflow row with children cascades once the cursor reaches its arrow or rests on the row.
    else if (it && mOverflow >= 0 && it->children.is_object() &&
             (pos.x() > it->rect.right() - 16 || (mRowSince.isValid() && mRowSince.elapsed() >= kSubmenuDwellMs)))
    {
        QPointF origin(it->rect.right() + 2, it->rect.top());
        mLevels.push_back(buildLevel(it->children, QPointF(it->rect.right(), it->rect.center().y()), origin));
        mSlot = -1;
        mOverflow = -1;
    }
    QString status;
    if (Item* it = highlighted(); it && it->action.is_string())
        if (const Action* a = actions::find(it->action.get<std::string>()))
            status = QString::fromStdString(a->doc);
    if (status != mStatus)
        showStatus(mStatus = status);
    schedulePreview();
    update();
}

void MarkingMenu::schedulePreview()
{
    json target;
    if (Item* it = highlighted(); it && !mOnBox && !it->action.is_null())
        target = it->action;
    if (target == mPreviewTarget)
        return;
    mPreviewTarget = target;
    commandPreview::clear();
    const int delay = int(PrefStore::get().value<double>("general", commandPreview::kDelayMsPref, commandPreview::kDelayMsDefault));
    if (!target.is_null())
        QTimer::singleShot(delay, this, [this, target] {
            if (gActive == this && mPreviewTarget == target)
                commandPreview::show(target, mPreviewFocus);
        });
}

bool MarkingMenu::release()
{
    json action, boxAction;
    // A flick can release before any mouse move reaches the menu.
    updateHighlight(mapFromGlobal(QCursor::pos()).toPointF());
    bool options = false;
    if (Item* it = highlighted())
    {
        action = it->action;
        boxAction = it->boxAction;
        options = mOnBox && !it->box.isEmpty() && (mOverflow >= 0 || (mOnBoxSince.isValid() && mOnBoxSince.elapsed() >= kBoxDwellMs));
        // Released on a submenu row: open it and stay up so the next click picks from it.
        if (action.is_null() && !options && it->children.is_object())
        {
            const QPointF origin(it->rect.right() + 2, it->rect.top());
            mLevels.push_back(buildLevel(it->children, QPointF(it->rect.right(), it->rect.center().y()), origin));
            mSlot = -1;
            mOverflow = -1;
            mSticky = true;
            update();
            return true;
        }
    }
    gActive = nullptr;
    commandPreview::clear();
    if (!mStatus.isEmpty())
        showStatus(mStatus = QString());
    releaseMouse();
    releaseKeyboard();
    close();
    if (action.is_null() && !(options && !boxAction.is_null()))
        return false;
    // After closing, so the action sees the real focus widget.
    if (options && shelves::addModifierHeld())
        shelves::addToCurrent(boxAction.is_null() ? json{{"optionBox", action.get<std::string>()}} : boxAction);
    else if (options && !boxAction.is_null())
        QTimer::singleShot(0, [boxAction] {
            actions::ViaScope via(actions::Via::MarkingMenu);
            actions::run(boxAction);
        });
    else if (options)
        QTimer::singleShot(0, [id = action.get<std::string>()] { optionBox::open(id); });
    else if (shelves::addModifierHeld())
        shelves::addToCurrent(action);
    else
        QTimer::singleShot(0, [action] {
            actions::ViaScope via(actions::Via::MarkingMenu);
            actions::run(action);
            achievementRules::hotboxPicked();
        });
    return true;
}

bool MarkingMenu::keyReleased()
{
    updateHighlight(mapFromGlobal(QCursor::pos()).toPointF());
    return highlighted() && release();
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* e)
{
    updateHighlight(e->position());
}

void MarkingMenu::mouseReleaseEvent(QMouseEvent* e)
{
    if (mSticky || (mTrigger == Trigger::MouseButton && e->button() == mButton))
        release();
}

void MarkingMenu::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Control)
    {
        update();
        return;
    }
    if (e->key() == Qt::Key_C && (e->modifiers() & Qt::ControlModifier))
    {
        if (Item* it = highlighted())
        {
            QGuiApplication::clipboard()->setText(QString::fromStdString(actions::pythonFor(it->action)));
            mCopied = true;
            update();
        }
        return;
    }
    if (e->key() == Qt::Key_Escape)
        dismiss();
}

void MarkingMenu::keyReleaseEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Control)
        update();
}

void MarkingMenu::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // The theme's greys and selection colour, so the menu matches the panels it opens over.
    const QColor itemBg = themeShade(44, 235), itemBorder = themeShade(80), text = themeColor("text");
    QColor hiBg = themeColor("highlight");
    hiBg.setAlpha(245);
    const QColor hiText = themeColor("highlightText"), disabledText = themeColor("disabled");
    const QColor shadow(0, 0, 0, 90);
    constexpr qreal kCorner = 4;

    for (size_t li = 0; li < mLevels.size(); ++li)
    {
        const Level& l = mLevels[li];
        bool top = li + 1 == mLevels.size();
        p.setOpacity(top ? 1.0 : 0.35);

        p.setPen(QPen(themeShade(200), 1.5));
        p.setBrush(themeShade(30, 200));
        p.drawEllipse(l.center, 5, 5);

        auto drawItem = [&](const Item& it, bool hi) {
            if (it.column)
                return;
            if (it.separator)
            {
                p.setPen(QPen(itemBorder, 1));
                double y = std::round(it.rect.center().y()) + 0.5;
                p.drawLine(QPointF(it.rect.left() + 6, y), QPointF(it.rect.right() - 6, y));
                return;
            }
            // On the option box the highlight moves to the square.
            const bool boxHot = hi && top && mOnBox && !it.box.isEmpty() && (mOverflow >= 0 || (mOnBoxSince.isValid() && mOnBoxSince.elapsed() >= kBoxDwellMs));
            hi = hi && !boxHot;
            QPainterPath path;
            path.addRoundedRect(it.rect, kCorner, kCorner);
            // A hard 1px shadow under each item keeps it legible over a bright model.
            p.setPen(Qt::NoPen);
            p.setBrush(shadow);
            p.drawPath(path.translated(0, 1.5));
            p.setPen(QPen(itemBorder, 1));
            p.setBrush(hi && top ? hiBg : itemBg);
            p.drawPath(path);
            p.setPen(!it.enabled ? disabledText : hi && top ? hiText : text);
            QRectF tr = it.rect.adjusted(kPadX, 0, -kPadX - (it.box.isEmpty() ? 0 : it.box.width() - 4), 0);
            p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, it.label);
            if (it.checked)
            {
                p.setBrush(p.pen().color());
                p.setPen(Qt::NoPen);
                p.drawEllipse(QPointF(it.rect.left() + kPadX / 2 + 0.5, it.rect.center().y()), 2.5, 2.5);
                p.setPen(!it.enabled ? disabledText : hi && top ? hiText : text);
            }
            if (it.children.is_object())
                p.drawText(tr, Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("▸"));
            if (!it.box.isEmpty())
            {
                if (boxHot)
                {
                    p.setPen(Qt::NoPen);
                    p.setBrush(hiBg);
                    p.drawRoundedRect(it.box.adjusted(1, 2, -2, -2), kCorner - 1, kCorner - 1);
                }
                QRectF square = QRectF(0, 0, 12, 12);
                square.moveCenter(it.box.center() - QPointF(1, 0));
                icon("app-window").paint(&p, square.toRect(), Qt::AlignCenter,
                                         !it.enabled ? QIcon::Disabled : boxHot ? QIcon::Active : QIcon::Normal);
            }
        };
        for (int i = 0; i < 8; ++i)
            if (l.compass[i])
                drawItem(*l.compass[i], top && i == mSlot);
        for (int i = 0; i < int(l.overflow.size()); ++i)
            drawItem(l.overflow[i], top && i == mOverflow);
    }

    p.setOpacity(1.0);
    p.setPen(QPen(themeShade(230, 200), 1.5));
    p.drawLine(mLevels.back().center, mCursor);

    // Hold Ctrl over an item to see the code behind it; Ctrl+C copies it.
    Item* hi = highlighted();
    if (hi && (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier))
    {
        QString code = QString::fromStdString(actions::pythonFor(hi->action));
        if (code.isEmpty())
            return;
        code += mCopied ? "\n\n# copied" : "\n\n# Ctrl+C to copy";
        p.setFont(monoFont());
        QFontMetricsF fm(p.font());
        QRectF codeRect = fm.boundingRect(QRectF(0, 0, 640, 2000), Qt::AlignLeft | Qt::TextWordWrap, code);
        QRectF box(hi->rect.left(), hi->rect.bottom() + 8, codeRect.width() + 20, codeRect.height() + 14);
        if (box.right() > width())
            box.moveRight(width() - 8);
        if (box.bottom() > height())
            box.moveBottom(hi->rect.top() - 8);
        p.setPen(QPen(themeShade(110), 1));
        p.setBrush(themeShade(22, 245));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(themeShade(225));
        p.drawText(box.adjusted(10, 7, -10, -7), Qt::AlignLeft | Qt::TextWordWrap, code);
    }
}
} // namespace lila::ui
