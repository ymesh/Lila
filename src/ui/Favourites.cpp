#include "ui/Favourites.h"

#include "core/PrefStore.h"
#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>

#include <algorithm>
#include <cmath>
#include <numbers>

#include <nlohmann/json.hpp>

namespace lila::ui::favourites
{
namespace
{
using json = nlohmann::json;

std::string key(const std::string& list) { return "/favourites/" + list; }

constexpr int kStar = 14;

using ListOf = std::function<std::pair<std::string, std::string>(const QModelIndex&)>;

/** Where an item's star sits in its visual rect. */
QRect starRect(const QAbstractItemView* view, const QRect& item)
{
    const auto* list = qobject_cast<const QListView*>(view);
    if (list && list->viewMode() == QListView::IconMode)
        return {item.right() - kStar - 3, item.top() + 3, kStar, kStar};
    return {item.right() - kStar - 6, item.center().y() - kStar / 2, kStar, kStar};
}

QPainterPath starPath(const QRectF& r)
{
    QPainterPath p;
    const QPointF c = r.center();
    const double outer = r.width() / 2.0, inner = outer * 0.45;
    for (int i = 0; i < 10; ++i)
    {
        const double a = -std::numbers::pi / 2.0 + i * std::numbers::pi / 5.0;
        const double rad = i % 2 ? inner : outer;
        const QPointF pt(c.x() + rad * std::cos(a), c.y() + rad * std::sin(a));
        i ? p.lineTo(pt) : p.moveTo(pt);
    }
    p.closeSubpath();
    return p;
}

class StarDelegate final : public QStyledItemDelegate
{
public:
    StarDelegate(QAbstractItemView* view, ListOf listOf) : QStyledItemDelegate(view), mView(view), mListOf(std::move(listOf)) {}

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(p, option, index);
        const auto [list, id] = mListOf(index);
        if (list.empty())
            return;
        const bool on = has(list, id);
        if (!on && !(option.state & QStyle::State_MouseOver))
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QPainterPath star = starPath(QRectF(starRect(mView, option.rect)).adjusted(1, 1, -1, -1));
        if (on)
            p->fillPath(star, themeColor("text"));
        else
        {
            p->setPen(QPen(themeColor("dimText"), 1.2));
            p->drawPath(star);
        }
        p->restore();
    }

private:
    QAbstractItemView* mView;
    ListOf mListOf;
};

class StarClicks final : public QObject
{
public:
    StarClicks(QAbstractItemView* view, ListOf listOf, std::function<void()> changed)
        : QObject(view), mView(view), mListOf(std::move(listOf)), mChanged(std::move(changed))
    {
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseButtonRelease && e->type() != QEvent::MouseButtonDblClick)
            return false;
        auto* m = static_cast<QMouseEvent*>(e);
        if (m->button() != Qt::LeftButton)
            return false;
        const QModelIndex index = mView->indexAt(m->position().toPoint());
        if (!index.isValid() || !starRect(mView, mView->visualRect(index)).contains(m->position().toPoint()))
            return false;
        const auto [list, id] = mListOf(index);
        if (list.empty())
            return false;
        // The press toggles; the rest of the click is swallowed so the item isn't picked too.
        if (e->type() == QEvent::MouseButtonPress)
        {
            set(list, id, !has(list, id));
            mView->viewport()->update();
            if (mChanged)
                mChanged();
        }
        return true;
    }

private:
    QAbstractItemView* mView;
    ListOf mListOf;
    std::function<void()> mChanged;
};
} // namespace

std::vector<std::string> all(const std::string& list)
{
    const json j = PrefStore::get().value<json>("general", key(list), json::array());
    std::vector<std::string> out;
    if (j.is_array())
        for (const json& v : j)
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

bool has(const std::string& list, const std::string& id)
{
    const std::vector<std::string> ids = all(list);
    return std::ranges::find(ids, id) != ids.end();
}

void set(const std::string& list, const std::string& id, bool on)
{
    std::vector<std::string> ids = all(list);
    std::erase(ids, id);
    if (on)
        ids.push_back(id);
    PrefStore::get().set("general", key(list), json(ids));
}

void attach(QAbstractItemView* view, ListOf listOf, std::function<void()> changed)
{
    view->setMouseTracking(true);
    view->viewport()->setAttribute(Qt::WA_Hover);
    view->setItemDelegate(new StarDelegate(view, listOf));
    view->viewport()->installEventFilter(new StarClicks(view, std::move(listOf), std::move(changed)));
}
} // namespace lila::ui::favourites
