#include "ui/ViewportHeaderConfig.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/BarPresets.h"
#include "ui/Controls.h"
#include "ui/FileDialog.h"
#include "ui/IconBars.h"
#include "ui/Icons.h"
#include "ui/Layouts.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace lila::ui::viewportHeader
{
using json = nlohmann::json;

namespace
{
constexpr const char* kMime = "application/x-lila-header-entry";
constexpr int kEntryRole = Qt::UserRole + 1;
constexpr int kKindRole = Qt::UserRole + 2;

/** What a header tree row is: one of the two sides, a menu holding commands, or a plain entry. */
enum class RowKind
{
    Side,
    Menu,
    Entry,
};

bool isSeparator(const json& e)
{
    return e.is_string() && e.get<std::string>() == kSeparator;
}

bool isMenu(const json& e)
{
    return e.is_object() && e.contains("menu");
}

/** Built-ins and separators belong on the row only; a menu holds commands and separators. */
bool fitsInMenu(const json& e)
{
    return isSeparator(e) || (e.is_object() && e.contains("action"));
}
} // namespace

const std::vector<Builtin>& builtins()
{
    static const std::vector<Builtin> all = {
        {"componentMode", "Component Mode", "cube", "Object, Vertex, Edge and Face as one strip."},
        {"shading", "Shading", "sphere", "Wireframe, Shaded and Textured as one strip."},
        {"lighting", "Lighting", "sun", "Lighting mode, environment, light rotation, shadows and exposure."},
        {"symmetry", "Symmetry", "brackets-contain", "Toggles symmetry; its arrow picks the plane."},
        {"softSelect", "Soft Select", "ripple", "Toggles soft select; its arrow sets radius and falloff."},
        {"selectionMasks", "Selection Masks", "filter", "What clicks can select: meshes, curves, joints, lights, locators, other."},
        {"snap", "Snap", "magnet", "Move, rotate and scale increments and what to snap to."},
        {"overlays", "Overlays", "eye", "Every display toggle for this view."},
        {"renderer", "Renderer", "aperture", "Which renderer draws this view, and its settings."},
        {"camera", "Camera", "camera", "The view's camera: persp, the ortho views, scene cameras, camera settings."},
        {"jointSize", "Joint Size", "bone", "How large joints draw in every view."},
    };
    return all;
}

const Builtin* builtin(const std::string& id)
{
    for (const Builtin& b : builtins())
        if (id == b.id)
            return &b;
    return nullptr;
}

json defaultConfig()
{
    return {
        {"left",
         {"componentMode", "shading", "lighting", kSeparator, "symmetry", "softSelect", kSeparator, "selectionMasks",
          {{"action", "viewport.toggleHighlightSelection"}}, {{"action", "viewport.togglePreselectObjects"}}, kSeparator,
          {{"menu", "Display"}, {"icon", "adjustments-horizontal"}, {"items",
            {"viewport.wireOnShaded", "viewport.defaultMaterial", "viewport.greyBaseColor", kSeparator,
             "viewport.togglePreselectComponents", kSeparator, "jointSize"}}}}},
        {"right",
         {"snap", "overlays", "camera", kSeparator, {{"action", "viewport.isolate"}}, {{"action", "viewport.toggleImagePlanes"}}, kSeparator,
          {{"action", "view.drone"}}, {{"action", "view.presenter"}}, {{"action", "viewport.frameSelected"}},
          {{"action", "viewport.frameAll"}}, {{"action", "viewport.toggleQuad"}}, {{"action", "window.toggleMaximizePanel"}}}},
    };
}

namespace
{
constexpr const char* kSides[] = {"left", "right"};

/** Identifies an entry across layouts; empty for separators. A menu's commands are keyed under the menu's title. */
std::string entryKey(const json& e, const std::string& menu = {})
{
    std::string k;
    if (isSeparator(e))
        return k;
    if (e.is_string())
        k = e.get<std::string>();
    else if (isMenu(e))
        k = "menu:" + e.value("menu", std::string());
    else if (e.is_object() && e.contains("action"))
        k = "action:" + e.value("action", std::string());
    else if (e.is_object() && e.contains("item"))
        k = "item:" + e.value("item", std::string());
    return menu.empty() ? k : "menu:" + menu + "/" + k;
}

void collectKeys(const json& c, std::vector<std::string>& out)
{
    for (const char* side : kSides)
        for (const json& e : c.value(side, json::array()))
        {
            if (std::string k = entryKey(e); !k.empty())
                out.push_back(k);
            if (isMenu(e))
                for (const json& item : e.value("items", json::array()))
                    if (std::string k = entryKey(item, e.value("menu", std::string())); !k.empty())
                        out.push_back(k);
        }
}

/**
 * Inserts the default entries a layout has never seen into list, each after the entry that precedes it in the defaults
 * (with the separator before it, if the defaults have one), or at the end.
 */
void mergeNew(json& list, const json& defaults, const std::string& menu, const std::vector<std::string>& known,
              const std::vector<std::string>& present)
{
    auto has = [](const std::vector<std::string>& keys, const std::string& k) { return std::find(keys.begin(), keys.end(), k) != keys.end(); };
    auto find = [&](const std::string& k) {
        for (size_t i = 0; i < list.size(); ++i)
            if (entryKey(list[i], menu) == k)
                return int(i);
        return -1;
    };
    for (size_t i = 0; i < defaults.size(); ++i)
    {
        const std::string k = entryKey(defaults[i], menu);
        if (k.empty() || has(known, k) || has(present, k))
            continue;
        int at = -1;
        for (size_t j = i; j-- > 0 && at < 0;)
            if (const std::string prev = entryKey(defaults[j], menu); !prev.empty())
                if (int found = find(prev); found >= 0)
                    at = found + 1;
        // With nothing before it in the layout, it goes ahead of whatever follows it in the defaults.
        for (size_t j = i + 1; j < defaults.size() && at < 0; ++j)
            if (const std::string next = entryKey(defaults[j], menu); !next.empty())
                at = find(next);
        if (at < 0)
            at = int(list.size());
        if (i > 0 && isSeparator(defaults[i - 1]))
            list.insert(list.begin() + at++, json(kSeparator));
        list.insert(list.begin() + at, defaults[i]);
    }
}
} // namespace

json config()
{
    json c = PrefStore::get().value("general", kPref, json());
    if (!c.is_object() || !c.contains("left") || !c.contains("right") || !c["left"].is_array() || !c["right"].is_array())
        return defaultConfig();
    std::vector<std::string> present;
    collectKeys(c, present);
    std::vector<std::string> known = present;
    for (const json& k : c.value("known", json::array()))
        if (k.is_string())
            known.push_back(k.get<std::string>());
    // Entries added to the defaults after the layout was saved show up in it; ones the user removed stay known and stay out.
    const json defaults = defaultConfig();
    for (const char* side : kSides)
    {
        mergeNew(c[side], defaults[side], {}, known, present);
        for (const json& d : defaults[side])
            if (isMenu(d))
                for (json& e : c[side])
                    if (isMenu(e) && e.value("menu", std::string()) == d.value("menu", std::string()))
                        mergeNew(e["items"], d.value("items", json::array()), d.value("menu", std::string()), known, present);
    }
    return c;
}

void setConfig(const json& c)
{
    json stored = c;
    if (!stored.contains("known"))
    {
        std::vector<std::string> known;
        collectKeys(defaultConfig(), known);
        stored["known"] = known;
    }
    PrefStore::get().set("general", kPref, stored);
}

std::string entryLabel(const json& e)
{
    if (isSeparator(e))
        return "Separator";
    if (e.is_string())
    {
        const Builtin* b = builtin(e.get<std::string>());
        return b ? b->label : e.get<std::string>();
    }
    if (isMenu(e))
        return e.value("menu", std::string("Menu"));
    if (e.is_object() && e.contains("item"))
    {
        const iconBars::Item* i = iconBars::registry().find(e.value("item", std::string()));
        if (!i)
            return e.value("item", std::string());
        return !i->label.empty() ? i->label : actions::find(i->action) ? actions::displayLabel(i->action) : i->id;
    }
    if (e.is_object() && e.contains("action"))
    {
        const std::string id = e.value("action", std::string());
        return actions::find(id) ? actions::displayLabel(id) : id;
    }
    return "?";
}

std::string entryIcon(const json& e)
{
    if (isSeparator(e))
        return "separator-vertical";
    if (e.is_string())
    {
        const Builtin* b = builtin(e.get<std::string>());
        return b ? b->icon : "square";
    }
    if (isMenu(e))
        return e.value("icon", std::string("menu-2"));
    if (e.is_object() && e.contains("item"))
    {
        const iconBars::Item* i = iconBars::registry().find(e.value("item", std::string()));
        return i && !i->icon.empty() ? i->icon : "square";
    }
    if (e.is_object() && e.contains("action"))
    {
        std::string ic = actions::iconFor(e.value("action", std::string()));
        return ic.empty() ? "point" : ic;
    }
    return "square";
}

namespace
{
QTreeWidgetItem* makeRow(const json& e)
{
    auto* row = new QTreeWidgetItem;
    row->setText(0, QString::fromStdString(entryLabel(e)));
    row->setIcon(0, icon(QString::fromStdString(entryIcon(e))));
    json stored = e;
    if (isMenu(e))
    {
        stored.erase("items");
        row->setData(0, kKindRole, int(RowKind::Menu));
        QFont f = row->font(0);
        f.setWeight(QFont::DemiBold);
        row->setFont(0, f);
        row->setFlags(row->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled);
        for (const json& item : e.value("items", json::array()))
            row->addChild(makeRow(item.is_string() && !isSeparator(item) && !builtin(item.get<std::string>()) ? json{{"action", item}} : item));
    }
    else
    {
        row->setData(0, kKindRole, int(RowKind::Entry));
        row->setFlags((row->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    }
    if (isSeparator(e))
        row->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
    if (e.is_object() && e.contains("action"))
        row->setToolTip(0, QString::fromStdString(actions::tooltip(e.value("action", std::string()))));
    else if (e.is_string())
        if (const Builtin* b = builtin(e.get<std::string>()))
            row->setToolTip(0, b->doc);
    row->setData(0, kEntryRole, QString::fromStdString(stored.dump()));
    return row;
}

json rowEntry(const QTreeWidgetItem* row)
{
    json e = json::parse(row->data(0, kEntryRole).toString().toStdString(), nullptr, false);
    if (row->data(0, kKindRole).toInt() == int(RowKind::Menu))
    {
        json items = json::array();
        for (int i = 0; i < row->childCount(); ++i)
        {
            json c = rowEntry(row->child(i));
            items.push_back(c.is_object() && c.contains("action") ? c["action"] : c);
        }
        e["items"] = std::move(items);
    }
    return e;
}

/** The bar the editor is working on: the viewport header (two sides) or an icon bar (one list). */
struct Target
{
    std::string bar;
    std::vector<std::pair<std::string, QString>> sides; // config key, title

    bool header() const { return bar == barPresets::kHeader; }

    json load() const { return header() ? config() : json{{"entries", iconBars::arrangement(bar)}}; }
    json defaults() const { return header() ? defaultConfig() : json{{"entries", iconBars::defaultArrangement(bar)}}; }

    void store(const json& c) const
    {
        if (header())
            setConfig(c);
        else
            iconBars::setArrangement(bar, c.value("entries", json::array()));
    }

    void reset() const
    {
        if (header())
            PrefStore::get().remove("general", kPref);
        else
            iconBars::resetArrangement(bar);
    }

    /** The bar's own items, for the top of the command list. */
    std::vector<json> items() const
    {
        std::vector<json> out;
        if (header())
            for (const Builtin& b : builtins())
                out.push_back(json(b.id));
        else
            for (const iconBars::Item* i : iconBars::itemsIn(bar))
                if (i->kind != iconBars::Kind::Separator)
                    out.push_back(json{{"item", i->id}});
        return out;
    }
};

Target targetFor(const std::string& bar)
{
    if (bar == barPresets::kHeader)
        return {bar, {{"left", "Left"}, {"right", "Right"}}};
    return {bar, {{"entries", QString::fromStdString(barPresets::barLabel(bar))}}};
}

/** Where a drop lands relative to the row under the pointer. */
enum class DropAt
{
    None,
    Above,
    Below,
    Onto,
};

/** A bar's contents: its sides, each a list of entries, menus holding commands. Accepts entries dragged in. */
class BarTree : public QTreeWidget
{
public:
    std::function<void()> changed;

    explicit BarTree(QWidget* parent) : QTreeWidget(parent)
    {
        setHeaderHidden(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setSelectionMode(QAbstractItemView::SingleSelection);
        // The line is drawn here: the style's own indicator is too faint to follow.
        setDropIndicatorShown(false);
        setIndentation(16);
        setUniformRowHeights(true);
    }

    void load(const Target& target, const json& c)
    {
        mSides = target.sides;
        clear();
        for (const auto& [key, title] : mSides)
        {
            auto* root = new QTreeWidgetItem(this);
            root->setText(0, title);
            root->setData(0, kKindRole, int(RowKind::Side));
            root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
            QFont f = root->font(0);
            f.setWeight(QFont::DemiBold);
            root->setFont(0, f);
            root->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
            for (const json& e : c.value(key, json::array()))
                root->addChild(makeRow(e));
        }
        expandAll();
    }

    json save() const
    {
        json c = json::object();
        for (int s = 0; s < topLevelItemCount() && s < int(mSides.size()); ++s)
        {
            json list = json::array();
            QTreeWidgetItem* root = topLevelItem(s);
            for (int i = 0; i < root->childCount(); ++i)
                list.push_back(rowEntry(root->child(i)));
            c[mSides[size_t(s)].first] = std::move(list);
        }
        return c;
    }

    /** Where a new entry goes: into the selected menu or side, else after the selected row, else the end of the first side. */
    void insertEntry(const json& e)
    {
        QTreeWidgetItem* at = currentItem();
        QTreeWidgetItem* parent = topLevelItem(0);
        int index = parent ? parent->childCount() : 0;
        if (at)
        {
            const auto kind = RowKind(at->data(0, kKindRole).toInt());
            if (kind == RowKind::Side || (kind == RowKind::Menu && fitsInMenu(e)))
            {
                parent = at;
                index = at->childCount();
            }
            else if (at->parent())
            {
                parent = at->parent();
                index = parent->indexOfChild(at) + 1;
            }
        }
        if (!parent)
            return;
        place(e, parent, index, false);
    }

    void removeCurrent()
    {
        QTreeWidgetItem* at = currentItem();
        if (!at || !at->parent())
            return;
        delete at;
        if (changed)
            changed();
    }

protected:
    QStringList mimeTypes() const override { return {kMime}; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        auto* mime = new QMimeData;
        mime->setData(kMime, QByteArray::fromStdString(rowEntry(items.front()).dump()));
        return mime;
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMime))
            e->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        QTreeWidget::dragMoveEvent(e);
        if (!e->mimeData()->hasFormat(kMime))
            return;
        e->acceptProposedAction();
        trackDrop(e->position().toPoint());
    }

    void dragLeaveEvent(QDragLeaveEvent* e) override
    {
        clearDrop();
        QTreeWidget::dragLeaveEvent(e);
    }

    void dropEvent(QDropEvent* e) override
    {
        trackDrop(e->position().toPoint());
        const DropAt at = mDropAt;
        QTreeWidgetItem* target = mDropRow;
        clearDrop();
        if (!e->mimeData()->hasFormat(kMime))
            return;
        const json entry = json::parse(e->mimeData()->data(kMime).toStdString(), nullptr, false);
        if (entry.is_discarded())
            return;
        QTreeWidgetItem* parent = nullptr;
        int index = 0;
        if (!target)
        {
            parent = topLevelItem(topLevelItemCount() - 1);
            index = parent ? parent->childCount() : 0;
        }
        else if (RowKind kind = RowKind(target->data(0, kKindRole).toInt());
                 kind == RowKind::Side || (kind == RowKind::Menu && at == DropAt::Onto && fitsInMenu(entry)))
        {
            parent = target;
            index = at == DropAt::Onto ? target->childCount() : 0;
            if (kind == RowKind::Side && at == DropAt::Above && indexOfTopLevelItem(target) > 0)
            {
                parent = topLevelItem(indexOfTopLevelItem(target) - 1);
                index = parent->childCount();
            }
        }
        else
        {
            parent = target->parent();
            index = parent->indexOfChild(target) + (at == DropAt::Above ? 0 : 1);
        }
        if (!parent)
            return;
        if (!place(entry, parent, index, e->source() == this))
            return;
        e->setDropAction(Qt::CopyAction);
        e->accept();
    }

    void paintEvent(QPaintEvent* e) override
    {
        QTreeWidget::paintEvent(e);
        if (mDropAt == DropAt::None || !mDropRow)
            return;
        const QRect r = visualItemRect(mDropRow);
        if (!r.isValid())
            return;
        QPainter p(viewport());
        const QColor line = themeColor("accent");
        if (mDropAt == DropAt::Onto)
        {
            p.setPen(QPen(line, 2));
            p.drawRect(r.adjusted(1, 1, -2, -2));
            return;
        }
        const int y = mDropAt == DropAt::Above ? r.top() : r.bottom();
        p.fillRect(QRect(r.left(), y - 1, viewport()->width() - r.left() - 4, 2), line);
        p.setBrush(line);
        p.setPen(Qt::NoPen);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawEllipse(QPointF(r.left() + 2, y), 3, 3);
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
        {
            removeCurrent();
            return;
        }
        QTreeWidget::keyPressEvent(e);
    }

private:
    /** Menus and sides take a drop onto their middle; elsewhere the drop goes between rows. */
    void trackDrop(const QPoint& at)
    {
        QTreeWidgetItem* row = itemAt(at);
        DropAt where = DropAt::None;
        if (row)
        {
            const QRect r = visualItemRect(row);
            const int y = at.y() - r.top();
            const auto kind = RowKind(row->data(0, kKindRole).toInt());
            if (kind != RowKind::Entry && y > r.height() / 4 && y < r.height() * 3 / 4)
                where = DropAt::Onto;
            else
                where = y < r.height() / 2 ? DropAt::Above : DropAt::Below;
            // Above the first side has nowhere to go but into it.
            if (kind == RowKind::Side && where == DropAt::Above && indexOfTopLevelItem(row) == 0)
                where = DropAt::Onto;
        }
        else if (topLevelItemCount() > 0)
        {
            // Past the last row: the end of the last side.
            QTreeWidgetItem* last = topLevelItem(topLevelItemCount() - 1);
            row = last->childCount() > 0 ? last->child(last->childCount() - 1) : last;
            where = row == last ? DropAt::Onto : DropAt::Below;
        }
        if (row != mDropRow || where != mDropAt)
        {
            mDropRow = row;
            mDropAt = where;
            viewport()->update();
        }
    }

    void clearDrop()
    {
        mDropRow = nullptr;
        mDropAt = DropAt::None;
        viewport()->update();
    }

    /** Inserts the entry, moving the current row when the drag came from this tree. False when the move is onto itself. */
    bool place(const json& entry, QTreeWidgetItem* parent, int index, bool move)
    {
        // A menu takes commands and separators only; anything else lands beside it.
        if (RowKind(parent->data(0, kKindRole).toInt()) == RowKind::Menu && !fitsInMenu(entry))
        {
            index = parent->parent()->indexOfChild(parent) + 1;
            parent = parent->parent();
        }
        if (move)
            if (QTreeWidgetItem* moved = currentItem(); moved && moved->parent())
            {
                QTreeWidgetItem* from = moved->parent();
                if (moved == parent || parent->parent() == moved)
                    return false;
                const int at = from->indexOfChild(moved);
                if (from == parent && at < index)
                    --index;
                from->takeChild(at);
            }
        QTreeWidgetItem* row = makeRow(entry);
        parent->insertChild(std::clamp(index, 0, parent->childCount()), row);
        parent->setExpanded(true);
        row->setExpanded(true);
        setCurrentItem(row);
        if (changed)
            changed();
        return true;
    }

    std::vector<std::pair<std::string, QString>> mSides;
    QTreeWidgetItem* mDropRow = nullptr;
    DropAt mDropAt = DropAt::None;
};

/** Everything that can go on the bar: its own items, a separator, a menu, and every registered command by category. */
class SourceTree : public QTreeWidget
{
public:
    explicit SourceTree(QWidget* parent) : QTreeWidget(parent)
    {
        setHeaderHidden(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setUniformRowHeights(true);
        setIndentation(16);
    }

    void fill(const Target& target, const QString& filter)
    {
        clear();
        auto group = [this](const QString& title) {
            auto* g = new QTreeWidgetItem(this);
            g->setText(0, title);
            g->setFlags(Qt::ItemIsEnabled);
            QFont f = g->font(0);
            f.setWeight(QFont::DemiBold);
            g->setFont(0, f);
            return g;
        };
        auto matches = [&](const QString& text) { return filter.isEmpty() || text.contains(filter, Qt::CaseInsensitive); };
        QTreeWidgetItem* items = group(QString::fromStdString(barPresets::barLabel(target.bar)) + " Items");
        for (const json& e : target.items())
            if (matches(QString::fromStdString(entryLabel(e))))
                items->addChild(makeRow(e));
        if (matches("Separator"))
            items->addChild(makeRow(json(kSeparator)));
        if (matches("Menu"))
            items->addChild(makeRow(json{{"menu", "Menu"}, {"icon", "menu-2"}, {"items", json::array()}}));
        std::map<std::string, std::vector<const Action*>> byCategory;
        for (const Action* a : actions::all())
            if (matches(QString::fromStdString(a->label)) || matches(QString::fromStdString(a->category)))
                byCategory[a->category.empty() ? "Other" : a->category].push_back(a);
        for (auto& [cat, list] : byCategory)
        {
            std::sort(list.begin(), list.end(), [](const Action* x, const Action* y) { return x->label < y->label; });
            QTreeWidgetItem* g = group(QString::fromStdString(cat));
            for (const Action* a : list)
                g->addChild(makeRow(json{{"action", a->id}}));
        }
        for (int i = topLevelItemCount() - 1; i >= 0; --i)
            if (topLevelItem(i)->childCount() == 0)
                delete takeTopLevelItem(i);
        if (!filter.isEmpty())
            expandAll();
        else if (topLevelItemCount() > 0)
            topLevelItem(0)->setExpanded(true);
    }

protected:
    QStringList mimeTypes() const override { return {kMime}; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& rows) const override
    {
        if (rows.isEmpty() || !rows.front()->parent())
            return nullptr;
        auto* mime = new QMimeData;
        mime->setData(kMime, QByteArray::fromStdString(rowEntry(rows.front()).dump()));
        return mime;
    }
};

QPushButton* pushButton(const QString& text, QWidget* parent)
{
    auto* b = new QPushButton(text, parent);
    b->setAutoDefault(false);
    return b;
}

class BarEditor : public QWidget
{
public:
    explicit BarEditor(QWidget* parent) : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Customize Bars");
        setAttribute(Qt::WA_DeleteOnClose);
        resize(680, 600);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        auto* presetRow = new QHBoxLayout;
        presetRow->setSpacing(4);
        auto* presetLabel = new QLabel("Preset", this);
        presetRow->addWidget(presetLabel);
        mPresets = new QComboBox(this);
        mPresets->setToolTip("A saved set of bar arrangements. Edits go into the current preset; Lila Default needs Save As to keep them.");
        mPresets->setMinimumWidth(160);
        connect(mPresets, &QComboBox::activated, this, [this](int i) {
            barPresets::activate(mPresets->itemData(i).toString().toStdString());
            loadBar();
            refreshPresets();
        });
        presetRow->addWidget(mPresets, 1);
        QPushButton* saveAs = pushButton("Save As...", this);
        connect(saveAs, &QPushButton::clicked, this, [this] { saveAsPreset(); });
        mDelete = pushButton("Delete", this);
        connect(mDelete, &QPushButton::clicked, this, [this] {
            const std::string name = barPresets::current();
            if (name.empty())
                return;
            barPresets::remove(name);
            loadBar();
            refreshPresets();
        });
        QPushButton* importButton = pushButton("Import...", this);
        connect(importButton, &QPushButton::clicked, this, [this] { importPreset(); });
        mExport = pushButton("Export...", this);
        connect(mExport, &QPushButton::clicked, this, [this] { exportPreset(); });
        for (QPushButton* b : {saveAs, mDelete, importButton, mExport})
            presetRow->addWidget(b);
        root->addLayout(presetRow);

        mBind = new QCheckBox(this);
        mBind->setToolTip("Applies this preset whenever the layout is applied");
        connect(mBind, &QCheckBox::toggled, this, [this](bool on) {
            const std::string preset = barPresets::current();
            barPresets::bind(layouts::current(), on ? (preset.empty() ? std::string(barPresets::kDefaultBinding) : preset) : std::string());
        });
        root->addWidget(mBind);

        mBars = new controls::Segmented(this);
        for (size_t i = 0; i < barPresets::bars().size(); ++i)
            mBars->add(QString::fromStdString(barPresets::barLabel(barPresets::bars()[i])), {}, {}, int(i));
        connect(mBars, &controls::Segmented::picked, this, [this](int i) { setBar(barPresets::bars()[size_t(i)]); });
        auto* barRow = new QHBoxLayout;
        barRow->addWidget(mBars);
        barRow->addStretch(1);
        root->addSpacing(4);
        root->addLayout(barRow);

        auto* columns = new QHBoxLayout;
        columns->setSpacing(12);
        auto* left = new QVBoxLayout;
        mTree = new BarTree(this);
        mTree->changed = [this] { apply(); };
        mTree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(mTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) { rowMenu(mTree->itemAt(at), mTree->viewport()->mapToGlobal(at)); });
        connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
            if (row && RowKind(row->data(0, kKindRole).toInt()) == RowKind::Menu)
                rename(row);
        });
        left->addWidget(mTree, 1);
        auto* treeButtons = new QHBoxLayout;
        treeButtons->setSpacing(4);
        QPushButton* newMenu = pushButton("New Menu", this);
        connect(newMenu, &QPushButton::clicked, this, [this] {
            const QString title = QInputDialog::getText(this, "New Menu", "Name", QLineEdit::Normal, "Display");
            if (!title.trimmed().isEmpty())
                mTree->insertEntry(json{{"menu", title.trimmed().toStdString()}, {"icon", "menu-2"}, {"items", json::array()}});
        });
        QPushButton* remove = pushButton("Remove", this);
        connect(remove, &QPushButton::clicked, this, [this] { mTree->removeCurrent(); });
        QPushButton* resetBar = pushButton("Reset Bar", this);
        resetBar->setToolTip("Puts this bar back to Lila's default items");
        connect(resetBar, &QPushButton::clicked, this, [this] {
            mTarget.reset();
            barPresets::edited();
            loadBar();
        });
        treeButtons->addWidget(newMenu);
        treeButtons->addWidget(remove);
        treeButtons->addStretch(1);
        treeButtons->addWidget(resetBar);
        left->addLayout(treeButtons);
        columns->addLayout(left, 1);

        auto* right = new QVBoxLayout;
        auto* search = new QLineEdit(this);
        search->setPlaceholderText("Search commands");
        search->addAction(icon("search"), QLineEdit::LeadingPosition);
        search->setClearButtonEnabled(true);
        right->addWidget(search);
        mSource = new SourceTree(this);
        connect(search, &QLineEdit::textChanged, this, [this](const QString& t) { mSource->fill(mTarget, t.trimmed()); });
        mSearch = search;
        connect(mSource, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
            if (row && row->parent())
                mTree->insertEntry(rowEntry(row));
        });
        right->addWidget(mSource, 1);
        QPushButton* add = pushButton("Add", this);
        add->setToolTip("Adds the selected command to the bar; dragging it there works too");
        connect(add, &QPushButton::clicked, this, [this] {
            if (QTreeWidgetItem* row = mSource->currentItem(); row && row->parent())
                mTree->insertEntry(rowEntry(row));
        });
        auto* addRow = new QHBoxLayout;
        addRow->addStretch(1);
        addRow->addWidget(add);
        right->addLayout(addRow);
        columns->addLayout(right, 1);
        root->addLayout(columns, 1);

        auto* footer = new QHBoxLayout;
        footer->addStretch(1);
        auto* done = new QPushButton("Done", this);
        done->setDefault(true);
        connect(done, &QPushButton::clicked, this, &QWidget::close);
        footer->addWidget(done);
        root->addLayout(footer);

        refreshPresets();
        setBar(barPresets::kHeader);
    }

    void setBar(const std::string& bar)
    {
        mTarget = targetFor(bar);
        const auto& all = barPresets::bars();
        mBars->setCurrent(int(std::find(all.begin(), all.end(), bar) - all.begin()));
        loadBar();
    }

private:
    void loadBar()
    {
        mTree->load(mTarget, mTarget.load());
        mSource->fill(mTarget, mSearch->text().trimmed());
    }

    void apply()
    {
        mTarget.store(mTree->save());
        barPresets::edited();
    }

    void refreshPresets()
    {
        QSignalBlocker block(mPresets);
        mPresets->clear();
        mPresets->addItem("Lila Default", QString());
        for (const std::string& name : barPresets::names())
            mPresets->addItem(QString::fromStdString(name), QString::fromStdString(name));
        const std::string cur = barPresets::current();
        mPresets->setCurrentIndex(std::max(0, mPresets->findData(QString::fromStdString(cur))));
        mDelete->setEnabled(!cur.empty());
        mExport->setEnabled(!cur.empty());
        const std::string layout = layouts::current();
        const std::string bound = barPresets::boundTo(layout);
        QSignalBlocker blockBind(mBind);
        mBind->setText(QString("Apply with the %1 layout").arg(QString::fromStdString(layout)));
        mBind->setEnabled(!layout.empty());
        mBind->setChecked(!bound.empty() && (bound == cur || (cur.empty() && bound == barPresets::kDefaultBinding)));
    }

    void saveAsPreset()
    {
        const std::string cur = barPresets::current();
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save Bar Preset", "Name", QLineEdit::Normal,
                                                   cur.empty() ? QString("My Bars") : QString::fromStdString(cur), &ok)
                                 .trimmed();
        if (!ok || name.isEmpty())
            return;
        barPresets::save(name.toStdString());
        refreshPresets();
    }

    void exportPreset()
    {
        const std::string cur = barPresets::current();
        if (cur.empty())
            return;
        const QString filter = QString("Bar presets (*%1)").arg(barPresets::kFileSuffix);
        QString path = fileDialog::getSaveFileName(this, "Export Bar Preset", QString::fromStdString(cur) + barPresets::kFileSuffix, filter);
        if (path.isEmpty())
            return;
        if (!path.endsWith(barPresets::kFileSuffix))
            path += barPresets::kFileSuffix;
        std::string error;
        if (!barPresets::exportTo(cur, paths::fromUtf8(path.toStdString()), error))
            log::error("Export Bar Preset: {}", error);
        else
            log::info("Exported bar preset \"{}\" to {}", cur, path.toStdString());
    }

    void importPreset()
    {
        const QString filter = QString("Bar presets (*%1);;All files (*)").arg(barPresets::kFileSuffix);
        const QString path = fileDialog::getOpenFileName(this, "Import Bar Preset", {}, filter);
        if (path.isEmpty())
            return;
        std::string error;
        const std::string name = barPresets::importFrom(paths::fromUtf8(path.toStdString()), error);
        if (name.empty())
        {
            log::error("Import Bar Preset: {}", error);
            return;
        }
        barPresets::activate(name);
        refreshPresets();
        loadBar();
    }

    void rename(QTreeWidgetItem* row)
    {
        json e = json::parse(row->data(0, kEntryRole).toString().toStdString(), nullptr, false);
        bool ok = false;
        const QString title = QInputDialog::getText(this, "Rename Menu", "Name", QLineEdit::Normal, row->text(0), &ok);
        if (!ok || title.trimmed().isEmpty())
            return;
        e["menu"] = title.trimmed().toStdString();
        row->setData(0, kEntryRole, QString::fromStdString(e.dump()));
        row->setText(0, title.trimmed());
        apply();
    }

    void rowMenu(QTreeWidgetItem* row, const QPoint& global)
    {
        if (!row || !row->parent())
            return;
        QMenu menu;
        if (RowKind(row->data(0, kKindRole).toInt()) == RowKind::Menu)
            menu.addAction("Rename...", this, [this, row] { rename(row); });
        menu.addAction(icon("x"), "Remove", this, [this] { mTree->removeCurrent(); });
        mTree->setCurrentItem(row);
        menu.exec(global);
    }

    Target mTarget = targetFor(barPresets::kHeader);
    QComboBox* mPresets = nullptr;
    QPushButton* mDelete = nullptr;
    QPushButton* mExport = nullptr;
    QCheckBox* mBind = nullptr;
    controls::Segmented* mBars = nullptr;
    BarTree* mTree = nullptr;
    SourceTree* mSource = nullptr;
    QLineEdit* mSearch = nullptr;
};

QPointer<BarEditor> gEditor;
} // namespace

void openEditor(QWidget* anchor, const std::string& bar)
{
    if (!gEditor)
    {
        QWidget* owner = anchor ? anchor->window() : QApplication::activeWindow();
        gEditor = new BarEditor(owner);
        if (owner)
            gEditor->move(owner->geometry().center() - QPoint(gEditor->width() / 2, gEditor->height() / 2));
    }
    gEditor->setBar(bar.empty() ? std::string(barPresets::kHeader) : bar);
    gEditor->show();
    gEditor->raise();
    gEditor->activateWindow();
}

void registerActions()
{
    barPresets::install();
    iconBars::setCustomizeHook([](const std::string& bar, QWidget* anchor) { openEditor(anchor, bar); });
    actions::add({.id = "viewport.customizeHeader",
                  .label = "Customize Viewport Header...",
                  .category = "View",
                  .run = [] { openEditor(); },
                  .enabled = {},
                  .doc = "Choose what the row at the top of each viewport shows: add any command, make your own menus of them, reorder "
                         "or remove items. Also on the header's right-click menu.",
                  .icon = "layout-navbar"});
    actions::add({.id = "window.customizeToolBox",
                  .label = "Customize Tool Box...",
                  .category = "Windows",
                  .run = [] { openEditor(nullptr, iconBars::kToolBox); },
                  .enabled = {},
                  .doc = "Choose what the tool box at the window's left edge shows. Also on the tool box's right-click menu.",
                  .icon = "layout-sidebar"});
    actions::add({.id = "viewport.resetHeader",
                  .label = "Reset Viewport Header",
                  .category = "View",
                  .run = [] {
                      PrefStore::get().remove("general", kPref);
                      barPresets::edited();
                  },
                  .enabled = {},
                  .doc = "Puts the viewport header back to Lila's default items.",
                  .icon = "restore"});
}
} // namespace lila::ui::viewportHeader
