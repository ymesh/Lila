#include "ui/PanelDefaults.h"

#include "core/PrefStore.h"
#include "ops/Undo.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/SpinScrub.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMenu>
#include <QSpinBox>
#include <QToolButton>

#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>

namespace lila::ui::panelDefaults
{
namespace
{
using json = nlohmann::json;

constexpr const char* kBuiltInProperty = "lilaBuiltInDefault";

bool isField(QWidget* w)
{
    if (!w->property(kDefaultProperty).isValid())
        return false;
    if (qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QComboBox*>(w))
        return true;
    auto* b = qobject_cast<QAbstractButton*>(w);
    return b && b->isCheckable();
}

std::vector<QWidget*> fields(QWidget* root)
{
    std::vector<QWidget*> out;
    for (QWidget* w : root->findChildren<QWidget*>())
        if (isField(w))
            out.push_back(w);
    return out;
}

json current(QWidget* w)
{
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w))
        return d->value();
    if (auto* i = qobject_cast<QSpinBox*>(w))
        return i->value();
    if (auto* c = qobject_cast<QComboBox*>(w))
        return c->currentIndex();
    if (auto* b = qobject_cast<QAbstractButton*>(w))
        return b->isChecked();
    return nullptr;
}

QVariant toVariant(QWidget* w, const json& v)
{
    if (qobject_cast<QAbstractSpinBox*>(w))
        return v.is_number() ? QVariant(v.get<double>()) : QVariant();
    if (qobject_cast<QComboBox*>(w))
        return v.is_number_integer() ? QVariant(v.get<int>()) : QVariant();
    return v.is_boolean() ? QVariant(v.get<bool>()) : QVariant();
}

void apply(QWidget* w, const QVariant& v)
{
    if (!v.isValid())
        return;
    if (auto* d = qobject_cast<QDoubleSpinBox*>(w))
        d->setValue(v.toDouble());
    else if (auto* i = qobject_cast<QSpinBox*>(w))
        i->setValue(int(std::lround(v.toDouble())));
    else if (auto* c = qobject_cast<QComboBox*>(w))
    {
        if (v.toInt() >= 0 && v.toInt() < c->count())
            c->setCurrentIndex(v.toInt());
    }
    else if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isEnabled())
        b->setChecked(v.toBool());
}

QVariant builtIn(QWidget* w)
{
    QVariant v = w->property(kBuiltInProperty);
    return v.isValid() ? v : w->property(kDefaultProperty);
}

void setDefault(QWidget* w, const QVariant& v)
{
    if (!w->property(kBuiltInProperty).isValid())
        w->setProperty(kBuiltInProperty, w->property(kDefaultProperty));
    w->setProperty(kDefaultProperty, v);
}

bool sameValue(QWidget* w, const QVariant& v)
{
    const json c = current(w);
    if (c.is_number_float() || qobject_cast<QDoubleSpinBox*>(w))
        return std::abs(c.get<double>() - v.toDouble()) < 1e-9;
    if (c.is_number_integer())
        return c.get<int>() == int(std::lround(v.toDouble()));
    return c.is_boolean() && c.get<bool>() == v.toBool();
}
} // namespace

void setToggleDefault(QAbstractButton* button, bool on)
{
    button->setProperty(kDefaultProperty, on);
}

void reset(QWidget* root)
{
    for (QWidget* w : fields(root))
    {
        const QVariant def = w->property(kDefaultProperty);
        if (sameValue(w, def))
            continue;
        apply(w, def);
        if (w->isVisible())
            flashReset(w);
    }
}

bool hasDefaults(QWidget* root)
{
    return !fields(root).empty();
}

void addResetMenu(QWidget* header, QWidget* root, std::function<void()> extra)
{
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(header, &QWidget::customContextMenuRequested, header, [header, root, extra](const QPoint& at) {
        if (!extra && !hasDefaults(root))
            return;
        QMenu menu;
        menu.addAction(icon("restore"), "Reset All", header, [root, extra] {
            UndoStack::get().openChunk("Reset All");
            reset(root);
            if (extra)
                extra();
            UndoStack::get().closeChunk();
        });
        menu.exec(header->mapToGlobal(at));
    });
}

void saveAsDefaults(QWidget* root, const std::string& prefsKey)
{
    json stored = json::object();
    for (QWidget* w : fields(root))
    {
        if (w->objectName().isEmpty())
            continue;
        const json v = current(w);
        stored[w->objectName().toStdString()] = v;
        setDefault(w, toVariant(w, v));
    }
    PrefStore::get().set("general", prefsKey, stored);
}

void resetBuiltIn(QWidget* root, const std::string& prefsKey)
{
    PrefStore::get().set("general", prefsKey, nullptr);
    for (QWidget* w : fields(root))
    {
        w->setProperty(kDefaultProperty, builtIn(w));
        apply(w, builtIn(w));
    }
}

void load(QWidget* root, const std::string& prefsKey)
{
    const json stored = PrefStore::get().value("general", prefsKey);
    if (!stored.is_object())
        return;
    for (QWidget* w : fields(root))
    {
        auto it = stored.find(w->objectName().toStdString());
        if (w->objectName().isEmpty() || it == stored.end())
            continue;
        const QVariant v = toVariant(w, *it);
        if (!v.isValid())
            continue;
        const bool atBuiltIn = sameValue(w, builtIn(w));
        setDefault(w, v);
        if (atBuiltIn)
            apply(w, v);
    }
}

QToolButton* resetButton(QWidget* root, const std::string& prefsKey, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon("restore"));
    b->setToolTip("Reset: every setting in the panel to its default");
    b->setAccessibleName("Reset");
    b->setPopupMode(QToolButton::MenuButtonPopup);
    controls::setRole(b, "push");
    auto* menu = new QMenu(b);
    menu->addAction(icon("restore"), "Reset to Defaults", b, [root] { reset(root); });
    menu->addAction("Save as Defaults", b, [root, prefsKey] { saveAsDefaults(root, prefsKey); });
    menu->addSeparator();
    menu->addAction("Reset to Built-in Defaults", b, [root, prefsKey] { resetBuiltIn(root, prefsKey); });
    b->setMenu(menu);
    QObject::connect(b, &QToolButton::clicked, b, [root] { reset(root); });
    return b;
}
} // namespace lila::ui::panelDefaults
