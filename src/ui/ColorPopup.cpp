#include "ui/ColorPopup.h"

#include "core/PrefStore.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QConicalGradient>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>

namespace lila::ui
{
namespace colorMath
{
double toSrgb(double c)
{
    c = std::max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double toLinear(double c)
{
    c = std::max(c, 0.0);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

glm::dvec3 toSrgb(const glm::dvec3& c) { return {toSrgb(c.x), toSrgb(c.y), toSrgb(c.z)}; }
glm::dvec3 toLinear(const glm::dvec3& c) { return {toLinear(c.x), toLinear(c.y), toLinear(c.z)}; }

QColor display(const glm::dvec3& linear, double alpha)
{
    const glm::dvec3 s = glm::clamp(toSrgb(linear), 0.0, 1.0);
    return QColor::fromRgbF(float(s.x), float(s.y), float(s.z), float(std::clamp(alpha, 0.0, 1.0)));
}

glm::dvec3 linear(const QColor& c) { return toLinear(glm::dvec3(c.redF(), c.greenF(), c.blueF())); }

glm::dvec3 rgbToHsv(const glm::dvec3& c, double fallbackHue, double fallbackSat)
{
    const double hi = std::max({c.x, c.y, c.z}), lo = std::min({c.x, c.y, c.z}), d = hi - lo;
    if (hi <= 1e-9)
        return {fallbackHue, fallbackSat, 0.0};
    if (d <= 1e-9 * hi)
        return {fallbackHue, 0.0, hi};
    double h = hi == c.x ? (c.y - c.z) / d : hi == c.y ? 2.0 + (c.z - c.x) / d : 4.0 + (c.x - c.y) / d;
    h = std::fmod(h / 6.0 + 1.0, 1.0);
    return {h, d / hi, hi};
}

glm::dvec3 hsvToRgb(const glm::dvec3& hsv)
{
    const double h = std::fmod(std::fmod(hsv.x, 1.0) + 1.0, 1.0) * 6.0, s = std::clamp(hsv.y, 0.0, 1.0), v = std::max(hsv.z, 0.0);
    const int i = std::min(int(h), 5);
    const double f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i)
    {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

glm::dvec3 linearToOklab(const glm::dvec3& c)
{
    const double l = std::cbrt(0.4122214708 * c.x + 0.5363325363 * c.y + 0.0514459929 * c.z);
    const double m = std::cbrt(0.2119034982 * c.x + 0.6806995451 * c.y + 0.1073969566 * c.z);
    const double s = std::cbrt(0.0883024619 * c.x + 0.2817188376 * c.y + 0.6299787005 * c.z);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

glm::dvec3 oklabToLinear(const glm::dvec3& lab)
{
    const double l = std::pow(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z, 3.0);
    const double m = std::pow(lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z, 3.0);
    const double s = std::pow(lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z, 3.0);
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

glm::dvec3 linearToOklch(const glm::dvec3& c, double fallbackHue)
{
    const glm::dvec3 lab = linearToOklab(glm::max(c, 0.0));
    const double chroma = std::hypot(lab.y, lab.z);
    const double h = chroma < 1e-5 ? fallbackHue : std::fmod(std::atan2(lab.z, lab.y) * 180.0 / std::numbers::pi + 360.0, 360.0);
    return {lab.x, chroma, h};
}

glm::dvec3 oklchToLinear(const glm::dvec3& lch)
{
    const double rad = lch.z * std::numbers::pi / 180.0;
    auto at = [&](double chroma) { return oklabToLinear({lch.x, chroma * std::cos(rad), chroma * std::sin(rad)}); };
    const double ceiling = std::max(1.0, std::pow(std::max(lch.x, 0.0), 3.0)) + 1e-6;
    auto fits = [&](const glm::dvec3& c) { return std::min({c.x, c.y, c.z}) >= -1e-6 && std::max({c.x, c.y, c.z}) <= ceiling; };
    if (glm::dvec3 c = at(lch.y); fits(c))
        return glm::max(c, 0.0);
    double lo = 0.0, hi = std::max(lch.y, 0.0);
    for (int i = 0; i < 24; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (fits(at(mid)) ? lo : hi) = mid;
    }
    return glm::clamp(at(lo), 0.0, ceiling);
}

QString hex(const glm::dvec3& linear, double alpha, bool withAlpha)
{
    const QColor c = display(linear, alpha);
    QString s = QString("#%1%2%3").arg(c.red(), 2, 16, QChar('0')).arg(c.green(), 2, 16, QChar('0')).arg(c.blue(), 2, 16, QChar('0'));
    if (withAlpha)
        s += QString("%1").arg(c.alpha(), 2, 16, QChar('0'));
    return s.toUpper();
}

std::optional<glm::dvec4> parseHex(QString t)
{
    t = t.trimmed();
    if (t.startsWith('#'))
        t.remove(0, 1);
    else if (t.startsWith("0x", Qt::CaseInsensitive))
        t.remove(0, 2);
    if (t.size() == 3 || t.size() == 4)
    {
        QString wide;
        for (QChar ch : t)
            wide += QString(2, ch);
        t = wide;
    }
    if (t.size() != 6 && t.size() != 8)
        return std::nullopt;
    bool ok = false;
    const uint v = t.toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    const uint rgb = t.size() == 8 ? v >> 8 : v;
    const double a = t.size() == 8 ? (v & 0xFF) / 255.0 : 1.0;
    return glm::dvec4(toLinear(((rgb >> 16) & 0xFF) / 255.0), toLinear(((rgb >> 8) & 0xFF) / 255.0), toLinear((rgb & 0xFF) / 255.0), a);
}
} // namespace colorMath

namespace colorLibrary
{
namespace
{
constexpr const char* kFile = "colors";

json toJson(const Swatch& s)
{
    json j = {{"color", {s.color.x, s.color.y, s.color.z}}};
    if (s.alpha != 1.0)
        j["alpha"] = s.alpha;
    if (!s.name.empty())
        j["name"] = s.name;
    return j;
}

std::optional<Swatch> swatchFrom(const json& j)
{
    if (!j.is_object() || !j.contains("color") || !j["color"].is_array() || j["color"].size() != 3)
        return std::nullopt;
    try
    {
        const json& c = j["color"];
        return Swatch{{c[0].get<double>(), c[1].get<double>(), c[2].get<double>()}, j.value("alpha", 1.0), j.value("name", std::string())};
    }
    catch (const json::exception&)
    {
        return std::nullopt;
    }
}

std::vector<Swatch> swatchesFrom(const json& arr)
{
    std::vector<Swatch> out;
    if (arr.is_array())
        for (const json& j : arr)
            if (auto s = swatchFrom(j))
                out.push_back(*s);
    return out;
}

Swatch srgb(int r, int g, int b, const char* name)
{
    return {colorMath::linear(QColor(r, g, b)), 1.0, name};
}

Palette starter()
{
    return {"Basic",
            {srgb(255, 0, 0, "Red"), srgb(255, 128, 0, "Orange"), srgb(255, 255, 0, "Yellow"), srgb(0, 255, 0, "Green"),
             srgb(0, 255, 255, "Cyan"), srgb(0, 0, 255, "Blue"), srgb(255, 0, 255, "Magenta"), srgb(255, 255, 255, "White"),
             srgb(191, 191, 191, "75% Grey"), srgb(128, 128, 128, "50% Grey"), srgb(118, 118, 118, "18% Grey (middle grey)"),
             srgb(64, 64, 64, "25% Grey"), srgb(0, 0, 0, "Black")}};
}
} // namespace

std::vector<Swatch> history() { return swatchesFrom(PrefStore::get().value(kFile, "/history")); }

void remember(const Swatch& s)
{
    std::vector<Swatch> h = history();
    std::erase_if(h, [&](const Swatch& o) { return colorMath::hex(o.color, o.alpha, true) == colorMath::hex(s.color, s.alpha, true); });
    h.insert(h.begin(), Swatch{s.color, s.alpha, {}});
    if (h.size() > kHistorySize)
        h.resize(kHistorySize);
    json arr = json::array();
    for (const Swatch& o : h)
        arr.push_back(toJson(o));
    PrefStore::get().set(kFile, "/history", arr);
}

void clearHistory() { PrefStore::get().set(kFile, "/history", json::array()); }

std::vector<Palette> palettes()
{
    std::vector<Palette> out;
    const json arr = PrefStore::get().value(kFile, "/palettes");
    if (arr.is_array())
        for (const json& p : arr)
            if (p.is_object())
                out.push_back({p.value("name", std::string("Palette")), swatchesFrom(p.value("swatches", json::array()))});
    if (out.empty())
        out.push_back(starter());
    return out;
}

Palette builtInPalette() { return starter(); }

void resetPalettes()
{
    PrefStore::get().remove(kFile, "/palettes");
    PrefStore::get().remove(kFile, "/palette");
}

void setPalettes(const std::vector<Palette>& ps)
{
    json arr = json::array();
    for (const Palette& p : ps)
    {
        json sw = json::array();
        for (const Swatch& s : p.swatches)
            sw.push_back(toJson(s));
        arr.push_back({{"name", p.name}, {"swatches", sw}});
    }
    PrefStore::get().set(kFile, "/palettes", arr);
}

std::string currentPalette() { return PrefStore::get().value<std::string>(kFile, "/palette", std::string()); }
void setCurrentPalette(const std::string& name) { PrefStore::get().set(kFile, "/palette", name); }

std::optional<Palette> readGpl(const std::filesystem::path& file, std::string* error)
{
    std::ifstream in(file);
    if (!in)
    {
        if (error)
            *error = "Can't open " + file.string();
        return std::nullopt;
    }
    Palette p{file.stem().string(), {}};
    std::string line;
    bool header = false;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!header)
        {
            header = line.rfind("GIMP Palette", 0) == 0;
            if (!header)
            {
                if (error)
                    *error = "Not a GIMP palette (no 'GIMP Palette' header)";
                return std::nullopt;
            }
            continue;
        }
        if (line.rfind("Name:", 0) == 0)
        {
            std::string n = line.substr(5);
            n.erase(0, n.find_first_not_of(" \t"));
            if (!n.empty())
                p.name = n;
            continue;
        }
        if (line.empty() || line[0] == '#' || line.rfind("Columns:", 0) == 0)
            continue;
        std::istringstream row(line);
        int r, g, b;
        if (!(row >> r >> g >> b))
            continue;
        std::string name;
        std::getline(row, name);
        name.erase(0, name.find_first_not_of(" \t"));
        if (name == "Untitled")
            name.clear();
        p.swatches.push_back({colorMath::linear(QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255))), 1.0, name});
    }
    return p;
}

bool writeGpl(const std::filesystem::path& file, const Palette& p)
{
    std::ofstream out(file);
    if (!out)
        return false;
    out << "GIMP Palette\nName: " << p.name << "\nColumns: 8\n#\n";
    for (const Swatch& s : p.swatches)
    {
        const QColor c = colorMath::display(s.color);
        out << c.red() << ' ' << c.green() << ' ' << c.blue() << '\t' << (s.name.empty() ? "Untitled" : s.name) << '\n';
    }
    return bool(out);
}
} // namespace colorLibrary

namespace colorPopup
{
namespace
{
using colorLibrary::Swatch;
using namespace colorMath;

constexpr double kMinKelvin = 1500.0, kMaxKelvin = 12000.0;
constexpr const char* kMime = "application/x-lila-color";
constexpr int kCell = 16, kGap = 2;

/** The black body tint the light's own kelvin conversion gives, for the temperature bar. */
glm::dvec3 kelvinTint(double kelvin)
{
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
    double g = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    double b = t >= 66.0 ? 255.0 : t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    glm::dvec3 out = toLinear(glm::clamp(glm::dvec3(r, g, b) / 255.0, 0.0, 1.0));
    return out / std::max({out.x, out.y, out.z, 1e-6});
}

void drawChecker(QPainter& p, const QRectF& r)
{
    p.save();
    p.setClipRect(r);
    p.fillRect(r, QColor(200, 200, 200));
    for (int y = 0; y * 5 < r.height(); ++y)
        for (int x = 0; x * 5 < r.width(); ++x)
            if ((x + y) % 2)
                p.fillRect(QRectF(r.left() + x * 5, r.top() + y * 5, 5, 5), QColor(140, 140, 140));
    p.restore();
}

void fillSwatch(QPainter& p, const QRectF& r, const glm::dvec3& linear, double alpha)
{
    if (alpha < 1.0)
        drawChecker(p, r);
    p.fillRect(r, display(linear, alpha));
}

QMimeData* mimeFor(const Swatch& s)
{
    auto* m = new QMimeData;
    m->setColorData(display(s.color, s.alpha));
    m->setData(kMime, QByteArray::fromStdString(json{{"color", {s.color.x, s.color.y, s.color.z}}, {"alpha", s.alpha}}.dump()));
    m->setText(hex(s.color, s.alpha, s.alpha < 1.0));
    return m;
}

std::optional<Swatch> swatchFrom(const QMimeData* m)
{
    if (m->hasFormat(kMime))
    {
        const json j = json::parse(m->data(kMime).toStdString(), nullptr, false);
        if (j.is_object() && j.contains("color") && j["color"].is_array() && j["color"].size() == 3)
            return Swatch{{j["color"][0].get<double>(), j["color"][1].get<double>(), j["color"][2].get<double>()}, j.value("alpha", 1.0), {}};
    }
    if (m->hasColor())
    {
        const QColor c = qvariant_cast<QColor>(m->colorData());
        return Swatch{linear(c), c.alphaF(), {}};
    }
    if (m->hasText())
        if (auto h = parseHex(m->text()))
            return Swatch{glm::dvec3(*h), h->w, {}};
    return std::nullopt;
}

void startDrag(QWidget* source, const Swatch& s)
{
    auto* drag = new QDrag(source);
    drag->setMimeData(mimeFor(s));
    QPixmap pm(20, 20);
    pm.fill(display(s.color));
    drag->setPixmap(pm);
    drag->exec(Qt::CopyAction);
}

/** Hue ring around a saturation/value square, both in display sRGB like Maya's and Krita's. */
class Wheel final : public QWidget
{
public:
    std::function<void(double h, double s, double v)> edited;
    double h = 0.0, s = 0.0, v = 1.0;

    explicit Wheel(QWidget* parent) : QWidget(parent) { setFixedSize(176, 176); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c = rect().toRectF().center();
        const double outer = width() * 0.5 - 1.0, inner = outer - kRing;

        QConicalGradient hue(c, 0.0);
        for (int i = 0; i <= 12; ++i)
            hue.setColorAt(i / 12.0, QColor::fromHsvF(float(i % 12) / 12.0f, 1.0f, 1.0f));
        QPainterPath ring;
        ring.addEllipse(c, outer, outer);
        ring.addEllipse(c, inner, inner);
        p.setPen(Qt::NoPen);
        p.fillPath(ring, hue);

        const double a = h * 2.0 * std::numbers::pi;
        const QPointF dir(std::cos(a), -std::sin(a));
        p.setPen(QPen(Qt::black, 4));
        p.drawLine(c + dir * (inner - 1), c + dir * (outer + 1));
        p.setPen(QPen(Qt::white, 2));
        p.drawLine(c + dir * (inner - 1), c + dir * (outer + 1));

        const QRectF sq = square();
        QLinearGradient across(sq.topLeft(), sq.topRight());
        across.setColorAt(0.0, Qt::white);
        across.setColorAt(1.0, QColor::fromHsvF(float(h), 1.0f, 1.0f));
        p.fillRect(sq, across);
        QLinearGradient down(sq.topLeft(), sq.bottomLeft());
        down.setColorAt(0.0, QColor(0, 0, 0, 0));
        down.setColorAt(1.0, Qt::black);
        p.fillRect(sq, down);

        const QPointF m(sq.left() + std::clamp(s, 0.0, 1.0) * sq.width(), sq.top() + (1.0 - std::clamp(v, 0.0, 1.0)) * sq.height());
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 3));
        p.drawEllipse(m, 5, 5);
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(m, 5, 5);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF d = e->position() - rect().toRectF().center();
        const double r = std::hypot(d.x(), d.y()), outer = width() * 0.5;
        mGrab = r >= outer - kRing - 3 && r <= outer + 3              ? Grab::Ring
                : square().adjusted(-6, -6, 6, 6).contains(e->position()) ? Grab::Square
                                                                          : Grab::None;
        mLast = e->position();
        mFineH = h, mFineS = s, mFineV = v;
        drag(e, false);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            drag(e, true);
    }

    void mouseReleaseEvent(QMouseEvent*) override { mGrab = Grab::None; }

private:
    static constexpr double kRing = 16.0;
    enum class Grab
    {
        None,
        Ring,
        Square,
    };

    QRectF square() const
    {
        const double half = (width() * 0.5 - kRing - 6.0) / std::numbers::sqrt2;
        const QPointF c = rect().toRectF().center();
        return {c.x() - half, c.y() - half, 2 * half, 2 * half};
    }

    /** Shift drags at a quarter speed; Ctrl snaps the hue to 15 degree steps. */
    void drag(QMouseEvent* e, bool moving)
    {
        const QPointF at = e->position();
        const bool fine = moving && (e->modifiers() & Qt::ShiftModifier);
        if (mGrab == Grab::Ring)
        {
            const QPointF d = at - rect().toRectF().center();
            double nh = std::fmod(std::atan2(-d.y(), d.x()) / (2.0 * std::numbers::pi) + 1.0, 1.0);
            if (fine)
            {
                const QPointF dl = mLast - rect().toRectF().center();
                const double was = std::atan2(-dl.y(), dl.x()) / (2.0 * std::numbers::pi);
                double step = std::atan2(-d.y(), d.x()) / (2.0 * std::numbers::pi) - was;
                step -= std::round(step);
                nh = std::fmod(mFineH + step * 0.25 + 1.0, 1.0);
            }
            if (e->modifiers() & Qt::ControlModifier)
                nh = std::fmod(std::round(nh * 24.0) / 24.0, 1.0);
            mFineH = h = nh;
        }
        else if (mGrab == Grab::Square)
        {
            const QRectF sq = square();
            if (fine)
            {
                mFineS = std::clamp(mFineS + (at.x() - mLast.x()) / sq.width() * 0.25, 0.0, 1.0);
                mFineV = std::clamp(mFineV - (at.y() - mLast.y()) / sq.height() * 0.25, 0.0, 1.0);
            }
            else
            {
                mFineS = std::clamp((at.x() - sq.left()) / sq.width(), 0.0, 1.0);
                mFineV = std::clamp(1.0 - (at.y() - sq.top()) / sq.height(), 0.0, 1.0);
            }
            s = mFineS, v = mFineV;
        }
        else
            return;
        mLast = at;
        update();
        if (edited)
            edited(h, s, v);
    }

    Grab mGrab = Grab::None;
    QPointF mLast;
    double mFineH = 0, mFineS = 0, mFineV = 0;
};

/** A gradient track with a handle; t is 0..1. */
class Track final : public QWidget
{
public:
    std::function<QColor(double t)> colorAt;
    std::function<void(double t)> edited;
    double t = 0.0;
    bool checker = false;

    explicit Track(QWidget* parent) : QWidget(parent)
    {
        setFixedHeight(14);
        setMinimumWidth(80);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRectF r = rect().toRectF().adjusted(3, 2, -3, -2);
        if (checker)
            drawChecker(p, r);
        QLinearGradient g(r.topLeft(), r.topRight());
        if (colorAt)
            for (int i = 0; i <= 24; ++i)
                g.setColorAt(i / 24.0, colorAt(i / 24.0));
        p.fillRect(r, g);
        const double x = r.left() + std::clamp(t, 0.0, 1.0) * r.width();
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 3));
        p.drawRoundedRect(QRectF(x - 2.5, 0.5, 5, height() - 1), 1.5, 1.5);
        p.setPen(QPen(Qt::white, 1.2));
        p.drawRoundedRect(QRectF(x - 2.5, 0.5, 5, height() - 1), 1.5, 1.5);
    }

    void mousePressEvent(QMouseEvent* e) override { set(e); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            set(e);
    }

private:
    void set(QMouseEvent* e)
    {
        const QRectF r = rect().toRectF().adjusted(3, 2, -3, -2);
        t = std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0);
        update();
        if (edited)
            edited(t);
    }
};

/** A wrapping row of swatch cells: click picks, drag carries the colour out, drops add one (where allowed). */
class SwatchGrid final : public QWidget
{
public:
    std::vector<Swatch> swatches;
    int minRows = 1;
    /** Draws empty slots up to this many cells, as drop targets. */
    int slots = 0;
    std::function<void(const Swatch&)> picked;
    std::function<void(int index, QPoint global)> menu;
    std::function<void(int index, const Swatch&)> dropped;

    explicit SwatchGrid(QWidget* parent) : QWidget(parent) { setMouseTracking(true); }

    void setSwatches(std::vector<Swatch> s)
    {
        swatches = std::move(s);
        setAcceptDrops(bool(dropped));
        updateGeometry();
        update();
    }

    QSize sizeHint() const override { return {columns() * (kCell + kGap), rows() * (kCell + kGap)}; }
    QSize minimumSizeHint() const override { return {kCell, rows() * (kCell + kGap)}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return rowsFor(w) * (kCell + kGap); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const int count = std::max(int(swatches.size()), slots);
        for (int i = 0; i < count; ++i)
        {
            const QRectF r = cell(i);
            if (i < int(swatches.size()))
            {
                fillSwatch(p, r, swatches[size_t(i)].color, swatches[size_t(i)].alpha);
                p.setPen(QColor(0, 0, 0, 120));
                p.drawRect(r.adjusted(0, 0, -1, -1));
            }
            else
            {
                p.setPen(palette().color(QPalette::Mid));
                p.drawRect(r.adjusted(0.5, 0.5, -1.5, -1.5));
            }
        }
        if (mDropAt >= 0)
        {
            const QRectF r = cell(mDropAt);
            p.setPen(QPen(palette().color(QPalette::Highlight), 2));
            p.drawLine(r.topLeft() - QPointF(1, 0), r.bottomLeft() - QPointF(1, 0));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        mPress = e->position();
        mPressed = at(e->position());
        if (e->button() == Qt::RightButton && menu)
            menu(mPressed, e->globalPosition().toPoint());
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int i = at(e->position());
        setToolTip(i >= 0 && i < int(swatches.size()) ? tip(swatches[size_t(i)]) : QString());
        if ((e->buttons() & Qt::LeftButton) && mPressed >= 0 && mPressed < int(swatches.size()) &&
            (e->position() - mPress).manhattanLength() > 6)
        {
            const int from = mPressed;
            mPressed = -1;
            startDrag(this, swatches[size_t(from)]);
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && mPressed >= 0 && mPressed == at(e->position()) && mPressed < int(swatches.size()) && picked)
            picked(swatches[size_t(mPressed)]);
        mPressed = -1;
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (dropped && swatchFrom(e->mimeData()))
            e->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        mDropAt = std::clamp(at(e->position(), true), 0, int(swatches.size()));
        update();
        e->acceptProposedAction();
    }

    void dragLeaveEvent(QDragLeaveEvent*) override
    {
        mDropAt = -1;
        update();
    }

    void dropEvent(QDropEvent* e) override
    {
        const int i = std::clamp(at(e->position(), true), 0, int(swatches.size()));
        mDropAt = -1;
        update();
        if (auto s = swatchFrom(e->mimeData()); s && dropped)
        {
            e->acceptProposedAction();
            dropped(i, *s);
        }
    }

private:
    static QString tip(const Swatch& s)
    {
        const QString h = hex(s.color, s.alpha, s.alpha < 1.0);
        return s.name.empty() ? h : QString::fromStdString(s.name) + "  " + h;
    }

    int columns() const { return std::max(1, (width() + kGap) / (kCell + kGap)); }
    int rowsFor(int w) const
    {
        const int cols = std::max(1, (w + kGap) / (kCell + kGap));
        const int count = std::max(int(swatches.size()) + (dropped ? 1 : 0), slots);
        return std::max(minRows, (count + cols - 1) / cols);
    }
    int rows() const { return rowsFor(width()); }

    QRectF cell(int i) const
    {
        const int cols = columns();
        return {double((i % cols) * (kCell + kGap)), double((i / cols) * (kCell + kGap)), double(kCell), double(kCell)};
    }

    int at(QPointF p, bool nearest = false) const
    {
        const int col = int(p.x() + (nearest ? kCell * 0.5 : 0.0)) / (kCell + kGap), row = int(p.y()) / (kCell + kGap);
        if (!nearest && col >= columns())
            return -1;
        return row * columns() + std::min(col, columns());
    }

    QPointF mPress;
    int mPressed = -1;
    int mDropAt = -1;
};

/** Old colour on the left (a click puts it back), the new one on the right (drag it onto a palette). */
class Compare final : public QWidget
{
public:
    Swatch before, now;
    std::function<void()> revert;

    explicit Compare(QWidget* parent) : QWidget(parent)
    {
        setFixedHeight(30);
        setToolTip("Left: the colour it opened with; click to swap back and forth with the new one. Right: the new colour (drag it to a palette).");
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRectF r = rect().toRectF();
        fillSwatch(p, QRectF(r.left(), r.top(), r.width() * 0.5, r.height()), before.color, before.alpha);
        fillSwatch(p, QRectF(r.center().x(), r.top(), r.width() * 0.5, r.height()), now.color, now.alpha);
        p.setPen(QColor(0, 0, 0, 140));
        p.drawRect(r.adjusted(0, 0, -1, -1));
    }

    void mousePressEvent(QMouseEvent* e) override { mPress = e->position(); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if ((e->buttons() & Qt::LeftButton) && (e->position() - mPress).manhattanLength() > 6)
        {
            mPress = QPointF(-1e6, -1e6);
            startDrag(this, e->position().x() < width() * 0.5 ? before : now);
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (mPress.x() > -1e5 && e->position().x() < width() * 0.5 && revert)
            revert();
    }

private:
    QPointF mPress;
};

/** A frozen screenshot of one screen with a magnifier; a click samples it. */
class Dropper final : public QWidget
{
public:
    Dropper(QScreen* screen, std::function<void(std::optional<QColor>)> done)
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool), mDone(std::move(done))
    {
        setAttribute(Qt::WA_DeleteOnClose);
        mShot = screen->grabWindow(0);
        mImage = mShot.toImage();
        setGeometry(screen->geometry());
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }

    void finish(std::optional<QColor> c)
    {
        if (auto done = std::exchange(mDone, nullptr))
            done(c);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.drawPixmap(rect(), mShot);
        const QPoint at = mapFromGlobal(QCursor::pos());
        if (!rect().contains(at))
            return;
        constexpr int kPixels = 11, kZoom = 9;
        const QPoint src = pixel(at);
        QRect loupe(at + QPoint(18, 18), QSize(kPixels * kZoom, kPixels * kZoom));
        if (loupe.right() > width())
            loupe.moveRight(at.x() - 18);
        if (loupe.bottom() + 20 > height())
            loupe.moveBottom(at.y() - 38);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(loupe, mImage, QRect(src - QPoint(kPixels / 2, kPixels / 2), QSize(kPixels, kPixels)));
        p.setPen(QPen(Qt::black, 1));
        p.drawRect(loupe.adjusted(0, 0, -1, -1));
        const QRect centre(loupe.left() + kPixels / 2 * kZoom, loupe.top() + kPixels / 2 * kZoom, kZoom, kZoom);
        p.setPen(Qt::white);
        p.drawRect(centre.adjusted(-1, -1, 0, 0));
        const QColor c = mImage.pixelColor(src);
        const QRect label(loupe.left(), loupe.bottom() + 2, loupe.width(), 18);
        p.fillRect(label, QColor(30, 30, 30, 230));
        p.fillRect(QRect(label.left() + 3, label.top() + 3, 12, 12), c);
        p.setPen(Qt::white);
        p.drawText(label.adjusted(20, 0, 0, 0), Qt::AlignVCenter, c.name().toUpper());
    }

    void mouseMoveEvent(QMouseEvent*) override { update(); }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton)
            finish(mImage.pixelColor(pixel(e->position().toPoint())));
        else
            finish(std::nullopt);
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
            finish(std::nullopt);
    }

private:
    QPoint pixel(QPoint local) const
    {
        const double sx = double(mImage.width()) / std::max(1, width()), sy = double(mImage.height()) / std::max(1, height());
        return {std::clamp(int(local.x() * sx), 0, mImage.width() - 1), std::clamp(int(local.y() * sy), 0, mImage.height() - 1)};
    }

    QPixmap mShot;
    QImage mImage;
    std::function<void(std::optional<QColor>)> mDone;
};

/** Samples a colour from anywhere on screen: one frozen overlay per screen, Esc or right click cancels. */
void grabScreen(std::function<void(std::optional<QColor>)> done)
{
    auto overlays = std::make_shared<std::vector<QPointer<Dropper>>>();
    auto finish = [overlays, done = std::move(done)](std::optional<QColor> c) {
        for (const QPointer<Dropper>& d : *overlays)
            if (d)
                d->close();
        overlays->clear();
        done(c);
    };
    for (QScreen* s : QGuiApplication::screens())
        overlays->push_back(new Dropper(s, finish));
    QScreen* under = QGuiApplication::screenAt(QCursor::pos());
    for (const QPointer<Dropper>& d : *overlays)
    {
        d->show();
        if (d->screen() == under)
        {
            d->activateWindow();
            d->grabKeyboard();
        }
    }
}

enum class Model
{
    Hsv,
    Rgb,
    Oklch,
};

enum class Space
{
    Linear,
    Srgb,
    Srgb8,
};

class Popup final : public QWidget
{
public:
    explicit Popup(Options o) : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), mOpts(std::move(o))
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::StrongFocus);
        mStart = mOpts.value;
        mCur = mStart;
        mOther = mStart;
        mGain = std::max({mCur.color.x, mCur.color.y, mCur.color.z, 1.0});
        const glm::dvec3 hsv = rgbToHsv(toSrgb(mCur.color / mGain));
        mHue = hsv.x, mSat = hsv.y;
        mModel = Model(std::clamp(PrefStore::get().value<int>("colors", "/model", 0), 0, 2));
        mSpace = Space(std::clamp(PrefStore::get().value<int>("colors", "/space", 0), 0, 2));
        build();
        refresh();
    }

    ~Popup() override
    {
        if (mAccepted && !(mCur == mStart))
            colorLibrary::remember({mCur.color, mCur.alpha, {}});
        if (mOpts.closed)
            mOpts.closed(mAccepted ? mCur : mStart, mAccepted);
    }

    void cancel()
    {
        mAccepted = false;
        if (mOpts.changed && !(mCur == mStart))
            mOpts.changed(mStart);
        close();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
            return cancel();
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            close();
            return;
        }
        if (e->key() == Qt::Key_I && e->modifiers() == Qt::NoModifier)
            return eyedropper();
        if (e->matches(QKeySequence::Copy))
        {
            QApplication::clipboard()->setText(mHex->text());
            return;
        }
        if (e->matches(QKeySequence::Paste))
        {
            pasteHex();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    QLabel* sectionLabel(const QString& text)
    {
        auto* l = new QLabel(text, this);
        QFont f = l->font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        l->setFont(f);
        l->setForegroundRole(QPalette::PlaceholderText);
        return l;
    }

    QToolButton* toolButton(const QString& text, const QString& tip)
    {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    }

    void build()
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 8, 10, 10);
        root->setSpacing(6);
        if (!mOpts.title.isEmpty())
        {
            auto* t = new QLabel(mOpts.title, this);
            QFont f = t->font();
            f.setBold(true);
            t->setFont(f);
            root->addWidget(t);
        }
        auto* columns = new QHBoxLayout;
        columns->setSpacing(12);
        root->addLayout(columns);

        auto* left = new QVBoxLayout;
        left->setSpacing(4);
        columns->addLayout(left, 1);
        auto* right = new QVBoxLayout;
        right->setSpacing(4);
        columns->addLayout(right);

        // Old/new, eyedropper, hex.
        mCompare = new Compare(this);
        mCompare->revert = [this] {
            const Value back = mCur;
            setValue(mOther);
            mOther = back;
        };
        auto* top = new QHBoxLayout;
        top->setSpacing(4);
        top->addWidget(mCompare, 1);
        auto* dropper = toolButton({}, "Pick a colour from anywhere on screen (I)");
        dropper->setIcon(icon("color-picker"));
        connect(dropper, &QToolButton::clicked, this, [this] { eyedropper(); });
        top->addWidget(dropper);
        left->addLayout(top);

        auto* hexRow = new QHBoxLayout;
        hexRow->setSpacing(4);
        hexRow->addWidget(sectionLabel("Hex"));
        mHex = new QLineEdit(this);
        mHex->setToolTip("sRGB hex: #RGB, #RRGGBB" + QString(mOpts.alpha ? " or #RRGGBBAA" : "") + ". Ctrl+C / Ctrl+V copy and paste it.");
        mHex->setMaximumWidth(96);
        connect(mHex, &QLineEdit::editingFinished, this, [this] {
            if (auto h = parseHex(mHex->text()))
                setSwatch({glm::dvec3(*h), mOpts.alpha && mHex->text().trimmed().remove('#').size() >= 8 ? h->w : mCur.alpha, {}});
            else
                refresh();
        });
        hexRow->addWidget(mHex);
        auto* copy = toolButton({}, "Copy the hex code (Ctrl+C)");
        copy->setIcon(icon("copy"));
        connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(mHex->text()); });
        auto* paste = toolButton({}, "Paste a hex code from the clipboard (Ctrl+V)");
        paste->setIcon(icon("clipboard"));
        connect(paste, &QToolButton::clicked, this, [this] { pasteHex(); });
        hexRow->addWidget(copy);
        hexRow->addWidget(paste);
        hexRow->addStretch(1);
        left->addLayout(hexRow);

        // Fields.
        auto* modeRow = new QHBoxLayout;
        modeRow->setSpacing(4);
        mModelBox = new QComboBox(this);
        mModelBox->addItems({"HSV", "RGB", "OKLCh"});
        mModelBox->setToolTip("HSV and RGB as in other apps; OKLCh is perceptual: equal steps in L look equally lighter, and hue keeps its "
                              "brightness.");
        mModelBox->setCurrentIndex(int(mModel));
        mModelBox->setFocusPolicy(Qt::NoFocus);
        connect(mModelBox, &QComboBox::currentIndexChanged, this, [this](int i) {
            mModel = Model(i);
            PrefStore::get().set("colors", "/model", i);
            refresh();
        });
        mSpaceBox = new QComboBox(this);
        mSpaceBox->addItems({"Linear", "sRGB", "sRGB 0-255"});
        mSpaceBox->setToolTip("The numbers' space. Linear is what Lila stores and renders with (Maya's rendering space); sRGB matches "
                              "image editors. The wheel and hex are always sRGB.");
        mSpaceBox->setCurrentIndex(int(mSpace));
        mSpaceBox->setFocusPolicy(Qt::NoFocus);
        connect(mSpaceBox, &QComboBox::currentIndexChanged, this, [this](int i) {
            mSpace = Space(i);
            PrefStore::get().set("colors", "/space", i);
            refresh();
        });
        modeRow->addWidget(mModelBox);
        modeRow->addWidget(mSpaceBox);
        modeRow->addStretch(1);
        left->addLayout(modeRow);

        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(4);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(1, 1);
        for (int i = 0; i < 4; ++i)
        {
            Row& r = mRows[size_t(i)];
            r.label = new QLabel(this);
            r.label->setMinimumWidth(12);
            r.track = new Track(this);
            r.spin = new QDoubleSpinBox(this);
            r.spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            r.spin->setKeyboardTracking(false);
            r.spin->setFixedWidth(56);
            r.spin->setAlignment(Qt::AlignRight);
            r.track->edited = [this, i](double t) { setComponent(i, t * range(i)); };
            r.track->colorAt = [this, i](double t) { return swatchColorFor(i, t * range(i)); };
            connect(r.spin, &QDoubleSpinBox::valueChanged, this, [this, i](double v) {
                if (!mSyncing)
                    setComponent(i, v);
            });
            grid->addWidget(r.label, i, 0);
            grid->addWidget(r.track, i, 1);
            grid->addWidget(r.spin, i, 2);
            if (i == 3 && !mOpts.alpha)
            {
                r.label->hide();
                r.track->hide();
                r.spin->hide();
            }
        }
        mRows[3].label->setText("A");
        mRows[3].track->checker = true;
        left->addLayout(grid);

        if (mOpts.temperature)
        {
            auto* row = new QHBoxLayout;
            row->setSpacing(4);
            mUseTemp = new QCheckBox(this);
            mUseTemp->setToolTip("Tint by a colour temperature");
            mUseTemp->setFocusPolicy(Qt::NoFocus);
            connect(mUseTemp, &QCheckBox::toggled, this, [this](bool on) {
                if (mSyncing)
                    return;
                mCur.useTemperature = on;
                report();
            });
            mKelvin = new Track(this);
            mKelvin->colorAt = [](double t) { return display(kelvinTint(kMinKelvin + (kMaxKelvin - kMinKelvin) * t)); };
            mKelvin->edited = [this](double t) {
                mCur.useTemperature = true;
                mCur.temperature = std::round((kMinKelvin + (kMaxKelvin - kMinKelvin) * t) / 50.0) * 50.0;
                report();
            };
            mKelvinSpin = new QDoubleSpinBox(this);
            mKelvinSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            mKelvinSpin->setKeyboardTracking(false);
            mKelvinSpin->setRange(1000, 40000);
            mKelvinSpin->setDecimals(0);
            mKelvinSpin->setSuffix(" K");
            mKelvinSpin->setFixedWidth(56);
            connect(mKelvinSpin, &QDoubleSpinBox::valueChanged, this, [this](double k) {
                if (mSyncing)
                    return;
                mCur.useTemperature = true;
                mCur.temperature = k;
                report();
            });
            row->addWidget(mUseTemp);
            row->addWidget(mKelvin, 1);
            row->addWidget(mKelvinSpin);
            left->addLayout(row);
        }

        // Recent and palettes.
        auto* recentRow = new QHBoxLayout;
        recentRow->addWidget(sectionLabel("Recent"));
        recentRow->addStretch(1);
        left->addLayout(recentRow);
        mHistory = new SwatchGrid(this);
        mHistory->slots = int(colorLibrary::kHistorySize);
        mHistory->setMinimumWidth(int(colorLibrary::kHistorySize) * (kCell + kGap));
        mHistory->picked = [this](const Swatch& s) { setSwatch(s); };
        mHistory->menu = [this](int, QPoint at) {
            QMenu m(this);
            m.addAction("Clear Recent", [this] {
                colorLibrary::clearHistory();
                mHistory->setSwatches({});
            });
            m.exec(at);
        };
        mHistory->setSwatches(colorLibrary::history());
        left->addWidget(mHistory);

        auto* palRow = new QHBoxLayout;
        palRow->setSpacing(2);
        mPaletteBox = new QComboBox(this);
        mPaletteBox->setFocusPolicy(Qt::NoFocus);
        mPaletteBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        connect(mPaletteBox, &QComboBox::activated, this, [this](int i) {
            mPaletteIndex = i;
            colorLibrary::setCurrentPalette(mPalettes[size_t(i)].name);
            showPalette();
        });
        auto* add = toolButton("+", "Add the current colour to this palette (or drag a colour onto it)");
        connect(add, &QToolButton::clicked, this, [this] { addSwatch(int(activePalette().swatches.size()), {mCur.color, mCur.alpha, {}}); });
        auto* more = toolButton(QString::fromUtf8("\xE2\x8B\xAF"), "Palettes");
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        menu->addAction("New Palette...", [this] { newPalette(); });
        menu->addAction("Rename Palette...", [this] { renamePalette(); });
        menu->addAction("Delete Palette", [this] { deletePalette(); });
        menu->addAction("Reset Basic Palette", [this] { resetPalette(); });
        menu->addAction("Reset All Palettes...", [this] { resetAllPalettes(); });
        menu->addSeparator();
        menu->addAction("Import .gpl...", [this] { importGpl(); });
        menu->addAction("Export .gpl...", [this] { exportGpl(); });
        more->setMenu(menu);
        palRow->addWidget(mPaletteBox, 1);
        palRow->addWidget(add);
        palRow->addWidget(more);
        left->addLayout(palRow);

        mPalette = new SwatchGrid(this);
        mPalette->minRows = 2;
        mPalette->setMinimumWidth(int(colorLibrary::kHistorySize) * (kCell + kGap));
        mPalette->picked = [this](const Swatch& s) { setSwatch(s); };
        mPalette->dropped = [this](int i, const Swatch& s) { addSwatch(i, s); };
        mPalette->menu = [this](int i, QPoint at) { swatchMenu(i, at); };
        left->addWidget(mPalette);
        left->addStretch(1);

        // Wheel and shades.
        mWheel = new Wheel(this);
        mWheel->edited = [this](double h, double s, double v) {
            mHue = h, mSat = s;
            mCur.color = toLinear(hsvToRgb({h, s, v})) * mGain;
            report(Source::Wheel);
        };
        right->addWidget(mWheel);
        right->addWidget(sectionLabel("Shades"));
        mShades = new SwatchGrid(this);
        mShades->setToolTip("Lighter and darker at the same hue and chroma (OKLCh)");
        mShades->picked = [this](const Swatch& s) { setSwatch(s, true); };
        right->addWidget(mShades);
        mHues = new SwatchGrid(this);
        mHues->picked = [this](const Swatch& s) { setSwatch(s, true); };
        right->addWidget(mHues);
        mTones = new SwatchGrid(this);
        mTones->picked = [this](const Swatch& s) { setSwatch(s, true); };
        right->addWidget(mTones);
        for (SwatchGrid* g : {mShades, mHues, mTones})
            g->setFixedWidth(mWheel->width());
        mHues->setToolTip("Neighbouring hues at the same lightness (OKLCh)");
        mTones->setToolTip("Toward grey and more saturated, same lightness (OKLCh)");
        right->addStretch(1);

        mPalettes = colorLibrary::palettes();
        const std::string current = colorLibrary::currentPalette();
        for (size_t i = 0; i < mPalettes.size(); ++i)
            if (mPalettes[i].name == current)
                mPaletteIndex = int(i);
        showPalette();
    }

    // Fields.

    enum class Source
    {
        Other,
        Wheel,
    };

    struct Row
    {
        QLabel* label = nullptr;
        Track* track = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    /** The three components in the current model and space. */
    glm::dvec3 components() const
    {
        switch (mModel)
        {
        case Model::Hsv:
        {
            if (mSpace == Space::Linear)
            {
                glm::dvec3 hsv = rgbToHsv(mCur.color, mHue, mSat);
                hsv.x *= 360.0;
                return hsv;
            }
            glm::dvec3 hsv = rgbToHsv(toSrgb(mCur.color), mHue, mSat);
            hsv.x *= 360.0;
            if (mSpace == Space::Srgb8)
                hsv.y *= 100.0, hsv.z *= 100.0;
            return hsv;
        }
        case Model::Rgb:
            return mSpace == Space::Linear ? mCur.color : mSpace == Space::Srgb ? toSrgb(mCur.color) : toSrgb(mCur.color) * 255.0;
        case Model::Oklch:
        {
            glm::dvec3 lch = linearToOklch(mCur.color, mHue * 360.0);
            lch.x *= 100.0;
            return lch;
        }
        }
        return {};
    }

    glm::dvec3 linearFrom(const glm::dvec3& c) const
    {
        switch (mModel)
        {
        case Model::Hsv:
        {
            glm::dvec3 hsv{c.x / 360.0, c.y, c.z};
            if (mSpace == Space::Srgb8)
                hsv.y /= 100.0, hsv.z /= 100.0;
            const glm::dvec3 rgb = hsvToRgb(hsv);
            return mSpace == Space::Linear ? rgb : toLinear(rgb);
        }
        case Model::Rgb: return mSpace == Space::Linear ? c : mSpace == Space::Srgb ? toLinear(c) : toLinear(c / 255.0);
        case Model::Oklch: return oklchToLinear({c.x / 100.0, c.y, c.z});
        }
        return c;
    }

    /** The track's full width in field units; fields may go past it (HDR). */
    double range(int i) const
    {
        if (i == 3)
            return 1.0;
        switch (mModel)
        {
        case Model::Hsv: return i == 0 ? 360.0 : mSpace == Space::Srgb8 ? 100.0 : 1.0;
        case Model::Rgb: return mSpace == Space::Srgb8 ? 255.0 : 1.0;
        case Model::Oklch: return i == 0 ? 100.0 : i == 1 ? 0.37 : 360.0;
        }
        return 1.0;
    }

    QColor swatchColorFor(int i, double v) const
    {
        if (i == 3)
            return display(mCur.color, v);
        glm::dvec3 c = components();
        c[i] = v;
        return display(linearFrom(c));
    }

    void setComponent(int i, double v)
    {
        if (i == 3)
        {
            mCur.alpha = std::clamp(v, 0.0, 1.0);
            return report();
        }
        glm::dvec3 c = components();
        c[i] = v;
        if (mModel == Model::Hsv)
        {
            if (i == 0)
                mHue = std::fmod(c.x / 360.0 + 1.0, 1.0);
            if (i == 1)
                mSat = std::clamp(c.y / (mSpace == Space::Srgb8 ? 100.0 : 1.0), 0.0, 1.0);
        }
        if (mModel == Model::Oklch && i == 2)
            mHue = rgbToHsv(toSrgb(oklchToLinear({0.7, 0.15, c.z}))).x;
        setLinear(linearFrom(c));
    }

    void setLinear(const glm::dvec3& c)
    {
        mCur.color = glm::max(c, 0.0);
        mGain = std::max({mCur.color.x, mCur.color.y, mCur.color.z, 1.0});
        const glm::dvec3 hsv = rgbToHsv(toSrgb(mCur.color / mGain), mHue, mSat);
        mHue = hsv.x, mSat = hsv.y;
        report();
    }

    void setSwatch(const Swatch& s, bool keepAlpha = false)
    {
        if (!keepAlpha && mOpts.alpha)
            mCur.alpha = s.alpha;
        setLinear(s.color);
    }

    void setValue(const Value& v)
    {
        mCur = v;
        mGain = std::max({v.color.x, v.color.y, v.color.z, 1.0});
        const glm::dvec3 hsv = rgbToHsv(toSrgb(v.color / mGain), mHue, mSat);
        mHue = hsv.x, mSat = hsv.y;
        report();
    }

    void report(Source from = Source::Other)
    {
        refresh(from);
        if (mOpts.changed)
            mOpts.changed(mCur);
    }

    void refresh(Source from = Source::Other)
    {
        mSyncing = true;
        if (from != Source::Wheel)
        {
            const glm::dvec3 hsv = rgbToHsv(toSrgb(mCur.color / mGain), mHue, mSat);
            mWheel->h = hsv.x, mWheel->s = hsv.y, mWheel->v = hsv.z;
        }
        mWheel->update();

        static const std::array<std::array<const char*, 3>, 3> kLabels{{{"H", "S", "V"}, {"R", "G", "B"}, {"L", "C", "h"}}};
        const glm::dvec3 c = components();
        for (int i = 0; i < 3; ++i)
        {
            Row& r = mRows[size_t(i)];
            r.label->setText(kLabels[size_t(mModel)][size_t(i)]);
            const double span = range(i);
            const bool hdr = mSpace == Space::Linear && (mModel == Model::Rgb || (mModel == Model::Hsv && i == 2));
            const bool whole = span >= 100.0 && !(mModel == Model::Oklch && i == 0);
            r.spin->setDecimals(whole ? 0 : mModel == Model::Oklch && i == 1 ? 3 : mModel == Model::Oklch ? 1 : 3);
            r.spin->setRange(0.0, hdr ? 100000.0 : mModel == Model::Oklch && i == 1 ? 0.5 : span);
            r.spin->setSingleStep(whole ? 1.0 : span / 100.0);
            if (!r.spin->hasFocus())
                r.spin->setValue(c[i]);
            r.track->t = c[i] / span;
            r.track->update();
        }
        if (mOpts.alpha)
        {
            mRows[3].spin->setDecimals(3);
            mRows[3].spin->setRange(0.0, 1.0);
            mRows[3].spin->setSingleStep(0.01);
            if (!mRows[3].spin->hasFocus())
                mRows[3].spin->setValue(mCur.alpha);
            mRows[3].track->t = mCur.alpha;
            mRows[3].track->update();
        }
        if (mOpts.temperature)
        {
            mUseTemp->setChecked(mCur.useTemperature);
            mKelvin->t = (std::clamp(mCur.temperature, kMinKelvin, kMaxKelvin) - kMinKelvin) / (kMaxKelvin - kMinKelvin);
            mKelvin->update();
            if (!mKelvinSpin->hasFocus())
                mKelvinSpin->setValue(mCur.temperature);
        }
        if (!mHex->hasFocus())
            mHex->setText(hex(mCur.color, mCur.alpha, mOpts.alpha && mCur.alpha < 1.0));

        mCompare->now = {effective(mCur), mCur.alpha, {}};
        mCompare->before = {effective(mOther), mOther.alpha, {}};
        mCompare->update();
        updateShades();
        mSyncing = false;
    }

    static glm::dvec3 effective(const Value& v) { return v.useTemperature ? v.color * kelvinTint(v.temperature) : v.color; }

    void updateShades()
    {
        const glm::dvec3 lch = linearToOklch(mCur.color, mHue * 360.0);
        std::vector<Swatch> shades, hues, tones;
        constexpr int n = 9;
        for (int i = 0; i < n; ++i)
        {
            const double f = i / double(n - 1);
            shades.push_back({oklchToLinear({0.12 + 0.86 * (1.0 - f), lch.y, lch.z}), 1.0, {}});
            hues.push_back({oklchToLinear({lch.x, std::max(lch.y, 0.04), lch.z + (f - 0.5) * 80.0}), 1.0, {}});
            tones.push_back({oklchToLinear({lch.x, std::max(lch.y, 0.08) * 2.0 * f, lch.z}), 1.0, {}});
        }
        mShades->setSwatches(std::move(shades));
        mHues->setSwatches(std::move(hues));
        mTones->setSwatches(std::move(tones));
    }

    // Palettes.

    colorLibrary::Palette& activePalette() { return mPalettes[size_t(std::clamp(mPaletteIndex, 0, int(mPalettes.size()) - 1))]; }

    void showPalette()
    {
        if (mPalettes.empty())
            mPalettes.push_back({"Palette", {}});
        mPaletteIndex = std::clamp(mPaletteIndex, 0, int(mPalettes.size()) - 1);
        mPaletteBox->clear();
        for (const colorLibrary::Palette& p : mPalettes)
            mPaletteBox->addItem(QString::fromStdString(p.name));
        mPaletteBox->setCurrentIndex(mPaletteIndex);
        mPalette->setSwatches(activePalette().swatches);
    }

    void savePalettes()
    {
        colorLibrary::setPalettes(mPalettes);
        colorLibrary::setCurrentPalette(activePalette().name);
        showPalette();
    }

    void addSwatch(int at, Swatch s)
    {
        auto& sw = activePalette().swatches;
        sw.insert(sw.begin() + std::clamp(at, 0, int(sw.size())), std::move(s));
        savePalettes();
    }

    void swatchMenu(int i, QPoint at)
    {
        QMenu m(this);
        const bool on = i >= 0 && i < int(activePalette().swatches.size());
        m.addAction("Add Current Colour", [this] { addSwatch(int(activePalette().swatches.size()), {mCur.color, mCur.alpha, {}}); });
        if (on)
        {
            m.addAction("Replace with Current", [this, i] {
                Swatch& s = activePalette().swatches[size_t(i)];
                s.color = mCur.color;
                s.alpha = mCur.alpha;
                savePalettes();
            });
            m.addAction("Rename...", [this, i] {
                const std::string old = activePalette().swatches[size_t(i)].name;
                const QString name = askText("Swatch Name", QString::fromStdString(old));
                if (!name.isNull())
                {
                    activePalette().swatches[size_t(i)].name = name.toStdString();
                    savePalettes();
                }
            });
            m.addAction("Copy Hex", [this, i] {
                const Swatch& s = activePalette().swatches[size_t(i)];
                QApplication::clipboard()->setText(hex(s.color, s.alpha, s.alpha < 1.0));
            });
            m.addAction("Delete", [this, i] {
                activePalette().swatches.erase(activePalette().swatches.begin() + i);
                savePalettes();
            });
        }
        m.exec(at);
    }

    /** Runs a dialog with the popup out of the way, since a popup closes on any click outside it. */
    template <class F> auto aside(F&& f)
    {
        hide();
        struct Back
        {
            Popup* p;
            ~Back()
            {
                p->show();
                p->activateWindow();
                p->setFocus();
            }
        } back{this};
        return f();
    }

    QString askText(const QString& title, const QString& start)
    {
        return aside([&] {
            bool ok = false;
            const QString t = QInputDialog::getText(nullptr, title, "Name:", QLineEdit::Normal, start, &ok).trimmed();
            return ok && !t.isEmpty() ? t : QString();
        });
    }

    void newPalette()
    {
        const QString name = askText("New Palette", "Palette");
        if (name.isNull())
            return;
        mPalettes.push_back({name.toStdString(), {}});
        mPaletteIndex = int(mPalettes.size()) - 1;
        savePalettes();
    }

    void renamePalette()
    {
        const QString name = askText("Rename Palette", QString::fromStdString(activePalette().name));
        if (name.isNull())
            return;
        activePalette().name = name.toStdString();
        savePalettes();
    }

    /** Puts the built-in Basic palette back as shipped, re-adding it if it was deleted. */
    void resetPalette()
    {
        const colorLibrary::Palette basic = colorLibrary::builtInPalette();
        auto it = std::find_if(mPalettes.begin(), mPalettes.end(), [&](const colorLibrary::Palette& p) { return p.name == basic.name; });
        if (it == mPalettes.end())
            it = mPalettes.insert(mPalettes.begin(), basic);
        else
            it->swatches = basic.swatches;
        mPaletteIndex = int(it - mPalettes.begin());
        savePalettes();
    }

    void resetAllPalettes()
    {
        const bool sure = aside([] {
            return QMessageBox::question(nullptr, "Reset All Palettes", "Delete every saved palette and go back to the default one?",
                                         QMessageBox::Reset | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Reset;
        });
        if (!sure)
            return;
        colorLibrary::resetPalettes();
        mPalettes = colorLibrary::palettes();
        mPaletteIndex = 0;
        showPalette();
    }

    void deletePalette()
    {
        mPalettes.erase(mPalettes.begin() + mPaletteIndex);
        mPaletteIndex = std::max(0, mPaletteIndex - 1);
        savePalettes();
    }

    void importGpl()
    {
        const QString file = aside([] { return fileDialog::getOpenFileName(nullptr, "Import Palette", {}, "GIMP Palette (*.gpl)"); });
        if (file.isEmpty())
            return;
        std::string error;
        if (auto p = colorLibrary::readGpl(file.toStdWString(), &error))
        {
            mPalettes.push_back(std::move(*p));
            mPaletteIndex = int(mPalettes.size()) - 1;
            savePalettes();
        }
    }

    void exportGpl()
    {
        const QString file = aside([this] {
            return fileDialog::getSaveFileName(nullptr, "Export Palette", QString::fromStdString(activePalette().name) + ".gpl", "GIMP Palette (*.gpl)");
        });
        if (!file.isEmpty())
            colorLibrary::writeGpl(file.toStdWString(), activePalette());
    }

    void pasteHex()
    {
        if (auto h = parseHex(QApplication::clipboard()->text()))
        {
            setSwatch({glm::dvec3(*h), mOpts.alpha ? h->w : mCur.alpha, {}});
            mHex->setText(hex(mCur.color, mCur.alpha, mOpts.alpha && mCur.alpha < 1.0));
        }
    }

    // Eyedropper.

    void eyedropper()
    {
        hide();
        QPointer<Popup> self(this);
        // Lets the popup leave the screen before it is captured.
        QTimer::singleShot(80, [self] {
            if (!self)
                return;
            grabScreen([self](std::optional<QColor> c) {
                if (!self)
                    return;
                self->show();
                self->activateWindow();
                self->setFocus();
                if (c)
                    self->setSwatch({linear(*c), 1.0, {}}, true);
            });
        });
    }

    Options mOpts;
    Value mStart, mCur;
    /** The left swatch: the start colour, or after a swap the one swapped away from. */
    Value mOther;
    double mGain = 1.0;
    /** Hue and saturation survive greys and black, so dragging value back up keeps them. */
    double mHue = 0.0, mSat = 0.0;
    Model mModel = Model::Hsv;
    Space mSpace = Space::Linear;
    bool mAccepted = true;
    bool mSyncing = false;

    Wheel* mWheel = nullptr;
    Compare* mCompare = nullptr;
    QLineEdit* mHex = nullptr;
    QComboBox* mModelBox = nullptr;
    QComboBox* mSpaceBox = nullptr;
    std::array<Row, 4> mRows;
    QCheckBox* mUseTemp = nullptr;
    Track* mKelvin = nullptr;
    QDoubleSpinBox* mKelvinSpin = nullptr;
    SwatchGrid* mHistory = nullptr;
    SwatchGrid* mPalette = nullptr;
    SwatchGrid* mShades = nullptr;
    SwatchGrid* mHues = nullptr;
    SwatchGrid* mTones = nullptr;
    QComboBox* mPaletteBox = nullptr;
    std::vector<colorLibrary::Palette> mPalettes;
    int mPaletteIndex = 0;
};

QPointer<Popup>& openPopup()
{
    static QPointer<Popup> p;
    return p;
}
} // namespace

void show(QPoint globalPos, Options options)
{
    if (Popup* old = openPopup())
        old->close();
    auto* popup = new Popup(std::move(options));
    popup->adjustSize();
    QRect r(globalPos, popup->size());
    if (QScreen* screen = QApplication::screenAt(globalPos))
    {
        const QRect avail = screen->availableGeometry();
        r.moveLeft(std::clamp(r.left(), avail.left(), avail.right() - r.width()));
        r.moveTop(std::clamp(r.top(), avail.top(), avail.bottom() - r.height()));
    }
    popup->move(r.topLeft());
    openPopup() = popup;
    popup->show();
    popup->setFocus();
}

std::optional<Value> exec(QPoint globalPos, Options options)
{
    QEventLoop loop;
    std::optional<Value> out;
    bool done = false;
    auto userClosed = std::move(options.closed);
    options.closed = [&, userClosed](const Value& v, bool accepted) {
        if (accepted)
            out = v;
        if (userClosed)
            userClosed(v, accepted);
        done = true;
        loop.quit();
    };
    show(globalPos, std::move(options));
    if (!done)
        loop.exec();
    return out;
}

QColor getColor(const QColor& initial, QWidget* anchor, const QString& title, bool alpha, std::function<void(const QColor&)> changed)
{
    Options o;
    o.value.color = linear(initial.isValid() ? initial : QColor(Qt::white));
    o.value.alpha = initial.isValid() ? initial.alphaF() : 1.0;
    o.alpha = alpha;
    o.title = title;
    // The popup's event loop can delete the anchor (a panel rebuilt under it); callers then hold pointers into it.
    const QPointer<QWidget> alive(anchor);
    if (changed)
        o.changed = [changed = std::move(changed), alive, anchored = anchor != nullptr](const Value& v) {
            if (!anchored || alive)
                changed(display(v.color, v.alpha));
        };
    const std::optional<Value> v = exec(placeFor(anchor), std::move(o));
    if (anchor && !alive)
        return QColor();
    return v ? display(v->color, alpha ? v->alpha : 1.0) : QColor();
}

void pickFromScreen(std::function<void(std::optional<glm::dvec3>)> done)
{
    if (Popup* open = openPopup())
        open->close();
    // Lets a menu or tooltip leave the screen before it is captured.
    QTimer::singleShot(80, [done = std::move(done)] {
        grabScreen([done](std::optional<QColor> c) { done(c ? std::optional(linear(*c)) : std::nullopt); });
    });
}

QToolButton* eyedropperButton(QWidget* parent, std::function<void(const glm::dvec3&)> picked)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon("color-picker"));
    b->setAutoRaise(true);
    b->setToolTip("Pick a colour from anywhere on screen");
    QObject::connect(b, &QToolButton::clicked, b, [b, picked = std::move(picked)] {
        QPointer<QToolButton> alive(b);
        pickFromScreen([alive, picked](std::optional<glm::dvec3> c) {
            if (alive && c)
                picked(*c);
        });
    });
    return b;
}

namespace
{
double brightest(const glm::dvec3& c) { return std::max({c.x, c.y, c.z}); }
} // namespace

ValueSlider::ValueSlider(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(14);
    setMinimumWidth(48);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    setToolTip("Brightness");
}

void ValueSlider::setColor(const glm::dvec3& linear)
{
    if (mDragging || linear == mColor)
        return;
    mColor = linear;
    update();
}

QRectF ValueSlider::track() const { return rect().toRectF().adjusted(3, 2, -3, -2); }

glm::dvec3 ValueSlider::colorAt(double x) const
{
    const QRectF r = track();
    const double target = toLinear(std::clamp((x - r.left()) / r.width(), 0.0, 1.0));
    const double from = brightest(mStart);
    return from > 1e-9 ? mStart * (target / from) : glm::dvec3(target);
}

void ValueSlider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    if (!isEnabled())
        p.setOpacity(0.4);
    const QRectF r = track();
    // Hue from the press colour while dragging, so the track keeps it after a pass through black.
    const glm::dvec3 hue = mDragging ? mStart : mColor;
    const double top = brightest(hue);
    const glm::dvec3 full = top > 1e-9 ? hue / top : glm::dvec3(1.0);
    QLinearGradient g(r.topLeft(), r.topRight());
    for (int i = 0; i <= 12; ++i)
        g.setColorAt(i / 12.0, display(full * toLinear(i / 12.0)));
    p.fillRect(r, g);
    const double t = std::clamp(toSrgb(brightest(mColor)), 0.0, 1.0);
    const double x = r.left() + t * r.width();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(QRectF(x - 2.5, 0.5, 5, height() - 1), 1.5, 1.5);
    p.setPen(QPen(Qt::white, 1.2));
    p.drawRoundedRect(QRectF(x - 2.5, 0.5, 5, height() - 1), 1.5, 1.5);
}

void ValueSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    mDragging = true;
    mStart = mColor;
    mouseMoveEvent(e);
}

void ValueSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (!mDragging)
        return;
    mColor = colorAt(e->position().x());
    update();
    if (dragged)
        dragged(mColor);
}

void ValueSlider::mouseReleaseEvent(QMouseEvent* e)
{
    if (!mDragging || e->button() != Qt::LeftButton)
        return;
    mDragging = false;
    update();
    if (released)
        released(mStart, mColor);
}

QWidget* withEyedropper(QWidget* swatch, std::function<void(const glm::dvec3&)> picked, ValueSlider* slider)
{
    auto* box = new QWidget(swatch->parentWidget());
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    h->addWidget(swatch);
    h->addWidget(eyedropperButton(box, std::move(picked)));
    if (slider)
    {
        h->addSpacing(4);
        h->addWidget(slider, 1);
    }
    return box;
}

QPoint placeFor(QWidget* anchor)
{
    if (anchor && anchor->isVisible() && anchor->height() <= 48 && anchor->width() <= 240)
        return anchor->mapToGlobal(QPoint(0, anchor->height() + 2));
    return QCursor::pos() + QPoint(12, 12);
}
} // namespace colorPopup
} // namespace lila::ui
