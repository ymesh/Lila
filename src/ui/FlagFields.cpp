#include "ui/FlagFields.h"
#include "ui/ChoiceTips.h"
#include "ui/Controls.h"
#include "ui/Theme.h"

#include "ui/FontPicker.h"
#include "ui/CodeEdit.h"
#include "ui/MultilineEdit.h"
#include "ui/NodePicker.h"
#include "ui/SpinScrub.h"
#include "ui/UvEditor.h"
#include "uv/Uv.h"
#include "ui/ValueReset.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSpinBox>

#include <array>
#include <cmath>
#include <memory>

namespace lila::ui
{
namespace opt = ops::options;

namespace
{
/** Label that scrubs a number when dragged sideways, like the channel box's name column. */
class ScrubLabel : public QLabel
{
public:
    ScrubLabel(const QString& text, std::function<void(double)> step, QWidget* parent) : QLabel(text, parent), mStep(std::move(step))
    {
        setCursor(Qt::SizeHorCursor);
        setUsageHint(this, "Drag sideways to scrub; Ctrl for fine steps, Shift for coarse");
    }

    /** Scrubbing steps follow the field's own single step. */
    void setStepScale(double s) { mScale = s; }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
            mLast = e->globalPosition().x();
        if (e->button() == Qt::MiddleButton)
            mMiddle.press(int(e->globalPosition().x()), int(e->globalPosition().y()), 0);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton)
            mMiddle.cancel();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton)))
            return;
        // A middle press only scrubs once it is clearly a drag; a click resets the value instead.
        if (mMiddle.down() && !mMiddle.dragging())
        {
            mMiddle.move(int(e->globalPosition().x()), int(e->globalPosition().y()));
            mLast = e->globalPosition().x();
            if (!mMiddle.dragging())
                return;
        }
        double dx = e->globalPosition().x() - mLast;
        mLast = e->globalPosition().x();
        double step = (e->modifiers() & Qt::ControlModifier) ? 0.01 : (e->modifiers() & Qt::ShiftModifier) ? 1.0 : 0.1;
        if (dx != 0.0)
            mStep(dx * step * mScale);
    }

private:
    std::function<void(double)> mStep;
    double mScale = 1.0;
    double mLast = 0.0;
    valueReset::ClickTracker mMiddle;
};

QString tip(const opt::Field& f)
{
    return QString::fromStdString(f.flag + (f.doc.empty() ? "" : ": " + f.doc));
}

double jsonNumber(const json& v, double fallback = 0.0)
{
    return v.is_number() ? v.get<double>() : v.is_boolean() ? double(v.get<bool>()) : fallback;
}

QString listText(const json& v)
{
    QStringList parts;
    for (const json& e : v.is_array() ? v : json::array())
        parts << QString::fromStdString(e.is_string() ? e.get<std::string>() : e.dump());
    return parts.join(", ");
}

json parseList(const QString& text)
{
    json out = json::array();
    for (const QString& p : text.split(',', Qt::SkipEmptyParts))
        if (QString t = p.trimmed(); !t.isEmpty())
            out.push_back(t.toStdString());
    return out;
}

/** What an empty node field stands for: the selection when the flag's doc falls back to it, else nothing. */
QString emptyNodeText(const opt::Field& f, const NodePicker& picker, bool list)
{
    const QString doc = QString::fromStdString(f.doc);
    if (doc.contains("selected", Qt::CaseInsensitive) || doc.contains("selection", Qt::CaseInsensitive))
        return "Selected " + (list ? picker.pluralText() : picker.typeText());
    return "None";
}

bool flagBounded(const opt::Field& f)
{
    return !std::isnan(f.min) && !std::isnan(f.max) && f.max > f.min;
}

/** The unit as the field shows it after the number: " cm", degrees as a bare degree sign. */
QString flagSuffix(const std::string& suffix)
{
    const QString t = QString::fromStdString(suffix).trimmed();
    if (t.isEmpty())
        return {};
    if (t == "deg" || t == "degrees" || t == QString(QChar(0x00b0)))
        return QString(QChar(0x00b0));
    return " " + t;
}

/**
 * A number field: right-aligned, unit inside. A bounded one (minimum and maximum both set) is a slider field showing
 * where the value sits in its range; an unbounded one is a plain value field.
 */
QDoubleSpinBox* flagNumber(const opt::Field& f, int decimals, double lo, double hi, QWidget* parent)
{
    QDoubleSpinBox* spin = nullptr;
    if (flagBounded(f))
    {
        spin = new controls::SliderField(parent);
        spin->setDecimals(decimals);
        spin->setRange(f.min, f.max);
    }
    else
        spin = controls::valueField(lo, hi, decimals, {}, parent);
    spin->setSingleStep(std::isnan(f.step) ? (decimals ? 0.1 : 1.0) : f.step);
    spin->setSuffix(flagSuffix(f.suffix));
    if (!f.presets.empty())
        controls::setPresets(spin, f.presets);
    return spin;
}

/** A choice as users read it: the override's label, else the stored value in words ("boundingBox" as "Bounding Box"). */
QString flagChoiceLabel(const opt::Field& f, size_t i)
{
    const std::string& value = f.choices[i];
    if (i < f.choiceLabels.size() && !f.choiceLabels[i].empty() && f.choiceLabels[i] != value)
        return QString::fromStdString(f.choiceLabels[i]);
    if (!value.empty() && value.find_first_not_of("xyzXYZ") == std::string::npos)
        return QString::fromStdString(value).toUpper();
    return QString::fromStdString(opt::prettyLabel(value));
}

/** A row label: right-aligned against its field, dim like the Channel Box's names. */
void flagLabelStyle(QWidget* w)
{
    if (auto* l = qobject_cast<QLabel*>(w))
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QPalette pal = w->palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
        pal.setColor(group, QPalette::WindowText, themeColor("dimText"));
    w->setPalette(pal);
}
} // namespace

FlagFieldWidgets makeFlagField(const opt::Field& f, const json& value, std::function<void(const json&)> changed, QWidget* parent)
{
    FlagFieldWidgets out;
    QString label = QString::fromStdString(f.label);
    QString defText = QString::fromStdString(valueReset::describe(f.defaultValue));
    switch (f.kind)
    {
    case opt::FieldKind::Check:
    {
        auto* box = new QCheckBox(label, parent);
        box->setChecked(jsonNumber(value) != 0.0);
        QObject::connect(box, &QCheckBox::toggled, box, [changed](bool on) { changed(on); });
        bool on = jsonNumber(f.defaultValue) != 0.0;
        onMiddleClickReset(box, [box, on] { box->setChecked(on); }, on ? "on" : "off");
        out.field = box;
        out.set = [box](const json& v) {
            QSignalBlocker b(box);
            box->setChecked(jsonNumber(v) != 0.0);
        };
        break;
    }
    case opt::FieldKind::Int:
    {
        int v = int(std::lround(jsonNumber(value)));
        int d = int(std::lround(jsonNumber(f.defaultValue)));
        QDoubleSpinBox* spin = flagNumber(f, 0, !std::isnan(f.min) ? f.min : (v < 0 || d < 0) ? -1000000 : 0, std::isnan(f.max) ? 1000000 : f.max, parent);
        spin->setValue(v);
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [changed](double x) { changed(int(std::lround(x))); });
        // An int scrubs a step per ten pixels.
        auto acc = std::make_shared<double>(0.0);
        auto* scrub = new ScrubLabel(label, [spin, acc](double dx) {
            *acc += dx;
            int whole = int(*acc);
            if (whole != 0)
            {
                *acc -= whole;
                if (!controls::stepPresets(spin, whole))
                    spin->setValue(spin->value() + whole);
            }
        }, parent);
        setSpinDefault(spin, d);
        onMiddleClickReset(scrub, [spin, d] { spin->setValue(d); }, defText, spin);
        out.label = scrub;
        out.field = spin;
        out.set = [spin](const json& x) {
            QSignalBlocker b(spin);
            spin->setValue(double(std::lround(jsonNumber(x))));
            spin->update();
        };
        break;
    }
    case opt::FieldKind::Float:
    {
        QDoubleSpinBox* spin = flagNumber(f, 3, std::isnan(f.min) ? -1e6 : f.min, std::isnan(f.max) ? 1e6 : f.max, parent);
        spin->setValue(jsonNumber(value));
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [changed](double x) { changed(x); });
        auto acc = std::make_shared<double>(0.0);
        auto* scrub = new ScrubLabel(label, [spin, acc](double dx) {
            if (controls::stepPresets(spin, 0))
            {
                // dx is pixels times the single step; a preset per 24 pixels keeps each one pickable.
                *acc += dx / spin->singleStep() / 24.0;
                const int whole = int(*acc);
                *acc -= whole;
                controls::stepPresets(spin, whole);
                return;
            }
            spin->setValue(spin->value() + dx);
        }, parent);
        scrub->setStepScale(spin->singleStep() / 0.1);
        double d = jsonNumber(f.defaultValue);
        setSpinDefault(spin, d);
        onMiddleClickReset(scrub, [spin, d] { spin->setValue(d); }, defText, spin);
        out.label = scrub;
        out.field = spin;
        out.set = [spin](const json& x) {
            QSignalBlocker b(spin);
            spin->setValue(jsonNumber(x));
            spin->update();
        };
        break;
    }
    case opt::FieldKind::Choice:
    {
        // A mode with a few values: one segmented strip, never a dropdown.
        if (f.editor == "segmented")
        {
            auto* strip = new controls::Segmented(parent);
            for (size_t i = 0; i < f.choices.size(); ++i)
                strip->add(flagChoiceLabel(f, i), QString(), flagChoiceLabel(f, i), int(i));
            auto select = [strip, &f](const json& v) {
                const std::string s = v.is_string() ? v.get<std::string>() : std::string();
                for (size_t i = 0; i < f.choices.size(); ++i)
                    if (f.choices[i] == s)
                        strip->setCurrent(int(i));
            };
            select(value);
            const std::vector<std::string> values = f.choices;
            QObject::connect(strip, &controls::Segmented::picked, strip, [values, changed](int id) {
                if (id >= 0 && size_t(id) < values.size())
                    changed(values[size_t(id)]);
            });
            out.label = new QLabel(label, parent);
            out.field = strip;
            out.set = [strip, values](const json& v) {
                QSignalBlocker b(strip);
                const std::string s = v.is_string() ? v.get<std::string>() : std::string();
                for (size_t i = 0; i < values.size(); ++i)
                    if (values[i] == s)
                        strip->setCurrent(int(i));
            };
            break;
        }
        auto* combo = new QComboBox(parent);
        for (size_t i = 0; i < f.choices.size(); ++i)
            combo->addItem(flagChoiceLabel(f, i), QString::fromStdString(f.choices[i]));
        auto select = [combo](const json& v) {
            int at = combo->findData(QString::fromStdString(v.is_string() ? v.get<std::string>() : std::string()));
            if (at >= 0)
                combo->setCurrentIndex(at);
        };
        select(value);
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, changed](int) { changed(combo->currentData().toString().toStdString()); });
        auto* l = new QLabel(label, parent);
        int defIndex = combo->findData(QString::fromStdString(f.defaultValue.is_string() ? f.defaultValue.get<std::string>() : std::string()));
        if (defIndex >= 0)
            onMiddleClickReset(l, [combo, defIndex] { combo->setCurrentIndex(defIndex); }, combo->itemText(defIndex), combo);
        out.label = l;
        out.field = combo;
        out.set = [combo, select](const json& v) {
            {
                QSignalBlocker b(combo);
                select(v);
            }
            choiceTips::refresh(combo);
        };
        break;
    }
    case opt::FieldKind::Text:
    case opt::FieldKind::List:
    {
        bool list = f.kind == opt::FieldKind::List;
        auto str = [](const json& v) { return v.is_string() ? v.get<std::string>() : std::string(); };
        if (!f.nodeType.empty())
        {
            auto* picker = new NodePicker(f.nodeType, list, parent);
            picker->setValue(value);
            picker->setSubject(label);
            picker->setPlaceholder(emptyNodeText(f, *picker, list));
            QObject::connect(picker, &NodePicker::edited, picker, [changed](const json& v) { changed(v); });
            out.label = new QLabel(label, parent);
            out.field = picker;
            out.set = [picker](const json& v) { picker->setValue(v); };
            break;
        }
        if (!list && f.editor == "font")
        {
            auto* picker = new FontPicker(parent);
            picker->setValue(str(value));
            QObject::connect(picker, &FontPicker::chosen, picker, [changed](const QString& font) { changed(json(font.toStdString())); });
            out.label = new QLabel(label, parent);
            out.field = picker;
            out.set = [picker, str](const json& v) { picker->setValue(str(v)); };
            break;
        }
        if (!list && f.editor == "uvSet")
        {
            auto* combo = new QComboBox(parent);
            combo->setEditable(true);
            combo->setInsertPolicy(QComboBox::NoInsert);
            combo->addItems(uvSetNames());
            // Left empty, the command uses the current set; typing a new name makes that set.
            combo->lineEdit()->setPlaceholderText(QString("Current (%1)").arg(QString::fromStdString(uv::currentUvSet())));
            auto show = [combo, str](const json& v) {
                QSignalBlocker b(combo);
                combo->setEditText(QString::fromStdString(str(v)));
            };
            show(value);
            auto commit = [combo, changed] {
                const std::string t = combo->currentText().trimmed().toStdString();
                changed(t.empty() ? json(nullptr) : json(t));
            };
            QObject::connect(combo, &QComboBox::activated, combo, commit);
            QObject::connect(combo->lineEdit(), &QLineEdit::editingFinished, combo, commit);
            auto* l = new QLabel(label, parent);
            onMiddleClickReset(l, [combo, changed] { combo->setEditText({}); changed(nullptr); }, combo->lineEdit()->placeholderText(), combo);
            out.label = l;
            out.field = combo;
            out.set = show;
            break;
        }
        if (!list && f.editor == "code")
        {
            auto* box = new CodeEdit(parent);
            box->showText(str(value));
            QObject::connect(box, &CodeEdit::applied, box, [changed](const QString& t) { changed(json(t.toStdString())); });
            out.label = new QLabel(label, parent);
            out.field = box;
            out.set = [box, str](const json& v) { box->showText(str(v)); };
            break;
        }
        if (!list && f.editor == "multiline")
        {
            auto* box = new MultilineEdit(parent);
            box->showText(str(value));
            QObject::connect(box, &MultilineEdit::committed, box, [changed](const QString& t) { changed(json(t.toStdString())); });
            out.label = new QLabel(label, parent);
            out.field = box;
            out.set = [box, str](const json& v) { box->showText(str(v)); };
            break;
        }
        auto* line = new QLineEdit(parent);
        auto text = [list](const json& v) { return list ? listText(v) : QString::fromStdString(v.is_string() ? v.get<std::string>() : std::string()); };
        line->setText(text(value));
        if (list)
            line->setPlaceholderText("comma separated");
        QObject::connect(line, &QLineEdit::editingFinished, line, [line, list, changed] {
            changed(list ? parseList(line->text()) : json(line->text().toStdString()));
        });
        out.label = new QLabel(label, parent);
        out.field = line;
        out.set = [line, text](const json& v) {
            QSignalBlocker b(line);
            line->setText(text(v));
        };
        break;
    }
    case opt::FieldKind::Vec3:
    {
        std::vector<QDoubleSpinBox*> made;
        QWidget* box = controls::vectorField(made, std::isnan(f.min) ? -1e6 : f.min, std::isnan(f.max) ? 1e6 : f.max, 3, {}, parent);
        auto spins = std::make_shared<std::array<QDoubleSpinBox*, 3>>();
        auto emitAll = [spins, changed] { changed(json::array({(*spins)[0]->value(), (*spins)[1]->value(), (*spins)[2]->value()})); };
        for (int i = 0; i < 3; ++i)
        {
            QDoubleSpinBox* spin = made[size_t(i)];
            spin->setSingleStep(std::isnan(f.step) ? 0.1 : f.step);
            spin->setSuffix(flagSuffix(f.suffix));
            spin->setValue(value.is_array() && value.size() == 3 ? jsonNumber(value[i]) : 0.0);
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [emitAll](double) { emitAll(); });
            (*spins)[i] = spin;
        }
        out.label = new QLabel(label, parent);
        out.field = box;
        out.set = [spins](const json& v) {
            for (int i = 0; i < 3; ++i)
            {
                QSignalBlocker b((*spins)[i]);
                (*spins)[i]->setValue(v.is_array() && v.size() == 3 ? jsonNumber(v[i]) : 0.0);
            }
        };
        break;
    }
    }
    out.field->setToolTip(tip(f));
    if (out.label)
        flagLabelStyle(out.label);
    if (auto* combo = qobject_cast<QComboBox*>(out.field); combo && f.kind == opt::FieldKind::Choice)
        choiceTips::apply(combo, f.choiceDocs);
    if (out.label && out.label->toolTip().isEmpty())
        out.label->setToolTip(tip(f));
    return out;
}
} // namespace lila::ui
