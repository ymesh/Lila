#include "ui/TurntableUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Turntable.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/DrawOverrides.h"
#include "ui/Hotkeys.h"
#include "ui/Overlay.h"
#include "ui/UiCommon.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToast.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;
namespace tt = scene::turntable;

constexpr const char* kControlsPref = "/turntable/controls";
constexpr const char* kRingPref = "/turntable/ring";
constexpr const char* kPitchPref = "/turntable/pitch";
constexpr const char* kKeysPref = "/turntable/keys";

void flipPref(const char* key, bool fallback) { PrefStore::get().set("general", key, !generalPref(key, fallback)); }
/** Seconds the on-view controls stay after the last turntable action when nothing turntable-related is selected. */
constexpr double kControlsLinger = 3.0;

struct TurntableUiState
{
    QTimer* timer = nullptr;
    double last = 0.0;
    double lastAction = -1e9;
    NodeId lastUsed = scene::kNoNode;
    // Hand turning.
    NodeId dragging = scene::kNoNode;
    double dragRaw = 0.0;
    double dragTime = 0.0;
    double dragSign = 1.0;
    glm::dvec3 pitchAxis{1.0, 0.0, 0.0}, pitchCentre{0.0};
    QPointF dragLast;
    bool dragSnap = false;
    struct Button
    {
        QRectF rect;
        int id = 0;
    };
    /** On-view controls as one viewport last drew them, for its clicks. */
    struct Controls
    {
        QRectF bar;
        std::vector<Button> buttons;
        QSizeF view;
    };
    /** Keyed by the viewport's isolation set, the one per-viewport pointer a ViewInfo carries. */
    std::map<const void*, Controls> controls;
};

TurntableUiState& turntableState()
{
    static TurntableUiState s;
    return s;
}

void redrawTurntables() { viewportTools::requestRedraw(); }

/** Keeps the clock running while anything turns; stops it once all are still. */
void wakeTurntables()
{
    TurntableUiState& s = turntableState();
    if (!s.timer)
        return;
    if (!s.timer->isActive())
    {
        s.last = tt::now();
        s.timer->start();
    }
    redrawTurntables();
}

void turntableTouched()
{
    turntableState().lastAction = tt::now();
    // A redraw once the controls have gone, so they don't linger on a still frame.
    QTimer::singleShot(int(kControlsLinger * 1000.0) + 50, [] { redrawTurntables(); });
    wakeTurntables();
}

void tickTurntables()
{
    TurntableUiState& s = turntableState();
    const double now = tt::now();
    const double dt = now - s.last;
    s.last = now;
    const bool moving = tt::advance(Graph::current(), dt, now);
    redrawTurntables();
    // A held turntable resumes on its own, so the clock keeps running for it.
    bool held = false;
    for (tt::Turntable* t : tt::all(Graph::current()))
        held = held || (t->held && t->playing);
    if (!moving && !held && s.dragging == scene::kNoNode)
        s.timer->stop();
}

/** The turntable the UI acts on: the selected one or the one the selection is on, else the last used, else the first. */
tt::Turntable* activeTurntable()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
    {
        if (tt::Turntable* t = tt::get(g, id))
            return turntableState().lastUsed = t->id(), t;
        if (NodeId on = tt::turntableOf(g, id); on != scene::kNoNode)
            return turntableState().lastUsed = on, tt::get(g, on);
    }
    if (tt::Turntable* t = tt::get(g, turntableState().lastUsed))
        return t;
    const auto every = tt::all(g);
    return every.empty() ? nullptr : every.front();
}

/** Whether the selection is a turntable or something on one. */
bool selectionOnTurntableNow()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
        if (tt::get(g, id) || tt::turntableOf(g, id) != scene::kNoNode)
            return true;
    return false;
}

void runTurntableCommand(const char* name, json call = json::object())
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    turntableTouched();
}

QString turntableSpeedText(const tt::Turntable& t)
{
    const double seconds = std::abs(360.0 / tt::cruiseSpeed(t));
    return QString("%1 s a turn").arg(seconds, 0, 'f', seconds < 10.0 ? 1 : 0);
}

// The ring each turntable draws on the ground round its members, with a tick at its front.

double turntableRingRadius(const scene::Node& node) { return std::max(tt::reach(Graph::current(), node.id()) * 1.05, 5.0); }

std::vector<glm::dvec3> turntableRingPoints(const scene::Node& node, const glm::dmat4& world)
{
    // Unscaled frame: the ring sits at the pivot, round the axis.
    const glm::dvec3 up = glm::normalize(glm::dmat3(world) * scene::sceneUp());
    const glm::dvec3 centre(world[3]);
    const glm::dvec3 side = glm::normalize(glm::cross(up, std::abs(up.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1)));
    const glm::dvec3 other = glm::cross(up, side);
    const double r = turntableRingRadius(node);
    std::vector<glm::dvec3> pts;
    for (int i = 0; i <= 96; ++i)
    {
        const double a = glm::two_pi<double>() * i / 96.0;
        pts.push_back(centre + (side * std::cos(a) + other * std::sin(a)) * r);
    }
    // The front: restAngle turned by the live angle, from the side opposite the camera-independent "other" axis.
    const auto* t = static_cast<const tt::Turntable*>(&node);
    const double front = glm::radians(t->as<double>(tt::Turntable::kRestAngle) + t->angle);
    const glm::dvec3 dir = glm::dvec3(glm::rotate(glm::dmat4(1.0), front, up) * glm::dvec4(other, 0.0));
    pts.push_back(centre + dir * r * 0.88);
    pts.push_back(centre + dir * r * 1.06);
    // The ring tilts with the model.
    if (t->pitch != 0.0)
    {
        const glm::dmat4 tilt = glm::translate(glm::dmat4(1.0), t->pitchCentre) * glm::rotate(glm::dmat4(1.0), glm::radians(t->pitch), t->pitchAxis) *
                                glm::translate(glm::dmat4(1.0), -t->pitchCentre);
        for (glm::dvec3& q : pts)
            q = glm::dvec3(tilt * glm::dvec4(q, 1.0));
    }
    return pts;
}

void drawTurntableRing(drawOverrides::DrawContext& c)
{
    if (!generalPref(kRingPref, true))
        return;
    const std::vector<glm::dvec3> pts = turntableRingPoints(c.node, c.world);
    for (size_t i = 0; i + 1 < pts.size() - 2; ++i)
    {
        c.frame.lines.push_back({glm::dvec3(pts[i]), c.color});
        c.frame.lines.push_back({glm::dvec3(pts[i + 1]), c.color});
    }
    c.frame.lines.push_back({glm::dvec3(pts[pts.size() - 2]), c.color});
    c.frame.lines.push_back({glm::dvec3(pts.back()), c.color});
}

std::optional<double> pickTurntableRing(const drawOverrides::PickContext& c)
{
    if (!generalPref(kRingPref, true))
        return std::nullopt;
    const std::vector<glm::dvec3> pts = turntableRingPoints(c.node, c.world);
    const QRectF box = QRectF(c.a, c.b).normalized();
    std::optional<double> best;
    for (size_t i = 0; i + 1 < pts.size() - 2; ++i)
    {
        auto p0 = c.view.project(pts[i]), p1 = c.view.project(pts[i + 1]);
        if (!p0 || !p1)
            continue;
        if (!c.click)
        {
            if (box.contains(*p0))
                return 0.0;
            continue;
        }
        const QPointF d = *p1 - *p0;
        const double len2 = QPointF::dotProduct(d, d);
        const double k = len2 > 0 ? std::clamp(QPointF::dotProduct(c.a - *p0, d) / len2, 0.0, 1.0) : 0.0;
        const QPointF q = *p0 + d * k - c.a;
        const double dist = std::hypot(q.x(), q.y());
        if (!best || dist < *best)
            best = dist;
    }
    return best;
}

// On-view controls: a small bar at the bottom centre of the view.

enum ButtonId
{
    kStepBack = 1,
    kPlay,
    kStepNext,
    kFront,
    kPitch,
    kRing,
    kKeys,
};

QString turntableKeyOr(const char* action, const char* fallback)
{
    const QString k = Hotkeys::get().keyFor(action);
    return k.isEmpty() ? QString(fallback) : k;
}

/** The turntable keys, listed above the controls. */
void drawTurntableKeys(OverlayPainter& p, QPointF bottomLeft, float alpha)
{
    drawKeyList(p, bottomLeft, {
        {turntableKeyOr("turntable.spin", "Alt+Shift+T"), "Spin or pause"},
        {"Alt+Shift + drag", generalPref(kPitchPref, true) ? "Turn by hand; up and down tilts" : "Turn by hand"},
        {"Alt+Shift+Ctrl + drag", "Turn, snapping to the stops"},
        {"Alt+Shift + wheel", "Faster or slower"},
        {turntableKeyOr("turntable.stepBack", "Alt+Shift+Left") + " / " + turntableKeyOr("turntable.stepNext", "Alt+Shift+Right"), "Previous or next stop"},
        {turntableKeyOr("turntable.front", "Alt+Shift+Home"), "Back to the front, level, paused"},
    }, alpha);
}

void drawTurntableControls(render::FrameState&, OverlayPainter& p, const ViewInfo& view)
{
    TurntableUiState& s = turntableState();
    TurntableUiState::Controls& c = s.controls[view.isolation];
    c = {};
    if (!PrefStore::get().value<bool>("general", kControlsPref, true))
        return;
    tt::Turntable* t = activeTurntable();
    if (!t)
        return;
    const double since = tt::now() - s.lastAction;
    double alpha = 1.0;
    if (!selectionOnTurntableNow() && s.dragging == scene::kNoNode)
    {
        if (since > kControlsLinger)
            return;
        alpha = std::clamp((kControlsLinger - since) / 0.5, 0.0, 1.0);
    }
    const double size = 26.0, gap = 2.0;
    const QString info = turntableSpeedText(*t);
    const double textW = OverlayPainter::textWidth(info) + 16.0;
    const double w = 7 * (size + gap) + textW + 14.0;
    const QRectF bar(view.width * 0.5 - w * 0.5, view.height - size - 34.0, w, size + 8.0);
    p.rect(bar, {0.08f, 0.08f, 0.09f, float(0.72 * alpha)});
    c.bar = bar;
    const glm::vec4 white(0.92f, 0.92f, 0.92f, float(alpha));
    const glm::vec4 accent(0.35f, 0.65f, 1.0f, float(alpha));
    double x = bar.left() + 4.0;
    const bool spinning = t->playing && !t->held;
    for (auto [id, icon] : {std::pair{kStepBack, "chevron-left"}, std::pair{kPlay, spinning ? "player-pause" : "player-play"},
                            std::pair{kStepNext, "chevron-right"}, std::pair{kFront, "focus-centered"}})
    {
        const QRectF r(x, bar.top() + 4.0, size, size);
        p.icon(r.adjusted(4, 4, -4, -4), icon, id == kPlay && spinning ? accent : white);
        c.buttons.push_back({r, id});
        x += size + gap;
    }
    p.text(QPointF(x + 8.0, bar.center().y() + 5.0), info, white);
    x += textW;
    p.line(QPointF(x, bar.top() + 7.0), QPointF(x, bar.bottom() - 7.0), 1.0f, {0.5f, 0.5f, 0.5f, float(alpha)});
    x += 6.0;
    // Toggles: lit when on.
    const glm::vec4 off(0.55f, 0.55f, 0.55f, float(alpha));
    for (auto [id, icon, on] : {std::tuple{kPitch, "arrows-vertical", generalPref(kPitchPref, true)}, std::tuple{kRing, "circle-dashed", generalPref(kRingPref, true)},
                                std::tuple{kKeys, "keyboard", generalPref(kKeysPref, false)}})
    {
        const QRectF r(x, bar.top() + 4.0, size, size);
        p.icon(r.adjusted(4, 4, -4, -4), icon, on ? accent : off);
        c.buttons.push_back({r, id});
        x += size + gap;
    }
    if (generalPref(kKeysPref, false))
        drawTurntableKeys(p, QPointF(bar.left(), bar.top() - 10.0), float(alpha));
    c.view = QSizeF(view.width, view.height);
}

bool clickTurntableControls(QPointF px, const ViewInfo& view)
{
    TurntableUiState& s = turntableState();
    auto it = s.controls.find(view.isolation);
    if (it == s.controls.end() || it->second.view != QSizeF(view.width, view.height) || !it->second.bar.contains(px))
        return false;
    for (const TurntableUiState::Button& b : it->second.buttons)
        if (b.rect.contains(px))
        {
            Graph& g = Graph::current();
            tt::Turntable* t = activeTurntable();
            if (!t)
                return true;
            switch (b.id)
            {
            case kStepBack: tt::step(g, t->id(), -1, tt::now()); break;
            case kStepNext: tt::step(g, t->id(), 1, tt::now()); break;
            case kFront: tt::front(g, t->id(), tt::now()); break;
            case kPlay: tt::setPlaying(g, t->id(), !(t->playing && !t->held)); break;
            case kPitch: flipPref(kPitchPref, true); break;
            case kRing: flipPref(kRingPref, true); break;
            case kKeys: flipPref(kKeysPref, false); break;
            default: break;
            }
            turntableTouched();
            return true;
        }
    // The bar between its buttons is still the bar, not the scene behind it.
    turntableTouched();
    return true;
}

// Alt+Shift+drag turns the active turntable by hand; Ctrl as well snaps to its stops.

bool turntableModifiers(Qt::KeyboardModifiers m) { return (m & Qt::AltModifier) && (m & Qt::ShiftModifier); }

bool pressTurnDrag(QMouseEvent* e, const ViewInfo& view)
{
    if (e->button() != Qt::LeftButton || !turntableModifiers(e->modifiers()))
        return false;
    tt::Turntable* t = activeTurntable();
    if (!t)
        return false;
    TurntableUiState& s = turntableState();
    Graph& g = Graph::current();
    s.dragging = t->id();
    s.dragRaw = t->angle;
    s.dragTime = tt::now();
    s.dragLast = e->position();
    s.pitchAxis = glm::dvec3(view.view[0][0], view.view[1][0], view.view[2][0]);
    glm::dvec3 lo(1e300), hi(-1e300);
    s.pitchCentre = tt::sweepBounds(g, {t->id()}, lo, hi) ? (lo + hi) * 0.5 : tt::pivot(g, t->id());
    // Seen with the axis pointing down the screen, dragging right turns it the other way.
    const glm::dvec3 camUp(view.view[0][1], view.view[1][1], view.view[2][1]);
    s.dragSign = glm::dot(camUp, tt::axis(g, t->id())) < 0.0 ? -1.0 : 1.0;
    tt::grab(g, t->id());
    turntableTouched();
    return true;
}

void moveTurnDrag(QMouseEvent* e, const ViewInfo& view)
{
    TurntableUiState& s = turntableState();
    tt::Turntable* t = tt::get(Graph::current(), s.dragging);
    if (!t)
        return;
    const double dx = e->position().x() - s.dragLast.x();
    const double dy = e->position().y() - s.dragLast.y();
    s.dragLast = e->position();
    const double now = tt::now();
    const double dt = now - s.dragTime;
    s.dragTime = now;
    // A drag across the whole view is half a turn.
    const double degrees = dx / std::max(view.width, 1.0) * 180.0 * s.dragSign;
    s.dragRaw += degrees;
    s.dragSnap = e->modifiers() & Qt::ControlModifier;
    double to = s.dragRaw;
    if (s.dragSnap)
    {
        const double stepAngle = std::max(t->as<double>(tt::Turntable::kStepAngle), 1.0);
        const double rest = t->as<double>(tt::Turntable::kRestAngle);
        to = rest + std::round((s.dragRaw - rest) / stepAngle) * stepAngle;
    }
    double d = std::fmod(to - t->angle, 360.0);
    if (d > 180.0)
        d -= 360.0;
    else if (d < -180.0)
        d += 360.0;
    tt::drag(Graph::current(), t->id(), d, dt);
    // Dragging down brings the top toward the camera.
    if (generalPref(kPitchPref, true) && dy != 0.0)
        tt::dragPitch(Graph::current(), t->id(), dy / std::max(view.height, 1.0) * 180.0, s.pitchAxis, s.pitchCentre);
    turntableState().lastAction = now;
    redrawTurntables();
}

void releaseTurnDrag(QMouseEvent*, const ViewInfo&)
{
    TurntableUiState& s = turntableState();
    Graph& g = Graph::current();
    if (tt::Turntable* t = tt::get(g, s.dragging))
    {
        // A pause before letting go is a place, not a flick.
        if (s.dragSnap || tt::now() - s.dragTime > 0.08)
            t->velocity = 0.0;
        tt::release(g, t->id());
    }
    s.dragging = scene::kNoNode;
    turntableTouched();
}

bool turntableWheel(QWheelEvent* e, const ViewInfo&)
{
    if (!turntableModifiers(e->modifiers()))
        return false;
    tt::Turntable* t = activeTurntable();
    if (!t)
        return false;
    // Alt turns a vertical wheel horizontal on some platforms.
    const int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if (delta == 0)
        return true;
    tt::scaleSpeed(Graph::current(), t->id(), std::pow(1.15, delta / 120.0));
    viewportToast::showActive("Turntable: " + turntableSpeedText(*t), 1200);
    turntableTouched();
    return true;
}

// Attribute Editor section.

QWidget* turntablePanel(NodeId id)
{
    Graph& g = Graph::current();
    if (!tt::get(g, id))
        return nullptr;
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    const std::vector<NodeId> members = tt::members(g, id);
    QStringList names;
    for (NodeId m : members)
        names << QString::fromStdString(g.node(m)->name());
    auto* label = new QLabel(members.empty() ? QString("Nothing on it") : QString("On it: %1").arg(names.join(", ")));
    label->setWordWrap(true);
    v->addWidget(label);
    const std::string path = g.node(id)->path();
    auto row = [&](std::initializer_list<std::tuple<const char*, const char*, std::function<void()>>> buttons) {
        auto* h = new QHBoxLayout;
        for (const auto& [text, tip, fn] : buttons)
        {
            auto* b = new QPushButton(text);
            b->setToolTip(tip);
            QObject::connect(b, &QPushButton::clicked, b, fn);
            h->addWidget(b);
        }
        v->addLayout(h);
    };
    row({{"Spin / Pause", "Spin it, or ease it to rest (Alt+Shift+T).",
          [path] { runTurntableCommand("turntablePlay", {{"args", {path}}, {"toggle", true}}); }},
         {"Front", "Ease back to the rest angle and pause (Alt+Shift+Home).", [path] { runTurntableCommand("turntableTurn", {{"args", {path}}, {"front", true}}); }}});
    row({{"Add Selected", "Put the selected objects on it.",
          [path] {
              json add = json::array();
              Graph& g = Graph::current();
              for (NodeId s : g.selection())
                  if (const scene::Node* n = g.node(s); n && !tt::isTurntable(*n))
                      add.push_back(n->path());
              runTurntableCommand("turntable", {{"edit", path}, {"add", add}});
          }},
         {"Remove Selected", "Take the selected objects off it.",
          [path] {
              json rm = json::array();
              Graph& g = Graph::current();
              for (NodeId s : g.selection())
                  if (const scene::Node* n = g.node(s); n && !tt::isTurntable(*n))
                      rm.push_back(n->path());
              runTurntableCommand("turntable", {{"edit", path}, {"remove", rm}});
          }},
         {"Build Base", "Build a base under its members (the option box is Create > Turntable Base).",
          [path] { actions::runWithOptions("turntableBase", {{"args", {path}}}); turntableTouched(); }}});
    return w;
}

void registerTurntableActions()
{
    actions::add({.id = "turntable.putOn",
                  .label = "Put on Turntable",
                  .category = "Modify",
                  .doc = "Put the selected objects on a turntable (the selected one, or a new one under them), so they can turn in "
                         "front of a still camera.",
                  .command = "putOnTurntable",
                  .icon = "rotate-360"});
    actions::add({.id = "turntable.takeOff",
                  .label = "Take off Turntable",
                  .category = "Modify",
                  .doc = "Take the selected objects off their turntable; they go back to where they stand unturned.",
                  .command = "takeOffTurntable",
                  .icon = "rotate-360"});
    actions::add({.id = "turntable.spin",
                  .label = "Spin / Pause Turntable",
                  .category = "Modify",
                  .run = [] { runTurntableCommand("turntablePlay", {{"toggle", true}}); },
                  .doc = "Spin the turntable (the selected one, the one the selection is on, else the last used), easing in, or ease "
                         "it to rest.",
                  .python = "turntablePlay(toggle=True)",
                  .checked =
                      [] {
                          tt::Turntable* t = activeTurntable();
                          return t && t->playing && !t->held;
                      },
                  .icon = "player-play"});
    actions::add({.id = "turntable.stepNext",
                  .label = "Turntable Step Forward",
                  .category = "Modify",
                  .run = [] { runTurntableCommand("turntableTurn", {{"step", 1}}); },
                  .doc = "Ease the turntable on to its next stop (stepAngle apart: front, side, back, side).",
                  .python = "turntableTurn(step=1)",
                  .icon = "chevron-right"});
    actions::add({.id = "turntable.stepBack",
                  .label = "Turntable Step Back",
                  .category = "Modify",
                  .run = [] { runTurntableCommand("turntableTurn", {{"step", -1}}); },
                  .doc = "Ease the turntable back to its previous stop.",
                  .python = "turntableTurn(step=-1)",
                  .icon = "chevron-left"});
    actions::add({.id = "turntable.front",
                  .label = "Turntable to Front",
                  .category = "Modify",
                  .run = [] { runTurntableCommand("turntableTurn", {{"front", true}}); },
                  .doc = "Ease the turntable back to its rest angle and pause.",
                  .python = "turntableTurn(front=True)",
                  .icon = "focus-centered"});
    actions::add({.id = "turntable.controls",
                  .label = "Turntable Controls",
                  .category = "View",
                  .run =
                      [] {
                          PrefStore& p = PrefStore::get();
                          p.set("general", kControlsPref, !p.value<bool>("general", kControlsPref, true));
                          redrawTurntables();
                      },
                  .doc = "Show the turntable's spin and step buttons at the bottom of the view while a turntable or something on one "
                         "is selected. Turn off for a clean stream.",
                  .checked = [] { return PrefStore::get().value<bool>("general", kControlsPref, true); },
                  .icon = "rotate-360"});
    actions::add({.id = "turntable.ring",
                  .label = "Turntable Ring",
                  .category = "View",
                  .run =
                      [] {
                          flipPref(kRingPref, true);
                          redrawTurntables();
                      },
                  .doc = "Show the ring each turntable draws on the ground round what it turns.",
                  .checked = [] { return generalPref(kRingPref, true); },
                  .icon = "circle-dashed"});
    actions::add({.id = "turntable.pitch",
                  .label = "Turntable Tilt by Hand",
                  .category = "View",
                  .run = [] { flipPref(kPitchPref, true); },
                  .doc = "Dragging up and down with Alt+Shift tilts the turntable toward or away from the camera. Front levels it again.",
                  .checked = [] { return generalPref(kPitchPref, true); },
                  .icon = "arrows-vertical"});
    actions::add({.id = "create.turntableBase",
                  .label = "Turntable Base",
                  .category = "Create",
                  .doc = "Build a turntable base under the selection from a revolved profile curve, and put the selection and the "
                         "base on a turntable.",
                  .command = "turntableBase",
                  .icon = "rotate-360"});
}

/** Edits to anything on a spinning turntable hold it still until things go quiet. */
void watchTurntableEdits()
{
    Graph::current().addListener([](scene::Change c, NodeId id) {
        if (c != scene::Change::Value || id == scene::kNoNode)
            return;
        Graph& g = Graph::current();
        bool spinning = false;
        for (tt::Turntable* t : tt::all(g))
            spinning = spinning || ((t->playing || t->coasting) && t->as<bool>(tt::Turntable::kHoldWhileEditing));
        if (!spinning || tt::get(g, id))
            return;
        if (NodeId on = tt::turntableOf(g, id); on != scene::kNoNode)
        {
            tt::memberEdited(g, on, tt::now());
            wakeTurntables();
        }
    });
}
} // namespace

void registerTurntableUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    TurntableUiState& s = turntableState();
    s.timer = new QTimer(qApp);
    s.timer->setTimerType(Qt::PreciseTimer);
    const double hz = QGuiApplication::primaryScreen() ? std::max(QGuiApplication::primaryScreen()->refreshRate(), 30.0) : 60.0;
    s.timer->setInterval(std::max(1, int(1000.0 / hz)));
    QObject::connect(s.timer, &QTimer::timeout, qApp, tickTurntables);

    registerTurntableActions();
    drawOverrides::add({.id = "lila.turntable", .nodeType = tt::kTypeName, .draw = drawTurntableRing, .pick = pickTurntableRing});
    overlayIcons::add("chevron-left", {.tabler = "chevron-left"});
    overlayIcons::add("chevron-right", {.tabler = "chevron-right"});
    overlayIcons::add("player-play", {.tabler = "player-play"});
    overlayIcons::add("player-pause", {.tabler = "player-pause"});
    overlayIcons::add("focus-centered", {.tabler = "focus-centered"});
    overlayIcons::add("arrows-vertical", {.tabler = "arrows-vertical"});
    overlayIcons::add("circle-dashed", {.tabler = "circle-dashed"});
    overlayIcons::add("keyboard", {.tabler = "keyboard"});
    viewportHooks::addDrawer("lila.turntableControls", drawTurntableControls);
    viewportHooks::setClickHook("turntableControls", clickTurntableControls);
    viewportHooks::setDragHook("turntable", {.press = pressTurnDrag, .move = moveTurnDrag, .release = releaseTurnDrag});
    viewportHooks::setWheelHook("turntable", turntableWheel);
    attributeEditor::addSection(tt::kTypeName, "Turntable", [](NodeId id) { return turntablePanel(id); });
    watchTurntableEdits();
    // Commands from scripts and agents start the clock as the buttons do.
    ops::addExecuteListener([](const ExecuteEvent& e) {
        if (e.done && (e.name.starts_with("turntable") || e.name == "putOnTurntable" || e.name == "takeOffTurntable"))
            turntableTouched();
    });
}
} // namespace lila::ui
