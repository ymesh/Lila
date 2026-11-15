#include "ui/TexParams.h"

#include "ops/CommandOptions.h"
#include "tex/Types.h"
#include "ui/ChoiceTips.h"
#include "ui/CodeEdit.h"
#include "ui/ColorPopup.h"
#include "ui/FlagFields.h"
#include "ui/Icons.h"
#include "ui/ImagePicker.h"
#include "ui/TexturePicker.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace lila::ui::texParams
{
namespace
{
namespace opt = ops::options;
using Rgba = std::array<float, 4>;

QString qs(const std::string& s) { return QString::fromStdString(s); }

QColor gray(int v) { return themeShade(v); }

float toSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

QColor display(const Rgba& c) { return QColor::fromRgbF(toSrgb(c[0]), toSrgb(c[1]), toSrgb(c[2])); }

Rgba rgbaOf(const json& v)
{
    float f[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!tex::valueToFloat4(v, tex::PortType::Color, f))
        return {0.0f, 0.0f, 0.0f, 1.0f};
    return {f[0], f[1], f[2], f[3]};
}

json jsonOf(const Rgba& c) { return json::array({c[0], c[1], c[2], c[3]}); }

std::optional<double> number(const json& p, const char* key)
{
    if (p.contains(key) && p[key].is_number())
        return p[key].get<double>();
    return std::nullopt;
}

double niceStep(double raw)
{
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0})
        if (raw <= m * p)
            return m * p;
    return 10.0 * p;
}

/** A field for a float or int parameter with its hard range, scrub step from the soft (else hard) range. */
opt::Field numberField(const json& p, bool integer)
{
    opt::Field f;
    f.flag = p.value("id", "");
    f.label = p.contains("label") ? p["label"].get<std::string>() : opt::prettyLabel(f.flag);
    f.kind = integer ? opt::FieldKind::Int : opt::FieldKind::Float;
    f.defaultValue = p.value("default", json(0));
    f.doc = p.value("doc", "");
    if (auto v = number(p, "min"))
        f.min = *v;
    if (auto v = number(p, "max"))
        f.max = *v;
    if (auto v = number(p, "step"))
        f.step = *v;
    else if (!integer)
    {
        std::optional<double> lo = number(p, "softMin") ? number(p, "softMin") : number(p, "min");
        std::optional<double> hi = number(p, "softMax") ? number(p, "softMax") : number(p, "max");
        if (lo && hi && *hi > *lo)
            f.step = niceStep((*hi - *lo) / 100.0);
    }
    return f;
}

/** A swatch showing a linear colour; click to pick. The swatch is the value itself, so it is coloured. */
QWidget* colorSwatch(QWidget* parent, const json& value, std::function<void(const json&)> changed)
{
    auto* b = new QToolButton(parent);
    b->setMinimumWidth(60);
    auto rgba = std::make_shared<Rgba>(rgbaOf(value));
    auto* bright = new colorPopup::ValueSlider(parent);
    auto paint = [b, rgba, bright] {
        bright->setColor({(*rgba)[0], (*rgba)[1], (*rgba)[2]});
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid rgb(20,20,20); min-height: 16px; }").arg(display(*rgba).name()));
        b->setToolTip(QString("Linear %1, %2, %3, alpha %4").arg((*rgba)[0], 0, 'f', 3).arg((*rgba)[1], 0, 'f', 3).arg((*rgba)[2], 0, 'f', 3).arg((*rgba)[3], 0, 'f', 3));
    };
    paint();
    QObject::connect(b, &QToolButton::clicked, b, [b, rgba, paint, changed] {
        QColor init = display(*rgba);
        init.setAlphaF((*rgba)[3]);
        QColor c = colorPopup::getColor(init, b, "Color", true);
        if (!c.isValid())
            return;
        *rgba = {toLinear(float(c.redF())), toLinear(float(c.greenF())), toLinear(float(c.blueF())), float(c.alphaF())};
        paint();
        changed(jsonOf(*rgba));
    });
    bright->dragged = [rgba, paint](const glm::dvec3& c) {
        *rgba = {float(c.x), float(c.y), float(c.z), (*rgba)[3]};
        paint();
    };
    bright->released = [changed, rgba](const glm::dvec3& start, const glm::dvec3& c) {
        if (c != start)
            changed(jsonOf(*rgba));
    };
    return colorPopup::withEyedropper(b, [rgba, paint, changed](const glm::dvec3& c) {
        *rgba = {float(c.x), float(c.y), float(c.z), (*rgba)[3]};
        paint();
        changed(jsonOf(*rgba));
    }, bright);
}

struct Stop
{
    double t = 0.0;
    Rgba color{0.0f, 0.0f, 0.0f, 1.0f};
};

Rgba lerp(const Rgba& a, const Rgba& b, float f)
{
    return {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f};
}

/** The gradient at t, as texGradientEval computes it. */
Rgba evalGradient(const std::vector<Stop>& stops, const std::string& interp, double t)
{
    if (stops.empty())
        return {float(t), float(t), float(t), 1.0f};
    if (t <= stops.front().t)
        return stops.front().color;
    for (size_t k = 1; k < stops.size(); ++k)
        if (t <= stops[k].t)
        {
            if (interp == "constant")
                return stops[k - 1].color;
            float f = float(std::clamp((t - stops[k - 1].t) / std::max(stops[k].t - stops[k - 1].t, 1e-6), 0.0, 1.0));
            if (interp == "smooth")
                f = f * f * (3.0f - 2.0f * f);
            return lerp(stops[k - 1].color, stops[k].color, f);
        }
    return stops.back().color;
}

class GradientBar final : public QWidget
{
public:
    GradientBar(QWidget* parent, const json& value, std::function<void()> changed) : QWidget(parent), mChanged(std::move(changed))
    {
        for (const json& s : gradientStops(value))
            mStops.push_back({s[0].get<double>(), rgbaOf(s[1])});
        setMinimumHeight(40);
        setMouseTracking(true);
        setToolTip("Click the bar to add a stop, drag a stop to move it, double-click it to pick its colour, right-click it to remove it");
    }

    std::string interpolation = "linear";

    json stops() const
    {
        std::vector<Stop> sorted = mStops;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
        json out = json::array();
        for (const Stop& s : sorted)
            out.push_back(json::array({s.t, jsonOf(s.color)}));
        return out;
    }

protected:
    QRectF bar() const { return QRectF(8, 4, width() - 16, 20); }
    double tAt(double x) const { return std::clamp((x - bar().left()) / bar().width(), 0.0, 1.0); }
    double xOf(double t) const { return bar().left() + t * bar().width(); }

    int stopAt(QPointF p) const
    {
        for (int i = int(mStops.size()) - 1; i >= 0; --i)
            if (std::abs(p.x() - xOf(mStops[size_t(i)].t)) <= 6.0 && p.y() >= bar().bottom() - 2)
                return i;
        return -1;
    }

    std::vector<Stop> sorted() const
    {
        std::vector<Stop> s = mStops;
        std::stable_sort(s.begin(), s.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
        return s;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QRectF r = bar();
        std::vector<Stop> s = sorted();
        for (int x = int(r.left()); x < int(r.right()); ++x)
        {
            p.setPen(display(evalGradient(s, interpolation, tAt(x + 0.5))));
            p.drawLine(QPointF(x + 0.5, r.top()), QPointF(x + 0.5, r.bottom()));
        }
        p.setPen(QPen(gray(20), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
        p.setRenderHint(QPainter::Antialiasing);
        for (size_t i = 0; i < mStops.size(); ++i)
        {
            double x = xOf(mStops[i].t);
            QPolygonF tri;
            tri << QPointF(x, r.bottom() + 1) << QPointF(x + 6, r.bottom() + 11) << QPointF(x - 6, r.bottom() + 11);
            p.setPen(QPen(int(i) == mSelected ? gray(240) : gray(20), int(i) == mSelected ? 1.6 : 1.0));
            p.setBrush(display(mStops[i].color));
            p.drawPolygon(tri);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        QPointF pos = e->position();
        int hit = stopAt(pos);
        if (e->button() == Qt::RightButton)
        {
            if (hit >= 0 && mStops.size() > 1)
            {
                mStops.erase(mStops.begin() + hit);
                mSelected = -1;
                update();
                mChanged();
            }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        if (hit < 0 && bar().adjusted(0, 0, 0, 12).contains(pos) && int(mStops.size()) < tex::kMaxGradientStops)
        {
            double t = tAt(pos.x());
            mStops.push_back({t, evalGradient(sorted(), interpolation, t)});
            hit = int(mStops.size()) - 1;
            mMoved = true;
        }
        mSelected = hit;
        mDragging = hit >= 0;
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!mDragging || mSelected < 0)
            return;
        mStops[size_t(mSelected)].t = tAt(e->position().x());
        mMoved = true;
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        mDragging = false;
        if (std::exchange(mMoved, false))
            mChanged();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        int hit = stopAt(e->position());
        if (hit < 0)
            return;
        Rgba& c = mStops[size_t(hit)].color;
        QColor init = display(c);
        init.setAlphaF(c[3]);
        QColor picked = colorPopup::getColor(init, this, "Stop Color", true);
        if (!picked.isValid())
            return;
        c = {toLinear(float(picked.redF())), toLinear(float(picked.greenF())), toLinear(float(picked.blueF())), float(picked.alphaF())};
        update();
        mChanged();
    }

private:
    std::vector<Stop> mStops;
    std::function<void()> mChanged;
    int mSelected = -1;
    bool mDragging = false;
    bool mMoved = false;
};

class CurveView final : public QWidget
{
public:
    CurveView(QWidget* parent, const json& value, std::function<void()> changed) : QWidget(parent), mChanged(std::move(changed))
    {
        for (const json& pt : curvePoints(value))
            mPoints.push_back({pt[0].get<double>(), pt[1].get<double>()});
        setMinimumSize(120, 120);
        setToolTip("Click to add a point, drag to move it, right-click a point to remove it");
    }

    bool smooth = true;

    json points() const
    {
        json out = json::array();
        for (const QPointF& p : sorted())
            out.push_back(json::array({p.x(), p.y()}));
        return out;
    }

    int heightForWidth(int w) const override { return w; }
    bool hasHeightForWidth() const override { return true; }

protected:
    QRectF area() const
    {
        double side = std::min(width(), height()) - 12.0;
        return QRectF((width() - side) * 0.5, 6.0, side, side);
    }
    QPointF toWidget(QPointF v) const { return {area().left() + v.x() * area().width(), area().bottom() - v.y() * area().height()}; }
    QPointF toValue(QPointF w) const
    {
        return {std::clamp((w.x() - area().left()) / area().width(), 0.0, 1.0), std::clamp((area().bottom() - w.y()) / area().height(), 0.0, 1.0)};
    }
    std::vector<QPointF> sorted() const
    {
        std::vector<QPointF> s = mPoints;
        std::stable_sort(s.begin(), s.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
        return s;
    }
    int pointAt(QPointF w) const
    {
        for (int i = int(mPoints.size()) - 1; i >= 0; --i)
            if (QLineF(toWidget(mPoints[size_t(i)]), w).length() <= 6.0)
                return i;
        return -1;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QRectF a = area();
        p.fillRect(a, gray(30));
        p.setPen(QPen(gray(48), 0));
        for (int k = 1; k < 4; ++k)
        {
            p.drawLine(QPointF(a.left() + a.width() * k / 4.0, a.top()), QPointF(a.left() + a.width() * k / 4.0, a.bottom()));
            p.drawLine(QPointF(a.left(), a.top() + a.height() * k / 4.0), QPointF(a.right(), a.top() + a.height() * k / 4.0));
        }
        p.setPen(QPen(gray(20), 1.0));
        p.drawRect(a);
        p.setRenderHint(QPainter::Antialiasing);
        json pts = points();
        QPainterPath path;
        for (int i = 0; i <= 128; ++i)
        {
            double x = i / 128.0;
            QPointF w = toWidget({x, std::clamp(evalCurve(pts, smooth, x), -0.05, 1.05)});
            if (i == 0)
                path.moveTo(w);
            else
                path.lineTo(w);
        }
        p.setPen(QPen(gray(220), 1.6));
        p.drawPath(path);
        for (size_t i = 0; i < mPoints.size(); ++i)
        {
            QPointF w = toWidget(mPoints[i]);
            p.setPen(QPen(gray(20), 1.0));
            p.setBrush(int(i) == mSelected ? gray(250) : gray(170));
            p.drawRect(QRectF(w.x() - 3.5, w.y() - 3.5, 7, 7));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        int hit = pointAt(e->position());
        if (e->button() == Qt::RightButton)
        {
            if (hit >= 0 && mPoints.size() > 2)
            {
                mPoints.erase(mPoints.begin() + hit);
                mSelected = -1;
                update();
                mChanged();
            }
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        if (hit < 0 && area().contains(e->position()) && int(mPoints.size()) < tex::kMaxCurvePoints)
        {
            mPoints.push_back(toValue(e->position()));
            hit = int(mPoints.size()) - 1;
            mMoved = true;
        }
        mSelected = hit;
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || mSelected < 0)
            return;
        mPoints[size_t(mSelected)] = toValue(e->position());
        mMoved = true;
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (std::exchange(mMoved, false))
            mChanged();
    }

private:
    std::vector<QPointF> mPoints;
    std::function<void()> mChanged;
    int mSelected = -1;
    bool mMoved = false;
};

QLabel* heading(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setStyleSheet("color: rgb(225,225,225); padding-top: 6px;");
    return l;
}

QLabel* dimLabel(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet("color: rgb(150,150,150);");
    return l;
}

QString labelOf(const json& p)
{
    return p.contains("label") ? qs(p["label"].get<std::string>()) : qs(opt::prettyLabel(p.value("id", "")));
}

/** Slang source for a code parameter. Every edit recompiles, so it is sent on Apply (or Ctrl+Enter), not per keystroke. */
QWidget* codeEditor(QWidget* parent, const std::string& value, const QString& tip, std::function<void(const json&)> changed)
{
    auto* box = new CodeEdit(parent);
    box->showText(value);
    box->setToolTip(tip);
    QObject::connect(box, &CodeEdit::applied, box, [changed](const QString& text) { changed(text.toStdString()); });
    return box;
}

/** Adds the editor for one parameter to the form. */
void addParam(QFormLayout* form, QWidget* parent, const json& p, const SetFn& set)
{
    const std::string id = p.value("id", "");
    const std::string type = p.value("type", "float");
    const json value = p.contains("value") ? p["value"] : p.value("default", json());
    auto changed = [set, id](const json& v) { set(id, v); };
    QString tip = qs(id + ": " + type) + (p.contains("doc") ? "\n" + qs(p["doc"].get<std::string>()) : QString());

    if (type == "float" || type == "int")
    {
        FlagFieldWidgets w = makeFlagField(numberField(p, type == "int"), value, changed, parent);
        w.field->setToolTip(tip);
        form->addRow(w.label, w.field);
        return;
    }
    if (type == "bool")
    {
        opt::Field f{.flag = id, .label = labelOf(p).toStdString(), .kind = opt::FieldKind::Check, .defaultValue = p.value("default", json(false))};
        FlagFieldWidgets w = makeFlagField(f, value, changed, parent);
        w.field->setToolTip(tip);
        form->addRow(w.field);
        return;
    }
    if (type == "enum")
    {
        opt::Field f{.flag = id, .label = labelOf(p).toStdString(), .kind = opt::FieldKind::Choice, .defaultValue = p.value("default", json())};
        for (const json& o : p.value("options", json::array()))
        {
            f.choices.push_back(o.get<std::string>());
            f.choiceLabels.push_back(opt::prettyLabel(o.get<std::string>()));
        }
        f.choiceDocs = opt::choiceDocs(p.value("doc", ""), f.choices);
        json shown = value;
        if (value.is_number_integer() && size_t(value.get<int64_t>()) < f.choices.size())
            shown = f.choices[size_t(value.get<int64_t>())];
        if (f.defaultValue.is_number_integer() && size_t(f.defaultValue.get<int64_t>()) < f.choices.size())
            f.defaultValue = f.choices[size_t(f.defaultValue.get<int64_t>())];
        FlagFieldWidgets w = makeFlagField(f, shown, changed, parent);
        w.field->setToolTip(tip);
        if (auto* combo = qobject_cast<QComboBox*>(w.field))
            choiceTips::refresh(combo);
        form->addRow(w.label, w.field);
        return;
    }
    auto* label = new QLabel(labelOf(p), parent);
    label->setToolTip(tip);
    if (type == "color")
    {
        form->addRow(label, colorSwatch(parent, value, changed));
        return;
    }
    if (type == "gradient")
    {
        form->addRow(label, gradientEditor(parent, value, changed));
        return;
    }
    if (type == "curve")
    {
        form->addRow(label, curveEditor(parent, value, changed));
        return;
    }
    if (type == "seed")
    {
        auto* row = new QWidget(parent);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* spin = new QSpinBox(row);
        spin->setRange(0, 1 << 30);
        spin->setValue(value.is_number() ? int(value.get<double>()) : 0);
        spin->setKeyboardTracking(false);
        QObject::connect(spin, &QSpinBox::valueChanged, spin, [changed](int v) { changed(v); });
        auto* dice = new QToolButton(row);
        dice->setIcon(icon("dice"));
        dice->setToolTip("Randomise the seed");
        QObject::connect(dice, &QToolButton::clicked, spin, [spin] { spin->setValue(int(QRandomGenerator::global()->bounded(100000))); });
        h->addWidget(spin, 1);
        h->addWidget(dice);
        spin->setToolTip(tip);
        form->addRow(label, row);
        return;
    }
    if (type == "image")
    {
        auto* picker = new ImagePicker(nullptr, labelOf(p), parent);
        picker->setValue(value.is_string() ? value.get<std::string>() : std::string());
        picker->setToolTip(tip);
        QObject::connect(picker, &ImagePicker::chosen, picker, [changed](const QString& path) { changed(path.toStdString()); });
        form->addRow(label, picker);
        return;
    }
    if (type == "code")
    {
        form->addRow(label);
        form->addRow(codeEditor(parent, value.is_string() ? value.get<std::string>() : std::string(), tip, changed));
        return;
    }
    form->addRow(label, dimLabel(parent, qs(value.dump())));
}

/** The editor for an unlinked input's value. */
void addInput(QFormLayout* form, QWidget* parent, const json& in, const SetFn& set)
{
    const std::string id = in.value("id", "");
    const std::string type = in.value("type", "float");
    QString tip = qs(id + ": " + type) + (in.contains("doc") ? "\n" + qs(in["doc"].get<std::string>()) : QString());
    if (type == "material")
        return;
    if (in.value("linked", false))
    {
        auto* l = dimLabel(parent, "linked");
        l->setToolTip(tip);
        form->addRow(labelOf(in), l);
        return;
    }
    const json value = in.contains("value") ? in["value"] : in.value("default", json());
    auto changed = [set, id](const json& v) { set(id, v); };
    if (type == "color")
    {
        auto* label = new QLabel(labelOf(in), parent);
        label->setToolTip(tip);
        form->addRow(label, colorSwatch(parent, value, changed));
        return;
    }
    if (type == "vector")
    {
        opt::Field f{.flag = id, .label = labelOf(in).toStdString(), .kind = opt::FieldKind::Vec3, .defaultValue = in.value("default", json())};
        json v3 = value;
        if (value.is_array() && value.size() != 3)
        {
            v3 = json::array();
            for (size_t k = 0; k < 3; ++k)
                v3.push_back(k < value.size() ? value[k] : json(0.0));
        }
        else if (value.is_number())
            v3 = json::array({value, value, value});
        FlagFieldWidgets w = makeFlagField(f, v3, changed, parent);
        w.field->setToolTip(tip);
        form->addRow(w.label ? w.label : new QLabel(labelOf(in), parent), w.field);
        return;
    }
    json p = in;
    if (type == "gray" && !p.contains("min"))
    {
        p["softMin"] = 0.0;
        p["softMax"] = 1.0;
    }
    json number = value.is_array() && !value.empty() ? value[0] : value;
    FlagFieldWidgets w = makeFlagField(numberField(p, false), number, changed, parent);
    w.field->setToolTip(tip);
    form->addRow(w.label, w.field);
}
} // namespace

json gradientStops(const json& value)
{
    const json& stops = value.is_object() ? value.value("stops", json::array()) : value;
    json out = json::array();
    if (stops.is_array())
        for (const json& s : stops)
            if (s.is_array() && s.size() == 2 && s[0].is_number())
                out.push_back(json::array({s[0].get<double>(), jsonOf(rgbaOf(s[1]))}));
    std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) { return a[0].get<double>() < b[0].get<double>(); });
    if (out.empty())
        out = json::array({json::array({0.0, json::array({0.0, 0.0, 0.0, 1.0})}), json::array({1.0, json::array({1.0, 1.0, 1.0, 1.0})})});
    return out;
}

json curvePoints(const json& value)
{
    const json& pts = value.is_object() ? value.value("points", json::array()) : value;
    json out = json::array();
    if (pts.is_array())
        for (const json& p : pts)
            if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number())
                out.push_back(json::array({p[0].get<double>(), p[1].get<double>()}));
    std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) { return a[0].get<double>() < b[0].get<double>(); });
    if (out.empty())
        out = json::array({json::array({0.0, 0.0}), json::array({1.0, 1.0})});
    return out;
}

double evalCurve(const json& points, bool smooth, double x)
{
    const size_t n = points.size();
    if (n == 0)
        return x;
    auto pt = [&](size_t k) { return std::pair<double, double>(points[k][0].get<double>(), points[k][1].get<double>()); };
    auto p0 = pt(0);
    if (x <= p0.first)
        return p0.second;
    for (size_t k = 1; k < n; ++k)
    {
        auto p1 = pt(k);
        if (x <= p1.first)
        {
            double span = std::max(p1.first - p0.first, 1e-6);
            double f = (x - p0.first) / span;
            if (!smooth)
                return p0.second + (p1.second - p0.second) * f;
            auto pm = k >= 2 ? pt(k - 2) : p0;
            auto pn = k + 1 < n ? pt(k + 1) : p1;
            double m0 = (p1.second - pm.second) / std::max(p1.first - pm.first, 1e-6) * span;
            double m1 = (pn.second - p0.second) / std::max(pn.first - p0.first, 1e-6) * span;
            double f2 = f * f, f3 = f2 * f;
            return (2 * f3 - 3 * f2 + 1) * p0.second + (f3 - 2 * f2 + f) * m0 + (3 * f2 - 2 * f3) * p1.second + (f3 - f2) * m1;
        }
        p0 = p1;
    }
    return p0.second;
}

QWidget* gradientEditor(QWidget* parent, const json& value, std::function<void(const json&)> changed)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* interp = new QComboBox(w);
    for (const char* mode : {"linear", "smooth", "constant"})
        interp->addItem(qs(opt::prettyLabel(mode)), mode);
    auto shared = std::make_shared<GradientBar*>(nullptr);
    auto emitValue = [shared, interp, changed] {
        if (*shared)
            changed(json{{"stops", (*shared)->stops()}, {"interpolation", interp->currentData().toString().toStdString()}});
    };
    auto* bar = new GradientBar(w, value, emitValue);
    *shared = bar;
    std::string mode = value.is_object() ? value.value("interpolation", "linear") : "linear";
    bar->interpolation = mode;
    interp->setCurrentIndex(std::max(0, interp->findData(qs(mode))));
    interp->setToolTip("How colours blend between stops");
    QObject::connect(interp, &QComboBox::activated, w, [bar, interp, emitValue](int) {
        bar->interpolation = interp->currentData().toString().toStdString();
        bar->update();
        emitValue();
    });
    v->addWidget(bar);
    v->addWidget(interp);
    return w;
}

QWidget* curveEditor(QWidget* parent, const json& value, std::function<void(const json&)> changed)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* smooth = new QCheckBox("Smooth", w);
    auto shared = std::make_shared<CurveView*>(nullptr);
    auto emitValue = [shared, smooth, changed] {
        if (*shared)
            changed(json{{"points", (*shared)->points()}, {"smooth", smooth->isChecked()}});
    };
    auto* view = new CurveView(w, value, emitValue);
    *shared = view;
    view->smooth = value.is_object() ? value.value("smooth", true) : true;
    smooth->setChecked(view->smooth);
    smooth->setToolTip("Smooth curve through the points, else straight lines");
    QObject::connect(smooth, &QCheckBox::toggled, w, [view, emitValue](bool on) {
        view->smooth = on;
        view->update();
        emitValue();
    });
    v->addWidget(view);
    v->addWidget(smooth);
    return w;
}

QWidget* nodePanel(QWidget* parent, const json& node, SetFn set)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    const std::string name = node.value("name", "");
    QString title = qs(node.value("label", node.value("type", ""))) + (name.empty() ? QString() : "  (" + qs(name) + ")");
    QLabel* head = heading(w, title);
    head->setToolTip(qs(node.value("type", "")) + (node.contains("category") ? ", " + qs(node["category"].get<std::string>()) : QString()));
    v->addWidget(head);
    if (node.contains("error"))
        v->addWidget(dimLabel(w, qs(node["error"].get<std::string>())));

    const json params = node.value("params", json::array());
    const std::string kind = node.value("kind", "");
    if (!params.empty())
    {
        if (kind == "graph" || kind == "group")
            v->addWidget(dimLabel(w, "Exposed parameters"));
        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        for (const json& p : params)
            addParam(form, w, p, set);
        v->addLayout(form);
    }

    bool anyInput = false;
    for (const json& in : node.value("inputs", json::array()))
        anyInput |= in.value("type", "") != "material";
    if (anyInput)
    {
        v->addWidget(heading(w, "Inputs"));
        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        for (const json& in : node.value("inputs", json::array()))
            addInput(form, w, in, set);
        v->addLayout(form);
    }
    if (params.empty() && !anyInput)
        v->addWidget(dimLabel(w, "No settings."));
    return w;
}
} // namespace lila::ui::texParams
