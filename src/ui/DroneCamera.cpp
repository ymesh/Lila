#include "ui/DroneCamera.h"
#include "ui/ViewportTool.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/HotkeyBindings.h"
#include "ui/Hotkeys.h"
#include "ui/Overlay.h"
#include "ui/UiCommon.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>

namespace lila::ui
{
using drone::setSpeed;
using drone::speed;

namespace
{
constexpr const char* kSpeedPref = "/viewport/drone/speed";
constexpr double kDefaultSpeed = 1000.0; // cm/s
constexpr double kMinSpeed = 1.0, kMaxSpeed = 1e7;
constexpr double kSpeedStep = 1.41421356237;
constexpr double kLookDegPerPx = 0.15;
/** Mouse drags move this many seconds of flight per pixel, so they scale with the speed like the keys. */
constexpr double kDragSecondsPerPx = 0.0025;
constexpr double kWheelSeconds = 0.12; // per wheel notch
constexpr double kEaseSeconds = 0.06;  // velocity smoothing
constexpr double kZoomPerSecond = 1.2; // focal length growth while Z or C is held, as an exponent rate
/** A key that turned flying on and is released later than this was held to fly, so letting go lands. */
constexpr qint64 kHoldMs = 300;

enum class Key
{
    None,
    Forward,
    Back,
    Left,
    Right,
    Up,
    Down,
    ZoomIn,
    ZoomOut,
    Faster,
    Slower,
    Exit,
};
constexpr int kMoveKeys = int(Key::ZoomOut) + 1;

/** Unreal's viewport fly keys, letters by their US position when the keymap follows positions. */
Key keyFor(QKeyEvent* e)
{
    // The number pad with Num Lock off sends arrows and paging keys, still from the pad.
    if (e->modifiers() & Qt::KeypadModifier)
        switch (e->key())
        {
        case Qt::Key_8: case Qt::Key_Up: return Key::Forward;
        case Qt::Key_2: case Qt::Key_Down: return Key::Back;
        case Qt::Key_4: case Qt::Key_Left: return Key::Left;
        case Qt::Key_6: case Qt::Key_Right: return Key::Right;
        case Qt::Key_9: case Qt::Key_PageUp: return Key::Up;
        case Qt::Key_7: case Qt::Key_Home: return Key::Down;
        case Qt::Key_1: case Qt::Key_End: return Key::ZoomIn;
        case Qt::Key_3: case Qt::Key_PageDown: return Key::ZoomOut;
        default: return Key::None;
        }
    switch (e->key())
    {
    case Qt::Key_Escape: return Key::Exit;
    case Qt::Key_Up: return Key::Forward;
    case Qt::Key_Down: return Key::Back;
    case Qt::Key_Left: return Key::Left;
    case Qt::Key_Right: return Key::Right;
    case Qt::Key_PageUp: return Key::Up;
    case Qt::Key_PageDown: return Key::Down;
    default: break;
    }
    const std::optional<QKeyCombination> chord = hotkeys::chordFromEvent(e);
    switch (chord ? chord->key() : Qt::Key_unknown)
    {
    case Qt::Key_W: return Key::Forward;
    case Qt::Key_S: return Key::Back;
    case Qt::Key_A: return Key::Left;
    case Qt::Key_D: return Key::Right;
    case Qt::Key_E: return Key::Up;
    case Qt::Key_Q: return Key::Down;
    case Qt::Key_Z: return Key::ZoomIn;
    case Qt::Key_C: return Key::ZoomOut;
    case Qt::Key_R: return Key::Faster;
    case Qt::Key_F: return Key::Slower;
    default: return Key::None;
    }
}

bool isModifierKey(int key)
{
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

QString speedText(double cmPerSecond)
{
    if (cmPerSecond < 100.0)
        return QString("%1 cm/s").arg(cmPerSecond, 0, 'f', cmPerSecond < 10.0 ? 1 : 0);
    const double m = cmPerSecond / 100.0;
    if (m < 1000.0)
        return QString("%1 m/s").arg(m, 0, 'f', m < 10.0 ? 1 : 0);
    const double km = m / 1000.0;
    return QString("%1 km/s").arg(km, 0, 'f', km < 10.0 ? 1 : 0);
}

double nowMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return double(clock.elapsed());
}
} // namespace

/** One view's flight: its keys, velocity, banner and the camera step it records on landing. */
class DroneFlight : public QObject
{
public:
    explicit DroneFlight(ViewportWidget* view) : QObject(view), mView(view)
    {
        mTimer.setTimerType(Qt::PreciseTimer);
        const QScreen* screen = QGuiApplication::primaryScreen();
        mTimer.setInterval(std::max(1, int(1000.0 / std::max(screen ? screen->refreshRate() : 60.0, 30.0))));
        connect(&mTimer, &QTimer::timeout, this, [this] { tick(); });
        qApp->installEventFilter(this);
        mBefore = view->cameraView();
        mBaseFocal = view->mCamera.focalLength;
        mClock.start();
        // A view that turns orthographic or looks through a camera node can't fly.
        connect(view, &ViewportWidget::viewStateChanged, this, [this] {
            if (mView->mCamera.ortho || mView->mCamera.free)
                leaveLater();
        });
    }

    ~DroneFlight() override { qApp->removeEventFilter(this); }

    /** Restores what the flight changed temporarily and records it as one camera step. */
    void land()
    {
        mTimer.stop();
        mView->mCamera.focalLength = mBaseFocal;
        mDragging = false;
        mView->recordCamera(mBefore);
    }

    /** The view's own cursor again, once the flight is no longer listed for it. */
    void restoreCursor() { mView->updateCursor(); }

    void armHold()
    {
        mHoldArmed = true;
        mEnteredMs = nowMs();
    }

    bool mousePress(QMouseEvent* e)
    {
        if (e->button() == Qt::LeftButton && mClose.contains(e->position()))
        {
            leaveLater();
            return true;
        }
        if (e->button() == Qt::LeftButton && mBanner.contains(e->position()))
            return true;
        if (!mDragging && (e->modifiers() & Qt::AltModifier))
            return false;
        mButtons = e->buttons();
        mView->setFocus();
        if (!mDragging)
        {
            mDragging = true;
            mAnchor = e->globalPosition();
            mView->updateCursor();
        }
        return true;
    }

    bool mouseMove(QMouseEvent* e)
    {
        if (!mDragging)
        {
            const bool hover = mClose.contains(e->position());
            if (hover != mCloseHover)
            {
                mCloseHover = hover;
                mView->requestFrame();
            }
            // Selection preselect stays off while flying; Alt navigation still sees its moves.
            return !(e->buttons() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton));
        }
        const QPointF d = e->globalPosition() - mAnchor;
        if (d.isNull())
            return true;
        QCursor::setPos(mAnchor.toPoint());
        Camera& c = mView->mCamera;
        const Qt::MouseButtons b = e->buttons();
        const double perPx = speed() * kDragSecondsPerPx;
        const bool left = b & Qt::LeftButton, right = b & Qt::RightButton, middle = b & Qt::MiddleButton;
        if (middle || (left && right))
        {
            c.target += c.right() * (d.x() * perPx) - c.up() * (d.y() * perPx);
        }
        else if (right)
        {
            look(d.x() * kLookDegPerPx, d.y() * kLookDegPerPx);
        }
        else if (left)
        {
            look(d.x() * kLookDegPerPx, 0.0);
            c.target += groundForward() * (-d.y() * perPx);
        }
        mView->requestFrame();
        return true;
    }

    bool mouseRelease(QMouseEvent* e)
    {
        if (!mDragging)
            return false;
        mButtons = e->buttons();
        if (e->buttons() == Qt::NoButton)
            endDrag();
        return true;
    }

    bool wheel(QWheelEvent* e)
    {
        const double notches = e->angleDelta().y() / 120.0;
        if (notches == 0.0)
            return true;
        if (QApplication::mouseButtons() & Qt::RightButton)
            setSpeed(speed() * std::pow(kSpeedStep, notches));
        else
            mView->mCamera.target += mView->mCamera.forward() * (notches * speed() * kWheelSeconds);
        mView->requestFrame();
        return true;
    }

    bool applyCursor()
    {
        const Qt::CursorShape shape = mDragging ? Qt::BlankCursor : mCloseHover ? Qt::PointingHandCursor : Qt::ArrowCursor;
        if (mView->cursor().shape() != shape)
            mView->setCursor(shape);
        return true;
    }

    void draw(OverlayPainter& p)
    {
        const double w = mView->width(), h = mView->height();
        // A frame round the whole view says a mode is on, without tinting what is seen through it.
        const glm::vec4 frame(0.95f, 0.95f, 0.95f, 0.55f);
        const double in = 1.0;
        p.line({in, in}, {w - in, in}, 2.0f, frame);
        p.line({w - in, in}, {w - in, h - in}, 2.0f, frame);
        p.line({w - in, h - in}, {in, h - in}, 2.0f, frame);
        p.line({in, h - in}, {in, in}, 2.0f, frame);

        const QString title = "Drone Camera Active";
        const QString bound = Hotkeys::get().keyFor("view.drone");
        const QString key = bound.isEmpty() ? QString("Esc") : bound;
        const QString speedLabel = "Speed";
        const QString speedValue = speedText(speed());
        // Fixed width for the speed, so the banner doesn't change size as it climbs.
        const double speedW = OverlayPainter::textWidth("000 km/s");
        const double height = 30.0, pad = 10.0, icon = 16.0, gap = 8.0, chipPad = 6.0, closeW = 30.0;
        const double titleW = OverlayPainter::textWidth(title), keyW = OverlayPainter::textWidth(key) + chipPad * 2.0;
        const double labelW = OverlayPainter::textWidth(speedLabel);
        const double total = pad + icon + gap + titleW + gap + keyW + gap * 2.0 + 1.0 + gap * 2.0 + labelW + gap + speedW + gap + 1.0 + closeW;
        const QRectF bar(std::round(w * 0.5 - total * 0.5), 10.0, std::round(total), height);
        mBanner = bar;
        p.rect(bar, {0.08f, 0.08f, 0.09f, 0.85f});
        const glm::vec4 text(0.95f, 0.95f, 0.95f, 1.0f), dim(0.62f, 0.62f, 0.62f, 1.0f), rule(0.4f, 0.4f, 0.4f, 1.0f);
        const double baseline = bar.center().y() + 5.0;
        double x = bar.left() + pad;
        p.icon(QRectF(x, bar.center().y() - icon * 0.5, icon, icon), "drone", text);
        x += icon + gap;
        p.text({x, baseline}, title, text);
        x += titleW + gap;
        const QRectF chip(x, bar.top() + 6.0, keyW, height - 12.0);
        p.rect(chip, {1.0f, 1.0f, 1.0f, 0.14f});
        p.text({chip.left() + chipPad, baseline}, key, text);
        x += keyW + gap * 2.0;
        p.line({x, bar.top() + 7.0}, {x, bar.bottom() - 7.0}, 1.0f, rule);
        x += 1.0 + gap * 2.0;
        p.text({x, baseline}, speedLabel, dim);
        x += labelW + gap;
        p.text({x, baseline}, speedValue, text);
        x += speedW + gap;
        p.line({x, bar.top() + 7.0}, {x, bar.bottom() - 7.0}, 1.0f, rule);
        x += 1.0;
        mClose = QRectF(x, bar.top(), bar.right() - x, height);
        if (mCloseHover)
            p.rect(mClose, {1.0f, 1.0f, 1.0f, 0.18f});
        p.icon(QRectF(mClose.center().x() - 8.0, mClose.center().y() - 8.0, 16.0, 16.0), "x", text);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease)
            return QObject::eventFilter(obj, e);
        auto* k = static_cast<QKeyEvent*>(e);
        const bool press = e->type() == QEvent::KeyPress;
        if (!press)
            return release(k);
        if (QApplication::focusWidget() != mView)
            return false;
        // Looking with the right button held, Ctrl sinks and Space climbs, as in Unreal.
        const bool looking = mDragging && (mButtons & Qt::RightButton);
        if (looking && (k->key() == Qt::Key_Control || k->key() == Qt::Key_Space))
        {
            if (!k->isAutoRepeat())
            {
                mPressed.push_back(keyId(k));
                mHeld[int(k->key() == Qt::Key_Control ? Key::Down : Key::Up)] = true;
                if (!mTimer.isActive())
                {
                    mClock.restart();
                    mTimer.start();
                }
            }
            return true;
        }
        if (isModifierKey(k->key()))
            return false;
        if (k->modifiers() & ((looking ? Qt::NoModifier : Qt::ControlModifier) | Qt::AltModifier | Qt::MetaModifier))
            return false;
        if (Hotkeys::get().isKeyFor(k, "view.drone"))
            return false;
        const Key key = keyFor(k);
        if (key == Key::Exit)
        {
            leaveLater();
            return true;
        }
        // F frames the selection as usual; it slows the flight only while looking with the right button.
        if (key == Key::Slower && !looking)
            return false;
        if (key == Key::Faster || key == Key::Slower)
        {
            setSpeed(speed() * (key == Key::Faster ? kSpeedStep : 1.0 / kSpeedStep));
            mView->requestFrame();
            return true;
        }
        // Function keys still reach their hotkeys; the rest would edit or switch tools, so flying swallows them.
        if (k->key() >= Qt::Key_F1 && k->key() <= Qt::Key_F35)
            return false;
        if (!k->isAutoRepeat())
            mPressed.push_back(keyId(k));
        if (key != Key::None && !k->isAutoRepeat())
        {
            mHeld[int(key)] = true;
            if (!mTimer.isActive())
            {
                mClock.restart();
                mTimer.start();
            }
        }
        return true;
    }

private:
    static int keyId(QKeyEvent* k) { return k->nativeScanCode() ? int(k->nativeScanCode()) : k->key(); }

    /** Leaving deletes this flight, so it waits until the event or signal being handled returns. */
    void leaveLater()
    {
        QTimer::singleShot(0, mView, [v = QPointer<ViewportWidget>(mView)] {
            if (v)
                drone::leave(v);
        });
    }

    bool release(QKeyEvent* k)
    {
        if (k->isAutoRepeat())
            return false;
        const bool ours = std::erase(mPressed, keyId(k)) > 0;
        if (ours && (k->key() == Qt::Key_Control || k->key() == Qt::Key_Space))
        {
            mHeld[int(k->key() == Qt::Key_Control ? Key::Down : Key::Up)] = false;
            return true;
        }
        const Key key = keyFor(k);
        if (ours && key != Key::None && int(key) < kMoveKeys)
            mHeld[int(key)] = false;
        // The first key let go that wasn't pressed while flying is the one that turned flying on.
        if (!ours && mHoldArmed && !isModifierKey(k->key()))
        {
            mHoldArmed = false;
            if (nowMs() - mEnteredMs > kHoldMs)
                leaveLater();
        }
        return ours;
    }

    void endDrag()
    {
        if (!mDragging)
            return;
        mDragging = false;
        mView->updateCursor();
    }

    /** Turns the view about its eye: right and down in degrees. */
    void look(double rightDeg, double downDeg)
    {
        Camera& c = mView->mCamera;
        const glm::dvec3 eye = c.eye();
        c.yaw = std::fmod(c.yaw - rightDeg, 360.0);
        c.pitch = std::clamp(c.pitch - downDeg, -89.9, 89.9);
        c.target = eye + c.forward() * c.distance;
    }

    glm::dvec3 groundForward() const
    {
        const Camera& c = mView->mCamera;
        glm::dvec3 f = c.forward() - c.up() * glm::dot(c.forward(), c.up());
        return glm::length(f) > 1e-9 ? glm::normalize(f) : glm::cross(c.up(), c.right());
    }

    void tick()
    {
        const double dt = std::clamp(mClock.restart() / 1000.0, 0.0, 0.1);
        if (QApplication::focusWidget() != mView)
        {
            mHeld.fill(false);
            mPressed.clear();
        }
        Camera& c = mView->mCamera;
        auto held = [this](Key k) { return mHeld[int(k)] ? 1.0 : 0.0; };
        glm::dvec3 wish = c.forward() * (held(Key::Forward) - held(Key::Back)) + c.right() * (held(Key::Right) - held(Key::Left)) +
                          c.up() * (held(Key::Up) - held(Key::Down));
        if (glm::length(wish) > 1e-9)
            wish = glm::normalize(wish) * speed();
        mVelocity += (wish - mVelocity) * (1.0 - std::exp(-dt / kEaseSeconds));
        c.target += mVelocity * dt;
        const double zoom = held(Key::ZoomIn) - held(Key::ZoomOut);
        if (zoom != 0.0)
            c.focalLength = std::clamp(c.focalLength * std::exp(zoom * kZoomPerSecond * dt), 4.0, 2000.0);
        else
            c.focalLength = mBaseFocal;
        const bool anyHeld = std::any_of(mHeld.begin(), mHeld.end(), [](bool b) { return b; });
        if (!anyHeld && glm::length(mVelocity) < speed() * 1e-3)
        {
            mVelocity = glm::dvec3(0.0);
            mTimer.stop();
        }
        mView->requestFrame();
    }

    ViewportWidget* mView;
    QTimer mTimer;
    QElapsedTimer mClock;
    std::array<bool, kMoveKeys> mHeld{};
    std::vector<int> mPressed; // keys pressed while flying, by scancode (or Qt key)
    glm::dvec3 mVelocity{0.0};
    CameraView mBefore;
    double mBaseFocal = 35.0;
    bool mDragging = false;
    /** The buttons held in the view's drag, as its mouse events say. */
    Qt::MouseButtons mButtons;
    QPointF mAnchor;
    QRectF mBanner, mClose;
    bool mCloseHover = false;
    bool mHoldArmed = false;
    double mEnteredMs = 0.0;
};

namespace drone
{
namespace
{
std::map<const ViewportWidget*, DroneFlight*>& flights()
{
    static std::map<const ViewportWidget*, DroneFlight*> m;
    return m;
}

DroneFlight* flight(const ViewportWidget* view)
{
    auto it = flights().find(view);
    return it == flights().end() ? nullptr : it->second;
}
} // namespace

bool active(const ViewportWidget* view) { return flight(view) != nullptr; }

double speed() { return std::clamp(generalPref(kSpeedPref, kDefaultSpeed), kMinSpeed, kMaxSpeed); }

void setSpeed(double cmPerSecond) { PrefStore::get().set("general", kSpeedPref, std::clamp(cmPerSecond, kMinSpeed, kMaxSpeed)); }

void enter(ViewportWidget* view)
{
    if (!view || active(view))
        return;
    if (view->camera().ortho || view->lookThroughCamera() != scene::kNoNode)
    {
        view->setView("persp");
        viewportToast::show("Drone Camera: switched to Perspective", view, 3000, viewportToast::Kind::Warning);
    }
    if (view->camera().ortho || view->camera().free)
        return;
    auto* f = new DroneFlight(view);
    flights()[view] = f;
    QObject::connect(view, &QObject::destroyed, [view] { flights().erase(view); });
    if (Hotkeys::fromKey())
        f->armHold();
    view->setFocus();
    viewportToast::show("Drone Camera on: WASD moves, R/F speed, Esc exits", view, 4000);
    view->requestFrame();
    Q_EMIT view->viewStateChanged();
}

void leave(ViewportWidget* view)
{
    DroneFlight* f = flight(view);
    if (!f)
        return;
    flights().erase(view);
    f->land();
    f->restoreCursor();
    delete f;
    viewportToast::show("Drone Camera off", view);
    view->requestFrame();
    Q_EMIT view->viewStateChanged();
}

void toggle(ViewportWidget* view)
{
    if (active(view))
        leave(view);
    else
        enter(view);
}

bool mousePress(ViewportWidget* view, QMouseEvent* e)
{
    DroneFlight* f = flight(view);
    return f && f->mousePress(e);
}

bool mouseMove(ViewportWidget* view, QMouseEvent* e)
{
    DroneFlight* f = flight(view);
    return f && f->mouseMove(e);
}

bool mouseRelease(ViewportWidget* view, QMouseEvent* e)
{
    DroneFlight* f = flight(view);
    return f && f->mouseRelease(e);
}

bool wheel(ViewportWidget* view, QWheelEvent* e)
{
    DroneFlight* f = flight(view);
    return f && f->wheel(e);
}

void draw(OverlayPainter& p, ViewportWidget* view)
{
    DroneFlight* f = flight(view);
    if (!f)
        return;
    f->draw(p);
    // In place of the layout's keys in the corner.
    drawKeyList(p, QPointF(16.0, double(view->height()) - 84.0),
                {{"W A S D", "Move"},
                 {"E / Q", "Up, down"},
                 {"Space / Ctrl", "Up, down (right drag)"},
                 {"Right drag", "Look"},
                 {"Z / C", "Zoom while held"},
                 {"R", "Faster"},
                 {"F", "Frame (slower with right drag)"},
                 {"Esc", "Leave"}});
}

bool applyCursor(ViewportWidget* view)
{
    DroneFlight* f = flight(view);
    return f && f->applyCursor();
}

void registerActions(std::function<ViewportWidget*()> activeView)
{
    overlayIcons::add("drone", {.tabler = "drone"});
    overlayIcons::add("x", {.tabler = "x"});
    actions::add({.id = "view.drone",
                  .label = "Drone Camera",
                  .category = "View",
                  .run =
                      [activeView] {
                          if (ViewportWidget* v = activeView())
                              toggle(v);
                      },
                  .doc = "Fly the view through the scene with Unreal's viewport keys, listed in the view while flying. Nothing can be "
                         "edited until you leave. Shift-click the button for speed and smoothing.",
                  .checked =
                      [activeView] {
                          ViewportWidget* v = activeView();
                          return v && active(v);
                      },
                  .icon = "drone",
                  .keywords = {"fly", "flythrough", "walk", "pilot", "navigate", "unreal", "game"}});
}
} // namespace drone
} // namespace lila::ui
