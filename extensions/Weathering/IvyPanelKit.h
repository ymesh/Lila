#pragma once

// The app's panel controls built from the style sheet roles (section, sectionHeader, tile, push, overflow, segmented,
// segment, dim), for extension panels, which reach Lila only through the SDK.

#include <lila/Actions.h>
#include <lila/Icons.h>
#include <lila/OptionBox.h>
#include <lila/Prefs.h>

#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <vector>

namespace weathering::ivykit
{
inline void setRole(QWidget* w, const char* role)
{
    w->setProperty("role", role);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

/** A collapsible section: `content` under a text header. Collapsed state is remembered under prefsKey + "/" + title. */
inline QWidget* section(const QString& title, QWidget* content, QWidget* parent, const char* prefsKey, bool openByDefault = true)
{
    auto* box = new QWidget(parent);
    box->setAttribute(Qt::WA_StyledBackground);
    setRole(box, "section");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 4, 0, 6);
    v->setSpacing(2);
    auto* header = new QToolButton(box);
    header->setText(title);
    header->setCheckable(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setIconSize(QSize(14, 14));
    setRole(header, "sectionHeader");
    const std::string key = std::string(prefsKey) + "/" + title.toStdString();
    const bool open = !lila::PrefStore::get().value<bool>("general", key, !openByDefault);
    header->setChecked(open);
    header->setIcon(lila::ui::icon(open ? "chevron-down" : "chevron-right"));
    content->setParent(box);
    content->setVisible(open);
    QObject::connect(header, &QToolButton::toggled, content, [header, content, key](bool on) {
        content->setVisible(on);
        header->setIcon(lila::ui::icon(on ? "chevron-down" : "chevron-right"));
        lila::PrefStore::get().set("general", key, !on);
    });
    v->addWidget(header);
    v->addWidget(content, 1);
    v->addStretch(0);
    return box;
}

/** An icon-over-label tile, checkable, for a brush or mode grid. */
inline QToolButton* tile(const QString& iconName, const QString& label, const QString& tip, QWidget* parent = nullptr)
{
    auto* b = new QToolButton(parent);
    b->setIcon(lila::ui::icon(iconName));
    b->setIconSize(QSize(24, 24));
    b->setText(label);
    b->setToolTip(tip);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    setRole(b, "tile");
    return b;
}

/** A labelled button on the raised fill. */
inline QToolButton* button(const QString& label, const QString& tip, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setText(label);
    b->setToolTip(tip);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRole(b, "push");
    return b;
}

/** A labelled button running the action, with its option box when it has one. The label defaults to the action's own. */
inline QToolButton* push(const std::string& actionId, QWidget* parent, const QString& label = {})
{
    QToolButton* b = button(label.isEmpty() ? QString::fromStdString(lila::ui::actions::displayLabel(actionId)) : label,
                            QString::fromStdString(lila::ui::actions::tooltip(actionId)), parent);
    QObject::connect(b, &QToolButton::clicked, b, [actionId] { lila::ui::actions::run(nlohmann::json(actionId)); });
    lila::ui::optionBox::withOptionBox(b, actionId, parent);
    return b;
}

/** "... N more": a menu of rare actions, each still in the menus and Ctrl+T search. */
inline QToolButton* overflow(const std::vector<std::string>& actionIds, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(lila::ui::icon("dots"));
    b->setText(QString("%1 more").arg(actionIds.size()));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setToolTip("More commands; every one is also in the menus and Ctrl+T search");
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRole(b, "overflow");
    auto* menu = new QMenu(b);
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, actionIds] {
        menu->clear();
        for (const std::string& id : actionIds)
        {
            QAction* a = menu->addAction(lila::ui::icon(QString::fromStdString(lila::ui::actions::iconFor(id))),
                                         QString::fromStdString(lila::ui::actions::displayLabel(id)),
                                         [id] { lila::ui::actions::run(nlohmann::json(id)); });
            a->setToolTip(QString::fromStdString(lila::ui::actions::tooltip(id)));
            a->setData(QString::fromStdString(id));
            a->setEnabled(lila::ui::actions::isEnabled(nlohmann::json(id)));
        }
        lila::ui::optionBox::decorateMenu(menu);
    });
    b->setMenu(menu);
    b->setVisible(!actionIds.empty());
    return b;
}

/** A grid of tiles or buttons, `columns` wide, 2 px apart. */
inline QWidget* grid(const std::vector<QWidget*>& items, int columns, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* g = new QGridLayout(w);
    g->setContentsMargins(0, 0, 0, 0);
    g->setSpacing(2);
    for (size_t i = 0; i < items.size(); ++i)
    {
        items[i]->setParent(w);
        g->addWidget(items[i], int(i) / columns, int(i) % columns);
    }
    for (int c = 0; c < columns; ++c)
        g->setColumnStretch(c, 1);
    return w;
}

/** An exclusive choice drawn as one strip; `group` reports and sets the current segment by id. */
struct Segmented
{
    QWidget* widget = nullptr;
    QButtonGroup* group = nullptr;

    void add(const QString& text, const QString& tip, int id)
    {
        auto* b = new QToolButton(widget);
        b->setCheckable(true);
        b->setText(text);
        b->setToolTip(tip);
        b->setAccessibleName(text);
        b->setFocusPolicy(Qt::NoFocus);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setRole(b, "segment");
        group->addButton(b, id);
        widget->layout()->addWidget(b);
    }

    void setCurrent(int id) const
    {
        if (QAbstractButton* b = group->button(id); b && !b->isChecked())
            b->setChecked(true);
    }
};

inline Segmented segmented(QWidget* parent)
{
    Segmented s;
    s.widget = new QWidget(parent);
    s.widget->setAttribute(Qt::WA_StyledBackground);
    setRole(s.widget, "segmented");
    auto* row = new QHBoxLayout(s.widget);
    row->setContentsMargins(2, 2, 2, 2);
    row->setSpacing(2);
    s.group = new QButtonGroup(s.widget);
    s.group->setExclusive(true);
    return s;
}

/** A number field: right-aligned, no step buttons, unit as a suffix. */
inline QDoubleSpinBox* valueField(double lo, double hi, int decimals, const QString& unit, QWidget* parent)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setKeyboardTracking(false);
    if (!unit.isEmpty())
        s->setSuffix(" " + unit);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return s;
}

/** A form with right-aligned labels, the Channel Box layout. */
inline QFormLayout* form(QWidget* w)
{
    auto* f = new QFormLayout(w);
    f->setContentsMargins(0, 0, 0, 0);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->setHorizontalSpacing(8);
    f->setVerticalSpacing(4);
    return f;
}

/** A dim line: readouts, status and empty states. */
inline QLabel* dim(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    setRole(l, "dim");
    return l;
}
} // namespace weathering::ivykit
