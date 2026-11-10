#include "brush/BrushUi.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Icons.h"
#include "ui/Overlay.h"
#include "ui/Tools.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"
#include "ui/FileDialog.h"
#include "ui/FlagFields.h"
#include "ui/ImagePicker.h"
#include "ui/SpinScrub.h"

#include "brush/Alphas.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QStandardItemModel>
#include <QUrl>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

namespace lila::brush::ui
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinRadius = 2.0, kMaxRadius = 2000.0;
constexpr const char* kOpenPref = "/brush/strokeOptionsOpen";
constexpr const char* kPressureOpenPref = "/brush/pressureOptionsOpen";

bool isViewport(QWidget* w) { return w && w->inherits("lila::ui::ViewportWidget"); }

int64_t nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

void stepRadius(BrushSettings& s, double factor)
{
    if (s.radiusUnit == RadiusUnit::World)
        s.worldRadius = std::clamp(s.worldRadius * factor, 1e-3, 1e6);
    else
        s.radius = std::clamp(s.radius * factor, kMinRadius, kMaxRadius);
}
} // namespace

// ------------------------------------------------------------------------------------------------ BrushControls

class BrushControls::Filter : public QObject
{
public:
    explicit Filter(BrushControls* c) : mControls(c) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::TabletPress:
        case QEvent::TabletMove:
            mControls->tablet(static_cast<QTabletEvent*>(e)->pressure());
            return false;
        case QEvent::ApplicationDeactivate:
            mControls->mArmed = Adjust::None;
            mControls->mArmedKey = 0;
            return false;
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride: break;
        default: return false;
        }
        // Qt filters each key event for the native window before its widget; only the widget's copy says where focus is.
        if (!obj->isWidgetType())
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        // Shift and Ctrl change what a stroke would do, which the tools' messages and cursors show.
        if (e->type() != QEvent::ShortcutOverride && (k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control))
            lila::ui::viewportTools::requestRedraw();
        QWidget* focus = QApplication::focusWidget();
        if (obj != focus || !isViewport(focus))
            return disarmOnRelease(e, k);
        const int key = k->key();
        const bool ours = key == Qt::Key_F || key == Qt::Key_B || key == Qt::Key_M ||
                          std::find(mControls->mExtraKeys.begin(), mControls->mExtraKeys.end(), key) != mControls->mExtraKeys.end();
        if (!ours || (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
            return disarmOnRelease(e, k);
        if (e->type() == QEvent::ShortcutOverride)
        {
            e->accept();
            return true;
        }
        if (k->isAutoRepeat())
            return true;
        return mControls->key(key, k->modifiers(), e->type() == QEvent::KeyPress);
    }

private:
    /** The held key's release still disarms when focus left the viewport or Ctrl/Alt went down, or the adjust sticks. */
    bool disarmOnRelease(const QEvent* e, const QKeyEvent* k)
    {
        if (e->type() == QEvent::KeyRelease && !k->isAutoRepeat() && k->key() == mControls->mArmedKey)
        {
            mControls->mArmed = Adjust::None;
            mControls->mArmedKey = 0;
        }
        return false;
    }

    BrushControls* mControls;
};

BrushControls::BrushControls(SettingsRef settings, std::function<void()> changed, std::function<void()> finished)
    : mSettings(std::move(settings)), mChanged(std::move(changed)), mFinished(std::move(finished))
{
}

BrushControls::~BrushControls() { deactivate(); }

void BrushControls::activate(std::vector<int> extraKeys, std::function<bool(int, Qt::KeyboardModifiers, bool)> onKey)
{
    mExtraKeys = std::move(extraKeys);
    mOnKey = std::move(onKey);
    if (!mFilter)
    {
        mFilter = std::make_unique<Filter>(this);
        qApp->installEventFilter(mFilter.get());
    }
}

void BrushControls::deactivate()
{
    if (mFilter && qApp)
        qApp->removeEventFilter(mFilter.get());
    mFilter.reset();
    mArmed = mAdjusting = Adjust::None;
}

bool BrushControls::key(int k, Qt::KeyboardModifiers mods, bool down)
{
    Adjust kind = Adjust::None;
    if (k == Qt::Key_B || (k == Qt::Key_F && !(mods & Qt::ShiftModifier)))
        kind = Adjust::Radius;
    else if (k == Qt::Key_M || (k == Qt::Key_F && (mods & Qt::ShiftModifier)))
        kind = Adjust::Strength;
    if (kind == Adjust::None)
        return mOnKey ? mOnKey(k, mods, down) : false;
    if (down)
    {
        mArmed = kind;
        mArmedKey = k;
        mArmedUsed = false;
        return true;
    }
    // A release we never saw pressed (pressed with focus elsewhere) belongs to whoever took the press.
    if (k != mArmedKey)
        return false;
    // A tapped F still frames the selection, as everywhere else; a tapped Shift+F is the tool's.
    if (k == Qt::Key_F && !mArmedUsed && mAdjusting == Adjust::None)
    {
        if (mArmed == Adjust::Radius)
            QTimer::singleShot(0, [] { lila::ui::actions::run(json("viewport.frameSelected")); });
        else if (mStrengthTap)
            QTimer::singleShot(0, mStrengthTap);
    }
    mArmed = Adjust::None;
    mArmedKey = 0;
    return true;
}

void BrushControls::tablet(double pressure)
{
    mPressure = std::clamp(pressure, 0.0, 1.0);
    mPressureAt = nowMs();
}

double BrushControls::pressure() const { return mPressureAt && nowMs() - mPressureAt < 100 ? mPressure : 1.0; }

StrokeInput BrushControls::input(const QMouseEvent* e) const
{
    return {glm::dvec2(e->position().x(), e->position().y()), pressure()};
}

bool BrushControls::mousePress(const QMouseEvent* e)
{
    const bool right = e->button() == Qt::RightButton;
    if (right)
    {
        if (mArmed == Adjust::None && !lila::ui::tools::softSelectAdjusting())
            return false;
        lila::ui::tools::markSoftSelectAdjusted();
    }
    else if (!lila::ui::tools::brushRadiusDrag(e, mArmed != Adjust::None))
        return false;
    mAdjusting = right ? Adjust::Strength : mArmed != Adjust::None ? mArmed : Adjust::Radius;
    mArmedUsed = true;
    mStart = e->position();
    const BrushSettings& s = mSettings();
    if (e->button() == Qt::MiddleButton)
        mFrom = mAdjusting == Adjust::Strength ? 0.0 : s.radiusUnit == RadiusUnit::World ? 1e-3 : kMinRadius;
    else
        mFrom = mAdjusting == Adjust::Strength ? s.strength : s.radiusUnit == RadiusUnit::World ? s.worldRadius : s.radius;
    return true;
}

bool BrushControls::mouseMove(const QMouseEvent* e)
{
    if (mAdjusting == Adjust::None)
        return false;
    // The button came up outside the viewport, so its release never reached the tool.
    if (e->buttons() == Qt::NoButton)
    {
        mAdjusting = Adjust::None;
        if (mFinished)
            mFinished();
        return false;
    }
    BrushSettings& s = mSettings();
    const double dx = e->position().x() - mStart.x();
    // Strength follows both axes: right or up raises it, left or down lowers it.
    if (mAdjusting == Adjust::Strength)
        s.strength = std::clamp(mFrom + (dx - (e->position().y() - mStart.y())) / 300.0, 0.0, 1.0);
    else if (s.radiusUnit == RadiusUnit::World)
        s.worldRadius = std::clamp(std::max(mFrom, 1e-3) * std::pow(2.0, dx / 100.0), 1e-3, 1e6);
    else
        s.radius = std::clamp(mFrom + dx, kMinRadius, kMaxRadius);
    if (mChanged)
        mChanged();
    return true;
}

bool BrushControls::mouseRelease(const QMouseEvent* e)
{
    if (mAdjusting == Adjust::None)
        return false;
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton && e->button() != Qt::RightButton)
        return true;
    mAdjusting = Adjust::None;
    if (mFinished)
        mFinished();
    return true;
}

bool BrushControls::keyPress(const QKeyEvent* e)
{
    if (e->key() != Qt::Key_BracketLeft && e->key() != Qt::Key_BracketRight)
        return false;
    stepRadius(mSettings(), e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25);
    if (mChanged)
        mChanged();
    if (mFinished)
        mFinished();
    return true;
}

// ------------------------------------------------------------------------------------------------ cursor

glm::dvec3 screenRight(const lila::ui::ViewInfo& view, const glm::dmat4& world)
{
    const glm::dvec3 right = glm::dvec3(glm::inverse(view.view)[0]);
    const glm::dvec3 local = glm::dvec3(glm::inverse(world) * glm::dvec4(right, 0.0));
    return glm::length(local) > 0.0 ? glm::normalize(local) : glm::dvec3(1.0, 0.0, 0.0);
}

QPointF ringPosition(const Stroke& stroke, QPointF hover)
{
    if (!stroke.active() || stroke.sampler().mode() != StrokeMode::Freehand || stroke.settings().lazyRadius <= 0.0)
        return hover;
    const glm::dvec2 b = stroke.sampler().brush();
    return {b.x, b.y};
}

void drawCursor(lila::ui::OverlayPainter& p, const lila::ui::ViewInfo& view, const BrushSettings& s, QPointF at,
                const std::optional<CursorSurface>& surface, const CursorOptions& o)
{
    const glm::vec4 rim = o.color.value_or(glm::vec4(0.95f, 0.95f, 0.95f, 1.0f)), dim(0.6f, 0.6f, 0.6f, 0.8f), strength(0.82f, 0.82f, 0.82f, 0.95f);
    if (o.stroke && o.stroke->active())
    {
        const StrokeSampler& sm = o.stroke->sampler();
        const QPointF cursor(sm.cursor().x, sm.cursor().y);
        if (sm.mode() == StrokeMode::Line)
        {
            const QPointF start(sm.start().x, sm.start().y);
            p.line(start, cursor, 1.5f, rim);
            p.disc(start, 3.0f, rim);
        }
        else if (sm.mode() == StrokeMode::Curve)
        {
            // The drawn path faint, the curve it will become bright.
            std::vector<QPointF> drawn, clean;
            for (const glm::dvec2& q : sm.path())
                drawn.push_back(QPointF(q.x, q.y));
            for (const glm::dvec2& q : sm.smoothedPath())
                clean.push_back(QPointF(q.x, q.y));
            p.polyline(drawn, 1.0f, dim);
            p.polyline(clean, 1.5f, rim);
        }
        else if (s.lazyRadius > 0.0)
        {
            p.line(at, cursor, 1.0f, dim);
            p.disc(cursor, 2.5f, dim);
        }
    }
    // Brush tools hide the system cursor, so the dot marks the exact point.
    p.disc(at, 2.5f, glm::vec4(0.05f, 0.05f, 0.05f, 0.9f));
    p.disc(at, 1.5f, rim);
    const double roundness = std::clamp(s.roundness, 0.01, 1.0);
    const double angle = s.angle * kPi / 180.0;
    if (surface)
    {
        const double r = s.radiusUnit == RadiusUnit::World ? s.worldRadius : s.radius * view.unitsPerPixel(surface->point);
        const glm::dvec3 n = glm::length(surface->normal) > 0.0 ? glm::normalize(surface->normal) : glm::dvec3(0, 1, 0);
        // The ellipse's long axis follows the tip angle from screen +x, laid into the surface.
        glm::dvec3 right = glm::dvec3(glm::inverse(view.view)[0]);
        right -= n * glm::dot(n, right);
        if (glm::length(right) < 1e-9)
            right = glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
        right = glm::normalize(right);
        const glm::dvec3 u = right * std::cos(angle) + glm::cross(n, right) * std::sin(angle);
        const glm::dvec3 v = glm::cross(n, u);
        auto ring = [&](double radius, double squeeze, int segs) {
            std::vector<QPointF> pts;
            for (int k = 0; k < segs; ++k)
            {
                const double a = 2.0 * kPi * k / segs;
                if (auto q = view.project(surface->point + (u * std::cos(a) + v * (std::sin(a) * squeeze)) * radius))
                    pts.push_back(*q);
            }
            return pts;
        };
        p.polyline(ring(r, 1.0, 64), 1.5f, rim, true);
        if (roundness < 1.0)
            p.polyline(ring(r, roundness, 48), 1.0f, dim, true);
        if (o.strengthRing)
            p.polyline(ring(r * s.strength, 1.0, 48), 1.5f, strength, true);
        if (o.normalTick)
        {
            auto c = view.project(surface->point), tip = view.project(surface->point + n * r * 0.35);
            if (c && tip)
                p.line(*c, *tip, 1.5f, rim);
        }
        return;
    }
    // Off the surface a world radius has no depth to be seen at, so the ring shows the screen radius.
    std::vector<QPointF> pts;
    for (int k = 0; k < 64; ++k)
    {
        const double a = 2.0 * kPi * k / 64;
        pts.push_back(at + QPointF(std::cos(a), std::sin(a)) * s.radius);
    }
    p.polyline(pts, 1.0f, dim, true);
    if (o.strengthRing)
    {
        for (QPointF& q : pts)
            q = at + (q - at) * s.strength;
        p.polyline(pts, 1.5f, strength, true);
    }
}

// ------------------------------------------------------------------------------------------------ StrokeOptions

namespace
{
namespace opt = lila::ops::options;

opt::Field numberField(const char* flag, const char* label, bool integer, double lo, double hi, double step, const char* doc,
                       const char* suffix = "")
{
    opt::Field f;
    f.flag = flag;
    f.label = label;
    f.kind = integer ? opt::FieldKind::Int : opt::FieldKind::Float;
    f.min = lo;
    f.max = hi;
    f.step = step;
    f.doc = doc;
    f.suffix = suffix;
    return f;
}

opt::Field checkField(const char* flag, const char* label, const char* doc)
{
    opt::Field f;
    f.flag = flag;
    f.label = label;
    f.kind = opt::FieldKind::Check;
    f.doc = doc;
    return f;
}

opt::Field choiceField(const char* flag, const char* label, std::vector<std::pair<std::string, std::string>> choices, const char* doc)
{
    opt::Field f;
    f.flag = flag;
    f.label = label;
    f.kind = opt::FieldKind::Choice;
    f.doc = doc;
    for (auto& [value, text] : choices)
    {
        f.choices.push_back(value);
        f.choiceLabels.push_back(text);
    }
    return f;
}

double numberOf(const json& v) { return v.is_number() ? v.get<double>() : v.is_boolean() ? double(v.get<bool>()) : 0.0; }

const QString kImageFilter = "Images (*.png *.exr *.tga *.tif *.tiff *.psd)";

/** The brush alpha library in the shared image picker, registered on first use. */
const lila::ui::imagePicker::Library* alphaLibrary()
{
    namespace ip = lila::ui::imagePicker;
    if (const ip::Library* lib = ip::library("alphas"))
        return lib;
    ip::addLibrary({
        .id = "alphas",
        .title = "Alphas",
        .entries =
            [] {
                std::vector<ip::Entry> out;
                for (const alphas::Entry& e : alphas::library())
                    out.push_back({e.ref, QString::fromStdString(e.name), e.file,
                                   QString::fromStdString(e.category.empty() ? e.sourceLabel : e.sourceLabel + ": " + e.category)});
                return out;
            },
        .userDir = alphas::userDir(),
        .projectDir = [] { return alphas::projectDir(); },
        .filter = kImageFilter,
        .projectTextures = false,
        .valueFor =
            [](const std::filesystem::path& file) {
                std::error_code ec;
                for (const alphas::Entry& e : alphas::library())
                    if (std::filesystem::equivalent(e.file, file, ec))
                        return e.ref;
                return lila::paths::toUtf8(file);
            },
    });
    return ip::library("alphas");
}

/** A header button over a body that it shows and hides, collapsed by default; the state is kept in the pref. */
QWidget* collapsible(QWidget* parent, QVBoxLayout* into, const char* title, const char* tip, const char* pref)
{
    auto* header = new QToolButton(parent);
    header->setText(title);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setAutoRaise(true);
    header->setToolTip(tip);
    into->addWidget(header);
    auto* body = new QWidget(parent);
    into->addWidget(body);
    const bool open = PrefStore::get().value<bool>("general", pref, false);
    body->setVisible(open);
    header->setIcon(lila::ui::icon(open ? "chevron-down" : "chevron-right"));
    QObject::connect(header, &QToolButton::clicked, header, [header, body, pref] {
        const bool show = !body->isVisible();
        body->setVisible(show);
        header->setIcon(lila::ui::icon(show ? "chevron-down" : "chevron-right"));
        PrefStore::get().set("general", pref, show);
    });
    return body;
}
} // namespace

StrokeOptions::StrokeOptions(QWidget* parent, SettingsRef settings, std::function<void()> changed)
    : QWidget(parent), mSettings(std::move(settings)), mChanged(std::move(changed))
{
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    mBody = collapsible(this, col, "Stroke", "Stabiliser, line strokes, jitter, tip shape, radial symmetry and pen pressure, shared by every brush",
                        kOpenPref);
    auto* bodyCol = new QVBoxLayout(mBody);
    bodyCol->setContentsMargins(8, 0, 0, 0);
    auto* form = new QFormLayout;
    bodyCol->addLayout(form);
    QFormLayout* target = form;

    using Get = std::function<json(const BrushSettings&)>;
    using Put = std::function<void(BrushSettings&, const json&)>;
    auto add = [&](opt::Field f, Get get, Put put) {
        f.defaultValue = get(BrushSettings{});
        lila::ui::FlagFieldWidgets w = lila::ui::makeFlagField(f, get(mSettings()), [this, put](const json& v) {
            put(mSettings(), v);
            edited();
        }, target->parentWidget());
        w.field->setToolTip(QString::fromStdString(f.doc));
        if (w.label)
            target->addRow(w.label, w.field);
        else
            target->addRow(w.field);
        mSync.push_back([this, set = w.set, get] { set(get(mSettings())); });
        return w.field;
    };
    auto number = [](double BrushSettings::* member) {
        return std::pair<Get, Put>{[member](const BrushSettings& s) { return json(s.*member); },
                                   [member](BrushSettings& s, const json& v) { s.*member = numberOf(v); }};
    };
    auto addNumber = [&](opt::Field f, double BrushSettings::* member) {
        auto [get, put] = number(member);
        return add(std::move(f), get, put);
    };

    add(choiceField("radiusUnit", "Radius in", {{"screen", "Screen"}, {"world", "World"}},
                    "Screen: the radius stays the same size on screen however far the surface is. World: a fixed size on the surface."),
        [](const BrushSettings& s) { return json(s.radiusUnit == RadiusUnit::World ? "world" : "screen"); },
        [](BrushSettings& s, const json& v) { s.radiusUnit = v == "world" ? RadiusUnit::World : RadiusUnit::Screen; });
    mWorldRadius = addNumber(numberField("worldRadius", "World radius", false, 0.001, 100000.0, 0.5, "Brush radius on the surface, in scene units", " cm"),
                             &BrushSettings::worldRadius);
    addNumber(numberField("flow", "Flow", false, 0.0, 1.0, 0.05, "How much each dab adds; strokes that build up do so more slowly below 1"),
              &BrushSettings::flow);

    add(choiceField("mode", "Stroke", {{"freehand", "Freehand"}, {"line", "Line"}, {"curve", "Curve"}},
                    "Line: drag from where the line starts to where it ends. Curve: draw the path freely and it is smoothed into a clean curve. Both lay their dabs on release"),
        [](const BrushSettings& s) { return json(s.mode == StrokeMode::Line ? "line" : s.mode == StrokeMode::Curve ? "curve" : "freehand"); },
        [](BrushSettings& s, const json& v) { s.mode = v == "line" ? StrokeMode::Line : v == "curve" ? StrokeMode::Curve : StrokeMode::Freehand; });
    addNumber(numberField("lazyRadius", "Lazy radius", false, 0.0, 500.0, 5.0,
                          "Lazy mouse: the brush trails the cursor on a string this long, so strokes come out steady", " px"),
              &BrushSettings::lazyRadius);
    addNumber(numberField("smoothing", "Smoothing", false, 0.0, 0.95, 0.05, "Eases each move toward the cursor instead of jumping to it"),
              &BrushSettings::smoothing);

    struct JitterField
    {
        const char* flag;
        const char* label;
        double hi, step;
        const char* doc;
        const char* suffix;
        double Jitter::* member;
    };
    const JitterField jitters[] = {
        {"jitterPosition", "Scatter", 4.0, 0.05, "Scatter each dab by up to this fraction of the radius", "", &Jitter::position},
        {"jitterSize", "Size jitter", 1.0, 0.05, "Shrink each dab by up to this fraction", "", &Jitter::size},
        {"jitterOpacity", "Opacity jitter", 1.0, 0.05, "Lighten each dab by up to this fraction", "", &Jitter::opacity},
        {"jitterAngle", "Angle jitter", 180.0, 5.0, "Turn each dab's tip by up to this many degrees", " deg", &Jitter::angle},
    };
    for (const JitterField& j : jitters)
    {
        double Jitter::* m = j.member;
        add(numberField(j.flag, j.label, false, 0.0, j.hi, j.step, j.doc, j.suffix), [m](const BrushSettings& s) { return json(s.jitter.*m); },
            [m](BrushSettings& s, const json& v) { s.jitter.*m = numberOf(v); });
    }

    addNumber(numberField("angle", "Tip angle", false, -360.0, 360.0, 5.0, "Tip angle, counterclockwise on screen", " deg"), &BrushSettings::angle);
    add(checkField("followStroke", "Follow stroke", "The tip angle turns with the direction of the stroke"),
        [](const BrushSettings& s) { return json(s.followStroke); }, [](BrushSettings& s, const json& v) { s.followStroke = numberOf(v) != 0.0; });
    addNumber(numberField("roundness", "Roundness", false, 0.01, 1.0, 0.05, "1 is round; lower squeezes the tip into an ellipse across its angle"),
              &BrushSettings::roundness);

    mAlpha = new lila::ui::ImagePicker(alphaLibrary(), "Alpha", mBody);
    mAlpha->setToolTip("A grayscale image that shapes the tip: white is where the brush acts, black where it doesn't. Lila's "
                       "alphas, extensions', yours and the project's; type to filter.");
    auto* alphaLabel = new QLabel("Alpha", mBody);
    form->addRow(alphaLabel, mAlpha);
    auto setAlpha = [this](const std::string& path) {
        BrushSettings& s = mSettings();
        if (path != s.alphaPath)
        {
            s.alphaPath = path;
            s.alpha.reset();
        }
        mAlpha->setValue(path);
        edited();
    };
    lila::ui::onMiddleClickReset(alphaLabel, [setAlpha] { setAlpha({}); }, "none", mAlpha);
    connect(mAlpha, &lila::ui::ImagePicker::chosen, this, [setAlpha](const QString& v) { setAlpha(v.toStdString()); });
    mSync.push_back([this] { mAlpha->setValue(mSettings().alphaPath); });

    add(numberField("radialCount", "Radial copies", true, 1, 64, 1, "Copies of every dab turned evenly about the object's axis; 1 is off"),
        [](const BrushSettings& s) { return json(s.radial.count); },
        [](BrushSettings& s, const json& v) { s.radial.count = int(std::lround(numberOf(v))); });
    add(choiceField("radialAxis", "Radial axis", {{"x", "X"}, {"y", "Y"}, {"z", "Z"}}, "The object axis the copies turn about, through the object's pivot origin"),
        [](const BrushSettings& s) { return json(std::string(1, char('x' + std::clamp(s.radial.axis, 0, 2)))); },
        [](BrushSettings& s, const json& v) {
            const std::string a = v.is_string() ? v.get<std::string>() : std::string("y");
            s.radial.axis = std::clamp(a.empty() ? 1 : a[0] - 'x', 0, 2);
        });

    // Pen tablets only, so out of the way until opened.
    QWidget* pressure = collapsible(mBody, bodyCol, "Pressure", "How pen pressure scales size, opacity and flow", kPressureOpenPref);
    target = new QFormLayout(pressure);
    target->setContentsMargins(8, 0, 0, 0);
    const char* pressureNames[] = {"Size", "Opacity", "Flow"};
    PressureCurve BrushSettings::* curves[] = {&BrushSettings::pressureSize, &BrushSettings::pressureOpacity, &BrushSettings::pressureFlow};
    for (int k = 0; k < 3; ++k)
    {
        PressureCurve BrushSettings::* c = curves[k];
        add(checkField("pressure", pressureNames[k], "Pen pressure scales this"), [c](const BrushSettings& s) { return json((s.*c).enabled); },
            [c](BrushSettings& s, const json& v) { (s.*c).enabled = numberOf(v) != 0.0; });
    }
    // One floor and curve for all three pressure targets.
    add(numberField("pressureFloor", "Minimum", false, 0.0, 1.0, 0.05, "What the lightest touch still gives"),
        [](const BrushSettings& s) { return json(s.pressureSize.floor); },
        [](BrushSettings& s, const json& v) { s.pressureSize.floor = s.pressureOpacity.floor = s.pressureFlow.floor = numberOf(v); });
    add(numberField("pressureCurve", "Curve", false, 0.1, 5.0, 0.1, "Below 1 a light touch already reaches far, above 1 it takes a firm press"),
        [](const BrushSettings& s) { return json(s.pressureSize.exponent); },
        [](BrushSettings& s, const json& v) { s.pressureSize.exponent = s.pressureOpacity.exponent = s.pressureFlow.exponent = numberOf(v); });
    sync();
}

void StrokeOptions::sync()
{
    for (auto& f : mSync)
        f();
    mWorldRadius->setEnabled(mSettings().radiusUnit == RadiusUnit::World);
}

void StrokeOptions::edited()
{
    BrushSettings& s = mSettings();
    mWorldRadius->setEnabled(s.radiusUnit == RadiusUnit::World);
    if (mChanged)
        mChanged();
}
} // namespace lila::brush::ui
