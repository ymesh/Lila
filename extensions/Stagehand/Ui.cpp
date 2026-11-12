// The Push tool (live physics in the viewport: grab, push, wiggle, drop, shake, sweep, pin; slow motion, auto keep at rest), its Level menu entries and the Level Toolkit's Stagehand sections.

#include "Session.h"
#include "Stagehand.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Prefs.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <optional>

using namespace lila;
using namespace lila::ui;

namespace stagehand
{
namespace
{
constexpr const char* kPushTool = "stagehand.push";
/** How much push the wiggle brush gives per second held, cm/s, spread over the frames. */
constexpr double kWiggleSpeedPerSecond = 400.0;
/** The sweep brush carries things at the cursor's speed up to this, and gathers at kGatherSpeed, cm/s. */
constexpr double kSweepSpeedMax = 600.0;
constexpr double kGatherSpeed = 80.0;
/** With auto keep on, the result is kept once everything has rested this long after a drag, seconds. */
constexpr double kKeepAfterRest = 0.3;
constexpr double kSlowMotion = 0.25;

enum class Mode : int
{
    Grab = 0,
    Push,
    Wiggle,
    Drop,
    Shake,
    Sweep,
    Count
};

const char* modeName(Mode m)
{
    switch (m)
    {
    case Mode::Grab:
        return "Grab";
    case Mode::Push:
        return "Push";
    case Mode::Wiggle:
        return "Wiggle";
    case Mode::Drop:
        return "Drop";
    case Mode::Shake:
        return "Shake";
    case Mode::Sweep:
        return "Sweep";
    default:
        return "";
    }
}

/** What to do in mode m: the prompt, and its keys added to s.keys. */
QString addModeHint(Mode m, ToolStatus& s)
{
    switch (m)
    {
    case Mode::Grab:
        s.keys.push_back({"Shift", "lift"});
        return "drag to carry";
    case Mode::Push:
        return "drag a direction, let go to shove";
    case Mode::Wiggle:
        s.keys.push_back({"[ ]", "size"});
        return "hold to jostle under the brush";
    case Mode::Drop:
        s.keys.push_back({"Shift-drag", "lift first"});
        return "click to let everything fall";
    case Mode::Shake:
        return "click a shelf or table to shake it";
    case Mode::Sweep:
        s.keys.push_back({"Shift", "gather"});
        s.keys.push_back({"[ ]", "size"});
        return "drag to sweep along the stroke";
    default:
        return {};
    }
}

/** The mode the next activation starts in, set by the menu actions. */
Mode gStartMode = Mode::Grab;

/**
 * Live physics while it is active: what is selected (and whatever touches it, and dynamic bodies) moves, everything
 * else is hit. Enter keeps the result as one undo step, Esc puts it all back, leaving the tool keeps it.
 */
class PushTool final : public ViewportTool
{
public:
    const char* name() const override { return "stagehandPush"; }

    void activate() override
    {
        mMode = gStartMode;
        mAutoKeep = PrefStore::get().value<bool>("stagehand", "/push/autoKeep", true);
        mTimer = new QTimer();
        mTimer->setInterval(16);
        QObject::connect(mTimer, &QTimer::timeout, [this] { tick(); });
        mTimer->start();
        mClock.start();
    }

    void deactivate() override
    {
        if (mTimer)
        {
            mTimer->stop();
            delete mTimer;
            mTimer = nullptr;
        }
        keep();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (!ensureSession())
            return true;
        glm::dvec3 o, d, at;
        view.ray(e->position(), o, d);
        mPress = e->position();
        mPressed = true;
        mTouched = true;
        mRestFor = 0.0;
        const int body = mSession->pick(o, d, &at);
        if (e->modifiers() & Qt::ControlModifier)
        {
            if (body >= 0)
                mSession->pin(body, !mSession->pinned(body));
            return true;
        }
        switch (mMode)
        {
        case Mode::Grab:
        case Mode::Drop:
            if (body >= 0)
            {
                mSession->grab(body, at);
                mPlanePoint = at;
                mPlaneNormal = -glm::normalize(d);
                mLift = mMode == Mode::Drop || (e->modifiers() & Qt::ShiftModifier);
            }
            else if (mMode == Mode::Drop)
                mSession->drop();
            break;
        case Mode::Push:
            mPushBody = body;
            mPushAt = at;
            break;
        case Mode::Wiggle:
            mWiggling = true;
            break;
        case Mode::Sweep:
            mSweeping = true;
            mGather = e->modifiers() & Qt::ShiftModifier;
            mSweepLast.reset();
            break;
        case Mode::Shake:
        {
            const auto hit = mSession->world().scene().raycast(o, d, 1e7);
            if (hit && hit->body >= 0)
            {
                glm::dvec3 right(view.view[0][0], view.view[1][0], view.view[2][0]);
                right.y = 0.0;
                right = glm::length(right) > 1e-9 ? glm::normalize(right) : glm::dvec3(1, 0, 0);
                mSession->shake(hit->body, right * 3.0, 1.0);
            }
            break;
        }
        default:
            break;
        }
        mCursor = e->position();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mCursor = e->position();
        mView = view;
        mGather = e->modifiers() & Qt::ShiftModifier;
        if (!mSession || !mPressed)
        {
            viewportTools::requestRedraw();
            return false;
        }
        if (mSession->held() >= 0)
        {
            glm::dvec3 o, d;
            view.ray(e->position(), o, d);
            if (mLift && (e->modifiers() & Qt::ShiftModifier))
            {
                // Straight up and down by the cursor's height on screen.
                const double dy = (mPress.y() - e->position().y()) * view.unitsPerPixel(mPlanePoint);
                mSession->drag(mPlanePoint + glm::dvec3(0, dy, 0));
            }
            else
            {
                const double denom = glm::dot(d, mPlaneNormal);
                if (std::abs(denom) > 1e-9)
                {
                    const double t = glm::dot(mPlanePoint - o, mPlaneNormal) / denom;
                    if (t > 0.0)
                        mSession->drag(o + d * t);
                }
            }
        }
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mPressed = false;
        mWiggling = false;
        mSweeping = false;
        if (!mSession)
            return true;
        if (mMode == Mode::Push && mPushBody >= 0)
        {
            // The drag on screen, laid on the ground plane through the point pushed: its length sets the speed.
            glm::dvec3 o0, d0, o1, d1;
            view.ray(mPress, o0, d0);
            view.ray(e->position(), o1, d1);
            auto onPlane = [&](const glm::dvec3& o, const glm::dvec3& d) -> std::optional<glm::dvec3> {
                if (std::abs(d.y) < 1e-9)
                    return std::nullopt;
                const double t = (mPushAt.y - o.y) / d.y;
                return t > 0.0 ? std::optional<glm::dvec3>(o + d * t) : std::nullopt;
            };
            const auto a = onPlane(o0, d0), b = onPlane(o1, d1);
            if (a && b && glm::length(*b - *a) > 0.5)
            {
                const glm::dvec3 drag = *b - *a;
                const double speed = std::clamp(glm::length(drag) * 6.0, 20.0, 1500.0);
                mSession->push(mPushBody, mPushAt, glm::normalize(drag) * speed);
            }
        }
        mPushBody = -1;
        mSession->release();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Escape:
            if (mSession)
            {
                mSession->revert(scene::Graph::current());
                mSession.reset();
            }
            viewportTools::set(nullptr);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            keep();
            return true;
        case Qt::Key_Tab:
            mMode = Mode((int(mMode) + 1) % int(Mode::Count));
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_P:
            if (mSession)
                mSession->setPaused(!mSession->paused());
            return true;
        case Qt::Key_BracketLeft:
            mRadius = std::max(4.0, mRadius / 1.25);
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_BracketRight:
            mRadius = std::min(2000.0, mRadius * 1.25);
            viewportTools::requestRedraw();
            return true;
        default:
            return toggleKey(e->key(), e->modifiers());
        }
    }

    bool hotkey(const QKeyEvent& e, std::string_view action) override
    {
        // 1 to 6 pick the mode while the tool is up, over their usual smooth preview.
        if (e.key() >= Qt::Key_1 && e.key() < Qt::Key_1 + int(Mode::Count) && e.modifiers() == Qt::NoModifier)
        {
            mMode = Mode(e.key() - Qt::Key_1);
            viewportTools::requestRedraw();
            return true;
        }
        if (toggleKey(e.key(), e.modifiers()))
            return true;
        if (action == "edit.undo" && mSession)
        {
            mSession->revert(scene::Graph::current());
            mSession.reset();
            viewportTools::requestRedraw();
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        mView = view;
        ToolStatus status{.tool = "Push Tool", .mode = modeName(mMode)};
        if (!mSession)
        {
            addModeHint(mMode, status);
            status.prompt = "select what may move, then click";
        }
        else
        {
            status.flags = {{"paused", mSession->paused()}, {"slow", mSlow}, {"auto keep", mAutoKeep}};
            status.keys = {{"1-6", "mode"}};
            status.prompt = addModeHint(mMode, status);
            for (const KeyHint& k : {KeyHint{"Ctrl-click", "pin"}, KeyHint{"P", "pause"}, KeyHint{"L", "slow"}, KeyHint{"K", "auto keep"},
                                     KeyHint{"Enter", "keep"}, KeyHint{"Esc", "revert"}})
                status.keys.push_back(k);
        }
        drawToolStatus(p, view, status);
        if (mMode == Mode::Wiggle || mMode == Mode::Sweep)
            drawBrush(p, view);
        if (mSession && mSession->held() >= 0)
        {
            if (auto a = view.project(mSession->world().scene().centerOfMass(mSession->held())))
                p.line(*a, mCursor, 1.5f, {0.9f, 0.9f, 0.9f, 0.8f});
        }
    }

    std::optional<QCursor> cursor() const override { return QCursor(mMode == Mode::Grab ? Qt::OpenHandCursor : Qt::CrossCursor); }

private:
    bool ensureSession()
    {
        if (mSession)
            return true;
        scene::Graph& g = scene::Graph::current();
        WorldOptions o;
        for (NodeId id : g.selection())
            if (physical(g, id))
                o.movers.push_back(id);
        if (o.movers.empty())
        {
            log::write(LogLevel::Warning, "Push tool: select the meshes that may move");
            return false;
        }
        // Whatever touches the selection may move too, as push() does.
        for (NodeId id : neighbours(g, o.movers, 2.0, 8.0, {}))
            o.movers.push_back(id);
        try
        {
            mSession = std::make_unique<Session>(g, o);
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Warning, std::string("Push tool: ") + e.what());
            return false;
        }
        mSession->setTimeScale(mSlow ? kSlowMotion : 1.0);
        mClock.restart();
        return true;
    }

    /** K toggles auto keep, L slow motion; both stay as set for the next session. */
    bool toggleKey(int key, Qt::KeyboardModifiers mods)
    {
        if (mods != Qt::NoModifier)
            return false;
        if (key == Qt::Key_K)
        {
            mAutoKeep = !mAutoKeep;
            PrefStore::get().set("stagehand", "/push/autoKeep", mAutoKeep);
        }
        else if (key == Qt::Key_L)
        {
            mSlow = !mSlow;
            if (mSession)
                mSession->setTimeScale(mSlow ? kSlowMotion : 1.0);
        }
        else
            return false;
        viewportTools::requestRedraw();
        return true;
    }

    /** Keeps the result once everything rests after the hand lets go, so each drag is its own undo step. */
    void keepAtRest(double dt)
    {
        const bool idle = !mPressed && !mWiggling && !mSweeping && mSession->held() < 0 && !mSession->paused();
        if (!mAutoKeep || !mTouched || !idle)
        {
            mRestFor = 0.0;
            return;
        }
        mRestFor = mSession->world().scene().atRest() ? mRestFor + dt : 0.0;
        if (mRestFor >= kKeepAfterRest)
        {
            keep();
            mTouched = false;
        }
    }

    void tick()
    {
        const double dt = double(mClock.restart()) / 1000.0;
        if (!mSession)
            return;
        if (mWiggling)
        {
            glm::dvec3 o, d;
            mView.ray(mCursor, o, d);
            if (const auto hit = mSession->world().scene().raycast(o, d, 1e7))
                mSession->wiggle(hit->point, mRadius, kWiggleSpeedPerSecond * dt);
        }
        if (mSweeping)
        {
            glm::dvec3 o, d;
            mView.ray(mCursor, o, d);
            if (const auto hit = mSession->world().scene().raycast(o, d, 1e7))
            {
                const glm::dvec3 motion = mSweepLast ? hit->point - *mSweepLast : glm::dvec3(0.0);
                const double speed = mGather ? kGatherSpeed : std::min(glm::length(motion) / std::max(dt, 1e-3), kSweepSpeedMax);
                mSession->sweep(hit->point, motion, mRadius, speed, mGather);
                mSweepLast = hit->point;
            }
        }
        mSession->advance(dt);
        mSession->show(scene::Graph::current());
        keepAtRest(dt);
        viewportTools::requestRedraw();
    }

    void keep()
    {
        if (!mSession)
            return;
        mSession->commit(scene::Graph::current(), "Push Tool");
        mSession.reset();
    }

    void drawBrush(OverlayPainter& p, const ViewInfo& view)
    {
        glm::dvec3 o, d;
        view.ray(mCursor, o, d);
        double px = 40.0;
        if (mSession)
            if (const auto hit = mSession->world().scene().raycast(o, d, 1e7))
                px = mRadius / std::max(view.unitsPerPixel(hit->point), 1e-9);
        std::vector<QPointF> ring;
        for (int k = 0; k < 48; ++k)
        {
            const double a = 2.0 * 3.14159265358979323846 * k / 48.0;
            ring.emplace_back(mCursor.x() + std::cos(a) * px, mCursor.y() + std::sin(a) * px);
        }
        p.polyline(ring, 1.5f, {0.9f, 0.9f, 0.9f, 0.8f}, true);
    }

    std::unique_ptr<Session> mSession;
    QTimer* mTimer = nullptr;
    QElapsedTimer mClock;
    ViewInfo mView;
    Mode mMode = Mode::Grab;
    QPointF mPress, mCursor;
    bool mPressed = false, mLift = false, mWiggling = false, mSweeping = false, mGather = false;
    bool mAutoKeep = true, mSlow = false, mTouched = false;
    double mRestFor = 0.0;
    std::optional<glm::dvec3> mSweepLast;
    glm::dvec3 mPlanePoint{0.0}, mPlaneNormal{0, 0, 1};
    int mPushBody = -1;
    glm::dvec3 mPushAt{0.0};
    double mRadius = 30.0; // cm
};

void startTool(Mode m)
{
    gStartMode = m;
    if (viewportTools::activeId() == kPushTool)
        viewportTools::set(nullptr);
    viewportTools::activate(kPushTool);
}

/** Runs a command on the selection from a menu, reporting failure on the status line. */
void onSelection(const char* command, nlohmann::json flags = nlohmann::json::object())
{
    scene::Graph& g = scene::Graph::current();
    nlohmann::json names = nlohmann::json::array();
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            names.push_back(n->path());
    flags["args"] = names;
    try
    {
        ops::execute(command, flags, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}
} // namespace

void registerUi(ext::Context& ctx)
{
    viewportTools::add(ctx, {.id = kPushTool, .label = "Push Tool", .icon = "hand-finger", .create = [] { return std::make_unique<PushTool>(); }});

    menus::addItem(ctx, {.id = "stagehand.menu.tools", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Physics Tools",
                         .before = levelToolkit::kAssetsSection});
    struct ToolAction
    {
        const char* id;
        const char* menuId;
        const char* label;
        Mode mode;
        const char* doc;
    };
    const ToolAction tools[] = {
        {"stagehand.tool.grab", "stagehand.menu.grab", "Grab with Physics", Mode::Grab, "Carry selected things through the scene; they collide, fall and knock others over."},
        {"stagehand.tool.push", "stagehand.menu.push", "Push Tool", Mode::Push, "Drag a direction on a thing and let go to shove it; neighbours it hits react."},
        {"stagehand.tool.wiggle", "stagehand.menu.wiggle", "Wiggle Brush", Mode::Wiggle, "Hold over things to jostle them into a natural, slightly messy rest."},
        {"stagehand.tool.drop", "stagehand.menu.drop", "Drop Tool", Mode::Drop, "Lift things and let them fall where they would land."},
        {"stagehand.tool.shake", "stagehand.menu.shake", "Shake Tool", Mode::Shake, "Click a shelf or table to shake what rests on it."},
        {"stagehand.tool.sweep", "stagehand.menu.sweep", "Sweep Brush", Mode::Sweep, "Drag to broom things along the stroke; hold Shift to gather them under the brush."},
    };
    for (const ToolAction& t : tools)
    {
        const Mode m = t.mode;
        actions::add(ctx, {.id = t.id, .label = t.label, .category = "Dress", .run = [m] { startTool(m); }, .doc = t.doc});
        menus::addItem(ctx, {.id = t.menuId, .menu = "Level/Physics Tools", .action = t.id});
    }
    menus::addItem(ctx, {.menu = "Level/Physics Tools", .kind = menus::Kind::Separator});

    actions::add(ctx, {.id = "stagehand.settle", .label = "Settle", .category = "Dress", .run = [] { onSelection("settle"); },
                       .doc = "Let the selection fall and come to rest against what is around it.", .python = "settle(['mug', 'book_01'])"});
    actions::add(ctx, {.id = "stagehand.drop", .label = "Drop", .category = "Dress", .run = [] { onSelection("drop"); },
                       .doc = "Drop the selection onto whatever is below it.", .python = "drop('mug', onto='desk')"});
    actions::add(ctx, {.id = "stagehand.wiggle", .label = "Wiggle", .category = "Dress", .run = [] { onSelection("wiggle"); },
                       .doc = "Jostle the selection with small random pushes and let it settle.", .python = "wiggle(['book_01', 'book_02'])"});
    actions::add(ctx, {.id = "stagehand.shake", .label = "Shake", .category = "Dress", .run = [] { onSelection("shake"); },
                       .doc = "Shake the selected shelf or table so what rests on it shuffles and settles.", .python = "shake('shelf')"});
    actions::add(ctx, {.id = "stagehand.check", .label = "Check Dressing", .category = "Dress", .run = [] { onSelection("dressCheck"); },
                       .doc = "Report anything selected that would move, floats or sits inside something.", .python = "dressCheck(['mug'])"});
    for (const char* id : {"stagehand.settle", "stagehand.drop", "stagehand.wiggle", "stagehand.shake", "stagehand.check"})
        menus::addItem(ctx, {.menu = "Level/Physics Tools", .action = id});

    // Arrangements: one command each, so their menu items get option boxes for items, seed and the rest.
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
    } arrangements[] = {
        {"stagehand.fillShelf", "Fill Shelf", "fillShelf", "Fill the selected boards with rows of items, settled.",
         "fillShelf(['Shelf_1'], items=['Books/Hardback', 'Books/Paperback'])"},
        {"stagehand.scatterProps", "Scatter Props", "scatterProps", "Scatter items over the selected surface.",
         "scatterProps('Floor', items=['Debris/Brick'], density=12)"},
        {"stagehand.pileProps", "Pile Props", "pileProps", "Heap items up in a footprint and settle them.",
         "pileProps(items=['Debris/Brick'], at=[0, 0, 0], count=40)"},
        {"stagehand.stackProps", "Stack Props", "stackProps", "Stack items one on another.", "stackProps(items=['Kitchen/Plate'], at=[0, 90, 0])"},
        {"stagehand.pathProps", "Props Along Curve", "pathProps", "Place items along the selected curves.", "pathProps('Kerb', items=['Debris/Rebar'], spacing=60)"},
        {"stagehand.debrisProps", "Debris Around", "debrisProps", "Strew chips around the selected broken objects, sized from their pieces.", "debrisProps('Wall_fractured')"},
        {"stagehand.arrangePreset", "Arrangement Preset", "arrangePreset", "Dress a place from a preset: storeroom, rubble corner, library shelf, messy desk.",
         "arrangePreset('Storeroom', slots={'floor': 'Floor'})"},
    };
    menus::addItem(ctx, {.id = "stagehand.menu.arrange", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Arrange",
                         .before = levelToolkit::kAssetsSection});
    for (const auto& e : arrangements)
    {
        actions::add(ctx, {.id = e.id, .label = e.label, .category = "Dress", .doc = e.doc, .python = e.python, .command = e.command, .call = nlohmann::json::object()});
        menus::addItem(ctx, {.menu = "Level/Arrange", .action = e.id});
    }

    menus::addItem(ctx, {.id = "stagehand.menu.body", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Physics Body",
                         .before = levelToolkit::kAssetsSection});
    actions::add(ctx, {.id = "stagehand.body.dynamic", .label = "Make Dynamic", .category = "Dress",
                       .run = [] { onSelection("physicsBody", {{"role", "dynamic"}}); }, .doc = "The selection moves whenever things settle or get pushed."});
    actions::add(ctx, {.id = "stagehand.body.static", .label = "Make Static", .category = "Dress",
                       .run = [] { onSelection("physicsBody", {{"role", "static"}}); }, .doc = "The selection only gets hit, never moves."});
    actions::add(ctx, {.id = "stagehand.body.off", .label = "Ignore in Physics", .category = "Dress",
                       .run = [] { onSelection("physicsBody", {{"role", "off"}}); }, .doc = "Physics passes through the selection."});
    actions::add(ctx, {.id = "stagehand.body.remove", .label = "Remove Physics Body", .category = "Dress",
                       .run = [] { onSelection("physicsBody", {{"remove", true}}); }, .doc = "Back to the defaults: static unless a command moves it."});
    for (const char* id : {"stagehand.body.dynamic", "stagehand.body.static", "stagehand.body.off", "stagehand.body.remove"})
        menus::addItem(ctx, {.menu = "Level/Physics Body", .action = id});

    levelToolkit::add(ctx, {.id = "stagehand.dress",
                            .title = "Dress",
                            .items = {{"window.propLibrary", "books", "Library", "Library"}, {},
                                      {"stagehand.tool.grab", "hand-grab", "Grab", "Grab"}, {"stagehand.tool.push", "hand-finger", "Push"},
                                      {"stagehand.tool.wiggle", "wave-sine", "Wiggle"}, {"stagehand.tool.drop", "arrow-bar-to-down", "Drop"},
                                      {"stagehand.tool.shake", "activity", "Shake"}, {"stagehand.tool.sweep", "brush", "Sweep"}, {},
                                      {"stagehand.settle", "anchor", "Settle", "Settle"}, {"stagehand.drop", "arrow-down", "Drop"}, {"stagehand.wiggle", "dice", "Wiggle"},
                                      {"stagehand.shake", "activity", "Shake"}, {"stagehand.check", "checklist", "Check"}, {},
                                      {"stagehand.body.dynamic", "", "Dynamic"}, {"stagehand.body.static", "", "Static"}, {"stagehand.body.off", "", "Ignore"},
                                      {"stagehand.body.remove", "", "Default"}},
                            .order = 40, .docs = "stagehand"});
    levelToolkit::add(ctx, {.id = "stagehand.arrange",
                            .title = "Arrange",
                            .items = {{"stagehand.fillShelf", "books", "Fill Shelf"}, {"stagehand.scatterProps", "grain", "Scatter", "Scatter"},
                                      {"stagehand.pileProps", "stack-2", "Pile"}, {"stagehand.stackProps", "stack", "Stack"}, {"stagehand.pathProps", "route", "Along Curve"},
                                      {"stagehand.debrisProps", "wall", "Debris"}, {"stagehand.arrangePreset", "template", "Preset"}},
                            .order = 41, .docs = "stagehand"});
    levelToolkit::add(ctx, {.id = "stagehand.kit",
                            .title = "Level Kit",
                            .items = {{"stagehand.tool.kitPlace", "puzzle", "Kit Place", "Kit"}, {"stagehand.snapAll", "magnet", "Snap All"},
                                      {"stagehand.addSnapPoint", "point", "Snap Point"}, {},
                                      {"stagehand.dropToSurface", "arrow-bar-to-down", "To Surface", "To Ground"}, {"stagehand.alignObjects", "layout-align-left", "Align"},
                                      {"stagehand.distribute", "layout-distribute-horizontal", "Distribute"}, {"stagehand.randomize", "arrows-shuffle", "Randomize"}},
                            .order = 42, .docs = "stagehand"});
}
} // namespace stagehand
