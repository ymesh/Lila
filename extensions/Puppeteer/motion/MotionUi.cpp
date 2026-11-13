// The Motion panel and Drive tool: generated motion for the character the animator works on. Every edit is a command
// (motionFill, motionFollowPath, motionSpot*, motionDrive), so it is undoable, echoed and scriptable.

#include "motion/PuppeteerMotion.h"

#include "Common.h"
#include "Puppeteer.h"
#include "anim/AnimOps.h"

#include <lila/Command.h>
#include <lila/Controls.h>
#include <lila/DebugDraw.h>
#include <lila/Events.h>
#include <lila/FileDialog.h>
#include <lila/Icons.h>
#include <lila/Motion.h>
#include <lila/Transform.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/ViewportTool.h>

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <optional>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#endif

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer::motionui
{
namespace
{
namespace mo = lila::motion;
using T = ArgType;

Graph& graph() { return Graph::current(); }

// Panel state -------------------------------------------------------------------------------------------------------

/** What the panel works on and with, kept while the selection moves to a curve or a spot. */
struct PanelState
{
    std::string character; // the character's root joint path
    std::string style = "walk";
    double speed = 0.0; // m/s, 0 the gait's own
    int variant = 0;
    json lastFill; // the last Fill call, for New Variant
};
PanelState& state()
{
    static PanelState s;
    return s;
}

/** The character a node belongs to (its root joint), or empty. */
std::string characterOf(Graph& g, NodeId id)
{
    try
    {
        const anim::Character ch = anim::resolveCharacter(g, id);
        return ch.root ? g.node(ch.root)->path() : std::string();
    }
    catch (const std::exception&)
    {
        return {};
    }
}

/** The panel's character: the selection's when it has one, else the one remembered. */
std::string currentCharacter()
{
    Graph& g = graph();
    for (NodeId id : g.selection())
        if (const std::string c = characterOf(g, id); !c.empty())
            return state().character = c;
    if (!state().character.empty() && !g.find(state().character))
        state().character.clear();
    return state().character;
}

json withCharacter(json call)
{
    const std::string c = currentCharacter();
    if (c.empty())
        throw std::invalid_argument("select a character");
    call["args"] = {c};
    return call;
}

/** Runs a command for the panel's character; errors go to the log. */
json onCharacter(const char* cmd, json call = json::object())
{
    try
    {
        return ops::execute(cmd, withCharacter(std::move(call)), CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Error, std::string(cmd) + ": " + e.what());
        return nullptr;
    }
}

/** Selected nodes that make a path: curves and spots, in selection order. */
json selectedPath()
{
    Graph& g = graph();
    json path = json::array();
    for (NodeId id : g.selection())
    {
        const Node& n = *g.node(id);
        if (n.type().isA(mo::kSpotType) || n.type().attrIndex("worldSpace") >= 0)
            path.push_back(n.path());
    }
    return path;
}

void fill(json call)
{
    state().lastFill = call;
    state().variant = int(call.value("variant", 0));
    onCharacter("motionFill", std::move(call));
}

void newVariant()
{
    json call = state().lastFill.is_object() ? state().lastFill : json::object();
    call["variant"] = ++state().variant;
    if (!call.contains("all") && !call.contains("time"))
        call["time"] = graph().time();
    state().lastFill = call;
    onCharacter("motionFill", call);
}

void followPath()
{
    const json path = selectedPath();
    if (path.empty())
    {
        log::write(LogLevel::Error, "Follow Path: select the path curve or spots too (draw one with Draw Path)");
        return;
    }
    json call = {{"path", path}, {"style", state().style}};
    if (state().speed > 0.0)
        call["speed"] = state().speed;
    onCharacter("motionFollowPath", call);
}

/** A starter spot a little ahead of the character, facing the way it does. */
void addSpot(const std::string& preset)
{
    Graph& g = graph();
    glm::dvec3 at(0.0);
    double yaw = 0.0;
    if (const std::string c = currentCharacter(); !c.empty())
        if (Node* root = g.find(c))
        {
            const glm::dmat4 w = worldOf(g, root->id());
            glm::dvec3 f = glm::dvec3(w[2]);
            f.y = 0.0;
            if (glm::length(f) < 1e-6)
                f = {0, 0, 1};
            f = glm::normalize(f);
            at = glm::dvec3(w[3].x, 0.0, w[3].z) + f * 250.0;
            yaw = glm::degrees(std::atan2(f.x, f.z));
        }
    const json r = runCommand("motionSpot", {{"preset", preset}, {"position", {at.x, at.y, at.z}}, {"yaw", yaw}});
    if (r.is_object() && r.contains("node"))
        runCommand("select", {{"args", {r["node"]}}});
}

std::string selectedSpot()
{
    Graph& g = graph();
    for (NodeId id : g.selection())
        if (g.node(id)->type().isA(mo::kSpotType))
            return g.node(id)->path();
    return {};
}

// Driving -----------------------------------------------------------------------------------------------------------

/** One drive in progress: the character's joints follow the driver frame by frame until it stops. */
struct DriveSession
{
    std::unique_ptr<mo::CharacterMap> map;
    std::unique_ptr<mo::LiveDriver> driver;
    std::vector<glm::dvec3> euler;
    double start = 0.0;
    std::string take = "Drive";
    std::string character;
    mo::DriveInput input;
    int frames = 0;
};
std::optional<DriveSession>& session()
{
    static std::optional<DriveSession> s;
    return s;
}

/** Shows a pose on the joints without keys or undo: driven channels hold the value until time moves. */
void show(DriveSession& s, const mo::Pose& soma)
{
    Graph& g = graph();
    const retarget::Pose p = s.map->toCharacter(soma);
    const retarget::SceneSkeleton& sk = s.map->scene();
    UndoStack& undo = UndoStack::get();
    const bool was = undo.enabled();
    undo.setEnabled(false);
    for (size_t j = 0; j < sk.joints.size(); ++j)
    {
        const NodeId id = sk.joints[j];
        const Node& n = *g.node(id);
        const auto order = RotateOrder(n.as<int64_t>(Transform::kRotateOrder));
        s.euler[j] = xformops::closestEuler(glm::mat3_cast(p.rotation[j]), order, s.euler[j]);
        try
        {
            g.setValue({id, Transform::kRotate, -1}, s.euler[j]);
            if (glm::length(p.translate[j] - n.as<glm::dvec3>(Transform::kTranslate)) > 1e-6)
                g.setValue({id, Transform::kTranslate, -1}, p.translate[j]);
        }
        catch (const std::invalid_argument&)
        {
            // Locked or driven by something other than a curve: leave it.
        }
    }
    undo.setEnabled(was);
    viewportTools::requestRedraw();
}

json driveStart(const std::string& character, const std::string& style, bool sync, const std::string& take)
{
    Graph& g = graph();
    if (session())
        throw std::invalid_argument("a drive is running; stop it first");
    mo::Engine* engine = mo::Engine::shared();
    if (!engine)
        throw std::invalid_argument(mo::Engine::status().message);
    Node* n = g.find(character);
    if (!n)
        throw std::invalid_argument("no node named '" + character + "'");
    const anim::Character ch = anim::resolveCharacter(g, n->id());
    DriveSession s;
    s.map = std::make_unique<mo::CharacterMap>(mo::CharacterMap::forJoint(g, ch.root));
    s.start = g.time();
    s.take = take.empty() ? "Drive" : take;
    s.character = g.node(ch.root)->path();
    const mo::Pose now = s.map->sampleSoma(g, s.start);
    std::vector<mo::Pose> before;
    for (int k = 3; k >= 1; --k)
        before.push_back(s.map->sampleSoma(g, s.start - double(k) / mo::kSampleRate));
    s.driver = std::make_unique<mo::LiveDriver>(*engine, now, before);
    s.driver->setSynchronous(sync);
    s.input.style = style;
    for (NodeId id : s.map->scene().joints)
        s.euler.push_back(g.node(id)->as<glm::dvec3>(Transform::kRotate));
    session() = std::move(s);
    return {{"character", session()->character}, {"device", engine->info().device}, {"start", session()->start}};
}

json driveStep(int frames)
{
    DriveSession& s = *session();
    s.driver->setInput(s.input);
    for (int i = 0; i < frames; ++i)
    {
        const mo::Pose& p = s.driver->tick();
        ++s.frames;
        if (i + 1 == frames)
            show(s, p);
    }
    return {{"frames", s.frames}, {"generations", s.driver->generations()}, {"latencyFrames", s.driver->latencyFrames()}, {"error", s.driver->error()}};
}

json driveStop(bool keep)
{
    std::optional<DriveSession>& opt = session();
    if (!opt)
        throw std::invalid_argument("no drive is running");
    DriveSession s = std::move(*opt);
    opt.reset();
    s.driver->wait();
    Graph& g = graph();
    // Held values go when the time moves; the take's keys then show.
    g.setTime(s.start);
    g.evaluate();
    json out = {{"frames", s.driver->recorded().size()}, {"generations", s.driver->generations()}};
    if (!keep || s.driver->recorded().size() < 2)
    {
        out["take"] = nullptr;
        viewportTools::requestRedraw();
        return out;
    }
    mo::Clip clip;
    clip.frames = s.driver->recorded();
    clip.contacts = s.driver->recordedContacts();
    mo::KeyOptions ko;
    ko.take = s.take;
    ko.start = s.start;
    const retarget::RunReport rep = mo::keyClip(g, *s.map, clip, ko);
    out.update(rep.toJson());
    viewportTools::requestRedraw();
    return out;
}

/** The gamepad's left stick (x right, y up) and whether its bumpers or stick press ask to run or crouch. */
struct Pad
{
    glm::vec2 stick{0.0f};
    bool run = false, crouch = false, connected = false;
};
Pad readPad()
{
    Pad pad;
#ifdef _WIN32
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetState get = [] {
        HMODULE h = LoadLibraryW(L"xinput1_4.dll");
        return h ? reinterpret_cast<GetState>(GetProcAddress(h, "XInputGetState")) : nullptr;
    }();
    XINPUT_STATE st{};
    if (get && get(0, &st) == ERROR_SUCCESS)
    {
        pad.connected = true;
        auto axis = [](SHORT v) {
            const float f = float(v) / 32767.0f;
            return std::abs(f) < 0.24f ? 0.0f : std::clamp(f, -1.0f, 1.0f);
        };
        pad.stick = {axis(st.Gamepad.sThumbLX), axis(st.Gamepad.sThumbLY)};
        pad.run = st.Gamepad.bRightTrigger > 64 || (st.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER);
        pad.crouch = (st.Gamepad.wButtons & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_B)) != 0;
    }
#endif
    return pad;
}

constexpr std::array<int, 6> kDriveKeys = {Qt::Key_W, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_C, Qt::Key_F};

/** Steers the character from WASD (camera relative) or a gamepad; the motion generates as it goes and becomes a take. */
class DriveTool final : public ViewportTool, public QObject
{
public:
    const char* name() const override { return "Drive"; }

    void activate() override
    {
        const std::string c = currentCharacter();
        try
        {
            if (c.empty())
                throw std::invalid_argument("select a character to drive");
            driveStart(c, state().style, false, "Drive");
        }
        catch (const std::exception& e)
        {
            log::write(LogLevel::Error, std::string("Drive: ") + e.what());
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return;
        }
        qApp->installEventFilter(this);
        mTimer = new QTimer;
        mTimer->setTimerType(Qt::PreciseTimer);
        QObject::connect(mTimer, &QTimer::timeout, [this] { tick(); });
        mTimer->start(int(1000.0 / mo::kSampleRate));
    }

    void deactivate() override
    {
        qApp->removeEventFilter(this);
        delete mTimer;
        mTimer = nullptr;
        mHeld.clear();
        if (session())
            try
            {
                const json r = ops::execute("motionDrive", {{"stop", true}, {"keep", !mCancelled}}, CallSource::UI);
                if (r.value("take", json()).is_string())
                    log::write(LogLevel::Info, "Drive: keyed " + std::to_string(r.value("frames", 0)) + " frames into " + r["take"].get<std::string>());
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Error, e.what());
            }
        mCancelled = false;
        viewportTools::requestRedraw();
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        // Driving keys are app hotkeys too; the tool takes them while it drives.
        return std::find(kDriveKeys.begin(), kDriveKeys.end(), e.key()) != kDriveKeys.end() && !(e.modifiers() & (Qt::ControlModifier | Qt::AltModifier));
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            mCancelled = e->key() == Qt::Key_Escape;
            viewportTools::set(nullptr);
            return true;
        }
        return hotkey(*e, {});
    }

    bool eventFilter(QObject*, QEvent* ev) override
    {
        if (ev->type() != QEvent::KeyPress && ev->type() != QEvent::KeyRelease)
            return false;
        auto* k = static_cast<QKeyEvent*>(ev);
        if (k->isAutoRepeat())
            return std::find(kDriveKeys.begin(), kDriveKeys.end(), k->key()) != kDriveKeys.end();
        if (k->key() == Qt::Key_Shift)
            mRun = ev->type() == QEvent::KeyPress;
        if (std::find(kDriveKeys.begin(), kDriveKeys.end(), k->key()) == kDriveKeys.end() || (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
            return false;
        if (ev->type() == QEvent::KeyPress)
        {
            if (k->key() == Qt::Key_C)
                mCrouch = !mCrouch;
            else if (k->key() == Qt::Key_F)
                mStrafe = !mStrafe;
            else
                mHeld.insert(k->key());
        }
        else
            mHeld.erase(k->key());
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        mForward = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
        const DriveSession* s = session() ? &*session() : nullptr;
        ToolStatus st{.tool = "Drive",
                      .subject = s ? QString::fromStdString(s->character) : QString(),
                      .mode = QString::fromStdString(styleLabel()),
                      .fields = {{"rec", s ? QString("%1 f").arg(s->frames) : QString("-")}},
                      .flags = {{"run", mRun || mPad.run}, {"crouch", mCrouch || mPad.crouch}, {"strafe", mStrafe}},
                      .prompt = mPad.connected ? "steer with the left stick" : "steer with W A S D",
                      .keys = {{"WASD", "move"}, {"Shift", "run"}, {"C", "crouch"}, {"F", "strafe"}, {"Enter", "keep take"}, {"Esc", "discard"}}};
        drawToolStatus(p, view, st);
        if (s && !s->driver->error().empty())
            drawToolMessage(p, view, QString::fromStdString(s->driver->error()), {1.0f, 0.6f, 0.4f, 1.0f});
    }

private:
    std::string styleLabel() const
    {
        if (mCrouch || mPad.crouch)
            return "crouch";
        if (mRun || mPad.run)
            return "run";
        return state().style == "crouch" || state().style == "run" ? state().style : "walk";
    }

    void tick()
    {
        if (!session())
            return;
        mPad = readPad();
        glm::dvec3 f = mForward;
        f.y = 0.0;
        f = glm::length(f) > 1e-6 ? glm::normalize(f) : glm::dvec3(0, 0, -1);
        const glm::dvec3 r = glm::normalize(glm::cross(f, glm::dvec3(0, 1, 0)));
        glm::dvec3 m(0.0);
        if (mHeld.count(Qt::Key_W))
            m += f;
        if (mHeld.count(Qt::Key_S))
            m -= f;
        if (mHeld.count(Qt::Key_D))
            m += r;
        if (mHeld.count(Qt::Key_A))
            m -= r;
        if (glm::length(m) > 1e-6)
            m = glm::normalize(m);
        m += f * double(mPad.stick.y) + r * double(mPad.stick.x);
        if (glm::length(m) > 1.0)
            m = glm::normalize(m);
        DriveSession& s = *session();
        const glm::vec2 move(float(m.x), float(m.z));
        if (mStrafe && glm::length(mFacing) < 1e-3f)
            mFacing = glm::vec2(float(f.x), float(f.z));
        if (!mStrafe)
            mFacing = {0.0f, 0.0f};
        s.input.move = move;
        s.input.facing = mFacing;
        s.input.style = styleLabel();
        driveStep(1);
    }

    QTimer* mTimer = nullptr;
    std::set<int> mHeld;
    bool mRun = false, mCrouch = false, mStrafe = false, mCancelled = false;
    glm::vec2 mFacing{0.0f};
    glm::dvec3 mForward{0.0, 0.0, -1.0};
    Pad mPad;
};

// Spot previews -----------------------------------------------------------------------------------------------------

/** Each spot's target poses as stick figures where the character will stand, the selected spot's brighter. */
void drawSpots(const debugDraw::Channel& ch)
{
    ch.clear();
    Graph& g = graph();
    const mo::SomaSkeleton& s = mo::SomaSkeleton::shipped();
    std::vector<int> model;
    for (const char* n : mo::modelJointNames())
        model.push_back(s.find(n));
    const auto& sel = g.selection();
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (!n || !n->type().isA(mo::kSpotType))
            continue;
        std::vector<mo::PlacedTarget> targets;
        try
        {
            const auto [at, heading] = mo::groundFrame(g, id);
            targets = mo::placeSpot(mo::spotOf(g, id), at, heading);
        }
        catch (const std::exception&)
        {
            continue;
        }
        const bool picked = std::find(sel.begin(), sel.end(), id) != sel.end();
        for (size_t t = 0; t < targets.size(); ++t)
        {
            const mo::Pose w = s.world(targets[t].poses.front());
            const float shade = picked ? 0.9f : 0.45f;
            debugDraw::Style style{.color = lila::math::LinearColor(shade, shade * (1.0f - 0.12f * float(t)), shade, 1.0f), .thickness = picked ? 2.0f : 1.0f,
                                   .persistent = true, .channel = ch};
            std::vector<lila::math::Vector> segs;
            for (int j : model)
            {
                const int parent = s.parents[size_t(j)];
                if (j <= 0 || parent < 0 || std::find(model.begin(), model.end(), parent) == model.end())
                    continue;
                const glm::dvec3 a = glm::dvec3(w[size_t(parent)].p) * mo::kCmPerMeter, b = glm::dvec3(w[size_t(j)].p) * mo::kCmPerMeter;
                segs.push_back({a.x, a.y, a.z});
                segs.push_back({b.x, b.y, b.z});
            }
            debugDraw::segments(segs, style);
        }
    }
    viewportTools::requestRedraw();
}

// Panel -------------------------------------------------------------------------------------------------------------

const std::vector<std::pair<std::string, QString>> kStyles = {
    {"walk", "Walk"}, {"run", "Run"}, {"crouch", "Crouch"}, {"strafeLeft", "Strafe L"}, {"strafeRight", "Strafe R"}, {"backward", "Back"},
};
const std::vector<std::pair<std::string, const char*>> kStarterSpots = {
    {"Pick Up", "hand-grab"}, {"Duck", "arrow-down-bar"}, {"Leap", "jump-rope"}, {"Crouch", "arrow-bar-to-down"},
};

QWidget* fieldRow(const QString& label, QWidget* field, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* l = new QLabel(label, row);
    l->setMinimumWidth(l->fontMetrics().horizontalAdvance("Character") + 4);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(l);
    h->addWidget(field, 1);
    return row;
}

QWidget* makePanel(QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(8);

    QWidget* banner = controls::warningBanner(w);
    v->addWidget(banner);

    auto* who = new QLabel(w);
    v->addWidget(fieldRow("Character", who, w));

    auto* fillBox = new QWidget(w);
    auto* fv = new QVBoxLayout(fillBox);
    fv->setContentsMargins(0, 0, 0, 0);
    fv->setSpacing(2);
    fv->addWidget(controls::grid({controls::row("puppeteer.motion.fillGap", "wand", fillBox, "Fill Gap"),
                                  controls::row("puppeteer.motion.fillAll", "arrows-horizontal", fillBox, "Fill All"),
                                  controls::row("puppeteer.motion.newVariant", "refresh", fillBox, "New Variant")},
                                 2, fillBox));
    v->addWidget(controls::section("Fill", fillBox, w, "puppeteer/motion"));

    auto* pathBox = new QWidget(w);
    auto* pv = new QVBoxLayout(pathBox);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(4);
    auto* styles = new controls::Segmented(pathBox);
    for (size_t i = 0; i < kStyles.size(); ++i)
        styles->add(kStyles[i].second, {}, kStyles[i].second, int(i));
    QObject::connect(styles, &controls::Segmented::picked, w, [](int id) { state().style = kStyles[size_t(id)].first; });
    styles->setCurrent(0);
    pv->addWidget(fieldRow("Style", styles, pathBox));
    QDoubleSpinBox* speed = controls::valueField(0.0, 8.0, 2, " m/s", pathBox);
    speed->setSpecialValueText("Gait");
    speed->setToolTip("Speed\nm/s along the path; Gait uses the gait's own");
    QObject::connect(speed, &QDoubleSpinBox::valueChanged, w, [](double s) { state().speed = s; });
    pv->addWidget(fieldRow("Speed", speed, pathBox));
    pv->addWidget(controls::grid({controls::row("puppeteer.motion.drawPath", "route", pathBox, "Draw Path"),
                                  controls::row("puppeteer.motion.followPath", "walk", pathBox, "Follow Path")},
                                 2, pathBox));
    v->addWidget(controls::section("Path", pathBox, w, "puppeteer/motion"));

    auto* spotBox = new QWidget(w);
    auto* sv = new QVBoxLayout(spotBox);
    sv->setContentsMargins(0, 0, 0, 0);
    sv->setSpacing(4);
    std::vector<QWidget*> starters;
    for (const auto& [name, iconName] : kStarterSpots)
        starters.push_back(controls::row("puppeteer.motion.spot." + name, iconName, spotBox, QString::fromStdString(name)));
    sv->addWidget(controls::grid(starters, 2, spotBox));
    sv->addWidget(controls::grid({controls::row("puppeteer.motion.spotCapture", "camera", spotBox, "Capture Target"),
                                  controls::row("puppeteer.motion.spotSave", "device-floppy", spotBox, "Save Spot")},
                                 2, spotBox));
    sv->addWidget(controls::overflow({"puppeteer.motion.spotLoad", "puppeteer.motion.spotEmpty"}, spotBox));
    v->addWidget(controls::section("Spots", spotBox, w, "puppeteer/motion"));

    auto* driveBox = new QWidget(w);
    auto* dv = new QVBoxLayout(driveBox);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->addWidget(controls::grid({controls::row("puppeteer.motion.drive", "steering-wheel", driveBox, "Drive")}, 2, driveBox));
    v->addWidget(controls::section("Live", driveBox, w, "puppeteer/motion"));

    v->addStretch(1);
    auto* device = new QLabel(w);
    device->setForegroundRole(QPalette::PlaceholderText);
    device->setWordWrap(true);
    v->addWidget(device);

    QPointer<QLabel> whoP(who), deviceP(device);
    QPointer<QWidget> bannerP(banner);
    auto refresh = [whoP] {
        if (!whoP)
            return;
        const std::string c = currentCharacter();
        whoP->setText(c.empty() ? QString("None selected") : QString::fromStdString(c));
        whoP->setForegroundRole(c.empty() ? QPalette::PlaceholderText : QPalette::WindowText);
    };
    // The model loads on first use; the status shows once it has.
    auto status = [deviceP, bannerP] {
        if (!deviceP || !bannerP)
            return;
        const mo::Status st = mo::Engine::status();
        controls::setWarning(bannerP, st.ready ? QString() : QString::fromStdString(st.message));
        deviceP->setText(st.ready ? QString::fromStdString(st.message) : QString());
    };
    QTimer::singleShot(1500, w, status);
    for (events::Event e : {events::Event::SelectionChanged, events::Event::SceneNew, events::Event::SceneOpened})
    {
        const auto id = events::subscribe(e, [refresh](const events::EventData&) { refresh(); }, "Puppeteer");
        QObject::connect(w, &QObject::destroyed, [id] { events::unsubscribe(id); });
    }
    const auto done = events::subscribe(events::Event::UndoStackChanged, [status](const events::EventData&) { status(); }, "Puppeteer");
    QObject::connect(w, &QObject::destroyed, [done] { events::unsubscribe(done); });
    refresh();
    return w;
}

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "motionDrive",
        .doc = "Drive a character live: start, set the input, step frames (scripts) or let the Drive tool run it in real time, "
               "then stop to key everything shown into a take. Returns the drive's state.",
        .positionalDoc = "the character, to start",
        .flags = {
            {"start", "s", T::Bool, false, "Start driving the character."},
            {"move", "m", T::Vec3, nullptr, "Ground direction [x, 0, z] scaled 0..1 of the gait's speed; zero stands."},
            {"style", "st", T::String, nullptr, "walk, run, crouch, strafeLeft, strafeRight, backward."},
            {"facing", "f", T::Vec3, nullptr, "Keep facing this way while moving (strafing); [0, 0, 0] faces the way it moves."},
            {"step", "n", T::Int, nullptr, "Frames to advance now (30 fps).", {}, {}, 1.0, 3600.0},
            {"sync", "sy", T::Bool, true, "With start from a script: generate on this thread, so stepped frames never wait."},
            {"stop", "x", T::Bool, false, "Stop driving."},
            {"keep", "k", T::Bool, true, "With stop: key what was shown into the take (false discards it)."},
            {"take", "t", T::String, "Drive", "The take a stopped drive keys into; one of that name is replaced."},
        },
        .undoable = true,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            try
            {
                json out = json::object();
                if (a.get<bool>("start", false))
                {
                    const std::vector<std::string> objs = a.objects();
                    if (objs.empty())
                        throw std::invalid_argument("name the character");
                    out = driveStart(objs.front(), a.get<std::string>("style", "walk"), a.get<bool>("sync", true), a.get<std::string>("take", "Drive"));
                    session()->take = a.get<std::string>("take", "Drive");
                }
                if (!session())
                    throw std::invalid_argument("no drive is running; start one");
                if (a.has("move"))
                    session()->input.move = {a.flags["move"][0].get<float>(), a.flags["move"][2].get<float>()};
                if (a.has("facing"))
                    session()->input.facing = {a.flags["facing"][0].get<float>(), a.flags["facing"][2].get<float>()};
                if (a.has("style"))
                    session()->input.style = a.get<std::string>("style");
                if (a.has("take"))
                    session()->take = a.get<std::string>("take");
                if (a.has("step"))
                    out.update(driveStep(int(a.get<int64_t>("step"))));
                if (a.get<bool>("stop", false))
                    out.update(driveStop(a.get<bool>("keep", true)));
                else if (!a.has("step"))
                    out.update({{"frames", session()->frames}, {"generations", session()->driver->generations()}});
                (void)g;
                return out;
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("motionDrive: ") + e.what());
            }
        },
        .examples = {"motionDrive('Manny', start=True)\nmotionDrive(move=[0, 0, 1], step=90)\nmotionDrive(stop=True)"},
    });
}
} // namespace

void registerUi(ext::Context& ctx)
{
    registerCommands(ctx);
    static debugDraw::Channel spots;
    spots = debugDraw::channel(ctx, "Motion Spots");
    for (events::Event e : {events::Event::UndoStackChanged, events::Event::SelectionChanged, events::Event::SceneOpened, events::Event::SceneNew})
        events::subscribe(e, [](const events::EventData&) { drawSpots(spots); }, "Puppeteer");
    viewportTools::add(ctx, {.id = "puppeteer.motion.driveTool", .label = "Drive", .icon = "steering-wheel",
                             .create = [] { return std::make_unique<DriveTool>(); }});

    const char* cat = "Animation";
    const std::vector<std::string> kw = {"motion", "generate", "inbetween", "tween", "motionbricks", "ml"};
    auto add = [&](const char* id, const char* label, const char* icon, const char* doc, std::function<void()> fn, std::vector<std::string> extra = {}) {
        std::vector<std::string> words = kw;
        words.insert(words.end(), extra.begin(), extra.end());
        actions::add(ctx, {.id = id, .label = label, .category = cat, .run = std::move(fn), .doc = doc, .icon = icon, .keywords = words});
    };
    actions::add(ctx, {.id = "puppeteer.motion.fillGap", .label = "Fill Gap", .category = cat,
                       .run = [] { fill({{"time", graph().time()}}); },
                       .doc = "Fill the gap around the current frame with generated motion, from the key before to the key after. "
                              "Generated keys are marked g in the Stacker; filling again replaces them.",
                       .command = "motionFill", .icon = "wand", .keywords = {"motion", "inbetween", "tween", "generate", "fill", "motionbricks", "ml"}});
    actions::add(ctx, {.id = "puppeteer.motion.fillAll", .label = "Fill All Gaps", .category = cat,
                       .run = [] { fill({{"all", true}}); },
                       .doc = "Fill every gap between the character's keys with generated motion.", .command = "motionFill",
                       .call = {{"all", true}}, .icon = "arrows-horizontal", .keywords = {"motion", "inbetween", "tween", "generate", "fill", "motionbricks"}});
    add("puppeteer.motion.newVariant", "New Fill Variant", "refresh", "Fill the last filled gaps again with other, equally valid motion.", newVariant,
        {"regenerate", "variation"});
    add("puppeteer.motion.drawPath", "Draw Path", "route", "Click points on the ground for a character's path (a Catmull-Rom curve); Enter finishes.",
        [] { viewportTools::activate("curve.catmullRom"); });
    add("puppeteer.motion.followPath", "Follow Path", "walk",
        "Generate the character walking the selected curve or spots in the Motion panel's style and speed, keyed into a Path take.", followPath,
        {"locomotion", "walk", "run", "strafe", "crouch", "motion matching"});
    for (const auto& [name, iconName] : kStarterSpots)
    {
        const std::string id = "puppeteer.motion.spot." + name;
        const std::string doc = "Place the " + name + " spot ahead of the character; put it in a path to perform it.";
        add(id.c_str(), name.c_str(), iconName, doc.c_str(), [n = name] { addSpot(n); }, {"spot", "interaction", "smart primitive"});
    }
    add("puppeteer.motion.spotEmpty", "New Empty Spot", "map-pin", "Place an empty spot to capture your own target poses into.", [] {
        const json r = runCommand("motionSpot", {{"name", "spot"}});
        if (r.is_object() && r.contains("node"))
            runCommand("select", {{"args", {r["node"]}}});
    }, {"spot", "interaction", "smart primitive"});
    add("puppeteer.motion.spotCapture", "Capture Spot Target", "camera",
        "Add the character's pose at the current frame to the selected spot as its next target.",
        [] {
            const std::string spot = selectedSpot();
            if (spot.empty())
                return log::write(LogLevel::Error, "Capture Target: select the spot and the character");
            onCharacter("motionSpotTarget", {{"args", {spot}}});
        },
        {"spot", "interaction", "smart primitive", "pose"});
    add("puppeteer.motion.spotSave", "Save Spot", "device-floppy", "Save the selected spot's targets as a .lilaspot asset to reuse.", [] {
        const std::string spot = selectedSpot();
        if (spot.empty())
            return log::write(LogLevel::Error, "Save Spot: select a spot");
        const QString f = fileDialog::getSaveFileName(nullptr, "Save Spot", {}, "Spots (*.lilaspot)");
        if (!f.isEmpty())
            runCommand("motionSpotSave", {{"args", {spot}}, {"file", f.toStdString()}});
    }, {"spot", "interaction", "smart primitive"});
    add("puppeteer.motion.spotLoad", "Load Spot", "folder-open", "Place a spot from a .lilaspot asset.", [] {
        const QString f = fileDialog::getOpenFileName(nullptr, "Load Spot", {}, "Spots (*.lilaspot)");
        if (!f.isEmpty())
            runCommand("motionSpot", {{"file", f.toStdString()}});
    }, {"spot", "interaction", "smart primitive"});
    actions::add(ctx, {.id = "puppeteer.motion.drive", .label = "Drive", .category = cat,
                       .run = [] {
                           if (viewportTools::activeId() == "puppeteer.motion.driveTool")
                               viewportTools::set(nullptr);
                           else
                               viewportTools::activate("puppeteer.motion.driveTool");
                       },
                       .doc = "Steer the character live with W A S D or a gamepad; the motion generates as it goes and becomes a Drive take.",
                       .checked = [] { return viewportTools::activeId() == "puppeteer.motion.driveTool"; }, .icon = "steering-wheel",
                       .keywords = {"motion", "drive", "steer", "live", "gamepad", "record", "locomotion", "motionbricks"}});
    add("puppeteer.motion.status", "Motion Status", "info-circle", "Show where motion generates (GPU or CPU, and why) and the model file.", [] {
        const json r = runCommand("motionStatus");
        if (r.is_object())
            log::write(r.value("ready", false) ? LogLevel::Info : LogLevel::Warning, "Motion: " + r.value("message", std::string()));
    });

    prefPages::add(ctx, {.id = "motion", .title = "Motion", .file = "motion", .build = [](prefPages::PageBuilder& b) {
                             b.choice("Device", "/device", {{"Auto", "auto"}, {"GPU", "gpu"}, {"CPU", "cpu"}}, "auto");
                         }});
    panels::add(ctx, {.id = "puppeteerMotion", .title = "Motion", .create = makePanel, .area = Qt::LeftDockWidgetArea, .icon = "walk", .open = false,
                      .keywords = {"motion", "generate", "inbetween", "locomotion", "spots", "drive", "motionbricks"}});

    const std::string menu = "Animation/Motion";
    menus::addItem(ctx, {.id = "puppeteer.motion.menu", .menu = "Animation", .kind = menus::Kind::Submenu, .label = "Motion"});
    int sep = 0;
    for (const char* a : {"window.puppeteerMotion", "", "puppeteer.motion.fillGap", "puppeteer.motion.fillAll", "puppeteer.motion.newVariant", "",
                          "puppeteer.motion.drawPath", "puppeteer.motion.followPath", "", "puppeteer.motion.spot.Pick Up", "puppeteer.motion.spot.Duck",
                          "puppeteer.motion.spot.Leap", "puppeteer.motion.spot.Crouch", "puppeteer.motion.spotEmpty", "puppeteer.motion.spotCapture",
                          "puppeteer.motion.spotSave", "puppeteer.motion.spotLoad", "", "puppeteer.motion.drive", "", "puppeteer.motion.status"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("puppeteer.motion.menu.") + a, .menu = menu, .action = a});
        else
            menus::addItem(ctx, {.id = "puppeteer.motion.menu.sep" + std::to_string(sep++), .menu = menu, .kind = menus::Kind::Separator});
    }
    // Fill sits with the key tools animators reach for in the Animate menu too.
    menus::addItem(ctx, {.id = "puppeteer.anim.menu.motionFill", .menu = "Animation/Animate", .action = "puppeteer.motion.fillGap"});
}
} // namespace puppeteer::motionui
