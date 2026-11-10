// The Curve Overlay: the selected joints' animation curves drawn over the viewport under the cursor, edited with the
// Graph Editor's grammar while Shift is held. Without Shift the viewport keeps the mouse, so posing, the Spinner and the
// camera work as usual. The horizontal axis is always the time slider's range.

#include "Puppeteer.h"

#include <lila/Actions.h>
#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Events.h>
#include <lila/Graph.h>
#include <lila/GraphModel.h>
#include <lila/Log.h>
#include <lila/Panels.h>
#include <lila/Prefs.h>
#include <lila/Viewport.h>

#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::ui;
using namespace lila::ui::graphed;
using lila::scene::Graph;
using lila::scene::NodeId;
namespace lanim = lila::scene::anim;

namespace puppeteer
{
namespace
{
constexpr const char* kScrimPref = "/curveOverlay/scrim";
constexpr double kMargin = 12.0;
constexpr double kChip = 22.0;
constexpr double kStripHeight = 22.0;
constexpr double kFlick = 6.0;

enum Kind : uint8_t
{
    KindT = 1,
    KindR = 2,
    KindS = 4,
};

glm::vec4 gray(float v, float a = 1.0f) { return {v, v, v, a}; }

double fps() { return lanim::playback().fps > 0.0 ? lanim::playback().fps : 30.0; }

/** Where everything sits in a view of the given size, in logical px. */
struct Frame
{
    QRectF names; // the joints' names line
    QRectF chips; // T R S, X Y Z, Graph Editor
    QRectF plot;
    QRectF strip;
    std::array<QRectF, 3> kindChips;
    std::array<QRectF, 3> axisChips;
    QRectF button;

    explicit Frame(double w, double h)
    {
        // Top centre: the corners belong to the HUD readouts and the view cube.
        const double row = 6 * (kChip + 2) + 16 + 8 + OverlayPainter::textWidth("Graph Editor") + 16;
        const double left = std::max(kMargin, (w - row) / 2);
        names = QRectF(left, kMargin, std::max(row, 120.0), 18);
        double x = left;
        const double y = names.bottom() + 6;
        for (QRectF& r : kindChips)
        {
            r = QRectF(x, y, kChip, kChip);
            x += kChip + 2;
        }
        x += 8;
        for (QRectF& r : axisChips)
        {
            r = QRectF(x, y, kChip, kChip);
            x += kChip + 2;
        }
        x += 8;
        button = QRectF(x, y, OverlayPainter::textWidth("Graph Editor") + 16, kChip);
        chips = QRectF(left, y, button.right() - left, kChip);
        // The strip stays clear of the camera name along the bottom edge.
        plot = QRectF(QPointF(48, chips.bottom() + 18), QPointF(std::max(60.0, w - 16), std::max(chips.bottom() + 40, h - kStripHeight - 40)));
        strip = QRectF(plot.left(), plot.bottom() + 6, plot.width(), kStripHeight);
    }
};

class CurveOverlay
{
public:
    bool open = false;
    GraphModel model;
    QPointer<QWidget> target;
    /** Display values at the plot's bottom and top. */
    double v0 = -1.0, v1 = 1.0;
    uint8_t kinds = 0; // empty shows every kind
    uint8_t axes = 0;
    bool needsGather = true;
    bool needsReload = false;
    std::vector<std::string> names;

    /** Each shown channel's value at every pixel column of the plot, kept while orbiting. */
    struct Cache
    {
        int columns = 0;
        double t0 = 0.0, t1 = 0.0;
        std::map<NodeId, std::vector<double>> values;
    } cache;

    enum class Drag : uint8_t
    {
        None,
        Box,
        Keys,
        Tangent,
        Pan,
        Scale,
        Scrub,
        RightPending,
    };
    Drag drag = Drag::None;
    QPointF press, last;
    bool moved = false;
    SelectOp boxOp = SelectOp::Replace;
    bool rightOnKey = false;
    double pressV0 = 0.0, pressV1 = 0.0;
    std::optional<KeySel> anchor;
    double anchorValue = 0.0;
    int lock = 0; // 1 time locked, 2 value locked

    int graphListener = -1;

    void setOpen(bool on);
    void redraw(bool all = false);
    void considerTarget(QWidget* w);
    bool isTarget(QWidget* w) const { return open && w && w == target.data(); }
    bool overTarget(QPointF global) const
    {
        QWidget* w = target.data();
        return w && w->isVisible() && w->rect().contains(w->mapFromGlobal(global).toPoint());
    }

    void gather();
    void applyFilter();
    void frameValues(bool selectedOnly);
    void ensureCache(const Frame& f);

    // Mapping for one frame of the view.
    double t0() const { return lanim::playback().minTime; }
    double t1() const { return std::max(lanim::playback().maxTime, t0() + 1.0 / fps()); }
    double xOf(const Frame& f, double t) const { return f.plot.left() + (t - t0()) / (t1() - t0()) * f.plot.width(); }
    double timeAt(const Frame& f, double x) const { return t0() + (x - f.plot.left()) / f.plot.width() * (t1() - t0()); }
    double yOf(const Frame& f, double v) const { return f.plot.bottom() - (v - v0) / (v1 - v0) * f.plot.height(); }
    double valueAt(const Frame& f, double y) const { return v0 + (f.plot.bottom() - y) / f.plot.height() * (v1 - v0); }
    double sx(const Frame& f) const { return f.plot.width() / (t1() - t0()); }
    double sy(const Frame& f) const { return f.plot.height() / (v1 - v0); }

    QPointF handlePoint(const Frame& f, const lanim::Curve& c, size_t i, bool out) const;
    std::optional<KeySel> hitKey(const Frame& f, QPointF pos) const;
    bool hitHandle(const Frame& f, QPointF pos, KeySel& key, int& side) const;
    NodeId hitCurve(const Frame& f, QPointF pos) const;
    double angleFor(const Frame& f, QPointF pos, double* weight) const;

    void drawScrim(OverlayPainter& p, const ViewInfo& view);
    void draw(OverlayPainter& p, const ViewInfo& view);
    void drawChrome(OverlayPainter& p, const Frame& f);
    void drawGrid(OverlayPainter& p, const Frame& f);
    void drawStrip(OverlayPainter& p, const Frame& f);
    QString readout() const;

    bool click(QPointF px, const ViewInfo& view);
    bool pressed(QMouseEvent* e, const ViewInfo& view);
    void dragged(QMouseEvent* e, const ViewInfo& view);
    void released(QMouseEvent* e, const ViewInfo& view);
    void scrubTo(const Frame& f, double x, bool holdPose);
    void cancelDrag();
    void showTangentMenu(QPoint global);
};

CurveOverlay& overlay()
{
    static CurveOverlay o;
    return o;
}

std::vector<QWidget*> viewports()
{
    std::vector<QWidget*> out;
    for (QWidget* top : QApplication::topLevelWidgets())
        for (QWidget* w : top->findChildren<QWidget*>())
            if (w->inherits("lila::ui::ViewportWidget") && w->isVisible())
                out.push_back(w);
    return out;
}

void requestFrame(QWidget* w)
{
    if (w)
        QCoreApplication::postEvent(w, new QEvent(QEvent::UpdateRequest));
}

/** Moving into another viewport moves the overlay there. */
class EnterFilter : public QObject
{
public:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::Enter && obj->isWidgetType() && obj->inherits("lila::ui::ViewportWidget"))
        {
            CurveOverlay& o = overlay();
            if (o.open && o.drag == CurveOverlay::Drag::None && o.target.data() != obj)
            {
                QWidget* old = o.target.data();
                o.target = static_cast<QWidget*>(obj);
                requestFrame(old);
                requestFrame(o.target.data());
            }
        }
        return false;
    }
};

EnterFilter*& enterFilter()
{
    static EnterFilter* f = nullptr;
    return f;
}

/** T, R or S from the channel's attribute; 0 for anything else. */
uint8_t kindOf(const Channel& ch)
{
    Graph& g = Graph::current();
    if (!g.node(ch.plug.node))
        return 0;
    const std::string& name = g.attr(ch.plug).name;
    return name == "translate" ? KindT : name == "rotate" ? KindR : name == "scale" ? KindS : 0;
}

void CurveOverlay::setOpen(bool on)
{
    if (on == open)
        return;
    if (!on)
    {
        if (model.edit.kind != KeyEdit::Kind::None)
            model.cancel();
        drag = Drag::None;
        open = false;
        if (graphListener >= 0)
            Graph::current().removeListener(graphListener);
        graphListener = -1;
        if (EnterFilter* f = enterFilter())
        {
            qApp->removeEventFilter(f);
            delete f;
            enterFilter() = nullptr;
        }
        model.channels.clear();
        model.reloadCurves();
        cache = {};
        redraw(true);
        return;
    }
    open = true;
    needsGather = true;
    target = nullptr;
    for (QWidget* w : viewports())
        if (w->rect().contains(w->mapFromGlobal(QCursor::pos())))
            target = w;
    graphListener = Graph::current().addListener([](scene::Change c, NodeId id) {
        CurveOverlay& o = overlay();
        if (c == scene::Change::Value)
        {
            const bool ours = std::any_of(o.model.channels.begin(), o.model.channels.end(), [&](const Channel& ch) { return realCurve(ch.curve) == id; });
            if (!ours)
                return;
            o.needsReload = true;
        }
        else
            o.needsGather = true;
        o.redraw();
    });
    enterFilter() = new EnterFilter;
    qApp->installEventFilter(enterFilter());
    redraw(true);
}

void CurveOverlay::redraw(bool all)
{
    if (all || !target)
    {
        for (QWidget* w : viewports())
            requestFrame(w);
        return;
    }
    requestFrame(target.data());
}

void CurveOverlay::considerTarget(QWidget* w)
{
    if (!open || !w || drag != Drag::None)
        return;
    if (!target || !target->isVisible())
        target = w;
}

void CurveOverlay::gather()
{
    Graph& g = Graph::current();
    std::vector<NodeId> nodes;
    names.clear();
    for (NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
        {
            nodes.push_back(id);
            names.push_back(n->name());
        }
    std::vector<KeySel> keep = model.sel;
    model.channels = GraphModel::gather(g, nodes, RotationDisplay::Euler);
    model.sel = std::move(keep);
    model.reloadCurves();
    applyFilter();
    cache = {};
    frameValues(false);
    needsGather = needsReload = false;
}

void CurveOverlay::applyFilter()
{
    for (Channel& ch : model.channels)
    {
        const uint8_t k = kindOf(ch);
        const bool kindOk = kinds == 0 || (k & kinds);
        const bool axisOk = axes == 0 || (ch.axis >= 0 && (axes & (1 << ch.axis)));
        ch.shown = kindOk && axisOk;
    }
    model.dropMissing();
    cache = {};
}

void CurveOverlay::frameValues(bool selectedOnly)
{
    double lo = 1e300, hi = -1e300;
    auto grow = [&](double v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };
    if (selectedOnly && !model.sel.empty())
    {
        for (const KeySel& k : model.sel)
            if (auto it = model.curves.find(k.curve); it != model.curves.end())
                if (int i = it->second.find(k.time); i >= 0)
                    grow(it->second.keys[size_t(i)].value);
    }
    else
        for (const Channel& ch : model.channels)
        {
            auto it = model.curves.find(ch.curve);
            if (!ch.shown || it == model.curves.end() || it->second.keys.empty())
                continue;
            auto eval = model.sampler(ch.curve, it->second);
            for (int s = 0; s <= 96; ++s)
                grow(eval(t0() + (t1() - t0()) * s / 96.0));
            for (const lanim::Key& k : it->second.keys)
                if (k.time >= t0() - kEps && k.time <= t1() + kEps)
                    grow(k.value);
        }
    if (lo > hi)
    {
        lo = -1.0;
        hi = 1.0;
    }
    if (hi - lo < 1e-3)
    {
        lo -= 1.0;
        hi += 1.0;
    }
    const double m = 0.1 * (hi - lo);
    v0 = lo - m;
    v1 = hi + m;
}

void CurveOverlay::ensureCache(const Frame& f)
{
    const int columns = std::max(2, int(f.plot.width()));
    if (cache.columns == columns && cache.t0 == t0() && cache.t1 == t1() && !cache.values.empty())
        return;
    cache = {};
    cache.columns = columns;
    cache.t0 = t0();
    cache.t1 = t1();
    for (const Channel& ch : model.channels)
    {
        auto it = model.curves.find(ch.curve);
        if (!ch.shown || it == model.curves.end() || it->second.keys.empty())
            continue;
        auto eval = model.sampler(ch.curve, it->second);
        std::vector<double>& out = cache.values[ch.curve];
        out.resize(size_t(columns));
        for (int i = 0; i < columns; ++i)
            out[size_t(i)] = eval(t0() + (t1() - t0()) * i / (columns - 1));
    }
}

QPointF CurveOverlay::handlePoint(const Frame& f, const lanim::Curve& c, size_t i, bool out) const
{
    const lanim::Key& k = c.keys[i];
    const QPointF pt(xOf(f, k.time), yOf(f, k.value));
    const double slope = out ? c.outSlope(i) : c.inSlope(i);
    const bool neighbour = out ? i + 1 < c.keys.size() : i > 0;
    if (c.weighted && neighbour)
    {
        const double dt = out ? c.keys[i + 1].time - k.time : k.time - c.keys[i - 1].time;
        const double h = (out ? k.outWeight : k.inWeight) * dt / 3.0;
        const double t = out ? k.time + h : k.time - h;
        const double v = out ? k.value + slope * h : k.value - slope * h;
        return QPointF(xOf(f, t), yOf(f, v));
    }
    QPointF d(sx(f), -slope * sy(f));
    const double len = std::hypot(d.x(), d.y());
    d = len > 0 ? d * (look::kHandleLength / len) : QPointF(look::kHandleLength, 0);
    return out ? pt + d : pt - d;
}

std::optional<KeySel> CurveOverlay::hitKey(const Frame& f, QPointF pos) const
{
    std::optional<KeySel> best;
    double bestD = look::kKeyHitRadius;
    for (const Channel& ch : model.channels)
    {
        auto it = model.curves.find(ch.curve);
        if (!ch.shown || it == model.curves.end())
            continue;
        for (const lanim::Key& k : it->second.keys)
            if (const double d = std::hypot(xOf(f, k.time) - pos.x(), yOf(f, k.value) - pos.y()); d < bestD)
            {
                bestD = d;
                best = KeySel{ch.curve, k.time};
            }
    }
    return best;
}

bool CurveOverlay::hitHandle(const Frame& f, QPointF pos, KeySel& key, int& side) const
{
    for (const KeySel& s : model.sel)
    {
        auto it = model.curves.find(s.curve);
        if (it == model.curves.end())
            continue;
        const int i = it->second.find(s.time);
        if (i < 0)
            continue;
        for (int sd : {-1, 1})
        {
            const QPointF h = handlePoint(f, it->second, size_t(i), sd > 0);
            if (std::hypot(h.x() - pos.x(), h.y() - pos.y()) < look::kHandleHitRadius)
            {
                key = s;
                side = sd;
                return true;
            }
        }
    }
    return false;
}

NodeId CurveOverlay::hitCurve(const Frame& f, QPointF pos) const
{
    if (pos.x() < f.plot.left() || pos.x() > f.plot.right())
        return scene::kNoNode;
    for (const auto& [id, values] : cache.values)
    {
        const size_t i = size_t(std::clamp((pos.x() - f.plot.left()) / f.plot.width() * double(values.size() - 1), 0.0, double(values.size() - 1)));
        if (std::abs(yOf(f, values[i]) - pos.y()) < look::kCurveHitRadius)
            return id;
    }
    return scene::kNoNode;
}

double CurveOverlay::angleFor(const Frame& f, QPointF pos, double* weight) const
{
    const KeySel& t = model.edit.tangent;
    auto it = model.curves.find(t.curve);
    if (it == model.curves.end())
        return 0.0;
    const lanim::Curve& c = it->second;
    const int i = c.find(t.time);
    if (i < 0)
        return 0.0;
    const lanim::Key& k = c.keys[size_t(i)];
    const QPointF pt(xOf(f, t.time), yOf(f, k.value));
    double dx = pos.x() - pt.x();
    const double dy = pos.y() - pt.y();
    const int side = model.edit.tangentSide;
    dx = side > 0 ? std::max(dx, 1.0) : std::min(dx, -1.0);
    const bool neighbour = side > 0 ? size_t(i) + 1 < c.keys.size() : i > 0;
    if (weight && c.weighted && neighbour)
    {
        const double dt = side > 0 ? c.keys[size_t(i) + 1].time - k.time : k.time - c.keys[size_t(i) - 1].time;
        *weight = std::clamp(std::abs(dx) / sx(f) / (dt / 3.0), 0.05, 3.0);
    }
    const double slope = (-dy / sy(f)) / (dx / sx(f));
    return std::clamp(std::atan(slope) * 180.0 / 3.14159265358979323846, -89.9, 89.9);
}

// Drawing ------------------------------------------------------------------------------------------------------------

void CurveOverlay::drawScrim(OverlayPainter& p, const ViewInfo& view)
{
    const float strength = float(std::clamp(PrefStore::get().value<double>(kPrefs, kScrimPref, 0.45), 0.0, 0.9));
    if (strength > 0.0f)
        p.rect(QRectF(0, 0, view.width, view.height), {0.08f, 0.08f, 0.09f, strength});
}

glm::vec4 curveColor(const Channel& ch, float alpha)
{
    if (ch.axis >= 0 && axisColors())
        return glm::vec4(viewportDisplay::axisColor(ch.axis), alpha);
    // Monotone: the axes differ by brightness, the kinds by dash.
    static constexpr float kLevels[] = {0.92f, 0.72f, 0.55f};
    return gray(ch.axis >= 0 ? kLevels[ch.axis] : 0.8f, alpha);
}

void CurveOverlay::drawGrid(OverlayPainter& p, const Frame& f)
{
    const double step = niceNumber(48.0 / sy(f));
    p.setShadow(false);
    for (double v = std::floor(v0 / step) * step; v <= v1; v += step)
    {
        const double y = yOf(f, v);
        if (y < f.plot.top() || y > f.plot.bottom())
            continue;
        const bool zero = std::abs(v) < step * 1e-3;
        p.line({f.plot.left(), y}, {f.plot.right(), y}, 1.0f, gray(1.0f, zero ? 0.16f : 0.07f));
        p.text({f.plot.left() - 6, y + 4}, formatValue(zero ? 0.0 : v), gray(0.75f, 0.8f), OverlayPainter::Right);
    }
    // The time slider's selected range: its keys are the ones range edits act on.
    if (const lanim::TimeSelection& s = lanim::timeSelection(); s.active)
    {
        const double a = std::max(f.plot.left(), xOf(f, s.start)), b = std::min(f.plot.right(), xOf(f, s.end));
        if (b > a)
            p.rect(QRectF(a, f.plot.top(), std::max(1.0, b - a), f.plot.height()), gray(1.0f, 0.06f));
    }
}

void CurveOverlay::drawStrip(OverlayPainter& p, const Frame& f)
{
    p.rect(f.strip, {0.05f, 0.05f, 0.06f, 0.55f});
    const double framesWide = (t1() - t0()) * fps();
    const double step = std::max(1.0, niceNumber(framesWide * 64.0 / std::max(1.0, f.strip.width())));
    const double first = std::ceil(lanim::frameOf(t0()) / step) * step;
    for (double fr = first; fr <= lanim::frameOf(t1()) + 1e-6; fr += step)
    {
        const double x = xOf(f, lanim::secondsOf(fr));
        p.line({x, f.strip.top()}, {x, f.strip.top() + 4}, 1.0f, gray(0.8f, 0.6f));
        p.text({x + 3, f.strip.bottom() - 6}, QString::number(fr, 'f', 0), gray(0.78f));
    }
    if (const lanim::TimeSelection& s = lanim::timeSelection(); s.active)
    {
        const double a = std::max(f.strip.left(), xOf(f, s.start)), b = std::min(f.strip.right(), xOf(f, s.end));
        if (b > a)
            p.rect(QRectF(a, f.strip.top(), std::max(2.0, b - a), 3), gray(0.9f, 0.8f));
    }
}

void CurveOverlay::drawChrome(OverlayPainter& p, const Frame& f)
{
    QString title;
    for (size_t i = 0; i < names.size() && i < 3; ++i)
        title += (i ? ", " : "") + QString::fromStdString(names[i]);
    if (names.size() > 3)
        title += QString("  +%1").arg(names.size() - 3);
    if (model.channels.empty())
        title = names.empty() ? QString("No selection") : title + QString("  No animated channels");
    p.label({f.names.left(), f.names.bottom() - 4}, title);

    auto chip = [&](const QRectF& r, const QString& text, bool on, int axis) {
        p.rect(r, on ? gray(0.86f, 0.95f) : gray(0.06f, 0.6f));
        p.text({r.center().x(), r.bottom() - 6}, text, on ? gray(0.08f) : gray(0.78f), OverlayPainter::Center);
        if (axis >= 0 && axisColors())
            p.rect(QRectF(r.left() + 4, r.bottom() - 3, r.width() - 8, 2), glm::vec4(viewportDisplay::axisColor(axis), 1.0f));
    };
    const char* kindLetters[] = {"T", "R", "S"};
    for (int i = 0; i < 3; ++i)
        chip(f.kindChips[size_t(i)], kindLetters[i], kinds == 0 || (kinds & (1 << i)), -1);
    const char* axisLetters[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i)
        chip(f.axisChips[size_t(i)], axisLetters[i], axes == 0 || (axes & (1 << i)), i);
    p.rect(f.button, gray(0.06f, 0.6f));
    p.text({f.button.center().x(), f.button.bottom() - 6}, "Graph Editor", gray(0.85f), OverlayPainter::Center);
}

void CurveOverlay::draw(OverlayPainter& p, const ViewInfo& view)
{
    if (needsGather)
        gather();
    else if (needsReload)
    {
        model.reloadCurves();
        applyFilter();
        needsReload = false;
    }
    const Frame f(view.width, view.height);
    if (f.plot.width() < 40 || f.plot.height() < 40)
        return;
    const bool editing = model.edit.kind != KeyEdit::Kind::None && (model.edit.moved || model.tweening());
    if (!editing)
        ensureCache(f);

    drawGrid(p, f);
    p.setShadow(true);
    const std::vector<NodeId> active = model.activeSet();
    auto isActive = [&](NodeId c) { return std::find(active.begin(), active.end(), c) != active.end(); };
    std::map<NodeId, lanim::Curve> shown;
    for (int pass = 0; pass < 2; ++pass)
        for (const Channel& ch : model.channels)
        {
            auto it = model.curves.find(ch.curve);
            if (!ch.shown || it == model.curves.end() || it->second.keys.empty() || isActive(ch.curve) != (pass == 1))
                continue;
            const lanim::Curve c = editing ? model.preview(ch.curve) : it->second;
            std::vector<QPointF> pts;
            if (editing)
            {
                auto eval = model.sampler(ch.curve, c);
                const int columns = std::max(2, int(f.plot.width() / 2));
                for (int i = 0; i < columns; ++i)
                {
                    const double t = t0() + (t1() - t0()) * i / (columns - 1);
                    pts.emplace_back(xOf(f, t), yOf(f, eval(t)));
                }
            }
            else if (auto v = cache.values.find(ch.curve); v != cache.values.end())
                for (size_t i = 0; i < v->second.size(); ++i)
                    pts.emplace_back(f.plot.left() + f.plot.width() * double(i) / double(v->second.size() - 1), yOf(f, v->second[i]));
            for (QPointF& pt : pts)
                pt.setY(std::clamp(pt.y(), f.plot.top() - 2, f.plot.bottom() + 2));
            const float alpha = active.empty() || pass == 1 ? 1.0f : float(look::kRecede);
            const glm::vec4 col = curveColor(ch, alpha);
            const float width = pass == 1 ? 2.0f : 1.5f;
            switch (kindOf(ch))
            {
            case KindR: p.dashedPolyline(pts, width, 7.0f, col); break;
            case KindS: p.dashedPolyline(pts, width, 2.5f, col); break;
            default: p.polyline(pts, width, col); break;
            }
            shown[ch.curve] = c;
        }

    // Keys, then the tangent handles of selected ones.
    p.setShadow(false);
    for (const Channel& ch : model.channels)
    {
        auto it = shown.find(ch.curve);
        if (it == shown.end())
            continue;
        const lanim::Curve& c = it->second;
        for (size_t i = 0; i < c.keys.size(); ++i)
        {
            const lanim::Key& k = c.keys[i];
            if (k.time < t0() - kEps || k.time > t1() + kEps)
                continue;
            const QPointF pt(xOf(f, k.time), yOf(f, k.value));
            if (pt.y() < f.plot.top() - 4 || pt.y() > f.plot.bottom() + 4)
                continue;
            const bool selected = model.isSelected(ch.curve, model.keyTimeBeforeDrag(ch.curve, k.time));
            if (selected)
            {
                const QPointF in = handlePoint(f, c, i, false), out = handlePoint(f, c, i, true);
                p.line(in, pt, 1.0f, gray(0.75f));
                p.line(pt, out, 1.0f, gray(0.75f));
                constexpr double b = look::kBrokenHandleSize;
                for (QPointF h : {in, out})
                    if (k.broken)
                        p.rect(QRectF(h.x() - b / 2, h.y() - b / 2, b, b), gray(0.85f));
                    else
                        p.disc(h, float(look::kUnifiedHandleRadius), gray(0.85f), 12);
            }
            const double hs = look::kKeySize / 2;
            const QRectF r(pt.x() - hs, pt.y() - hs, 2 * hs, 2 * hs);
            p.rect(r.adjusted(-1, -1, 1, 1), gray(0.0f, 0.7f));
            p.rect(r, selected ? gray(1.0f) : curveColor(ch, isActive(ch.curve) || active.empty() ? 1.0f : float(look::kRecede)));
            if (!selected)
                p.rect(r.adjusted(1.5, 1.5, -1.5, -1.5), gray(0.1f));
        }
    }

    const double now = xOf(f, Graph::current().time());
    if (now >= f.plot.left() - 1 && now <= f.plot.right() + 1)
        p.line({now, f.plot.top()}, {now, f.strip.bottom()}, 1.0f, gray(0.95f, 0.7f));
    drawStrip(p, f);
    if (now >= f.plot.left() - 1 && now <= f.plot.right() + 1)
        p.label({now + 4, f.strip.top() - 4}, QString::number(std::lround(lanim::frameOf(Graph::current().time()))));

    if (drag == Drag::Box && moved)
    {
        const QRectF b = QRectF(press, last).normalized();
        p.rect(b, gray(1.0f, 0.06f));
        p.polyline({b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft()}, 1.0f, gray(0.9f, 0.8f), true);
    }
    drawChrome(p, f);
    if (const QString text = readout(); !text.isEmpty())
        p.label(last + QPointF(16, 30), text);
}

QString CurveOverlay::readout() const
{
    if (!moved)
        return {};
    if (drag == Drag::Tangent)
        return std::isnan(model.edit.tangentAngle) ? QString() : QString("Angle %1°").arg(formatValue(model.edit.tangentAngle));
    if (drag != Drag::Keys || !anchor)
        return {};
    const Channel* ch = model.channelOf(anchor->curve);
    const lanim::KeyTransform x = model.transformFor(anchor->curve);
    QString text = QString("Frame %1  Value %2%3")
                       .arg(formatValue(lanim::frameOf(x.time(anchor->time, fps()))))
                       .arg(formatValue(x.value(anchorValue)))
                       .arg(ch ? ch->unit : QString());
    if (lock == 1)
        text += "  Time locked";
    else if (lock == 2)
        text += "  Value locked";
    return text;
}

// Input --------------------------------------------------------------------------------------------------------------

bool CurveOverlay::click(QPointF px, const ViewInfo& view)
{
    // The hook gets no widget: the view drawing the overlay is the one this size.
    const QWidget* w = target.data();
    if (!open || !w || std::lround(view.width) != w->width() || std::lround(view.height) != w->height())
        return false;
    const Frame f(view.width, view.height);
    const bool shift = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    auto toggle = [&](uint8_t& set, int bit) {
        const uint8_t b = uint8_t(1 << bit);
        // Shift adds to the isolated set; all three on is the same as none picked.
        set = shift ? uint8_t(set ^ b) : set == b ? uint8_t(0) : b;
        if (set == 7)
            set = 0;
    };
    for (int i = 0; i < 3; ++i)
    {
        if (f.kindChips[size_t(i)].contains(px))
        {
            toggle(kinds, i);
            applyFilter();
            frameValues(false);
            redraw();
            return true;
        }
        if (f.axisChips[size_t(i)].contains(px))
        {
            toggle(axes, i);
            applyFilter();
            frameValues(false);
            redraw();
            return true;
        }
    }
    if (f.button.contains(px))
    {
        panels::show("graphEditor");
        return true;
    }
    return false;
}

bool CurveOverlay::pressed(QMouseEvent* e, const ViewInfo& view)
{
    if (!open || !overTarget(e->globalPosition()))
        return false;
    const Qt::KeyboardModifiers mods = e->modifiers();
    if (mods & Qt::AltModifier)
        return false;
    const bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    // Shift is the curve layer; Ctrl alone joins it only once keys are picked.
    if (!shift && !(ctrl && !model.sel.empty() && e->button() == Qt::LeftButton))
        return false;
    const Frame f(view.width, view.height);
    press = last = e->position();
    moved = false;
    lock = 0;
    anchor.reset();
    if (f.strip.adjusted(0, -4, 0, 8).contains(press) && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton))
    {
        drag = Drag::Scrub;
        scrubTo(f, press.x(), e->button() == Qt::MiddleButton);
        return true;
    }
    if (e->button() == Qt::MiddleButton)
    {
        drag = Drag::Pan;
        pressV0 = v0;
        pressV1 = v1;
        return true;
    }
    if (e->button() == Qt::RightButton)
    {
        KeySel k;
        int side = 0;
        rightOnKey = hitKey(f, press).has_value() || hitHandle(f, press, k, side);
        drag = Drag::RightPending;
        pressV0 = v0;
        pressV1 = v1;
        return true;
    }
    if (e->button() != Qt::LeftButton)
        return false;
    // Ctrl toggles here (Shift is the layer); Ctrl+Shift adds.
    const SelectOp op = ctrl && shift ? SelectOp::Add : ctrl ? SelectOp::Toggle : SelectOp::Replace;
    KeySel handleKey;
    int side = 0;
    if (!ctrl && hitHandle(f, press, handleKey, side))
    {
        model.edit = {};
        model.edit.kind = KeyEdit::Kind::Tangent;
        model.edit.tangent = handleKey;
        model.edit.tangentSide = side;
        drag = Drag::Tangent;
        return true;
    }
    if (auto hit = hitKey(f, press))
    {
        if (op != SelectOp::Replace || !model.isSelected(hit->curve, hit->time))
            model.select(std::span(&*hit, 1), op);
        model.activeCurve = hit->curve;
        if (op != SelectOp::Toggle && model.isSelected(hit->curve, hit->time))
        {
            model.edit = {};
            model.edit.kind = KeyEdit::Kind::Transform;
            drag = Drag::Keys;
            anchor = *hit;
            if (auto it = model.curves.find(hit->curve); it != model.curves.end())
                if (int i = it->second.find(hit->time); i >= 0)
                    anchorValue = it->second.keys[size_t(i)].value;
        }
        else
            drag = Drag::None;
        redraw();
        return true;
    }
    drag = Drag::Box;
    boxOp = op;
    return true;
}

void CurveOverlay::scrubTo(const Frame& f, double x, bool holdPose)
{
    const double t = lanim::snapToFrame(std::clamp(timeAt(f, x), t0(), t1()));
    if (std::abs(t - Graph::current().time()) < kEps)
        return;
    try
    {
        ops::execute("currentTime", {{"args", {t}}, {"update", !holdPose}});
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void CurveOverlay::dragged(QMouseEvent* e, const ViewInfo& view)
{
    const Frame f(view.width, view.height);
    const QPointF pos = e->position();
    const QPointF total = pos - press;
    if (total.manhattanLength() > 3)
        moved = true;
    last = pos;
    switch (drag)
    {
    case Drag::Scrub: scrubTo(f, pos.x(), e->buttons() & Qt::MiddleButton); break;
    case Drag::Pan:
    {
        const double dv = total.y() / f.plot.height() * (pressV1 - pressV0);
        v0 = pressV0 + dv;
        v1 = pressV1 + dv;
        break;
    }
    case Drag::RightPending:
        if (total.manhattanLength() <= kFlick)
            break;
        if (rightOnKey)
        {
            // The marking menu takes the mouse from here, so the viewport's drag ends now.
            drag = Drag::None;
            QMouseEvent up(QEvent::MouseButtonRelease, pos, e->globalPosition(), Qt::RightButton, Qt::NoButton, e->modifiers());
            viewportHooks::releaseDrag(&up, view);
            const QPoint global = (e->globalPosition() - total).toPoint();
            QTimer::singleShot(0, [global] { popupTangentMarkingMenu(overlay().model, global, Qt::RightButton); });
            return;
        }
        drag = Drag::Scale;
        [[fallthrough]];
    case Drag::Scale:
    {
        // Scales about the value under the press: drag up to spread the curves, down to flatten them.
        const double k = std::exp(-total.y() * 0.01);
        const double pivot = pressV0 + (pressV1 - pressV0) * (f.plot.bottom() - press.y()) / f.plot.height();
        v0 = pivot + (pressV0 - pivot) / k;
        v1 = pivot + (pressV1 - pivot) / k;
        break;
    }
    case Drag::Keys:
    {
        model.edit.moved = moved;
        if (!moved)
            break;
        // Shift is held already, so drags lock by default; letting go of it moves freely.
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        const bool lockValue = shift && std::abs(total.x()) > std::abs(total.y());
        const bool lockTime = shift && !lockValue;
        lock = lockTime ? 1 : lockValue ? 2 : 0;
        model.edit.xform.timeOffset = lockTime ? 0.0 : std::round((timeAt(f, pos.x()) - timeAt(f, press.x())) * fps()) / fps();
        model.edit.displayOffset = lockValue ? 0.0 : valueAt(f, pos.y()) - valueAt(f, press.y());
        model.pushPreviews();
        break;
    }
    case Drag::Tangent:
    {
        model.edit.moved = moved;
        if (!moved)
            break;
        double weight = std::nan("");
        model.edit.tangentAngle = angleFor(f, pos, &weight);
        model.edit.tangentWeight = weight;
        model.pushPreviews();
        break;
    }
    default: break;
    }
}

void CurveOverlay::released(QMouseEvent* e, const ViewInfo& view)
{
    const Frame f(view.width, view.height);
    const Drag was = drag;
    drag = Drag::None;
    switch (was)
    {
    case Drag::RightPending: showTangentMenu(e->globalPosition().toPoint()); break;
    case Drag::Keys:
    case Drag::Tangent:
        model.commit();
        cache = {};
        break;
    case Drag::Box:
    {
        std::vector<KeySel> hits;
        if (!moved)
        {
            if (const NodeId c = hitCurve(f, press))
            {
                model.activeCurve = c;
                if (auto it = model.curves.find(c); it != model.curves.end())
                    for (const lanim::Key& k : it->second.keys)
                        hits.push_back({c, k.time});
                model.select(hits, boxOp);
            }
            else if (boxOp == SelectOp::Replace)
            {
                model.activeCurve = scene::kNoNode;
                model.select({}, SelectOp::Replace);
            }
            break;
        }
        const QRectF box = QRectF(press, last).normalized();
        for (const Channel& ch : model.channels)
        {
            auto it = model.curves.find(ch.curve);
            if (!ch.shown || it == model.curves.end())
                continue;
            for (const lanim::Key& k : it->second.keys)
                if (box.contains(QPointF(xOf(f, k.time), yOf(f, k.value))))
                    hits.push_back({ch.curve, k.time});
        }
        model.select(hits, boxOp);
        break;
    }
    default: break;
    }
    moved = false;
    anchor.reset();
    lock = 0;
    redraw();
}

void CurveOverlay::cancelDrag()
{
    if (model.edit.kind != KeyEdit::Kind::None)
        model.cancel();
    drag = Drag::None;
    moved = false;
    anchor.reset();
    lock = 0;
    redraw();
}

void CurveOverlay::showTangentMenu(QPoint global)
{
    QTimer::singleShot(0, [global] {
        QMenu menu;
        menu.addSection("Tangents");
        fillTangentMenu(&menu, overlay().model);
        menu.addSeparator();
        menu.addAction("Delete Keys", [] { overlay().model.deleteSelectedKeys(); })->setEnabled(!overlay().model.sel.empty());
        menu.addAction("Graph Editor", [] { panels::show("graphEditor"); });
        menu.exec(global);
        overlay().redraw();
    });
}

void close() { overlay().setOpen(false); }
} // namespace

void closeCurveOverlay() { close(); }

void registerCurveOverlayCommand(ext::Context& ctx);

void registerCurveOverlay(ext::Context& ctx)
{
    renderPasses::add(ctx, {.id = "puppeteer.curveOverlay.scrim", .stage = renderPasses::Stage::Scene, .order = 1000, .run = [](renderPasses::Context& c) {
                                CurveOverlay& o = overlay();
                                o.considerTarget(c.widget);
                                if (o.isTarget(c.widget))
                                    o.drawScrim(c.painter, c.view);
                            }});
    renderPasses::add(ctx, {.id = "puppeteer.curveOverlay", .stage = renderPasses::Stage::Overlay, .order = 1000, .run = [](renderPasses::Context& c) {
                                CurveOverlay& o = overlay();
                                if (o.isTarget(c.widget))
                                    o.draw(c.painter, c.view);
                            }});
    viewportHooks::setClickHook("puppeteer.curveOverlay", [](QPointF px, const ViewInfo& view) { return overlay().click(px, view); });
    viewportHooks::setDragHook("puppeteer.curveOverlay",
                               {.press = [](QMouseEvent* e, const ViewInfo& v) { return overlay().pressed(e, v); },
                                .move = [](QMouseEvent* e, const ViewInfo& v) { overlay().dragged(e, v); },
                                .release = [](QMouseEvent* e, const ViewInfo& v) { overlay().released(e, v); }});
    registerCurveOverlayCommand(ctx);
    ctx.track("puppeteer:curveOverlay", [] {
        close();
        viewportHooks::removeClickHook("puppeteer.curveOverlay");
        viewportHooks::removeDragHook("puppeteer.curveOverlay");
    });

    const char* cat = "Puppeteer Animate";
    actions::add(ctx, {.id = "puppeteer.curveOverlay", .label = "Curve Overlay", .category = cat,
                       .run = [] { overlay().setOpen(!overlay().open); },
                       .doc = "The selected joints' curves over the viewport. Hold Shift to edit them as in the Graph Editor: click or box "
                              "keys, drag keys and handles, Shift+MMB pans and Shift+RMB scales the graph vertically, Shift+RMB click or "
                              "flick sets tangents, Shift+LMB on the frame strip scrubs. Ctrl toggles keys once some are picked. Shift+F and "
                              "Shift+A frame the curves, Delete deletes keys. Space tap in Animate mode, Space or Esc closes.",
                       .checked = [] { return overlay().open; },
                       .icon = "chart-line",
                       .keywords = {"graph editor", "curves", "f-curves", "keys", "overlay"}});
    actions::add(ctx, {.id = "puppeteer.curveOverlay.frameSelected", .label = "Frame Selected Keys (Curve Overlay)", .category = cat,
                       .run =
                           [] {
                               CurveOverlay& o = overlay();
                               if (!o.open)
                                   return;
                               o.frameValues(true);
                               o.redraw();
                           },
                       .doc = "Fit the Curve Overlay's value range to the selected keys. Shift+F while it is open."});
    actions::add(ctx, {.id = "puppeteer.curveOverlay.frameAll", .label = "Frame All Curves (Curve Overlay)", .category = cat,
                       .run =
                           [] {
                               CurveOverlay& o = overlay();
                               if (!o.open)
                                   return;
                               o.frameValues(false);
                               o.redraw();
                           },
                       .doc = "Fit the Curve Overlay's value range to every curve it shows. Shift+A while it is open."});
    actions::add(ctx, {.id = "puppeteer.curveOverlay.delete", .label = "Delete (Curve Overlay)", .category = cat,
                       .run =
                           [] {
                               CurveOverlay& o = overlay();
                               if (o.open && !o.model.sel.empty())
                               {
                                   o.model.deleteSelectedKeys();
                                   o.redraw();
                                   return;
                               }
                               actions::run("edit.delete");
                           },
                       .doc = "Deletes the keys picked in the Curve Overlay; with none picked, or the overlay closed, deletes as usual."});
    actions::add(ctx, {.id = "puppeteer.curveOverlay.escape", .label = "Escape (Curve Overlay)", .category = cat,
                       .run =
                           [] {
                               CurveOverlay& o = overlay();
                               if (o.open && o.drag != CurveOverlay::Drag::None)
                                   o.cancelDrag();
                               else if (o.open)
                                   o.setOpen(false);
                               else
                                   actions::run("viewport.escape");
                           },
                       .doc = "Cancels a Curve Overlay drag or closes the overlay; otherwise Esc as usual."});
}

namespace
{
json curveOverlayState()
{
    CurveOverlay& o = overlay();
    json keys = json::array();
    for (const KeySel& k : o.model.sel)
        keys.push_back({{"curve", o.model.commandTarget(k.curve)}, {"frame", lanim::frameOf(k.time)}});
    json channels = json::array();
    for (const Channel& ch : o.model.channels)
        if (ch.shown)
            channels.push_back(o.model.commandTarget(ch.curve));
    QWidget* w = o.target.data();
    return {{"open", o.open},
            {"channels", channels},
            {"selectedKeys", keys},
            {"valueRange", {o.v0, o.v1}},
            {"view", w ? json{{"x", w->mapToGlobal(QPoint(0, 0)).x()}, {"y", w->mapToGlobal(QPoint(0, 0)).y()}, {"width", w->width()}, {"height", w->height()}}
                       : json(nullptr)}};
}
} // namespace

void registerCurveOverlayCommand(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "curveOverlay",
        .doc = "Opens or closes Puppeteer's Curve Overlay, and returns what it shows: whether it is open, its channels, the "
               "selected keys (curve and frame), the value range it frames and the viewport it draws in (screen position and size).",
        .flags = {{"open", "o", ArgType::Bool, nullptr, "Open (true) or close (false) it; leave out to only ask."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("open"))
                overlay().setOpen(a.get<bool>("open"));
            return curveOverlayState();
        },
    });
}
} // namespace puppeteer

