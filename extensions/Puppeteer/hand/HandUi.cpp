// The hand rig's UI: the Hands panel (pose tiles, shape sliders, per finger values), the Hand Pad drawn beside a
// selected hand in the viewport (drag a finger to curl and spread it), and their actions and menu items. Every edit is a
// puppeteerHand* command; drags preview with undo off and commit one undoable command on release.

#include "hand/Hands.h"

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Controls.h>
#include <lila/Events.h>
#include <lila/Icons.h>
#include <lila/Prefs.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>

#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer::hand
{
namespace
{
constexpr const char* kPadPref = "/hands/pad";
constexpr const char* kCat = "Puppeteer Animate";

// ---------------------------------------------------------------------------------------------------------------------
// The hand diagram: a hand seen from its back, fingers shortening as they curl and fanning as they splay. The pad and
// the pose tiles draw it, so a pose tile looks like the pad will after a click.

struct Diagram
{
    std::array<std::vector<glm::dvec2>, kRoles> fingers; // polyline per finger, base first; empty when the hand lacks it
    std::array<glm::dvec2, kRoles> base{};
    std::array<double, kRoles> heading{}, length{};
    std::vector<glm::dvec2> palm;
};

/** Units: the palm is about one wide, y up. thumbLeft draws a right hand from the back (a left hand mirrors it). */
Diagram diagramOf(const Pose& p, bool thumbLeft)
{
    const double side = thumbLeft ? 1.0 : -1.0;
    Diagram d;
    d.palm = {{-0.46, 0.14}, {0.46, 0.14}, {0.5, 0.05}, {0.48, -0.5}, {0.38, -0.62}, {-0.38, -0.62}, {-0.48, -0.5}, {-0.5, 0.05}};
    static const double xs[kRoles] = {0.0, -0.33, -0.11, 0.11, 0.33};
    static const double ys[kRoles] = {0.0, 0.14, 0.16, 0.14, 0.08};
    static const double lens[kRoles] = {0.92, 0.95, 1.05, 0.98, 0.78};
    static const double share[kCanon] = {0.45, 0.31, 0.24};
    for (int r = 0; r < kRoles; ++r)
    {
        d.length[r] = lens[r];
        if (r == 0)
        {
            d.base[0] = {-side * 0.44, -0.34};
            d.heading[0] = kPi / 2 + side * 0.75;
        }
        else
        {
            d.base[r] = {thumbLeft ? xs[r] : -xs[r], ys[r]};
            d.heading[r] = kPi / 2;
        }
        if (!p.fingers[r])
            continue;
        const FingerPose& f = *p.fingers[r];
        std::vector<glm::dvec2> line{d.base[r]};
        glm::dvec2 at = d.base[r];
        if (r == 0)
        {
            // The thumb swings across the palm with its base, comes out toward the viewer with out, and curls inward.
            double heading = d.heading[0] - side * rad(f.joints[0].bend) * 1.1;
            double c = rad(std::abs(f.joints[0].splay)) * 0.8;
            static const double tshare[kCanon] = {0.4, 0.33, 0.27};
            for (int j = 0; j < kCanon; ++j)
            {
                if (j > 0)
                {
                    c += rad(f.joints[j].bend);
                    heading -= side * rad(f.joints[j].bend) * 0.45;
                }
                at += tshare[j] * lens[0] * std::cos(c) * glm::dvec2(std::cos(heading), std::sin(heading));
                line.push_back(at);
            }
        }
        else
        {
            const double heading = d.heading[r] + side * rad(f.joints[0].splay);
            double c = rad(f.meta.bend);
            for (int j = 0; j < kCanon; ++j)
            {
                c += rad(f.joints[j].bend);
                at += share[j] * lens[r] * std::cos(c) * glm::dvec2(std::cos(heading), std::sin(heading));
                line.push_back(at);
            }
        }
        d.fingers[r] = std::move(line);
    }
    return d;
}

QPixmap diagramPixmap(const Pose& p, bool thumbLeft, int size, qreal dpr, const QColor& ink, const QColor& palm)
{
    QPixmap px(int(size * dpr), int(size * dpr));
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter g(&px);
    g.setRenderHint(QPainter::Antialiasing);
    const Diagram d = diagramOf(p, thumbLeft);
    const double scale = size / 1.95;
    const QPointF origin(size / 2.0, size * 0.7);
    auto to = [&](const glm::dvec2& v) { return origin + QPointF(v.x * scale, -v.y * scale); };
    QPainterPath palmPath;
    palmPath.moveTo(to(d.palm.front()));
    for (size_t k = 1; k < d.palm.size(); ++k)
        palmPath.lineTo(to(d.palm[k]));
    palmPath.closeSubpath();
    g.fillPath(palmPath, palm);
    QPen pen(ink, size / 10.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    g.setPen(pen);
    for (const std::vector<glm::dvec2>& f : d.fingers)
        for (size_t k = 1; k < f.size(); ++k)
            g.drawLine(to(f[k - 1]), to(f[k]));
    return px;
}

// ---------------------------------------------------------------------------------------------------------------------
// Previews: a drag shows its result with undo off, from the pose it started on, and commits one command on release.

template <class Fn> void quietly(Fn&& fn)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    const Graph::DeferMeshes defer;
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
    u.setEnabled(was);
}

struct Preview
{
    bool active = false;
    Snapshot snap;
    std::string command;
    json call;
};
Preview& preview()
{
    static Preview p;
    return p;
}

std::vector<QWidget*> viewportWidgets()
{
    std::vector<QWidget*> out;
    for (QWidget* top : QApplication::topLevelWidgets())
        for (QWidget* w : top->findChildren<QWidget*>())
            if (w->inherits("lila::ui::ViewportWidget") && w->isVisible())
                out.push_back(w);
    return out;
}
void redrawViewports()
{
    for (QWidget* w : viewportWidgets())
        QCoreApplication::postEvent(w, new QEvent(QEvent::UpdateRequest));
}

/** The hand nodes a call's args and side reach, for the preview's snapshot. */
std::vector<NodeId> handsForCall(const json& call)
{
    Graph& g = Graph::current();
    std::vector<NodeId> out;
    for (const json& a : call.value("args", json::array()))
        if (Node* n = named(g, a.get<std::string>()))
        {
            const NodeId ch = characterFor(g, n->id());
            for (NodeId h : handsOf(g, ch))
                out.push_back(h);
        }
    return out;
}

void showPreview(const char* command, json call)
{
    Preview& p = preview();
    Graph& g = Graph::current();
    if (!p.active)
    {
        p.snap = snapshot(g, handsForCall(call));
        p.active = true;
    }
    p.command = command;
    call["key"] = false;
    p.call = call;
    quietly([&] {
        p.snap.restore(g);
        ops::execute(command, call, CallSource::Script);
    });
    redrawViewports();
}

void commitPreview()
{
    Preview& p = preview();
    if (!p.active)
        return;
    p.active = false;
    Graph& g = Graph::current();
    quietly([&] { p.snap.restore(g); });
    json call = p.call;
    call.erase("key");
    runCommand(p.command.c_str(), call);
    redrawViewports();
}

void cancelPreview()
{
    Preview& p = preview();
    if (!p.active)
        return;
    p.active = false;
    quietly([&] { p.snap.restore(Graph::current()); });
    redrawViewports();
}

// ---------------------------------------------------------------------------------------------------------------------
// What the panel and the pad act on.

/** The character the panel follows: the selection's, else the last one while it exists, else the only one. */
NodeId panelCharacter()
{
    static std::string last;
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
        if (NodeId c = characterFor(g, id))
        {
            last = g.node(c)->path();
            return c;
        }
    if (Node* n = named(g, last))
        return n->id();
    const std::vector<NodeId> all = characters(g);
    return all.size() == 1 ? all.front() : kNoNode;
}

/** The selected hand: the hand of the first selected node in one. */
NodeId selectedHand()
{
    Graph& g = Graph::current();
    for (NodeId id : g.selection())
        if (NodeId h = handFor(g, id))
            return h;
    return kNoNode;
}

bool padOn() { return PrefStore::get().value<bool>(kPrefs, kPadPref, true); }

json savedNames()
{
    json out = json::array();
    const json saved = savedPoses();
    for (const auto& [k, v] : saved.items())
        out.push_back(k);
    return out;
}

/** A pose for drawing: built-in or saved, as it would land on the hand. */
Pose posePreviewOf(const std::string& name)
{
    if (const Library* l = findLibrary(name))
        return poseFrom(l->shape);
    const json all = savedPoses();
    if (auto it = all.find(name); it != all.end())
    {
        Pose p = poseFromJson(it->value("pose", json::object()));
        // Fingers the saved pose leaves out draw relaxed.
        const Pose rest = poseFrom(findLibrary("relaxed")->shape);
        for (int r = 0; r < kRoles; ++r)
            if (!p.fingers[r])
                p.fingers[r] = rest.fingers[r];
        return p;
    }
    return poseFrom(findLibrary("flat")->shape);
}

// ---------------------------------------------------------------------------------------------------------------------
// The Hand Pad.

struct PadLayout
{
    QPointF origin;
    double scale = 40.0;
    QRectF plate;
    bool thumbLeft = true;
    QPointF to(const glm::dvec2& v) const { return origin + QPointF(v.x * scale, -v.y * scale); }
};

struct PadState
{
    NodeId hand = kNoNode;
    int role = -2; // -1 the palm, 0..4 a finger, -2 nothing
    QPointF press;
    json base;
    bool moved = false;
};
PadState& pad()
{
    static PadState s;
    return s;
}

/** Where the pad sits for the hand in a view: beside the hand joint on screen, kept inside the view. */
std::optional<PadLayout> padLayout(const ViewInfo& view, const Hand& h)
{
    Graph& g = Graph::current();
    const glm::dvec3 at(worldOf(g, h.joint)[3]);
    const std::optional<QPointF> s = view.project(at);
    if (!s)
        return std::nullopt;
    PadLayout l;
    // From behind the hand (its back toward the camera) the thumb of a right hand is on the left.
    const glm::dvec3 palm = math::rotationOf(worldOf(g, h.joint)) * h.palm;
    const bool backToCamera = glm::dot(palm, view.viewDir(at)) > 0.0;
    l.thumbLeft = (h.side != Side::Left) == backToCamera;
    const QSizeF size(112, 116);
    QPointF corner = *s + QPointF(56, -size.height() - 24);
    corner.setX(std::clamp(corner.x(), 8.0, std::max(8.0, view.width - size.width() - 8)));
    corner.setY(std::clamp(corner.y(), 8.0, std::max(8.0, view.height - size.height() - 8)));
    l.plate = QRectF(corner, size);
    l.origin = corner + QPointF(size.width() / 2, size.height() * 0.68);
    l.scale = 40.0;
    return l;
}

/** The finger (or -1 the palm) under a point of the pad, by the finger's full straight length; -2 for none. */
int padHit(const PadLayout& l, const Diagram& d, QPointF px)
{
    if (!l.plate.contains(px))
        return -2;
    int best = -2;
    double bestDist = 12.0;
    for (int r = 0; r < kRoles; ++r)
    {
        if (d.fingers[r].empty())
            continue;
        const QPointF a = l.to(d.base[r]);
        const QPointF b = l.to(d.base[r] + d.length[r] * glm::dvec2(std::cos(d.heading[r]), std::sin(d.heading[r])));
        const QPointF ab = b - a, ap = px - a;
        const double t = std::clamp(QPointF::dotProduct(ap, ab) / std::max(1e-9, QPointF::dotProduct(ab, ab)), 0.0, 1.0);
        const QPointF q = a + t * ab;
        if (const double dist = std::hypot(px.x() - q.x(), px.y() - q.y()); dist < bestDist)
        {
            bestDist = dist;
            best = r;
        }
    }
    if (best >= -1)
        return best;
    QPolygonF palm;
    for (const glm::dvec2& v : d.palm)
        palm << l.to(v);
    return palm.containsPoint(px, Qt::OddEvenFill) || l.plate.contains(px) ? -1 : -2;
}

glm::vec4 grey(float v, float a = 1.0f) { return {v, v, v, a}; }

/** The hand the pad shows: the selected one, in Animate mode with the pad on. */
std::optional<Hand> padHand()
{
    if (!padOn() || modes::current() != kAnimateMode)
        return std::nullopt;
    Graph& g = Graph::current();
    static uint64_t version = ~0ull;
    static std::vector<NodeId> sel;
    static NodeId cached = kNoNode;
    if (version != g.version() || sel != g.selection())
    {
        version = g.version();
        sel = g.selection();
        cached = selectedHand();
    }
    if (pad().role != -2)
        cached = pad().hand;
    return cached ? load(g, cached) : std::nullopt;
}

/** The view the pad last drew in, for padState. */
struct LastPad
{
    QPointer<QWidget> widget;
    ViewInfo view;
};
LastPad& lastPad()
{
    static LastPad l;
    return l;
}

void drawPad(OverlayPainter& p, const ViewInfo& view, QWidget* widget)
{
    const std::optional<Hand> h = padHand();
    if (!h)
        return;
    const std::optional<PadLayout> l = padLayout(view, *h);
    if (!l)
        return;
    lastPad() = {widget, view};
    const Pose pose = currentPose(Graph::current(), *h);
    const Diagram d = diagramOf(pose, l->thumbLeft);
    int hot = pad().role;
    if (hot == -2 && widget)
        hot = padHit(*l, d, widget->mapFromGlobal(QCursor::pos()));
    p.rect(l->plate, grey(0.08f, 0.62f));
    std::vector<QPointF> palm;
    for (const glm::dvec2& v : d.palm)
        palm.push_back(l->to(v));
    p.fill(palm, grey(hot == -1 ? 0.42f : 0.30f, 0.9f));
    for (int r = 0; r < kRoles; ++r)
    {
        if (d.fingers[r].empty())
            continue;
        // The straight finger faintly behind, so the curl reads against it.
        const QPointF a = l->to(d.base[r]);
        const QPointF b = l->to(d.base[r] + d.length[r] * glm::dvec2(std::cos(d.heading[r]), std::sin(d.heading[r])));
        p.smoothLine(a, b, 9.0f, grey(0.5f, hot == r ? 0.35f : 0.16f));
        const glm::vec4 ink = hot == r ? grey(1.0f) : grey(0.82f);
        for (size_t k = 1; k < d.fingers[r].size(); ++k)
            p.smoothLine(l->to(d.fingers[r][k - 1]), l->to(d.fingers[r][k]), 7.0f, ink);
        p.smoothDisc(l->to(d.fingers[r].back()), 3.0, ink);
    }
    QString text = h->side == Side::Left ? "Left Hand" : h->side == Side::Right ? "Right Hand" : "Hand";
    if (hot >= 0)
        text = QString("%1  %2").arg(roleLabel(Role(hot))).arg(measureFingerCurl(pose, Role(hot)), 0, 'f', 2);
    else if (hot == -1)
        text = QString("Curl %1  Spread %2").arg(measureCurl(pose), 0, 'f', 2).arg(measureSpread(pose), 0, 'f', 2);
    p.label(l->plate.topLeft() + QPointF(6, 14), text, {0.9f, 0.9f, 0.9f, 1.0f});
}

/** Where the pad is, for scripts and tests: its plate and each finger's grab point in the view's pixels. */
json padState()
{
    const std::optional<Hand> h = padHand();
    LastPad& last = lastPad();
    if (!h || !last.widget)
        return {{"visible", false}, {"on", padOn()}};
    const std::optional<PadLayout> l = padLayout(last.view, *h);
    if (!l)
        return {{"visible", false}, {"on", padOn()}};
    const Diagram d = diagramOf(currentPose(Graph::current(), *h), l->thumbLeft);
    json fingers = json::object();
    for (int r = 0; r < kRoles; ++r)
        if (!d.fingers[r].empty())
        {
            const QPointF at = l->to(d.base[r] + 0.6 * d.length[r] * glm::dvec2(std::cos(d.heading[r]), std::sin(d.heading[r])));
            fingers[roleName(Role(r))] = {at.x(), at.y()};
        }
    const QPointF palm = l->to({0.0, -0.3});
    QWidget* w = last.widget.data();
    return {{"visible", true},
            {"on", padOn()},
            {"hand", Graph::current().node(h->node)->path()},
            {"thumbLeft", l->thumbLeft},
            {"plate", {l->plate.x(), l->plate.y(), l->plate.width(), l->plate.height()}},
            {"palm", {palm.x(), palm.y()}},
            {"fingers", fingers},
            {"view", {{"x", w->mapToGlobal(QPoint(0, 0)).x()}, {"y", w->mapToGlobal(QPoint(0, 0)).y()}, {"width", w->width()}, {"height", w->height()}}}};
}

void padPoseMenu(QPoint global, const std::string& hand)
{
    QTimer::singleShot(0, [global, hand] {
        QMenu menu;
        menu.addSection("Hand Pose");
        int shown = 0;
        QMenu* more = nullptr;
        for (const Library& l : library())
        {
            QMenu* into = &menu;
            if (++shown > 10)
            {
                if (!more)
                    more = menu.addMenu("More");
                into = more;
            }
            into->addAction(icon(QString::fromStdString(l.icon)), QString::fromStdString(l.label),
                            [hand, id = l.id] { runCommand("puppeteerHandPose", {{"args", {hand}}, {"pose", id}}); });
        }
        if (const json saved = savedNames(); !saved.empty())
        {
            menu.addSection("Saved");
            for (const json& n : saved)
                menu.addAction(QString::fromStdString(n.get<std::string>()),
                               [hand, n = n.get<std::string>()] { runCommand("puppeteerHandPose", {{"args", {hand}}, {"pose", n}}); });
        }
        menu.addSeparator();
        menu.addAction("Rest", [hand] { runCommand("puppeteerHandPose", {{"args", {hand}}, {"pose", "rest"}}); });
        menu.addAction("Mirror to Other Hand", [hand] { runCommand("puppeteerHandMirror", {{"args", {hand}}}); });
        menu.addAction("Key Hand", [hand] { runCommand("puppeteerHandKey", {{"args", {hand}}}); });
        menu.addAction("Hands Panel", [] { panels::show("puppeteerHands"); });
        menu.exec(global);
        redrawViewports();
    });
}

bool padPress(QMouseEvent* e, const ViewInfo& view)
{
    if (e->modifiers() & Qt::AltModifier)
        return false;
    const std::optional<Hand> h = padHand();
    if (!h)
        return false;
    const std::optional<PadLayout> l = padLayout(view, *h);
    if (!l)
        return false;
    Graph& g = Graph::current();
    const Pose pose = currentPose(g, *h);
    const int hit = padHit(*l, diagramOf(pose, l->thumbLeft), e->position());
    if (hit == -2)
        return false;
    const std::string handPath = g.node(h->node)->path();
    if (e->button() == Qt::RightButton)
    {
        padPoseMenu(e->globalPosition().toPoint(), handPath);
        return true;
    }
    if (e->button() != Qt::LeftButton)
        return false;
    PadState& s = pad();
    s.hand = h->node;
    s.role = hit;
    s.press = e->position();
    s.moved = false;
    s.base = shapeJson(*h, pose);
    s.base["thumbLeft"] = l->thumbLeft;
    return true;
}

void padMove(QMouseEvent* e, const ViewInfo&)
{
    PadState& s = pad();
    if (s.role == -2)
        return;
    const QPointF d = e->position() - s.press;
    if (!s.moved && std::hypot(d.x(), d.y()) < 3.0)
        return;
    s.moved = true;
    const double k = (e->modifiers() & Qt::ShiftModifier) ? 0.25 / 90.0 : 1.0 / 90.0;
    const double down = d.y() * k;
    // Toward the thumb on screen is spread for a finger; away from the palm is out for the thumb.
    const double toThumb = (s.base.value("thumbLeft", true) ? -d.x() : d.x()) * k;
    Graph& g = Graph::current();
    json call = {{"args", {g.node(s.hand)->path()}}};
    const json& fingers = s.base["fingers"];
    if (s.role == -1)
    {
        call["curl"] = s.base["curl"].get<double>() + down;
        call["spread"] = s.base["spread"].get<double>() + d.x() * k;
    }
    else if (s.role == 0)
    {
        call["thumb"] = s.base["thumb"].get<double>() + down;
        call["fingerSpread"] = {{"thumb", fingers["thumb"]["spread"].get<double>() + toThumb}};
    }
    else
    {
        const std::string name = roleName(Role(s.role));
        json curls = {{name, fingers[name]["curl"].get<double>() + down}};
        // Ctrl brings the neighbours along by half.
        if (e->modifiers() & Qt::ControlModifier)
            for (int r : {s.role - 1, s.role + 1})
                if (r >= 1 && r < kRoles && fingers.contains(roleName(Role(r))))
                    curls[roleName(Role(r))] = fingers[roleName(Role(r))]["curl"].get<double>() + 0.5 * down;
        call["fingerCurl"] = curls;
        call["fingerSpread"] = {{name, fingers[name]["spread"].get<double>() + toThumb}};
    }
    showPreview("puppeteerHandShape", call);
}

void padRelease(QMouseEvent*, const ViewInfo&)
{
    PadState& s = pad();
    if (s.role == -2)
        return;
    if (s.moved)
        commitPreview();
    s.role = -2;
    redrawViewports();
}

// ---------------------------------------------------------------------------------------------------------------------
// The Hands panel.

/** A slider field the panel previews while it is dragged or typed into, committing when it settles. */
class ShapeSlider : public controls::SliderField
{
public:
    ShapeSlider(double lo, double hi, QWidget* parent) : controls::SliderField(parent)
    {
        setRange(lo, hi);
        setDecimals(2);
        setSingleStep(0.05);
        setAlignment(Qt::AlignRight);
    }
    void display(double v)
    {
        if (hasFocus() && preview().active)
            return;
        const QSignalBlocker block(this);
        setValue(v);
    }
};

class HandsPanel : public QWidget
{
public:
    explicit HandsPanel(QWidget* parent) : QWidget(parent)
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(8, 8, 8, 8);
        root->setSpacing(8);

        mEmpty = new QWidget(this);
        auto* el = new QVBoxLayout(mEmpty);
        el->setContentsMargins(0, 0, 0, 0);
        mEmptyText = new QLabel("No hands. Select a character with finger joints.", mEmpty);
        mEmptyText->setWordWrap(true);
        mEmptyText->setForegroundRole(QPalette::PlaceholderText);
        el->addWidget(mEmptyText);
        auto* setup = new QToolButton(mEmpty);
        setup->setText("Set Up Hands");
        controls::setRole(setup, "push");
        connect(setup, &QToolButton::clicked, this, [] { actions::run("puppeteer.hands.setup"); });
        el->addWidget(setup, 0, Qt::AlignLeft);
        root->addWidget(mEmpty);

        mBody = new QWidget(this);
        auto* body = new QVBoxLayout(mBody);
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(8);
        root->addWidget(mBody);

        // Which hand, and which of its fingers edits reach.
        auto* top = new QHBoxLayout;
        top->setSpacing(4);
        mSide = new controls::Segmented(mBody);
        mSide->add("Left", {}, "Left hand", 0);
        mSide->add("Right", {}, "Right hand", 1);
        mSide->add("Both", {}, "Both hands", 2);
        mSide->setCurrent(0);
        connect(mSide, &controls::Segmented::picked, this, [this](int id) {
            mSideId = id;
            mSidePicked = true;
            refresh(true);
        });
        top->addWidget(mSide, 1);
        auto* more = new QToolButton(mBody);
        more->setIcon(icon("dots"));
        more->setToolTip("More hand actions");
        more->setAutoRaise(true);
        connect(more, &QToolButton::clicked, this, [this, more] { moreMenu(more); });
        top->addWidget(more);
        body->addLayout(top);

        auto* fingerRow = new QHBoxLayout;
        fingerRow->setSpacing(2);
        auto* fl = new QLabel("Fingers", mBody);
        fl->setForegroundRole(QPalette::PlaceholderText);
        fingerRow->addWidget(fl);
        fingerRow->addSpacing(6);
        for (int r = 0; r < kRoles; ++r)
        {
            auto* b = new QToolButton(mBody);
            b->setText(QString(QChar(roleLabel(Role(r))[0])));
            b->setToolTip(QString("%1: poses and sliders reach it while on").arg(roleLabel(Role(r))));
            b->setCheckable(true);
            b->setChecked(true);
            b->setFixedWidth(30);
            controls::setRole(b, "segment");
            connect(b, &QToolButton::toggled, this, [this, r](bool on) { mMask[r] = on; });
            mFingerChips[r] = b;
            fingerRow->addWidget(b);
        }
        fingerRow->addStretch(1);
        body->addLayout(fingerRow);

        // Poses: one click applies at Amount; drag a tile sideways to blend toward it live.
        auto* poses = new QWidget(mBody);
        auto* pl = new QVBoxLayout(poses);
        pl->setContentsMargins(0, 0, 0, 0);
        pl->setSpacing(4);
        mTiles = new QWidget(poses);
        mTileGrid = new QGridLayout(mTiles);
        mTileGrid->setContentsMargins(0, 0, 0, 0);
        mTileGrid->setSpacing(2);
        pl->addWidget(mTiles);
        auto* amountRow = new QHBoxLayout;
        amountRow->addWidget(fieldLabel("Amount", poses));
        mAmount = new controls::SliderField(poses);
        mAmount->setRange(0, 100);
        mAmount->setDecimals(0);
        mAmount->setSuffix(" %");
        mAmount->setValue(100);
        mAmount->setAlignment(Qt::AlignRight);
        mAmount->setToolTip("How far a pose tile goes toward its pose");
        amountRow->addWidget(mAmount, 1);
        pl->addLayout(amountRow);
        body->addWidget(controls::section("Poses", poses, mBody, "puppeteerHands"));

        // Shape: hand level values, read from the pose and set back through it.
        auto* shape = new QWidget(mBody);
        auto* sg = new QGridLayout(shape);
        sg->setContentsMargins(0, 0, 0, 0);
        sg->setHorizontalSpacing(8);
        sg->setVerticalSpacing(2);
        int row = 0;
        auto addSlider = [&](const char* label, const char* key, double lo, double hi, const char* tip) {
            auto* s = new ShapeSlider(lo, hi, shape);
            s->setToolTip(tip);
            sg->addWidget(fieldLabel(label, shape), row, 0);
            sg->addWidget(s, row++, 1);
            wire(s, [key](double v) { return json{{key, v}}; });
            return s;
        };
        mCurl = addSlider("Curl", "curl", -0.3, 1.2, "Curl of the fingers: 0 straight, 1 a fist");
        mSpread = addSlider("Spread", "spread", -1.0, 1.5, "Fan of the fingers: negative squeezes them together");
        mThumb = addSlider("Thumb", "thumb", -1.0, 1.0, "The thumb: -1 out, 1 across the palm");
        mCup = addSlider("Cup", "cup", -1.0, 1.0, "Hollow of the palm");
        mRelax = addSlider("Relax", "relax", -1.0, 1.0, "Drag toward relaxed (right) or tense (left) from the pose now");
        body->addWidget(controls::section("Shape", shape, mBody, "puppeteerHands"));

        // Per finger curl and spread.
        auto* per = new QWidget(mBody);
        auto* fg = new QGridLayout(per);
        fg->setContentsMargins(0, 0, 0, 0);
        fg->setHorizontalSpacing(8);
        fg->setVerticalSpacing(2);
        auto* hc = fieldLabel("Curl", per);
        auto* hs = fieldLabel("Spread", per);
        fg->addWidget(hc, 0, 1, Qt::AlignRight);
        fg->addWidget(hs, 0, 2, Qt::AlignRight);
        for (int r = 0; r < kRoles; ++r)
        {
            mFingerLabels[r] = fieldLabel(roleLabel(Role(r)), per);
            fg->addWidget(mFingerLabels[r], r + 1, 0);
            mFingerCurl[r] = new ShapeSlider(-0.3, 1.2, per);
            mFingerSpread[r] = new ShapeSlider(-1.5, 1.5, per);
            mFingerCurl[r]->setToolTip(QString("%1 curl").arg(roleLabel(Role(r))));
            mFingerSpread[r]->setToolTip(r == 0 ? "Thumb out from the palm" : QString("%1 spread toward the thumb").arg(roleLabel(Role(r))));
            fg->addWidget(mFingerCurl[r], r + 1, 1);
            fg->addWidget(mFingerSpread[r], r + 1, 2);
            const std::string name = roleName(Role(r));
            wire(mFingerCurl[r], [name](double v) { return json{{"fingerCurl", {{name, v}}}}; }, false);
            wire(mFingerSpread[r], [name](double v) { return json{{"fingerSpread", {{name, v}}}}; }, false);
        }
        body->addWidget(controls::section("Fingers", per, mBody, "puppeteerHands", false));

        // Blend two poses.
        auto* blend = new QWidget(mBody);
        auto* bg = new QGridLayout(blend);
        bg->setContentsMargins(0, 0, 0, 0);
        bg->setHorizontalSpacing(8);
        bg->setVerticalSpacing(2);
        mFrom = new QComboBox(blend);
        mTo = new QComboBox(blend);
        mMix = new controls::SliderField(blend);
        mMix->setRange(0, 100);
        mMix->setDecimals(0);
        mMix->setSuffix(" %");
        mMix->setValue(50);
        mMix->setAlignment(Qt::AlignRight);
        mMix->setToolTip("0 the first pose, 100 the second");
        bg->addWidget(fieldLabel("From", blend), 0, 0);
        bg->addWidget(mFrom, 0, 1);
        bg->addWidget(fieldLabel("To", blend), 1, 0);
        bg->addWidget(mTo, 1, 1);
        bg->addWidget(fieldLabel("Mix", blend), 2, 0);
        bg->addWidget(mMix, 2, 1);
        connect(mMix, &QDoubleSpinBox::valueChanged, this, [this](double) {
            if (mRefreshing)
                return;
            showPreview("puppeteerHandPose", blendCall());
            mCommit.start();
        });
        connect(mMix, &QDoubleSpinBox::editingFinished, this, [] { commitPreview(); });
        auto* applyBlend = new QToolButton(blend);
        applyBlend->setText("Apply");
        controls::setRole(applyBlend, "push");
        connect(applyBlend, &QToolButton::clicked, this, [this] {
            cancelPreview();
            runCommand("puppeteerHandPose", blendCall());
        });
        bg->addWidget(applyBlend, 3, 1, Qt::AlignLeft);
        body->addWidget(controls::section("Blend", blend, mBody, "puppeteerHands", false));

        // Weekly actions.
        auto* acts = new QHBoxLayout;
        acts->setSpacing(4);
        for (auto [label, action] : {std::pair{"Mirror", "puppeteer.hands.mirror"}, {"Grip", "puppeteer.hands.grip"}, {"Key", "puppeteer.hands.key"}})
            acts->addWidget(controls::push(action, mBody, label));
        acts->addStretch(1);
        body->addLayout(acts);
        body->addStretch(1);
        root->addStretch(1);

        mCommit.setSingleShot(true);
        mCommit.setInterval(450);
        connect(&mCommit, &QTimer::timeout, this, [] { commitPreview(); });
        mPoll.setInterval(150);
        connect(&mPoll, &QTimer::timeout, this, [this] { refresh(false); });
        mPoll.start();
        refresh(true);
    }

    /** The call reaching the panel's hands and fingers. */
    json targetCall() const
    {
        Graph& g = Graph::current();
        json call = json::object();
        const NodeId ch = panelCharacter();
        call["args"] = json::array({ch ? g.node(ch)->path() : std::string()});
        call["side"] = mSideId == 0 ? "left" : mSideId == 1 ? "right" : "both";
        json fingers = json::array();
        for (int r = 0; r < kRoles; ++r)
            if (mMask[r])
                fingers.push_back(roleName(Role(r)));
        if (fingers.size() < size_t(kRoles))
            call["fingers"] = fingers;
        return call;
    }

private:
    QLabel* fieldLabel(const char* text, QWidget* parent)
    {
        auto* l = new QLabel(text, parent);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return l;
    }

    /** Previews the slider's value as it changes and commits once it settles. */
    void wire(QDoubleSpinBox* s, std::function<json(double)> flags, bool masked = true)
    {
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, flags, masked](double v) {
            if (mRefreshing)
                return;
            json call = targetCall();
            if (!masked)
                call.erase("fingers");
            const json add = flags(v);
            for (auto& [k, x] : add.items())
                call[k] = x;
            showPreview("puppeteerHandShape", call);
            mCommit.start();
        });
        connect(s, &QDoubleSpinBox::editingFinished, this, [this] {
            mCommit.stop();
            commitPreview();
            const QSignalBlocker block(mRelax);
            mRelax->setValue(0.0);
        });
    }

    json blendCall() const
    {
        json call = targetCall();
        call["pose"] = mFrom->currentData().toString().toStdString();
        call["blendTo"] = mTo->currentData().toString().toStdString();
        call["mix"] = mMix->value() / 100.0;
        return call;
    }

    void moreMenu(QWidget* anchor)
    {
        QMenu menu;
        for (const char* a : {"puppeteer.hands.swap", "puppeteer.hands.savePose", "puppeteer.hands.setup", "puppeteer.hands.pad"})
            if (const Action* act = actions::find(a))
            {
                QAction* qa = menu.addAction(QString::fromStdString(act->label), [a] { actions::run(a); });
                if (act->checked)
                {
                    qa->setCheckable(true);
                    qa->setChecked(act->checked());
                }
            }
        if (const json saved = savedNames(); !saved.empty())
        {
            QMenu* del = menu.addMenu("Delete Saved Pose");
            for (const json& n : saved)
                del->addAction(QString::fromStdString(n.get<std::string>()),
                               [n = n.get<std::string>()] { runCommand("puppeteerHandDeletePose", {{"args", {n}}}); });
        }
        menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    }

    /** A pose tile: click applies it at Amount; dragging it sideways blends toward it as it goes. */
    QToolButton* poseTile(const std::string& name, const QString& label, bool saved)
    {
        auto* b = new QToolButton(mTiles);
        b->setText(label);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(32, 32));
        b->setFixedHeight(58);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setToolTip(QString("%1\nClick: apply at Amount. Drag right: blend toward it.%2").arg(label, saved ? "\nSaved pose" : ""));
        controls::setRole(b, "tile");
        b->setProperty("pose", QString::fromStdString(name));
        b->installEventFilter(&mTileFilter);
        return b;
    }

    struct TileFilter : QObject
    {
        HandsPanel* panel = nullptr;
        QPointF press;
        bool dragging = false;
        bool eventFilter(QObject* o, QEvent* e) override
        {
            auto* b = qobject_cast<QToolButton*>(o);
            if (!b)
                return false;
            const std::string pose = b->property("pose").toString().toStdString();
            switch (e->type())
            {
            case QEvent::MouseButtonPress:
                if (static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)
                {
                    press = static_cast<QMouseEvent*>(e)->position();
                    dragging = false;
                }
                return false;
            case QEvent::MouseMove:
            {
                auto* m = static_cast<QMouseEvent*>(e);
                if (!(m->buttons() & Qt::LeftButton))
                    return false;
                const double dx = m->position().x() - press.x();
                if (!dragging && std::abs(dx) < 4.0)
                    return false;
                dragging = true;
                const double w = std::clamp(dx / 160.0, 0.0, 1.0);
                json call = panel->targetCall();
                call["pose"] = pose;
                call["weight"] = w;
                showPreview("puppeteerHandPose", call);
                const QSignalBlocker block(panel->mAmount);
                panel->mAmount->setValue(std::round(w * 100.0));
                return true;
            }
            case QEvent::MouseButtonRelease:
                if (static_cast<QMouseEvent*>(e)->button() != Qt::LeftButton)
                    return false;
                if (dragging)
                {
                    dragging = false;
                    b->setDown(false);
                    commitPreview();
                    return true;
                }
                {
                    json call = panel->targetCall();
                    call["pose"] = pose;
                    call["weight"] = panel->mAmount->value() / 100.0;
                    cancelPreview();
                    runCommand("puppeteerHandPose", call);
                }
                b->setDown(false);
                return true;
            default:
                return false;
            }
        }
    };

    void rebuildTiles(bool thumbLeft)
    {
        const json saved = savedNames();
        const std::string key = saved.dump() + (thumbLeft ? "L" : "R") + std::to_string(palette().color(QPalette::Text).rgba());
        if (key == mTileKey)
            return;
        mTileKey = key;
        while (QLayoutItem* it = mTileGrid->takeAt(0))
        {
            delete it->widget();
            delete it;
        }
        mFrom->clear();
        mTo->clear();
        const QColor ink = palette().color(QPalette::Text), palmInk = palette().color(QPalette::Mid);
        const qreal dpr = devicePixelRatioF();
        int i = 0;
        auto place = [&](const std::string& name, const QString& label, bool isSaved) {
            QToolButton* b = poseTile(name, label, isSaved);
            b->setIcon(QIcon(diagramPixmap(posePreviewOf(name), thumbLeft, 32, dpr, ink, palmInk)));
            mTileGrid->addWidget(b, i / 4, i % 4);
            ++i;
            mFrom->addItem(label, QString::fromStdString(name));
            mTo->addItem(label, QString::fromStdString(name));
        };
        for (const Library& l : library())
            place(l.id, QString::fromStdString(l.label), false);
        for (const json& n : saved)
            place(n.get<std::string>(), QString::fromStdString(n.get<std::string>()), true);
        mFrom->addItem("Rest", "rest");
        mTo->addItem("Rest", "rest");
        mFrom->setCurrentIndex(0);
        mTo->setCurrentIndex(1);
    }

public:
    void refresh(bool force)
    {
        if (!isVisible() && !force)
            return;
        Graph& g = Graph::current();
        const NodeId ch = panelCharacter();
        const NodeId selHand = selectedHand();
        const std::string key = std::to_string(g.version()) + ":" + std::to_string(g.time()) + ":" + std::to_string(ch) + ":" +
                                std::to_string(selHand) + ":" + std::to_string(mSideId) + ":" + savedNames().dump();
        if (!force && key == mKey)
            return;
        mKey = key;
        std::vector<NodeId> hands = handsOf(g, ch);
        mEmpty->setVisible(hands.empty());
        mBody->setVisible(!hands.empty());
        mEmptyText->setText(ch ? "No hands found on this character. Set up again after adding finger joints."
                               : "No hands. Select a character with finger joints.");
        if (hands.empty())
            return;
        // A selected hand picks its side, until a side is picked here.
        if (selHand && !mSidePicked)
            if (const std::optional<Hand> sh = load(g, selHand); sh && sh->side != Side::Center)
            {
                mSideId = sh->side == Side::Left ? 0 : 1;
                mSide->setCurrent(mSideId);
            }
        std::optional<Hand> shown;
        for (NodeId hn : hands)
            if (std::optional<Hand> h = load(g, hn); h && (mSideId == 2 || (mSideId == 0) == (h->side == Side::Left)))
            {
                shown = h;
                break;
            }
        if (!shown)
            shown = load(g, hands.front());
        if (!shown)
            return;
        rebuildTiles(shown->side != Side::Left);
        if (preview().active)
            return;
        mRefreshing = true;
        const Pose p = currentPose(g, *shown);
        mCurl->display(measureCurl(p));
        mSpread->display(measureSpread(p));
        mThumb->display(measureThumb(p));
        mCup->display(measureCup(p, shown->hasMeta()));
        mRelax->display(0.0);
        for (int r = 0; r < kRoles; ++r)
        {
            const bool here = shown->finger(Role(r)) != nullptr;
            mFingerChips[r]->setEnabled(here);
            mFingerLabels[r]->setEnabled(here);
            mFingerCurl[r]->setEnabled(here);
            mFingerSpread[r]->setEnabled(here);
            mFingerCurl[r]->display(here ? measureFingerCurl(p, Role(r)) : 0.0);
            mFingerSpread[r]->display(here ? measureFingerSpread(p, Role(r)) : 0.0);
        }
        mThumb->setEnabled(shown->finger(Role::Thumb) != nullptr);
        mRefreshing = false;
    }

    QWidget* mEmpty;
    QLabel* mEmptyText;
    QWidget* mBody;
    controls::Segmented* mSide;
    int mSideId = 0;
    bool mSidePicked = false;
    std::array<QToolButton*, kRoles> mFingerChips{};
    Mask mMask = allFingers();
    QWidget* mTiles;
    QGridLayout* mTileGrid;
    std::string mTileKey;
    TileFilter mTileFilter;
    controls::SliderField* mAmount;
    ShapeSlider *mCurl, *mSpread, *mThumb, *mCup, *mRelax;
    std::array<QLabel*, kRoles> mFingerLabels{};
    std::array<ShapeSlider*, kRoles> mFingerCurl{}, mFingerSpread{};
    QComboBox *mFrom, *mTo;
    controls::SliderField* mMix;
    QTimer mCommit, mPoll;
    std::string mKey;
    bool mRefreshing = false;
};

QPointer<HandsPanel>& panelInstance()
{
    static QPointer<HandsPanel> p;
    return p;
}

QWidget* makeHandsPanel(QWidget* parent)
{
    auto* p = new HandsPanel(parent);
    p->mTileFilter.panel = p;
    panelInstance() = p;
    return p;
}

/** The call for an action: the panel's hands when it is open, else the selection's hand, else every hand of the character. */
json actionCall()
{
    if (HandsPanel* p = panelInstance().data(); p && p->isVisible())
        return p->targetCall();
    Graph& g = Graph::current();
    if (NodeId h = selectedHand())
        return {{"args", {g.node(h)->path()}}};
    const NodeId ch = panelCharacter();
    return {{"args", {ch ? g.node(ch)->path() : std::string()}}, {"side", "both"}};
}

void savePoseAction()
{
    bool ok = false;
    const QString name = QInputDialog::getText(QApplication::activeWindow(), "Save Hand Pose", "Name", QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    json call = actionCall();
    call["name"] = name.trimmed().toStdString();
    if (call.value("side", "") == "both")
        call.erase("side");
    runCommand("puppeteerHandSavePose", call);
}

void gripAction()
{
    json call = actionCall();
    Graph& g = Graph::current();
    // Whatever else is selected besides the hand is what it grips.
    json targets = json::array();
    const NodeId ch = panelCharacter();
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && !rig::isJoint(*n) && !n->type().isA(kHandType) && characterFor(g, id) != ch)
            targets.push_back(n->path());
    if (targets.empty())
    {
        log::write(LogLevel::Warning, "Grip: select the mesh or character to grip along with the hand");
        return;
    }
    call["target"] = targets;
    runCommand("puppeteerHandGrip", call);
}
} // namespace
} // namespace puppeteer::hand

namespace puppeteer
{
void registerHandUi(lila::ext::Context& ctx)
{
    using namespace hand;
    panels::add(ctx, {.id = "puppeteerHands", .title = "Hands", .create = makeHandsPanel, .area = Qt::LeftDockWidgetArea, .icon = "hand-stop",
                      .open = false, .keywords = {"fingers", "hand pose", "finger poser", "hand poser", "fist", "grip"}, .docs = "hands"});

    renderPasses::add(ctx, {.id = "puppeteer.handPad", .stage = renderPasses::Stage::Overlay, .order = 900,
                            .run = [](renderPasses::Context& c) { drawPad(c.painter, c.view, c.widget); }});
    viewportHooks::setDragHook("puppeteer.handPad", {.press = padPress, .move = padMove, .release = padRelease});
    ctx.track("puppeteer:handPad", [] {
        cancelPreview();
        viewportHooks::removeDragHook("puppeteer.handPad");
    });

    ctx.registerCommand({
        .name = "puppeteerHandPad",
        .doc = "Where the Hand Pad is drawn: whether it shows, its hand, its plate (x, y, width, height), the palm and each "
               "finger's grab point, in the pixels of the viewport it last drew in, and that viewport's place on screen.",
        .undoable = false,
        .run = [](const Args&) -> json { return padState(); },
    });

    const std::vector<std::string> kw = {"hand", "fingers", "finger poser", "hand poser", "hand rig"};
    actions::add(ctx, {.id = "puppeteer.hands.pad", .label = "Hand Pad", .category = kCat,
                       .run = [] {
                           PrefStore::get().set(kPrefs, kPadPref, !padOn());
                           redrawViewports();
                       },
                       .doc = "The Hand Pad beside a selected hand in Animate mode: drag a finger up or down to curl it and sideways to "
                              "spread it, the palm for every finger, the thumb to swing it across or out. Shift: fine. Ctrl: the "
                              "neighbours follow by half. Right-click: poses.",
                       .checked = [] { return padOn(); },
                       .icon = "hand-finger",
                       .keywords = kw});
    actions::add(ctx, {.id = "puppeteer.hands.setup", .label = "Set Up Hands", .category = kCat,
                       .run = [] {
                           Graph& g = Graph::current();
                           const NodeId ch = panelCharacter();
                           runCommand("puppeteerHandSetup", {{"args", {ch ? g.node(ch)->path() : std::string()}}});
                       },
                       .doc = "Find the character's hands and fingers again (after changing its skeleton); the pose now becomes their rest.",
                       .icon = "hand-stop",
                       .keywords = kw});
    actions::add(ctx, {.id = "puppeteer.hands.mirror", .label = "Mirror Hand", .category = kCat,
                       .run = [] { runCommand("puppeteerHandMirror", actionCall()); },
                       .doc = "Copy the hand's pose onto the other hand, mirrored.",
                       .icon = "flip-vertical",
                       .keywords = kw});
    actions::add(ctx, {.id = "puppeteer.hands.swap", .label = "Swap Hands", .category = kCat,
                       .run = [] {
                           json call = actionCall();
                           call["swap"] = true;
                           if (call.value("side", "") == "both")
                               call["side"] = "left";
                           runCommand("puppeteerHandMirror", call);
                       },
                       .doc = "Exchange the two hands' poses, each mirrored.",
                       .keywords = kw});
    actions::add(ctx, {.id = "puppeteer.hands.key", .label = "Key Hand", .category = kCat,
                       .run = [] { runCommand("puppeteerHandKey", actionCall()); },
                       .doc = "Key every finger joint of the hand at the current frame.",
                       .icon = "key",
                       .keywords = kw});
    actions::add(ctx, {.id = "puppeteer.hands.grip", .label = "Grip", .category = kCat,
                       .run = gripAction,
                       .doc = "Close the hand's fingers onto the other selected mesh or character until they touch, wrapping it.",
                       .icon = "hand-grab",
                       .keywords = {"hand", "grab", "hold", "fingers", "contact", "wrap"}});
    actions::add(ctx, {.id = "puppeteer.hands.savePose", .label = "Save Hand Pose", .category = kCat,
                       .run = savePoseAction,
                       .doc = "Save the hand's pose (the fingers the Hands panel reaches) to use on any hand of any character.",
                       .icon = "device-floppy",
                       .keywords = kw});
    for (const Library& l : library())
        actions::add(ctx, {.id = "puppeteer.hands.pose." + l.id, .label = l.label + " (Hands)", .category = kCat,
                           .run = [id = l.id] {
                               json call = actionCall();
                               call["pose"] = id;
                               runCommand("puppeteerHandPose", call);
                           },
                           .doc = "Put the hand into the " + l.label + " pose.",
                           .icon = l.icon,
                           .keywords = {"hand pose", "fingers", lowerOf(l.label)}});

    menus::addItem(ctx, {.id = "puppeteer.hands.menu", .menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Hands"});
    menus::addItem(ctx, {.id = "puppeteer.hands.menuAnim", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Hands"});
    int sep = 0;
    for (const std::string m : {"Puppeteer/Hands", "Animation/Hands"})
    {
        for (const char* a : {"window.puppeteerHands", "puppeteer.hands.pad", "", "puppeteer.hands.mirror", "puppeteer.hands.swap", "puppeteer.hands.grip",
                              "puppeteer.hands.key", "puppeteer.hands.savePose", "", "puppeteer.hands.setup"})
            if (*a)
                menus::addItem(ctx, {.id = "puppeteer.hands." + m + "." + a, .menu = m, .action = a});
            else
                menus::addItem(ctx, {.id = "puppeteer.hands.sep" + std::to_string(sep++), .menu = m, .kind = menus::Kind::Separator});
        menus::addItem(ctx, {.id = "puppeteer.hands." + m + ".poses", .menu = m, .kind = menus::Kind::Submenu, .label = "Poses"});
        for (const Library& l : library())
            menus::addItem(ctx, {.id = "puppeteer.hands." + m + ".pose." + l.id, .menu = m + "/Poses", .action = "puppeteer.hands.pose." + l.id});
    }

    const events::SubscriptionId sub = events::subscribe(
        events::Event::SelectionChanged, [](const events::EventData&) { redrawViewports(); }, "Puppeteer");
    ctx.track("puppeteer:handsSelection", [sub] { events::unsubscribe(sub); });
}
} // namespace puppeteer
