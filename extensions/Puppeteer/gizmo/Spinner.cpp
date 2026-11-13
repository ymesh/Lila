// The Spinner: Puppeteer's radial transform hub. Three coloured zones drag X, Y and Z (vertically: up increases), the
// middle mouse button turns the selection freely as a trackball, and the ring holds the tool and option buttons and,
// in Animate mode, the breakdown slider and the Strings mode arc. On an IK or reverse foot controller the zones become rings around a centre
// disc that swings the pole, and right-dragging the rings rolls, twists and banks the foot. Every drag previews with
// undo off and commits one Puppeteer command on release.

#include "gizmo/Spinner.h"
#include "Puppeteer.h"
#include "gizmo/ControllerDraw.h"
#include "rig/AnimProxy.h"
#include "rig/Rig.h"

#include <lila/Anim.h>
#include <lila/Events.h>
#include <lila/Ghosts.h>
#include <lila/Gizmo.h>
#include <lila/Prefs.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QActionGroup>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>

#include <cmath>
#include <map>
#include <optional>
#include <tuple>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
enum class Tool
{
    Move,
    Rotate,
    Scale,
};

enum class Space
{
    Local,
    World,
    Screen,
};

/** What the Spinner remembers between drags and sessions, shared by every view's instance. */
struct Settings
{
    Tool tool = Tool::Rotate;
    bool inherit = false;     // rotate: the children keep their world orientation
    bool toon = false;        // scale: the other axes compensate and the children keep their size
    double toonExponent = 1.0 / 3.0;
    Space space = Space::Local;
    bool pivot = false;       // rig: move a joint without its children
    bool discrete = false;
    bool stickyAim = false;   // rig: orients follow the bones while joints move
    bool sticky2nd = false;
    bool keepDirection = false;
    bool footAlign = false;   // the reverse foot context button aligns (levels) instead of snapping
    bool autoPose = true;     // full-body moves let the auto-poser place the unpinned body
    bool trail = false;       // animate: the selected joint's motion trail, its frames draggable
    double trailFrames = 10.0; // frames either side of now the trail shows, without a time range
    double trailFalloff = 5.0; // frames a trail drag eases out over, without a time range
    bool help = false;
    double moveStep = 5.0;
    double rotateStep = 15.0;
    double scaleStep = 0.1;
    double rotateSpeed = 0.5;     // degrees per pixel, joints
    double controllerSpeed = 0.5; // degrees per pixel, IK and reverse foot controllers
    double poleSpeed = 0.42;      // degrees per pixel, the centre disc's pole swing
    double scaleSpeed = 0.0046;   // scale per pixel
    double trackballSpeed = 0.5;  // degrees per pixel
    double breakdownDeadZone = 20.0;
    double breakdownFull = 100.0;
    double size = 1.0;
    /** Where each view (by camera name) has the Spinner, relative to the view's centre. */
    std::map<std::string, QPointF> offsets;

    template <class T> static void read(const PrefStore& p, const char* key, T& v) { v = p.value<T>(kPrefs, key, v); }

    void load()
    {
        const PrefStore& p = PrefStore::get();
        read(p, "/spinner/inheritRotate", inherit);
        read(p, "/spinner/toonScale", toon);
        read(p, "/spinner/toonExponent", toonExponent);
        space = Space(std::clamp(p.value<int>(kPrefs, "/spinner/axes", 0), 0, 2));
        read(p, "/spinner/pivot", pivot);
        read(p, "/spinner/discrete", discrete);
        read(p, "/spinner/stickyAim", stickyAim);
        read(p, "/spinner/sticky2nd", sticky2nd);
        read(p, "/spinner/keepDirection", keepDirection);
        read(p, "/spinner/footAlign", footAlign);
        read(p, "/spinner/autoPose", autoPose);
        read(p, "/spinner/trail", trail);
        read(p, "/spinner/trailFrames", trailFrames);
        read(p, "/spinner/trailFalloff", trailFalloff);
        read(p, "/spinner/moveStep", moveStep);
        read(p, "/spinner/rotateStep", rotateStep);
        read(p, "/spinner/scaleStep", scaleStep);
        read(p, "/spinner/rotateSpeed", rotateSpeed);
        read(p, "/spinner/controllerSpeed", controllerSpeed);
        read(p, "/spinner/poleSpeed", poleSpeed);
        read(p, "/spinner/scaleSpeed", scaleSpeed);
        read(p, "/spinner/trackballSpeed", trackballSpeed);
        read(p, "/spinner/breakdownDeadZone", breakdownDeadZone);
        read(p, "/spinner/breakdownFull", breakdownFull);
        size = std::clamp(p.value<double>(kPrefs, "/spinner/size", 1.0), 0.5, 2.0);
        offsets.clear();
        const json o = p.value(kPrefs, "/spinner/offsets", json::object());
        if (o.is_object())
            for (const auto& [view, xy] : o.items())
                if (xy.is_array() && xy.size() == 2 && xy[0].is_number() && xy[1].is_number())
                    offsets[view] = QPointF(xy[0].get<double>(), xy[1].get<double>());
    }

    void save() const
    {
        PrefStore& p = PrefStore::get();
        p.set(kPrefs, "/spinner/inheritRotate", inherit);
        p.set(kPrefs, "/spinner/toonScale", toon);
        p.set(kPrefs, "/spinner/toonExponent", toonExponent);
        p.set(kPrefs, "/spinner/axes", int(space));
        p.set(kPrefs, "/spinner/pivot", pivot);
        p.set(kPrefs, "/spinner/discrete", discrete);
        p.set(kPrefs, "/spinner/stickyAim", stickyAim);
        p.set(kPrefs, "/spinner/sticky2nd", sticky2nd);
        p.set(kPrefs, "/spinner/keepDirection", keepDirection);
        p.set(kPrefs, "/spinner/footAlign", footAlign);
        p.set(kPrefs, "/spinner/autoPose", autoPose);
        p.set(kPrefs, "/spinner/trail", trail);
        p.set(kPrefs, "/spinner/trailFrames", trailFrames);
        p.set(kPrefs, "/spinner/trailFalloff", trailFalloff);
        p.set(kPrefs, "/spinner/moveStep", moveStep);
        p.set(kPrefs, "/spinner/rotateStep", rotateStep);
        p.set(kPrefs, "/spinner/scaleStep", scaleStep);
        p.set(kPrefs, "/spinner/rotateSpeed", rotateSpeed);
        p.set(kPrefs, "/spinner/controllerSpeed", controllerSpeed);
        p.set(kPrefs, "/spinner/poleSpeed", poleSpeed);
        p.set(kPrefs, "/spinner/scaleSpeed", scaleSpeed);
        p.set(kPrefs, "/spinner/trackballSpeed", trackballSpeed);
        p.set(kPrefs, "/spinner/breakdownDeadZone", breakdownDeadZone);
        p.set(kPrefs, "/spinner/breakdownFull", breakdownFull);
        p.set(kPrefs, "/spinner/size", size);
        json o = json::object();
        for (const auto& [view, xy] : offsets)
            o[view] = json::array({xy.x(), xy.y()});
        p.set(kPrefs, "/spinner/offsets", o);
    }
};

Settings& settings()
{
    static Settings s = [] {
        Settings x;
        x.load();
        return x;
    }();
    return s;
}

void changed()
{
    settings().save();
    viewportTools::requestRedraw();
}

void setTool(Tool t)
{
    settings().tool = t;
    actions::run(t == Tool::Move ? "tool.move" : t == Tool::Rotate ? "tool.rotate" : "tool.scale");
    changed();
}

/** O and P: switch to the tool, or toggle its alternative when already in it. */
void toolOrAlternative(Tool t)
{
    Settings& s = settings();
    if (s.tool == t)
    {
        if (t == Tool::Rotate)
            s.inherit = !s.inherit;
        else if (t == Tool::Scale)
            s.toon = !s.toon;
    }
    setTool(t);
}

enum class Handle
{
    None,
    Plate, // the rim and the ring between buttons: dragging it moves the Spinner
    AxisX,
    AxisY,
    AxisZ,
    Center,
    Move,
    Rotate,
    Scale,
    Space,
    Discrete,
    Pivot,
    Context,
    Sticky2nd,
    KeepDirection,
    Settings,
    Help,
    Proxy,
    Breakdown,
    // The Strings mode arc, top to bottom; Forward, Backward and Bidirectional carry their All variant on the right button.
    ModeDefault,
    ModeFree,
    ModeForward,
    ModeBackward,
    ModeBidirectional,
    ModeIk,
    ModeFullBody,
    Trail, // a frame on the motion trail
};

constexpr strings::Mode kArcModes[] = {strings::Mode::Default,       strings::Mode::Free, strings::Mode::Forward, strings::Mode::Backward,
                                       strings::Mode::Bidirectional, strings::Mode::Ik,   strings::Mode::FullBody};
constexpr int kArcCount = int(std::size(kArcModes));

bool isMode(Handle h) { return h >= Handle::ModeDefault && h <= Handle::ModeFullBody; }
int slotOf(Handle h) { return int(h) - int(Handle::ModeDefault); }
/** The arc segment's mode: the All variant when that one is current. */
strings::Mode modeOf(Handle h)
{
    const strings::Mode m = kArcModes[slotOf(h)];
    return strings::allVariant(m, strings::isAllVariant(stringsMode()) && strings::allVariant(stringsMode(), false) == m);
}
/** The segment a mode lights: its own, or its non-All variant's. */
Handle handleOf(strings::Mode m)
{
    const strings::Mode base = strings::allVariant(m, false);
    for (int i = 0; i < kArcCount; ++i)
        if (kArcModes[i] == base)
            return Handle(int(Handle::ModeDefault) + i);
    return Handle::ModeForward;
}
bool hasAllVariant(strings::Mode m) { return strings::allVariant(m, true) != strings::allVariant(m, false); }


bool isAxis(Handle h) { return h == Handle::AxisX || h == Handle::AxisY || h == Handle::AxisZ; }
int axisOf(Handle h) { return h == Handle::AxisX ? 0 : h == Handle::AxisY ? 1 : 2; }

/** Where the Spinner sits by default: right of the view's centre, logical pixels. */
constexpr double kDefaultOffsetX = 250.0;
// The zones scale with the size setting; the band of buttons around them is a fixed width in logical pixels.
constexpr double kZoneRadius = 72.0;
constexpr double kMinZoneRadius = 48.0;
constexpr double kHub = 10.0;
constexpr double kBandHalf = 12.0; // segments and wells reach this far either side of the button ring
constexpr double kWell = 12.0;
constexpr double kTabWell = 10.0;
constexpr double kTabSpacing = 24.0;
constexpr double kSegmentGap = 2.0;
constexpr double kIcon = 16.0;
constexpr double kToolIcon = 20.0;
// The zones in screen degrees (y down): red X on top, green Y lower left, blue Z lower right.
constexpr double kZoneX[2] = {-150.0, -30.0};
constexpr double kZoneY[2] = {90.0, 210.0};
constexpr double kZoneZ[2] = {-30.0, 90.0};
// Move, Rotate and Scale: one strip across the top of the band.
constexpr double kToolFrom = -135.0;
constexpr double kToolTo = -45.0;
constexpr double kTabCenter = -26.0;
// Screen angles in degrees: 0 to the right, 90 straight down.
constexpr double kBreakdownFrom = 48.0;
constexpr double kBreakdownTo = 133.0;
constexpr double kGizmoPixels = 70.0;
// The Strings mode arc, left of the zones on the breakdown slider's radius, between the context and move buttons.
constexpr double kModeFrom = 162.0;
constexpr double kModeTo = 228.0;

struct Button
{
    Handle handle;
    double angle; // on the band; tab buttons use their slot instead
    int tabSlot = -1;
};

constexpr Button kButtons[] = {
    {Handle::Space, 0.0},          {Handle::Discrete, 30.0},  {Handle::Pivot, 180.0},  {Handle::Context, 150.0},
    {Handle::Sticky2nd, 120.0},    {Handle::KeepDirection, 96.0},
    {Handle::Settings, 0.0, 0},    {Handle::Help, 0.0, 1},    {Handle::Proxy, 0.0, 2},
};

constexpr Handle kTools[] = {Handle::Move, Handle::Rotate, Handle::Scale};

glm::vec4 spinnerGray(float v, float a = 1.0f) { return {v, v, v, a}; }
const glm::vec4 kOrange{0.93f, 0.55f, 0.13f, 1.0f};
const glm::vec4 kYellow{0.92f, 0.78f, 0.22f, 1.0f};

/** Every neutral the Spinner draws with, in one place. */
struct Palette
{
    glm::vec4 plate = spinnerGray(0.10f);     // behind the zones, seen in their gaps
    glm::vec4 bezel = spinnerGray(0.165f);    // the band holding the buttons
    glm::vec4 bezelEdge = spinnerGray(1.0f, 0.10f);  // light top edge
    glm::vec4 bezelInner = spinnerGray(0.0f, 0.45f); // dark line where the band meets the zones
    glm::vec4 well = spinnerGray(0.25f);
    glm::vec4 wellHot = spinnerGray(0.33f);
    glm::vec4 ring = spinnerGray(0.86f);              // the on toggle's ring
    glm::vec4 segment = spinnerGray(0.22f);
    glm::vec4 segmentHot = spinnerGray(0.30f);
    glm::vec4 segmentOn = spinnerGray(0.70f);
    glm::vec4 icon = spinnerGray(0.66f);
    glm::vec4 iconHot = spinnerGray(0.95f);
    glm::vec4 iconOn = spinnerGray(0.97f);
    glm::vec4 iconOnSegment = spinnerGray(0.08f);
    glm::vec4 groove = spinnerGray(0.07f);
    glm::vec4 grooveFill = spinnerGray(0.62f);
    glm::vec4 tick = spinnerGray(0.45f);
    glm::vec4 thumb = spinnerGray(0.88f);
    glm::vec4 thumbHot = spinnerGray(1.0f);
    glm::vec4 disabled = spinnerGray(0.30f);
    glm::vec4 letter = spinnerGray(0.0f, 0.5f);
};
const Palette kPal;

/** Hold ` (or its rebound key): the Spinner follows the cursor in the view it is over. */
struct Summon
{
    bool held = false;
    uint64_t hoverTick = 0; // bumped by every hover, so only the view last under the cursor follows
};
Summon& summon()
{
    static Summon s;
    return s;
}

std::vector<std::string> selectedTransforms()
{
    Graph& g = graph();
    std::vector<std::string> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("transform"))
            out.push_back(n->path());
    return out;
}

json namesJson(const std::vector<std::string>& names)
{
    json a = json::array();
    for (const std::string& n : names)
        a.push_back(n);
    return a;
}

/** Runs with undo recording off, putting it back however fn ends. Returns what fn threw, empty when nothing. */
template <class Fn> std::string quietly(Fn&& fn)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    // Previews read joints over and over; the skinned meshes update once, when the viewport draws.
    const Graph::DeferMeshes defer;
    std::string error;
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    u.setEnabled(was);
    return error;
}

/** What is selected, as the Spinner sees it. */
struct Target
{
    std::vector<std::string> names;
    NodeId chain = kNoNode; // the IK chain whose controller is selected
    NodeId foot = kNoNode;  // its reverse foot
    bool rig = false;       // Rig mode
    bool character = false; // every selected node is a joint of a character, so Strings can pose it
    bool related = false;   // something selected is glued or attached
};

Target currentTarget()
{
    Graph& g = graph();
    Target t;
    t.names = selectedTransforms();
    t.rig = modes::current() == kRigMode;
    t.character = !t.names.empty() && std::all_of(t.names.begin(), t.names.end(), [&](const std::string& n) {
        Node* node = g.find(n);
        return node && rig::isJoint(*node) && characterFor(g, node->id());
    });
    t.related = std::any_of(t.names.begin(), t.names.end(), [&](const std::string& n) {
        Node* node = g.find(n);
        return node && stringsRelated(node->id());
    });
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        // Only a controller is posed through its chain; its joints themselves turn FK.
        if (NodeId c = lila::scene::rigel::chainOfController(g, id))
        {
            t.chain = c;
            t.foot = footForChain(g, c);
            break;
        }
    }
    return t;
}

/** Off a key, in Animate mode, the Spinner greys out: the selection has keys but none at the current time. */
bool offKey(const std::vector<std::string>& names)
{
    if (names.empty() || modes::current() != kAnimateMode)
        return false;
    Graph& g = graph();
    static uint64_t version = ~0ull;
    static double time = 0.0;
    static std::vector<std::string> cachedNames;
    static bool cached = false;
    if (version == g.version() && time == g.time() && cachedNames == names)
        return cached;
    version = g.version();
    time = g.time();
    cachedNames = names;
    cached = false;
    try
    {
        const json times = ops::execute("keyframe", {{"args", namesJson(names)}, {"query", true}});
        bool any = false, here = false;
        auto visit = [&](const json& j, auto& self) -> void {
            if (j.is_number())
            {
                any = true;
                here |= std::abs(j.get<double>() - g.time()) < 1e-6;
            }
            else if (j.is_array())
                for (const json& e : j)
                    self(e, self);
        };
        visit(times, visit);
        cached = any && !here;
    }
    catch (const std::exception&)
    {
    }
    return cached;
}

QString tipFor(Handle h, const Target& t)
{
    const Settings& s = settings();
    const bool rings = t.chain && s.tool == Tool::Rotate;
    switch (h)
    {
    case Handle::AxisX:
    case Handle::AxisY:
    case Handle::AxisZ:
    {
        static const char* axes[] = {"X", "Y", "Z"};
        static const char* foot[] = {"roll", "twist", "bank"};
        QString tip = QString("Drag up or down: %1. Middle drag: free rotate").arg(axes[axisOf(h)]);
        if (rings && t.foot)
            tip += QString(". Right drag: %1").arg(foot[axisOf(h)]);
        return tip;
    }
    case Handle::Center:
        if (rings)
            return "Drag up or down: swing the pole. Right drag: swing the pole and turn the controller with it";
        if (s.tool == Tool::Scale)
            return "Drag up or down: scale every axis";
        return s.tool == Tool::Move ? "Drag: move in the screen plane" : "Drag up or down: turn around the view";
    case Handle::Move: return "Move (I)";
    case Handle::Rotate:
        return s.inherit ? "Inherit Rotate: the children keep their world orientation (right-click: Rotate) (O)"
                         : "Rotate (right-click: Inherit Rotate) (O)";
    case Handle::Scale:
        return s.toon ? "Toon Scale: the other axes compensate (right-click: Scale) (P)" : "Scale (right-click: Toon Scale) (P)";
    case Handle::Space:
        return s.space == Space::Local ? "Local space (8). Click: world" : s.space == Space::World ? "World space (9). Click: screen" : "Screen space (0). Click: local";
    case Handle::Discrete: return s.discrete ? "Discrete steps on" : "Discrete steps off";
    case Handle::Pivot: return s.pivot ? "Pivot: move joints without their children" : "Pivot off: joints carry their children";
    case Handle::Context:
        if (t.foot)
            return s.footAlign ? "Reverse Foot Align: level the controller keeping the pose (right-click: Snap)"
                               : "Reverse Foot Snap: put the controller on the sole keeping the pose (right-click: Align)";
        return s.stickyAim ? "Sticky Aim on: orients follow the bones as joints move" : "Sticky Aim off";
    case Handle::Sticky2nd: return s.sticky2nd ? "Sticky 2nd on: the secondary axis lies in the chain's plane" : "Sticky 2nd off";
    case Handle::KeepDirection: return s.keepDirection ? "Keep Direction on: the secondary axis keeps its sense" : "Keep Direction off";
    case Handle::Settings: return "Spinner settings";
    case Handle::Help: return "Show what each part of the Spinner does";
    case Handle::Proxy:
        return selectionShowsProxy() ? "Anim proxy shown: click for the real meshes. Right-click: the selected joint's real piece"
                                     : "Real meshes shown: click for the anim proxy. Right-click: the selected joint's real piece";
    case Handle::Breakdown:
        return "Breakdown: left toward the previous key, right toward the next; down lags hands, feet and head, up leads (Shift: whole character)";
    case Handle::Plate: return "Drag the rim to move the Spinner (reset in its settings)";
    case Handle::ModeDefault: return "Default: the rules pick per joint; moves bend the limb, turns are Forward";
    case Handle::ModeFree: return "Free: the joint alone, its children keep their world rotation";
    case Handle::ModeForward: return "Forward: the joint turns, its children follow, pins below hold. Right-click: Forward All";
    case Handle::ModeBidirectional: return "Bidirectional: both sides swing about the joint. Right-click: All";
    case Handle::ModeBackward: return "Backward: what is pinned below holds and the body swings";
    case Handle::ModeIk: return "IK: the limb bends to put the joint where you drag";
    case Handle::ModeFullBody: return "Full Body: the whole body moves, pins holding";
    case Handle::Trail: return "Drag a frame of the trail: the joint goes there on that frame, easing out either side";
    default: return {};
    }
}

class SpinnerGizmo final : public Gizmo
{
public:
    void setView(const ViewInfo& view) override { mView = view; }

    bool supports(std::string_view mode) const override
    {
        if (mode != mLastMode)
        {
            mLastMode = std::string(mode);
            if (mode == "move")
                settings().tool = Tool::Move;
            else if (mode == "rotate")
                settings().tool = Tool::Rotate;
            else if (mode == "scale")
                settings().tool = Tool::Scale;
        }
        return mode == "select" || mode == "move" || mode == "rotate" || mode == "scale";
    }

    bool visible() override { return !selectedTransforms().empty(); }

    void draw(OverlayPainter& p) override
    {
        if (!mDrag && !visible())
            return;
        const Target t = currentTarget();
        const Settings& s = settings();
        const bool disabled = offKey(t.names);
        drawViewportGizmo(p, t);

        drawRelations(p);
        drawTrail(p, t);
        drawArcs(p);
        drawChain(p);
        const QPointF c = center();
        const Handle hot = mDrag ? mDrag->handle : mHover;
        const Geo g = geo();
        if (summon().held && mHoverTick == summon().hoverTick && !mDrag)
            followCursor();

        // Plate: shadow, the band, its edges, then the dark field the zones sit in.
        p.softShadow(c + QPointF(0.0, 3.0), g.plate, 12.0, 0.32f);
        const bool proxyTab = shown(Handle::Proxy, t);
        drawTab(p, c, g, proxyTab ? 3 : 2);
        p.smoothDisc(c, g.plate, kPal.bezel);
        p.arc(c, g.plate - 0.75, -165.0, -15.0, 1.0f, kPal.bezelEdge);
        p.smoothDisc(c, g.bandIn, kPal.plate);
        p.arc(c, g.bandIn, 0.0, 360.0, 1.5f, kPal.bezelInner);

        // Zones.
        const bool color = viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors);
        const bool rings = ringLayout(t);
        const bool footChannel = mDrag && mDrag->kind == Kind::FootChannel;
        const double zones[3][2] = {{kZoneX[0], kZoneX[1]}, {kZoneY[0], kZoneY[1]}, {kZoneZ[0], kZoneZ[1]}};
        const double inner = rings ? g.ringInner : 0.0;
        for (int axis = 0; axis < 3; ++axis)
        {
            glm::vec4 col = color ? glm::vec4(viewportDisplay::axisColor(axis), 1.0f) : spinnerGray(0.62f - 0.1f * float(axis));
            if (footChannel)
                col = color ? kOrange : spinnerGray(0.7f);
            const Handle h = axis == 0 ? Handle::AxisX : axis == 1 ? Handle::AxisY : Handle::AxisZ;
            const bool lit = hot == h;
            double outer = g.zone;
            if (lit)
            {
                col = glm::mix(col, glm::vec4(1.0f), mDrag ? 0.22f : 0.12f);
                outer += 3.0;
            }
            if (disabled)
                col = spinnerGray(0.30f, 0.6f);
            const glm::vec4 deep = glm::vec4(glm::vec3(col) * 0.70f, col.a);
            p.sector(c, inner, outer, zones[axis][0], zones[axis][1], deep, col, 5.0);
            if (!disabled)
                p.arc(c, outer - 1.5, zones[axis][0] + 3.0, zones[axis][1] - 3.0, 1.5f, spinnerGray(1.0f, lit ? 0.35f : 0.18f), true);
            const double mid = (zones[axis][0] + zones[axis][1]) * 0.5;
            if (footChannel)
                iconAt(p, at(c, (inner + g.zone) * 0.5, mid), kToolIcon, "puppeteer.foot", spinnerGray(0.1f));
            else if (!rings)
            {
                static const char* letters[] = {"X", "Y", "Z"};
                const QPointF lp = at(c, g.zone - 14.0, mid);
                p.text(lp + QPointF(0.0, 4.5), letters[axis], disabled ? spinnerGray(0.0f, 0.25f) : kPal.letter, OverlayPainter::Center);
            }
        }
        for (double a : {-150.0, -30.0, 90.0})
            p.smoothLine(at(c, inner, a), at(c, g.zone + 4.0, a), 3.0f, glm::vec4(glm::vec3(kPal.plate), 1.0f));

        // Centre: the IK/foot pole disc, the uniform scale disc, or the free-drag hub.
        if (rings)
        {
            glm::vec4 col = !color ? spinnerGray(0.78f) : s.tool == Tool::Scale ? kYellow : kOrange;
            if (hot == Handle::Center)
                col = glm::mix(col, glm::vec4(1.0f), 0.2f);
            if (disabled)
                col = spinnerGray(0.30f, 0.6f);
            p.smoothDisc(c, g.centerDisc + 2.0, kPal.plate);
            p.sector(c, 0.0, g.centerDisc, 0.0, 360.0, glm::vec4(glm::vec3(col) * 1.05f, col.a), glm::vec4(glm::vec3(col) * 0.78f, col.a));
            p.arc(c, g.centerDisc - 1.0, -160.0, -20.0, 1.5f, spinnerGray(1.0f, 0.25f), true);
            iconAt(p, c, std::min(28.0, g.centerDisc * 1.1), s.tool == Tool::Scale ? "puppeteer.local" : "puppeteer.pole", spinnerGray(0.08f));
        }
        else
        {
            p.smoothDisc(c, kHub + 2.0, kPal.plate);
            p.smoothDisc(c, kHub, disabled ? kPal.disabled : hot == Handle::Center ? spinnerGray(0.98f) : spinnerGray(0.80f));
            p.smoothDisc(c, 3.0, spinnerGray(0.1f, 0.6f));
        }

        drawTools(p, c, g, t, hot, disabled);
        for (const Button& b : kButtons)
            if (shown(b.handle, t))
                drawButton(p, b, t);

        if (modes::current() != kRigMode && !t.rig)
            drawBreakdown(p, c, hot, disabled);
        if (modeArc(t))
            drawModes(p, c, hot);

        const QPointF tipAt(c.x() - g.plate, c.y() - g.plate - 12.0);
        if (mDrag && !mDrag->readout.isEmpty())
            p.label(tipAt, mDrag->readout);
        else if (!mDrag && !summon().held && mHover != Handle::None)
            p.label(QPointF(std::max(8.0, tipAt.x() - 160.0), tipAt.y()), tipFor(mHover, t));
        if (s.help && !mDrag)
            drawHelp(p, c);
    }

    bool hover(QPointF px) override
    {
        mCursor = px;
        mHoverTick = ++summon().hoverTick;
        if (summon().held)
        {
            mHover = Handle::None;
            return true;
        }
        const Handle h = visible() ? hit(px, currentTarget()) : Handle::None;
        if (h == Handle::Trail)
        {
            const int frame = trailAt(px);
            const bool moved = frame != mTrailHover;
            mTrailHover = frame;
            if (moved && h == mHover)
                return true;
        }
        if (h == mHover)
            return false;
        mHover = h;
        return true;
    }

    bool wantsRightButton(QPointF px) override
    {
        if (visible() && hit(px, currentTarget()) != Handle::None)
            return true;
        return controllerAt(graph(), mView, px) != kNoNode;
    }

    bool press(QPointF px, Qt::MouseButton button, Qt::KeyboardModifiers mods) override
    {
        const Target t = currentTarget();
        const Handle h = visible() ? hit(px, t) : Handle::None;
        if (h == Handle::None)
        {
            // Right-clicking an IK or reverse foot controller in the view locks or unlocks its key.
            if (button == Qt::RightButton)
                if (NodeId c = controllerAt(graph(), mView, px))
                {
                    runCommand("puppeteerFootLock", {{"args", {graph().node(c)->path()}}});
                    viewportTools::requestRedraw();
                    return true;
                }
            return false;
        }
        Settings& s = settings();
        const bool right = button == Qt::RightButton;
        switch (h)
        {
        case Handle::Move:
            if (!right)
                setTool(Tool::Move);
            return true;
        case Handle::Rotate:
            if (right)
                s.inherit = !s.inherit;
            setTool(Tool::Rotate);
            return true;
        case Handle::Scale:
            if (right)
                s.toon = !s.toon;
            setTool(Tool::Scale);
            return true;
        case Handle::Space:
            s.space = Space((int(s.space) + (right ? 2 : 1)) % 3);
            changed();
            return true;
        case Handle::Discrete:
            s.discrete = !s.discrete;
            changed();
            return true;
        case Handle::Pivot:
            s.pivot = !s.pivot;
            changed();
            return true;
        case Handle::Context:
            if (t.foot)
            {
                if (right)
                {
                    s.footAlign = !s.footAlign;
                    changed();
                }
                else
                    runCommand("puppeteerSnapReverseFoot", {{"args", {graph().node(t.foot)->path()}}, {"align", s.footAlign}});
            }
            else
            {
                s.stickyAim = !s.stickyAim;
                changed();
            }
            return true;
        case Handle::Sticky2nd:
            s.sticky2nd = !s.sticky2nd;
            changed();
            return true;
        case Handle::KeepDirection:
            s.keepDirection = !s.keepDirection;
            changed();
            return true;
        case Handle::Settings: openSettings(); return true;
        case Handle::Proxy:
            if (right)
                toggleSelectedRealPiece();
            else
                toggleSelectedProxy();
            return true;
        case Handle::Help:
            s.help = !s.help;
            changed();
            return true;
        case Handle::Plate:
            if (button == Qt::LeftButton)
                beginDrag(Kind::Reposition, h, px, mods, t);
            return true;
        case Handle::Breakdown:
            if (button == Qt::LeftButton && !offKey(t.names))
                beginDrag(Kind::Breakdown, h, px, mods, t);
            return true;
        case Handle::ModeDefault:
        case Handle::ModeFree:
        case Handle::ModeForward:
        case Handle::ModeBackward:
        case Handle::ModeBidirectional:
        case Handle::ModeIk:
        case Handle::ModeFullBody:
        {
            const strings::Mode m = kArcModes[slotOf(h)];
            // Right button: the All variant, or back from it; left keeps whichever variant is current.
            if (right && hasAllVariant(m))
                setStringsMode(strings::allVariant(m, !(strings::isAllVariant(stringsMode()) && strings::allVariant(stringsMode(), false) == m)));
            else if (!right)
                setStringsMode(modeOf(h));
            return true;
        }
        case Handle::Trail:
            if (button == Qt::LeftButton)
                beginTrailDrag(px);
            return true;
        default: break;
        }
        // The zones and the centre.
        if (offKey(t.names))
            return true;
        const bool rings = ringLayout(t);
        if (button == Qt::MiddleButton)
            beginDrag(Kind::Trackball, h, px, mods, t);
        else if (right)
        {
            if (h == Handle::Center && rings && s.tool == Tool::Rotate)
                beginDrag(Kind::PoleTurn, h, px, mods, t);
            else if (isAxis(h) && rings && t.foot && s.tool == Tool::Rotate)
                beginDrag(Kind::FootChannel, h, px, mods, t);
        }
        else if (button == Qt::LeftButton)
        {
            if (h == Handle::Center && rings && s.tool == Tool::Rotate)
                beginDrag(Kind::Pole, h, px, mods, t);
            else
                beginDrag(Kind::Transform, h, px, mods, t);
        }
        return true;
    }

    void drag(QPointF px, Qt::KeyboardModifiers mods) override
    {
        (void)mods;
        if (!mDrag)
            return;
        mDrag->current = px;
        apply(false);
    }

    void release() override
    {
        if (!mDrag)
            return;
        apply(true);
        mDrag.reset();
        lila::scene::ghosts::cache().clearPreview();
    }

    void cancel() override
    {
        if (!mDrag)
            return;
        restore();
        mDrag.reset();
        lila::scene::ghosts::cache().clearPreview();
    }

    /** IK drags: the wheel lengthens or shortens the chain that bends, shown live. */
    bool wheel(int steps) override
    {
        if (!mDrag || mDrag->kind != Kind::Transform || stringsMode() != strings::Mode::Ik || !posesStrings(mDrag->target))
            return false;
        const int from = mDrag->chainLength > 0 ? mDrag->chainLength : std::max(1, mDrag->autoChain);
        mDrag->chainLength = std::max(1, from + steps);
        apply(false);
        return true;
    }

    bool dragging() const override { return mDrag.has_value(); }
    bool hovering() const override { return mHover != Handle::None; }

    bool keyPress(QKeyEvent* e) override
    {
        if (mDrag && e->key() == Qt::Key_Escape)
        {
            cancel();
            return true;
        }
        return false;
    }

    QString readout() const override { return mDrag ? mDrag->readout : QString(); }
    void warped(QPointF offset) override
    {
        if (mDrag)
            mDrag->start -= offset;
    }
    bool wrapsCursor() const override
    {
        return !mDrag || (mDrag->kind != Kind::Breakdown && mDrag->kind != Kind::Reposition && mDrag->kind != Kind::Trail);
    }

private:
    enum class Kind
    {
        Transform,   // zones and the plain centre: move, rotate or scale
        Pole,        // centre disc on a controller: the pole alone
        PoleTurn,    // the pole with the controller turning along
        FootChannel, // right drag on a zone: roll, twist or bank
        Trackball,   // middle drag: free rotate
        Breakdown,
        Reposition,  // the rim: moves the Spinner
        Trail,       // a frame of the motion trail
    };

    struct Saved
    {
        NodeId id;
        std::vector<std::pair<int, PlugValue>> values;
    };

    struct Drag
    {
        Kind kind = Kind::Transform;
        Handle handle = Handle::None;
        Tool tool = Tool::Rotate;
        QPointF start, current;
        bool all = false;
        Target target;
        std::vector<Saved> saved;
        glm::dvec3 pivot{0.0};
        glm::dmat3 axes{1.0};
        std::vector<BreakdownSample> samples;
        double weight = 0.0;
        QPointF offset, startOffset;
        int trailFrame = -1;
        bool strung = false; // a breakdown through stringsBreakdown
        bool tween = false;  // a breakdown with a time range selected: stringsTween across it
        glm::dvec3 trailBy{0.0};
        /** The trail as the drag so far would leave it; empty shows the trail as keyed. */
        std::vector<glm::dvec3> trailLive;
        /** Breakdown in a Strings mode: how far the hands, feet and head lag (favour), and the arcs they travel along the slider. */
        double favour = 0.0, arcFavour = 0.0;
        std::vector<NodeId> arcJoints;
        std::vector<std::vector<glm::dvec3>> arcs;
        /** IK: bones the wheel set to bend (0 the rules' limb), the limb the last preview bent, and its joints. */
        int chainLength = 0, autoChain = 0;
        std::vector<std::string> chain;
        QString readout;
        bool warned = false;
    };

    /** Radii in logical pixels: the zones from the size setting, the band around them fixed. */
    struct Geo
    {
        double zone, ringInner, centerDisc, bandIn, ring, plate, tab;
    };
    static Geo geo()
    {
        Geo g;
        g.zone = std::max(kMinZoneRadius, kZoneRadius * settings().size);
        g.ringInner = g.zone * 0.46;
        g.centerDisc = g.zone * 0.40;
        g.bandIn = g.zone + 5.0;
        g.ring = g.bandIn + 3.0 + kBandHalf;
        g.plate = g.ring + kBandHalf + 3.0;
        g.tab = g.plate + 9.0;
        return g;
    }
    /** Degrees spanned by px of arc at radius r. */
    static double degreesFor(double px, double r) { return r > 1e-6 ? px / r * 180.0 / math::kPi : 0.0; }

    /** Where a tab button sits: its slot, centred on kTabCenter, on the tab outside the rim. */
    static double tabAngle(int slot, int count)
    {
        return kTabCenter + (slot - (count - 1) * 0.5) * degreesFor(kTabSpacing, geo().tab);
    }
    QPointF buttonPos(const Button& b, const Target& t) const
    {
        const Geo g = geo();
        if (b.tabSlot >= 0)
            return at(center(), g.tab, tabAngle(b.tabSlot, shown(Handle::Proxy, t) ? 3 : 2));
        return at(center(), g.ring, b.angle);
    }
    static std::pair<double, double> toolSpan(int i)
    {
        const double step = (kToolTo - kToolFrom) / 3.0, gap = degreesFor(kSegmentGap, geo().ring) * 0.5;
        return {kToolFrom + step * i + gap, kToolFrom + step * (i + 1) - gap};
    }

    /** While the summon key is held: the Spinner centres on the cursor. */
    void followCursor()
    {
        const QPointF want = mCursor - QPointF(mView.width * 0.5, mView.height * 0.5);
        auto& o = settings().offsets[mView.camera];
        const QPointF before = o;
        o = want;
        // Kept where center() clamps it, as a rim drag is.
        o = center() - QPointF(mView.width * 0.5, mView.height * 0.5);
        if (o != before)
            viewportTools::requestRedraw();
    }

    bool ringLayout(const Target& t) const
    {
        const Settings& s = settings();
        return s.tool == Tool::Scale || (t.chain && s.tool == Tool::Rotate);
    }

    QPointF offset() const
    {
        if (mDrag && mDrag->kind == Kind::Reposition)
            return mDrag->offset;
        const auto& o = settings().offsets;
        const auto it = o.find(mView.camera);
        return it != o.end() ? it->second : QPointF(kDefaultOffsetX, 0.0);
    }

    /** Kept wholly inside the view however small it gets. */
    QPointF center() const
    {
        const double r = geo().tab + 14.0;
        const QPointF c = QPointF(mView.width * 0.5, mView.height * 0.5) + offset();
        return {std::clamp(c.x(), r, std::max(r, mView.width - r)), std::clamp(c.y(), r, std::max(r, mView.height - r))};
    }

    static QPointF at(QPointF c, double r, double degrees)
    {
        const double a = math::radians(degrees);
        return {c.x() + r * std::cos(a), c.y() + r * std::sin(a)};
    }

    static void iconAt(OverlayPainter& p, QPointF c, double size, const char* name, const glm::vec4& color)
    {
        p.icon(QRectF(c.x() - size * 0.5, c.y() - size * 0.5, size, size), name, color);
    }

    bool shown(Handle h, const Target& t) const
    {
        const Settings& s = settings();
        switch (h)
        {
        case Handle::Pivot: return t.rig;
        case Handle::Context: return t.foot || t.rig;
        case Handle::Sticky2nd:
        case Handle::KeepDirection: return t.rig && s.stickyAim;
        case Handle::Proxy: return animProxyEnabled() && !selectedProxySets().empty();
        default: return true;
        }
    }

    bool active(Handle h) const
    {
        const Settings& s = settings();
        switch (h)
        {
        case Handle::Move: return s.tool == Tool::Move;
        case Handle::Rotate: return s.tool == Tool::Rotate;
        case Handle::Scale: return s.tool == Tool::Scale;
        case Handle::Discrete: return s.discrete;
        case Handle::Pivot: return s.pivot;
        case Handle::Context: return !currentTarget().foot && s.stickyAim;
        case Handle::Sticky2nd: return s.sticky2nd;
        case Handle::KeepDirection: return s.keepDirection;
        case Handle::Help: return s.help;
        case Handle::Proxy: return selectionShowsProxy();
        default: return false;
        }
    }

    /** The buttons with an alternative on the right button carry a small corner box. */
    static bool hasAlternative(Handle h)
    {
        return h == Handle::Rotate || h == Handle::Scale || h == Handle::Context || h == Handle::Space || h == Handle::Proxy;
    }

    const char* iconFor(Handle h, const Target& t) const
    {
        const Settings& s = settings();
        switch (h)
        {
        case Handle::Move: return "puppeteer.move";
        case Handle::Rotate: return s.inherit ? "puppeteer.inherit" : "puppeteer.rotate";
        case Handle::Scale: return s.toon ? "puppeteer.toon" : "puppeteer.scale";
        case Handle::Space: return s.space == Space::World ? "puppeteer.world" : s.space == Space::Local ? "puppeteer.local" : "puppeteer.screen";
        case Handle::Discrete: return "puppeteer.discrete";
        case Handle::Pivot: return "puppeteer.pivot";
        case Handle::Context: return t.foot ? (s.footAlign ? "puppeteer.footAlign" : "puppeteer.foot") : "puppeteer.sticky";
        case Handle::Sticky2nd: return "puppeteer.sticky2nd";
        case Handle::KeepDirection: return "puppeteer.keepDirection";
        case Handle::Settings: return "puppeteer.settings";
        case Handle::Help: return "puppeteer.help";
        case Handle::Proxy: return "puppeteer.proxy";
        default: return "";
        }
    }

    /** The small corner triangle marking a control with an alternative on the right button. */
    static void cornerMark(OverlayPainter& p, QPointF corner, double size, const glm::vec4& col)
    {
        p.triangle(corner, corner - QPointF(size, 0.0), corner - QPointF(0.0, size), col);
    }

    /** A toggle or action in a round well; on is a bright ring, not a filled disc. */
    void drawButton(OverlayPainter& p, const Button& b, const Target& t)
    {
        const QPointF pos = buttonPos(b, t);
        const double r = b.tabSlot >= 0 ? kTabWell : kWell;
        const bool on = active(b.handle);
        const bool hot = mHover == b.handle;
        p.smoothDisc(pos, r, hot ? kPal.wellHot : kPal.well);
        if (on)
            p.arc(pos, r - 1.0, 0.0, 360.0, 2.0f, kPal.ring);
        iconAt(p, pos, kIcon, iconFor(b.handle, t), on ? kPal.iconOn : hot ? kPal.iconHot : kPal.icon);
        if (hasAlternative(b.handle))
            cornerMark(p, pos + QPointF(r * 0.72, r * 0.72), r * 0.42, on || hot ? kPal.iconHot : kPal.icon);
    }

    /** The tab outside the rim holding Settings, Help and the anim proxy toggle. */
    void drawTab(OverlayPainter& p, QPointF c, const Geo& g, int count) const
    {
        const double half = degreesFor(kTabSpacing * (count - 1) * 0.5 + kTabWell + 5.0, g.tab);
        p.softShadow(at(c, g.tab, kTabCenter) + QPointF(0.0, 2.0), kTabWell + 4.0, 6.0, 0.2f);
        p.sector(c, g.plate - 8.0, g.tab + kTabWell + 4.0, kTabCenter - half, kTabCenter + half, kPal.bezel, kPal.bezel, kTabWell + 2.0);
    }

    /** Move, Rotate and Scale as one strip across the top of the band; the current tool is the bright segment. */
    void drawTools(OverlayPainter& p, QPointF c, const Geo& g, const Target& t, Handle hot, bool disabled) const
    {
        for (int i = 0; i < 3; ++i)
        {
            const Handle h = kTools[i];
            const auto [from, to] = toolSpan(i);
            const bool on = active(h), lit = hot == h;
            const glm::vec4 fill = on ? (disabled ? kPal.disabled : kPal.segmentOn) : lit ? kPal.segmentHot : kPal.segment;
            p.sector(c, g.ring - kBandHalf, g.ring + kBandHalf, from, to, fill, fill, 4.0);
            const glm::vec4 ink = on ? kPal.iconOnSegment : lit ? kPal.iconHot : kPal.icon;
            const double mid = (from + to) * 0.5;
            const double room = (to - from) * math::kPi / 180.0 * g.ring - 8.0;
            iconAt(p, at(c, g.ring, mid), std::clamp(room, kIcon, kToolIcon), iconFor(h, t), ink);
            if (hasAlternative(h))
                cornerMark(p, at(c, g.ring + kBandHalf - 3.0, to - degreesFor(3.0, g.ring + kBandHalf)), 5.0, ink);
        }
    }

    /** A recessed groove from the previous key to the next, ticks at both keys and now, filled toward the thumb. */
    void drawBreakdown(OverlayPainter& p, QPointF c, Handle hot, bool disabled)
    {
        const Geo g = geo();
        p.arc(c, g.ring, kBreakdownFrom, kBreakdownTo, 5.0f, kPal.groove, true);
        p.arc(c, g.ring + 3.0, kBreakdownFrom + 2.0, kBreakdownTo - 2.0, 1.0f, spinnerGray(1.0f, 0.06f));
        for (double a : {kBreakdownFrom, 90.0, kBreakdownTo})
            p.smoothLine(at(c, g.ring - 8.0, a), at(c, g.ring - (a == 90.0 ? 4.0 : 5.0), a), 1.5f, disabled ? kPal.disabled : kPal.tick);
        // The thumb follows the drag along the groove, back to the middle once released.
        double dot = 90.0;
        if (mDrag && mDrag->kind == Kind::Breakdown)
            dot = std::clamp(90.0 - (mDrag->current.x() - mDrag->start.x()) / g.plate * 45.0, kBreakdownFrom, kBreakdownTo);
        if (std::abs(dot - 90.0) > 0.5 && !disabled)
            p.arc(c, g.ring, std::min(dot, 90.0), std::max(dot, 90.0), 3.0f, kPal.grooveFill);
        const QPointF thumb = at(c, g.ring, dot);
        p.smoothDisc(thumb, 7.5, spinnerGray(0.0f, 0.5f));
        p.smoothDisc(thumb, 6.5, disabled ? kPal.disabled : hot == Handle::Breakdown ? kPal.thumbHot : kPal.thumb);
    }

    /** The motion trail of the first selected joint: its frames, positions and keyed frames, sampled when the scene changes. */
    struct Trail
    {
        NodeId joint = kNoNode;
        std::vector<double> times;
        std::vector<glm::dvec3> points;
        std::vector<bool> keyed;
        uint64_t version = 0;
    };

    /** The trail's frames: the time slider's range and its falloff, else the trail setting either side of now. */
    static std::vector<double> trailTimes()
    {
        namespace an = lila::scene::anim;
        const an::TimeSelection& sel = an::timeSelection();
        const an::Playback& pb = an::playback();
        double from, to;
        if (sel.active)
        {
            from = an::frameOf(sel.start) - sel.falloff;
            to = an::frameOf(sel.end) + sel.falloff;
        }
        else
        {
            const double now = std::round(an::frameOf(graph().time()));
            from = now - settings().trailFrames;
            to = now + settings().trailFrames;
        }
        from = std::max(from, std::round(an::frameOf(pb.animStart)));
        to = std::min(to, std::round(an::frameOf(pb.animEnd)));
        std::vector<double> out;
        for (double f = std::ceil(from); f <= to + 1e-6; f += 1.0)
            out.push_back(an::secondsOf(f));
        return out;
    }

    void refreshTrail(const Target& t)
    {
        if (!settings().trail || !t.character || t.rig || mDrag)
        {
            if (!mDrag)
                mTrail.joint = kNoNode;
            return;
        }
        Graph& g = graph();
        const NodeId joint = g.find(t.names.front())->id();
        std::vector<double> times = trailTimes();
        if (joint == mTrail.joint && times == mTrail.times && g.version() == mTrail.version)
            return;
        std::vector<glm::dvec3> points = strings::trail(g, joint, times);
        // While an edit is held the trail can't be sampled; the last one stays.
        if (points.empty() && joint == mTrail.joint)
            return;
        mTrail = {joint, std::move(times), std::move(points), {}, 0};
        const std::vector<double> keys = lila::scene::anim::keyTimes(g, {joint});
        for (double tm : mTrail.times)
            mTrail.keyed.push_back(std::any_of(keys.begin(), keys.end(), [&](double k) { return std::abs(k - tm) < 1e-6; }));
        mTrail.version = g.version();
    }

    /** While an IK drag runs: the bones that bend, so the wheel's chain length shows. */
    void drawChain(OverlayPainter& p) const
    {
        if (!mDrag || mDrag->chain.size() < 2)
            return;
        Graph& g = Graph::current();
        std::vector<QPointF> pts;
        for (const std::string& name : mDrag->chain)
            if (Node* n = g.find(name))
                if (const auto s = mView.project(glm::dvec3(worldOf(g, n->id())[3])))
                    pts.push_back(*s);
        p.polyline(pts, 3.0f, kYellow);
    }

    /** The trail frame under px, or -1. */
    int trailAt(QPointF px) const
    {
        if (mTrail.joint == kNoNode)
            return -1;
        int best = -1;
        double bestD = 7.0;
        for (size_t i = 0; i < mTrail.points.size(); ++i)
            if (const auto at = mView.project(mTrail.points[i]))
                if (const double d = std::hypot(at->x() - px.x(), at->y() - px.y()); d < bestD)
                {
                    bestD = d;
                    best = int(i);
                }
        return best;
    }

    /** A thin line through the frames, a dot per frame, larger on keys; the current frame ringed, the dragged one moved. */
    void drawTrail(OverlayPainter& p, const Target& t)
    {
        refreshTrail(t);
        if (mTrail.joint == kNoNode || mTrail.points.size() < 2)
            return;
        const double now = graph().time();
        const int hot = mDrag && mDrag->kind == Kind::Trail ? mDrag->trailFrame : mHover == Handle::Trail ? mTrailHover : -1;
        const std::vector<glm::dvec3>& points = mDrag && mDrag->trailLive.size() == mTrail.points.size() ? mDrag->trailLive : mTrail.points;
        std::vector<QPointF> line;
        for (const glm::dvec3& pt : points)
            if (const auto at = mView.project(pt))
                line.push_back(*at);
        p.polyline(line, 1.5f, spinnerGray(0.9f, 0.45f));
        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto at = mView.project(points[i]);
            if (!at)
                continue;
            const bool current = std::abs(mTrail.times[i] - now) < 1e-6;
            const float r = mTrail.keyed[i] ? 4.0f : 2.5f;
            p.disc(*at, int(i) == hot ? r + 1.5f : r, int(i) == hot ? spinnerGray(1.0f) : spinnerGray(0.85f, 0.85f), 12);
            if (current)
                p.disc(*at, r + 3.0f, spinnerGray(0.95f, 0.35f), 16);
        }
        if (mDrag && mDrag->kind == Kind::Trail && mDrag->trailFrame >= 0)
            if (const auto from = mView.project(mTrail.points[size_t(mDrag->trailFrame)]))
                if (const auto to = mView.project(mTrail.points[size_t(mDrag->trailFrame)] + mDrag->trailBy))
                {
                    p.line(*from, *to, 1.5f, spinnerGray(1.0f, 0.8f));
                    p.disc(*to, 5.0f, spinnerGray(1.0f), 16);
                }
    }

    void beginTrailDrag(QPointF px)
    {
        const int frame = trailAt(px);
        if (frame < 0)
            return;
        Drag d;
        d.kind = Kind::Trail;
        d.handle = Handle::Trail;
        d.start = d.current = px;
        d.target = currentTarget();
        d.trailFrame = frame;
        mDrag = std::move(d);
    }

    /** A trail frame dragged: previewed across its frames while dragging, posed on release, easing out across the falloff. */
    void applyTrail(bool commit)
    {
        Drag& d = *mDrag;
        const glm::dvec3 at = mTrail.points[size_t(d.trailFrame)];
        d.trailBy = screenDelta(d.start, d.current, at);
        const double frame = lila::scene::anim::frameOf(mTrail.times[size_t(d.trailFrame)]);
        d.readout = QString("Frame %1  %2 cm").arg(frame).arg(glm::length(d.trailBy), 0, 'f', 2);
        if (glm::length(d.trailBy) < 1e-9)
        {
            d.trailLive.clear();
            lila::scene::ghosts::cache().clearPreview();
            return;
        }
        // The modes that only turn the joint have nothing that bends to put it somewhere: the trail drags in IK there.
        const strings::Mode mode = strings::movesOrigin(stringsMode(), false) ? stringsMode() : strings::Mode::Ik;
        const lila::scene::anim::TimeSelection& sel = lila::scene::anim::timeSelection();
        const double t = mTrail.times[size_t(d.trailFrame)];
        json call = {{"args", {graph().node(mTrail.joint)->path()}},
                     {"mode", strings::modeName(mode)},
                     {"by", vecJson(d.trailBy)},
                     {"stretch", stringsStretch()}};
        if (sel.active && t >= sel.start - 1e-6 && t <= sel.end + 1e-6)
        {
            addRangeFlags(call, sel);
        }
        else
        {
            call["range"] = json::array({t, t});
            call["falloff"] = settings().trailFalloff;
        }
        // Keyed animation moves its keys; a frame between keys, or dense motion, moves every frame.
        call["dense"] = !mTrail.keyed[size_t(d.trailFrame)];
        if (!commit)
        {
            previewAcross(call);
            return;
        }
        runCommand("stringsPose", call);
        mTrail.version = 0;
    }

    /**
     * A drag across frames shown live: every frame solved without keying, the ghosts showing the result and the trail
     * following when it is the dragged joint's. Nothing is left changed, so the frame's own preview comes after.
     */
    void previewAcross(json call)
    {
        Drag& d = *mDrag;
        call["preview"] = true;
        const bool trailed = mTrail.joint != kNoNode && !call["args"].empty() && graph().find(call["args"][0].get<std::string>()) &&
                             graph().find(call["args"][0].get<std::string>())->id() == mTrail.joint;
        if (trailed)
            call["show"] = mTrail.times;
        json result;
        if (std::string error = quietly([&] { result = ops::execute("stringsPose", call, CallSource::Script); }); !error.empty())
        {
            if (!d.warned)
                log::write(LogLevel::Warning, "Spinner: " + error);
            d.warned = true;
            return;
        }
        if (!trailed || !result.is_array())
            return;
        d.trailLive = mTrail.points;
        for (const json& f : result)
            if (f.contains("at") && !f["at"].empty())
                if (const int i = trailIndex(f["time"].get<double>()); i >= 0)
                    d.trailLive[size_t(i)] = jsonVec(f["at"][0]);
    }

    int trailIndex(double t) const
    {
        for (size_t i = 0; i < mTrail.times.size(); ++i)
            if (std::abs(mTrail.times[i] - t) < 1e-6)
                return int(i);
        return -1;
    }

    static glm::dvec3 jsonVec(const json& v) { return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}; }

    /** The Strings modes show on joints being animated; controllers and rigging have their own behaviour. */
    bool modeArc(const Target& t) const { return modes::current() != kRigMode && !t.rig && !t.chain && t.character; }

    static std::pair<double, double> modeSpan(int slot)
    {
        // Default on top, down to Full Body, in the order the modes widen.
        const double step = (kModeTo - kModeFrom) / kArcCount, gap = degreesFor(kSegmentGap, geo().ring) * 0.5;
        const double from = kModeTo - step * (slot + 1);
        return {from + gap, from + step - gap};
    }

    /**
     * One joined strip of segments, styled as the tool strip; the current mode is the bright one. The segments with an
     * All variant carry the corner mark, and while that variant is current a bar along the segment's outer edge.
     */
    void drawModes(OverlayPainter& p, QPointF c, Handle hot)
    {
        static const char* icons[] = {"puppeteer.default", "puppeteer.free", "puppeteer.forward", "puppeteer.backward",
                                      "puppeteer.bidirectional", "puppeteer.ik", "puppeteer.fullBody"};
        const Geo g = geo();
        const strings::Mode current = stringsMode();
        const Handle lit = handleOf(current);
        for (int i = 0; i < kArcCount; ++i)
        {
            const Handle h = Handle(int(Handle::ModeDefault) + i);
            const auto [from, to] = modeSpan(i);
            const bool on = h == lit, hover = hot == h;
            const glm::vec4 fill = on ? kPal.segmentOn : hover ? kPal.segmentHot : kPal.segment;
            p.sector(c, g.ring - kBandHalf, g.ring + kBandHalf, from, to, fill, fill, 4.0);
            const glm::vec4 ink = on ? kPal.iconOnSegment : hover ? kPal.iconHot : kPal.icon;
            iconAt(p, at(c, g.ring, (from + to) * 0.5), kIcon, icons[i], ink);
            if (hasAllVariant(kArcModes[i]))
            {
                cornerMark(p, at(c, g.ring - kBandHalf + 3.0, from + degreesFor(3.0, g.ring - kBandHalf)), 5.0, ink);
                if (on && strings::isAllVariant(current))
                    p.sector(c, g.ring - kBandHalf, g.ring - kBandHalf + 3.0, from, to, kPal.iconOnSegment, kPal.iconOnSegment, 1.0);
            }
        }
        // The current mode by name beside the arc, so it reads without learning the icons.
        const auto [from, to] = modeSpan(slotOf(lit));
        const QPointF tag = at(c, g.plate + 8.0, (from + to) * 0.5);
        p.setShadow(true);
        p.text(QPointF(tag.x(), tag.y() + 4.5), stringsModeLabel(current), spinnerGray(0.92f), OverlayPainter::Right);
        p.setShadow(false);
    }

    void drawHelp(OverlayPainter& p, QPointF c)
    {
        const QStringList lines = {"Drag a zone up or down: X, Y, Z",
                                   "Middle drag: free rotate",
                                   "Controller centre: pole (right: pole and controller)",
                                   "Reverse foot, right drag a ring: roll, twist, bank",
                                   "Right-click a controller in the view: lock its key",
                                   "Right-click a button with a corner mark: its alternative",
                                   "Left arc: Strings mode. U + drag: pin, contacts, stretch, zero",
                                   "Drag the rim: move the Spinner",
                                   "Hold `: bring the Spinner to the cursor"};
        const double plate = geo().plate;
        double y = c.y() + plate + 18.0;
        for (const QString& l : lines)
        {
            p.label(QPointF(c.x() - plate, y), l);
            y += 18.0;
        }
    }

    /** Glue and attach sets: a thin line from each member to the set's leader (an attach set's last), dots on the members. */
    void drawRelations(OverlayPainter& p)
    {
        Graph& g = graph();
        for (const std::vector<NodeId>& set : stringsRelations())
        {
            const auto lead = mView.project(glm::dvec3(worldOf(g, set.back())[3]));
            if (!lead)
                continue;
            p.disc(*lead, 4.0f, spinnerGray(0.95f, 0.9f), 12);
            for (size_t i = 0; i + 1 < set.size(); ++i)
                if (const auto at = mView.project(glm::dvec3(worldOf(g, set[i])[3])))
                {
                    p.line(*at, *lead, 1.5f, spinnerGray(0.95f, 0.55f));
                    p.disc(*at, 3.0f, spinnerGray(0.95f, 0.9f), 12);
                }
        }
    }

    /** The joint-side gizmo: axis rings (rotate) or lines (move, scale) at the pivot, white on the hovered or dragged axis. */
    void drawViewportGizmo(OverlayPainter& p, const Target& t)
    {
        if (t.names.empty())
            return;
        Graph& g = graph();
        Node* n = g.find(t.names.front());
        if (!n)
            return;
        const glm::dvec3 pivot = mDrag ? mDrag->pivot : glm::dvec3(worldOf(g, n->id())[3]);
        const glm::dmat3 axes = mDrag ? mDrag->axes : axesFor(g, n->id());
        const double len = kGizmoPixels * mView.unitsPerPixel(pivot);
        const bool color = viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors);
        const Handle hot = mDrag ? mDrag->handle : mHover;
        if (mDrag && mDrag->kind == Kind::Trackball)
        {
            if (auto at = mView.project(pivot))
            {
                p.smoothDisc(*at, kGizmoPixels, spinnerGray(0.85f, 0.14f));
                p.arc(*at, kGizmoPixels, 0.0, 360.0, 2.0f, spinnerGray(0.92f, 0.7f));
            }
            return;
        }
        const glm::dvec3 toCamera = glm::normalize(glm::dvec3(glm::inverse(glm::dmat4(mView.view))[2]));
        static const char* letters[] = {"X", "Y", "Z"};
        p.setShadow(true);
        for (int axis = 0; axis < 3; ++axis)
        {
            const bool lit = isAxis(hot) && axisOf(hot) == axis;
            if (mDrag && mDrag->kind == Kind::Transform && !lit)
                continue;
            const glm::vec4 col = lit ? spinnerGray(1.0f) : color ? glm::vec4(viewportDisplay::axisColor(axis), 0.95f) : spinnerGray(0.85f - 0.15f * float(axis));
            const float width = lit ? 3.0f : 2.0f;
            if (settings().tool == Tool::Rotate)
            {
                // The far half of each ring dims, so the rings read as circles in depth.
                const glm::dvec3 a = axes[(axis + 1) % 3], b = axes[(axis + 2) % 3];
                std::vector<QPointF> run;
                bool front = true;
                auto flush = [&] {
                    if (run.size() > 1)
                        p.polyline(run, front ? width : width * 0.75f, front ? col : glm::vec4(glm::vec3(col), col.a * 0.35f));
                    run.clear();
                };
                for (int k = 0; k <= 64; ++k)
                {
                    const double ang = 2.0 * math::kPi * k / 64.0;
                    const glm::dvec3 dir = a * std::cos(ang) + b * std::sin(ang);
                    const bool f = glm::dot(dir, toCamera) >= -0.05;
                    const auto pt = mView.project(pivot + dir * len);
                    if (!pt)
                        continue;
                    if (f != front && !run.empty())
                    {
                        const QPointF last = run.back();
                        flush();
                        run.push_back(last);
                    }
                    front = f;
                    run.push_back(*pt);
                }
                flush();
                continue;
            }
            const auto base = mView.project(pivot), tip = mView.project(pivot + axes[axis] * len);
            if (!base || !tip)
                continue;
            const QPointF d = *tip - *base;
            const double l = std::hypot(d.x(), d.y());
            // Seen end on, an axis would be a dot: let it fade instead of flickering.
            const float fade = float(std::clamp((l - 6.0) / 14.0, 0.0, 1.0));
            if (fade <= 0.0f)
                continue;
            glm::vec4 c = col;
            c.a *= fade;
            const QPointF u = d / l, side(-u.y(), u.x());
            if (settings().tool == Tool::Move)
            {
                p.line(*base, *tip - u * 7.0, width, c);
                p.triangle(*tip + u * 4.0, *tip - u * 8.0 + side * 5.0, *tip - u * 8.0 - side * 5.0, c);
            }
            else
            {
                p.line(*base, *tip - u * 4.0, width, c);
                p.rect(QRectF(tip->x() - 4.5, tip->y() - 4.5, 9.0, 9.0), c);
            }
            p.text(*tip + u * 16.0 + QPointF(0.0, 4.5), letters[axis], glm::vec4(glm::vec3(lit ? spinnerGray(1.0f) : col), c.a), OverlayPainter::Center);
        }
        p.setShadow(false);
    }

    glm::dmat3 axesFor(Graph& g, NodeId first) const
    {
        switch (settings().space)
        {
        case Space::World: return glm::dmat3(1.0);
        case Space::Local: return math::rotationOf(worldOf(g, first));
        case Space::Screen: return glm::transpose(glm::dmat3(mView.view));
        }
        return glm::dmat3(1.0);
    }

    Handle hit(QPointF px, const Target& t) const
    {
        const QPointF c = center();
        const Geo g = geo();
        const double dx = px.x() - c.x(), dy = px.y() - c.y();
        const double d = std::hypot(dx, dy);
        // Wells take a few pixels more than they draw, so the small ones are easy to hit.
        for (const Button& b : kButtons)
            if (shown(b.handle, t))
            {
                const QPointF bp = buttonPos(b, t);
                if (std::hypot(px.x() - bp.x(), px.y() - bp.y()) <= (b.tabSlot >= 0 ? kTabWell : kWell) + 3.0)
                    return b.handle;
            }
        if (d > g.plate)
            return trailAt(px) >= 0 ? Handle::Trail : Handle::None;
        const double a = std::atan2(dy, dx) * 180.0 / math::kPi;
        const bool rings = ringLayout(t);
        if (rings ? d <= g.centerDisc : d <= kHub + 3.0)
            return Handle::Center;
        if (d <= g.zone + 2.0)
        {
            if (rings && d < g.ringInner)
                return Handle::Plate;
            return a >= kZoneX[0] && a <= kZoneX[1] ? Handle::AxisX : a > kZoneZ[0] && a <= kZoneZ[1] ? Handle::AxisZ : Handle::AxisY;
        }
        const bool inBand = d >= g.ring - kBandHalf - 2.0;
        if (inBand && a >= kToolFrom && a <= kToolTo)
            return kTools[std::clamp(int((a - kToolFrom) / ((kToolTo - kToolFrom) / 3.0)), 0, 2)];
        if (modeArc(t) && inBand)
        {
            const double around = a < 0.0 ? a + 360.0 : a;
            if (around >= kModeFrom && around <= kModeTo)
                return Handle(int(Handle::ModeDefault) + std::clamp(int((kModeTo - around) / ((kModeTo - kModeFrom) / kArcCount)), 0, kArcCount - 1));
        }
        if (!t.rig && modes::current() != kRigMode && inBand && a >= kBreakdownFrom && a <= kBreakdownTo)
            return Handle::Breakdown;
        return Handle::Plate;
    }

    void openSettings()
    {
        auto* m = new QMenu;
        m->setAttribute(Qt::WA_DeleteOnClose);
        Settings& s = settings();
        auto toggle = [m](const QString& label, bool& flag) {
            QAction* a = m->addAction(label);
            a->setCheckable(true);
            a->setChecked(flag);
            QObject::connect(a, &QAction::toggled, [&flag](bool on) {
                flag = on;
                changed();
            });
        };
        auto choice = [m](const QString& title, double& value, std::initializer_list<std::pair<QString, double>> options) {
            QMenu* sub = m->addMenu(title);
            auto* group = new QActionGroup(sub);
            for (const auto& [label, v] : options)
            {
                QAction* a = sub->addAction(label);
                a->setCheckable(true);
                a->setChecked(std::abs(value - v) < 1e-9);
                group->addAction(a);
                const double pick = v;
                QObject::connect(a, &QAction::triggered, [&value, pick] {
                    value = pick;
                    changed();
                });
            }
        };
        toggle("Discrete Steps", s.discrete);
        choice("Move Step", s.moveStep, {{"1 cm", 1.0}, {"5 cm", 5.0}, {"10 cm", 10.0}, {"25 cm", 25.0}});
        choice("Rotate Step", s.rotateStep, {{"5°", 5.0}, {"15°", 15.0}, {"45°", 45.0}, {"90°", 90.0}});
        choice("Scale Step", s.scaleStep, {{"0.05", 0.05}, {"0.1", 0.1}, {"0.25", 0.25}});
        m->addSeparator();
        choice("Joint Rotate Speed", s.rotateSpeed, {{"0.25°/px", 0.25}, {"0.43°/px", 0.43}, {"0.5°/px", 0.5}, {"1°/px", 1.0}});
        choice("Controller Rotate Speed", s.controllerSpeed, {{"0.25°/px", 0.25}, {"0.5°/px", 0.5}, {"0.62°/px", 0.62}, {"1°/px", 1.0}});
        choice("Pole Speed", s.poleSpeed, {{"0.25°/px", 0.25}, {"0.42°/px", 0.42}, {"0.62°/px", 0.62}, {"1°/px", 1.0}});
        choice("Toon Scale Coefficient", s.toonExponent, {{"0 (none)", 0.0}, {"1/3", 1.0 / 3.0}, {"1/2 (volume)", 0.5}, {"1", 1.0}});
        choice("Breakdown Full Travel", s.breakdownFull, {{"60 px", 60.0}, {"100 px", 100.0}, {"160 px", 160.0}});
        m->addSeparator();
        toggle("Inherit Rotate", s.inherit);
        toggle("Toon Scale", s.toon);
        toggle("Auto Pose", s.autoPose);
        toggle("Motion Trail", s.trail);
        choice("Motion Trail Frames", s.trailFrames, {{"5", 5.0}, {"10", 10.0}, {"20", 20.0}, {"40", 40.0}});
        choice("Trail Drag Falloff", s.trailFalloff, {{"None", 0.0}, {"2 frames", 2.0}, {"5 frames", 5.0}, {"10 frames", 10.0}});
        toggle("Sticky Aim (rigging)", s.stickyAim);
        toggle("Pivot (rigging)", s.pivot);
        m->addSeparator();
        choice("Size", s.size, {{"Small", 0.75}, {"Medium", 1.0}, {"Large", 1.35}});
        const std::string view = mView.camera;
        QObject::connect(m->addAction("Reset Position"), &QAction::triggered, [view] {
            settings().offsets.erase(view);
            changed();
        });
        QObject::connect(m->addAction("Reset Position in Every View"), &QAction::triggered, [] {
            settings().offsets.clear();
            changed();
        });
        m->popup(QCursor::pos());
    }

    void beginDrag(Kind kind, Handle h, QPointF px, Qt::KeyboardModifiers mods, const Target& t)
    {
        Graph& g = graph();
        Drag d;
        d.kind = kind;
        d.handle = h;
        d.tool = settings().tool;
        d.start = d.current = px;
        d.target = t;
        d.all = mods.testFlag(Qt::ShiftModifier);
        if (kind == Kind::Reposition)
        {
            d.offset = d.startOffset = offset();
            mDrag = std::move(d);
            return;
        }
        if (t.names.empty() && !(kind == Kind::Breakdown && d.all))
            return;
        g.evaluate();
        if (kind == Kind::Breakdown)
        {
            // In a Strings mode the breakdown is the space-aware one: the whole character blends, feet in world. With a
            // time range selected it tweens the range instead, whatever the mode.
            const bool tween = t.character && !t.rig && lila::scene::anim::timeSelection().active;
            if ((posesStrings(t) || tween) && t.character)
            {
                d.tween = tween;
                for (NodeId j : fullBodyJoints(g, g.find(t.names.front())->id()))
                    d.saved.push_back({j, {{int(Transform::kTranslate), g.value({j, Transform::kTranslate, -1})},
                                           {int(Transform::kRotate), g.value({j, Transform::kRotate, -1})}}});
                d.strung = true;
                d.arcJoints = extremities(g, g.find(t.names.front())->id());
            }
            else
                d.samples = sampleBreakdown(g, breakdownNodes(g, t.names, d.all), g.time());
            mDrag = std::move(d);
            return;
        }
        std::vector<NodeId> touched;
        auto touch = [&](NodeId id) {
            if (id && g.node(id) && g.node(id)->type().isA("transform") && std::find(touched.begin(), touched.end(), id) == touched.end())
                touched.push_back(id);
        };
        for (const std::string& n : t.names)
        {
            const NodeId id = g.find(n)->id();
            touch(id);
            touch(g.parent(id));
            for (NodeId c : g.children(id))
                touch(c);
        }
        if (t.foot)
            touch(t.foot);
        // A Strings pose can write every joint of the character.
        if (posesStrings(t) && d.tool != Tool::Scale)
        {
            for (const std::string& n : t.names)
                for (NodeId j : fullBodyJoints(g, g.find(n)->id()))
                    touch(j);
            for (const std::vector<NodeId>& set : stringsRelations())
                for (NodeId m : set)
                {
                    touch(m);
                    for (NodeId j : fullBodyJoints(g, m))
                        touch(j);
                }
        }
        // Posing a controller writes its chain's joint rotations, which a preview step must put back.
        if (t.chain)
        {
            touch(ikGoalOf(g, t.chain));
            for (NodeId j : lila::scene::rigel::posedJoints(g, t.chain))
                touch(j);
        }
        for (NodeId id : touched)
        {
            Saved s{id, {}};
            const Node& node = *g.node(id);
            for (int attr : {int(Transform::kTranslate), int(Transform::kRotate), int(Transform::kScale)})
                s.values.push_back({attr, g.value({id, attr, -1})});
            if (rig::isJoint(node))
                s.values.push_back({Joint::kJointOrient, g.value({id, Joint::kJointOrient, -1})});
            for (const char* ch : {"roll", "twist", "bank", "poleAngle"})
                if (const int a = attrOf(node, ch); a >= 0 && node.type().isA(kReverseFootType))
                    s.values.push_back({a, g.value({id, a, -1})});
            if (node.type().isA("ikHandle"))
                if (const int a = attrOf(node, "poleAngle"); a >= 0)
                    s.values.push_back({a, g.value({id, a, -1})});
            d.saved.push_back(std::move(s));
        }
        const NodeId first = g.find(t.names.front())->id();
        d.pivot = glm::dvec3(worldOf(g, first)[3]);
        d.axes = axesFor(g, first);
        mDrag = std::move(d);
    }

    void restore()
    {
        if (!mDrag)
            return;
        Graph& g = graph();
        quietly([&] {
            for (const Saved& s : mDrag->saved)
                if (g.node(s.id))
                    for (const auto& [attr, v] : s.values)
                        try
                        {
                            if (g.value({s.id, attr, -1}) != v)
                                g.setValue({s.id, attr, -1}, v);
                        }
                        catch (const std::exception&)
                        {
                        }
            for (const BreakdownSample& s : mDrag->samples)
                for (const auto& [attr, ch] : s.channels)
                    try
                    {
                        g.setValue({s.id, attr, -1}, s.current.at(attr));
                    }
                    catch (const std::exception&)
                    {
                    }
        });
    }

    /** Vertical travel since the press: up is positive. */
    double vertical() const { return -(mDrag->current.y() - mDrag->start.y()); }

    /** The drag so far as a command call; null name for no change. */
    std::pair<const char*, json> commandFor()
    {
        Drag& d = *mDrag;
        const Settings& s = settings();
        const double amount = vertical();
        const Target& t = d.target;
        json call = {{"args", namesJson(t.names)}};
        const int axis = isAxis(d.handle) ? axisOf(d.handle) : -1;
        const char* axisName[3] = {"X", "Y", "Z"};
        auto step = [&](double v, double st) { return s.discrete ? math::quantize(v, st) : v; };
        switch (d.kind)
        {
        case Kind::Pole:
        case Kind::PoleTurn:
        {
            const double by = step(amount * s.poleSpeed, s.rotateStep);
            call["args"] = {graph().node(t.chain)->path()};
            call["by"] = by;
            call["turn"] = d.kind == Kind::PoleTurn;
            d.readout = QString("Pole %1°%2").arg(by, 0, 'f', 1).arg(d.kind == Kind::PoleTurn ? "  with the controller" : "");
            return {"puppeteerPole", call};
        }
        case Kind::FootChannel:
        {
            static const char* names[] = {"roll", "twist", "bank"};
            const double by = step(amount * s.controllerSpeed, s.rotateStep);
            call["args"] = {graph().node(t.foot)->path()};
            call[names[axis]] = by;
            d.readout = QString("%1 %2°").arg(names[axis]).arg(by, 0, 'f', 1);
            return {"puppeteerFootChannels", call};
        }
        case Kind::Trackball:
        {
            const glm::dvec2 delta(d.current.x() - d.start.x(), d.current.y() - d.start.y());
            const auto [ax, angle] = math::trackball(delta, glm::transpose(glm::dmat3(mView.view)), s.trackballSpeed);
            if (angle == 0.0)
                return {"", call};
            call["axis"] = vecJson(ax);
            call["angle"] = angle;
            d.readout = QString("Free rotate %1°").arg(angle, 0, 'f', 1);
            if (posesStrings(t))
                return stringsCall(call);
            call["inherit"] = s.inherit;
            return {"puppeteerRotate", call};
        }
        default: break;
        }
        switch (d.tool)
        {
        case Tool::Move:
        {
            glm::dvec3 by(0.0);
            if (axis >= 0)
            {
                const double dist = step(amount * cmPerPixelAlong(d.pivot, d.axes[axis]), s.moveStep);
                by = d.axes[axis] * dist;
                d.readout = QString("%1 %2 cm").arg(axisName[axis]).arg(dist, 0, 'f', 2);
            }
            else
            {
                by = screenDelta(d.start, d.current, d.pivot);
                if (s.discrete)
                    for (int k = 0; k < 3; ++k)
                        by[k] = math::quantize(by[k], s.moveStep);
                d.readout = QString("%1 cm").arg(glm::length(by), 0, 'f', 2);
            }
            call["by"] = vecJson(by);
            if (t.rig)
            {
                call["keepChildren"] = s.pivot;
                call["stickyAim"] = s.stickyAim;
                call["sticky2nd"] = s.sticky2nd;
                call["keepDirection"] = s.keepDirection;
            }
            else if (posesStrings(t))
            {
                call["autoPose"] = s.autoPose;
                return stringsCall(call);
            }
            return {"puppeteerMove", call};
        }
        case Tool::Rotate:
        {
            const double speed = t.chain ? s.controllerSpeed : s.rotateSpeed;
            const double angle = step(amount * speed, s.rotateStep);
            const glm::dvec3 ax = axis >= 0 ? d.axes[axis] : -mView.viewDir(d.pivot);
            call["axis"] = vecJson(ax);
            call["angle"] = angle;
            const bool strung = posesStrings(t);
            const bool inherit = s.inherit && !t.chain && !strung;
            d.readout = QString("%1%2°%3").arg(axis >= 0 ? QString(axisName[axis]) + " " : QString()).arg(angle, 0, 'f', 1).arg(inherit ? "  inherit" : "");
            if (strung)
                return stringsCall(call);
            call["inherit"] = inherit;
            return {"puppeteerRotate", call};
        }
        case Tool::Scale:
        {
            double f = 1.0 + amount * s.scaleSpeed;
            if (s.discrete)
                f = 1.0 + math::quantize(f - 1.0, s.scaleStep);
            f = std::max(f, 0.01);
            const glm::dvec3 by = axis < 0 ? glm::dvec3(f) : math::toonScale(axis, f, s.toon ? s.toonExponent : 0.0);
            call["by"] = vecJson(by);
            call["compensate"] = s.toon;
            d.readout = QString("%1x%2%3").arg(axis >= 0 ? QString(axisName[axis]) + " " : QString()).arg(f, 0, 'f', 3).arg(s.toon ? "  toon" : "");
            return {"puppeteerScale", call};
        }
        }
        return {"", call};
    }

    /** Whether a drag on these joints poses through Strings rather than the plain move and turn. */
    static bool posesStrings(const Target& t)
    {
        // Forward All is the plain turn, except across a time range, where each frame needs its own solve.
        return !t.rig && !t.chain &&
               ((t.character && (stringsMode() != strings::Mode::ForwardAll || lila::scene::anim::timeSelection().active)) || t.related);
    }

    /** The drag as a stringsPose call in the current mode; the readout names the mode. */
    std::pair<const char*, json> stringsCall(json call)
    {
        call["mode"] = strings::modeName(stringsMode());
        call["stretch"] = stringsStretch();
        mDrag->readout += QString("  %1").arg(stringsModeLabel(stringsMode()));
        if (stringsMode() == strings::Mode::Ik && mDrag->chainLength > 0)
            call["chainLength"] = mDrag->chainLength;
        return {"stringsPose", call};
    }

    /**
     * Centimetres per pixel of drag along a world axis: the joint moves as far on screen as the cursor, along the axis's
     * own projection. An axis pointing at the camera is capped at four times the screen-plane rate.
     */
    double cmPerPixelAlong(const glm::dvec3& pivot, const glm::dvec3& axis) const
    {
        const double upp = mView.unitsPerPixel(pivot);
        const auto a = mView.project(pivot), b = mView.project(pivot + glm::normalize(axis) * upp * 10.0);
        if (!a || !b)
            return upp;
        const double px = std::hypot(b->x() - a->x(), b->y() - a->y());
        return upp * 10.0 / std::max(px, 2.5);
    }

    /** World offset between two cursor positions on the plane through pivot facing the view. */
    glm::dvec3 screenDelta(QPointF a, QPointF b, const glm::dvec3& pivot) const
    {
        const glm::dvec3 n = mView.viewDir(pivot);
        auto onPlane = [&](QPointF px) {
            glm::dvec3 o, dir;
            mView.ray(px, o, dir);
            const double den = glm::dot(dir, n);
            return std::abs(den) < 1e-12 ? pivot : o + dir * (glm::dot(pivot - o, n) / den);
        };
        // The drag starts on the Spinner, away from the joint, so only the cursor's travel counts.
        return onPlane(b) - onPlane(a);
    }

    void apply(bool commit)
    {
        Drag& d = *mDrag;
        Graph& g = graph();
        if (d.kind == Kind::Reposition)
        {
            d.offset = d.startOffset + (d.current - d.start);
            if (commit)
            {
                // Stored where center() clamped it, so a drag past the edge doesn't leave it out of reach.
                settings().offsets[mView.camera] = center() - QPointF(mView.width * 0.5, mView.height * 0.5);
                changed();
            }
            return;
        }
        if (d.kind == Kind::Trail)
        {
            applyTrail(commit);
            return;
        }
        if (d.kind == Kind::Breakdown)
        {
            const Settings& s = settings();
            d.weight = math::breakdownWeight(d.current.x() - d.start.x(), s.breakdownDeadZone, s.breakdownFull);
            d.readout = d.weight < 0.0 ? QString("Previous key %1%").arg(int(std::round(-d.weight * 100.0)))
                                       : QString("Next key %1%").arg(int(std::round(d.weight * 100.0)));
            if (d.tween)
            {
                // Right takes the range toward the inbetween of the poses either side of it, left pushes it away.
                d.readout = d.weight < 0.0 ? QString("Exaggerate range %1%").arg(int(std::round(-d.weight * 100.0)))
                                           : QString("Tween range %1%").arg(int(std::round(d.weight * 100.0)));
                restore();
                if (d.weight == 0.0)
                {
                    lila::scene::ghosts::cache().clearPreview();
                    return;
                }
                json call = {{"args", namesJson(d.target.names)}, {"amount", d.weight}};
                if (commit)
                {
                    lila::scene::ghosts::cache().clearPreview();
                    if (const std::string layer = stringsLayer(); !layer.empty())
                        call["layer"] = layer;
                    runCommand("stringsTween", call);
                }
                else
                {
                    call["preview"] = true;
                    if (std::string error = quietly([&] { ops::execute("stringsTween", call, CallSource::Script); }); !error.empty() && !d.warned)
                    {
                        d.warned = true;
                        log::write(LogLevel::Warning, "Spinner: " + error);
                    }
                }
                return;
            }
            if (d.strung)
            {
                // Down lags the hands, feet and head behind the body, up leads them, past a small dead zone.
                const double dy = d.current.y() - d.start.y();
                d.favour = std::clamp(math::breakdownWeight(dy, s.breakdownDeadZone, s.breakdownFull), -1.0, 1.0);
                if (d.favour != 0.0)
                    d.readout += QString("  %1 %2%").arg(d.favour > 0.0 ? "lag" : "lead").arg(int(std::round(std::abs(d.favour) * 100.0)));
                if (!commit && (d.arcs.empty() || std::abs(d.favour - d.arcFavour) > 0.04))
                    sampleArcs(d);
                restore();
                if (d.weight == 0.0)
                    return;
                json call = {{"args", namesJson(d.target.names)}, {"weight", d.weight}};
                if (d.favour != 0.0)
                    call["favour"] = d.favour;
                if (commit)
                    runCommand("stringsBreakdown", call);
                else if (std::string error = quietly([&] { ops::execute("stringsBreakdown", call, CallSource::Script); }); !error.empty() && !d.warned)
                {
                    d.warned = true;
                    log::write(LogLevel::Warning, "Spinner: " + error);
                }
                d.readout += "  in space";
                return;
            }
            if (commit)
            {
                restore();
                if (d.weight != 0.0)
                    runCommand("puppeteerBreakdown", {{"args", namesJson(d.target.names)}, {"weight", d.weight}, {"all", d.all}});
                return;
            }
            quietly([&] {
                for (const BreakdownSample& s : d.samples)
                    for (const auto& [attr, ch] : s.channels)
                        try
                        {
                            g.setValue({s.id, attr, -1}, breakdownValue(g, s, attr, d.weight));
                        }
                        catch (const std::exception&)
                        {
                        }
            });
            return;
        }
        auto [name, call] = commandFor();
        restore();
        if (!*name)
            return;
        // A range selected on the time slider: the drag previews on this frame and lands on every frame in it.
        const lila::scene::anim::TimeSelection& range = lila::scene::anim::timeSelection();
        if (range.active && std::string_view(name) == "stringsPose")
            d.readout += QString("  frames %1-%2").arg(lila::scene::anim::frameOf(range.start)).arg(lila::scene::anim::frameOf(range.end));
        if (commit)
        {
            if (range.active && std::string_view(name) == "stringsPose")
            {
                addRangeFlags(call, range);
                if (const std::string layer = stringsLayer(); !layer.empty())
                    call["layer"] = layer;
            }
            runCommand(name, call);
            return;
        }
        if (range.active && std::string_view(name) == "stringsPose")
        {
            json across = call;
            addRangeFlags(across, range);
            previewAcross(across);
        }
        // A failing preview says why once per drag rather than doing nothing silently.
        json result;
        if (std::string error = quietly([&] { result = ops::execute(name, call, CallSource::Script); }); !error.empty() && !d.warned)
        {
            d.warned = true;
            log::write(LogLevel::Warning, "Spinner: " + error);
        }
        livePointNow(d);
        if (result.is_array() && result.size() == 1)
            result = result[0];
        if (result.is_object() && result.contains("chain"))
        {
            d.chain = result["chain"].get<std::vector<std::string>>();
            if (d.chainLength == 0)
                d.autoChain = int(d.chain.size());
            d.readout += QString("  chain %1").arg(d.chain.size());
        }
        if (result.is_object())
        {
            if (result.contains("anchor"))
                d.readout += QString("  from %1").arg(QString::fromStdString(result["anchor"].get<std::string>()).section('|', -1).section(':', -1));
            if (result.contains("fallback"))
                d.readout += QString("  (%1)").arg(QString::fromStdString(result["fallback"].get<std::string>()));
            if (result.contains("resolved"))
                if (const auto m = strings::modeFromName(result["resolved"].get<std::string>()))
                    d.readout += QString(" (%1)").arg(stringsModeLabel(*m));
        }
    }

    /** The joints whose arcs a breakdown shows: the ends of the character's lines of action (hands, feet, head) and its contacts. */
    static std::vector<NodeId> extremities(Graph& g, NodeId joint)
    {
        const NodeId character = characterFor(g, joint);
        if (!character)
            return {};
        json rules;
        try
        {
            rules = ops::execute("stringsRules", {{"args", {g.node(character)->path()}}});
        }
        catch (const std::exception&)
        {
            return {};
        }
        std::vector<std::string> names;
        for (const auto& [name, ends] : rules["lines"].items())
            if (ends.size() == 2)
                names.push_back(ends[1].get<std::string>());
        for (const json& c : rules["contacts"])
            names.push_back(c.get<std::string>());
        std::vector<NodeId> out;
        for (NodeId j : fullBodyJoints(g, joint))
        {
            const std::string& n = g.node(j)->name();
            const std::string bare = n.substr(n.find_last_of("|:") + 1);
            if (std::find(names.begin(), names.end(), bare) != names.end() && std::find(out.begin(), out.end(), j) == out.end())
                out.push_back(j);
        }
        return out;
    }

    /** Where each extremity goes as the slider runs from the previous key's pose to the next, at the current favour. */
    void sampleArcs(Drag& d)
    {
        d.arcFavour = d.favour;
        d.arcs.assign(d.arcJoints.size(), {});
        if (d.arcJoints.empty())
            return;
        Graph& g = graph();
        constexpr int kSteps = 8;
        for (int k = -kSteps; k <= kSteps; ++k)
        {
            restore();
            const double w = double(k) / kSteps;
            json call = {{"args", namesJson(d.target.names)}, {"weight", w}};
            if (d.favour != 0.0)
                call["favour"] = d.favour;
            // A side with no key to blend toward has no arc.
            if (k != 0 && !quietly([&] { ops::execute("stringsBreakdown", call, CallSource::Script); }).empty())
                continue;
            quietly([&] {
                g.evaluate();
                for (size_t i = 0; i < d.arcJoints.size(); ++i)
                    d.arcs[i].push_back(glm::dvec3(worldOf(g, d.arcJoints[i])[3]));
            });
        }
        restore();
    }

    /** Arcs of the hands, feet and head while the breakdown slider drags: a dim path each, the ends marked, the joint ringed. */
    void drawArcs(OverlayPainter& p)
    {
        if (!mDrag || mDrag->kind != Kind::Breakdown || mDrag->arcs.empty())
            return;
        Graph& g = graph();
        for (size_t i = 0; i < mDrag->arcs.size() && i < mDrag->arcJoints.size(); ++i)
        {
            std::vector<QPointF> line;
            for (const glm::dvec3& pt : mDrag->arcs[i])
                if (const auto at = mView.project(pt))
                    line.push_back(*at);
            if (line.size() < 2)
                continue;
            p.polyline(line, 1.5f, spinnerGray(0.9f, 0.5f));
            p.disc(line.front(), 3.0f, spinnerGray(0.9f, 0.8f), 12);
            p.disc(line.back(), 3.0f, spinnerGray(0.9f, 0.8f), 12);
            if (g.node(mDrag->arcJoints[i]))
                if (const auto at = mView.project(glm::dvec3(worldOf(g, mDrag->arcJoints[i])[3])))
                    p.disc(*at, 4.5f, spinnerGray(1.0f), 16);
        }
    }

    /** The trail's current frame follows the joint as the drag poses it. */
    void livePointNow(Drag& d)
    {
        if (mTrail.joint == kNoNode || !graph().node(mTrail.joint))
            return;
        const int i = trailIndex(graph().time());
        if (i < 0)
            return;
        if (d.trailLive.size() != mTrail.points.size())
            d.trailLive = mTrail.points;
        graph().evaluateUpstream({mTrail.joint});
        d.trailLive[size_t(i)] = glm::dvec3(worldOf(graph(), mTrail.joint)[3]);
    }

    ViewInfo mView;
    Handle mHover = Handle::None;
    std::optional<Drag> mDrag;
    Trail mTrail;
    int mTrailHover = -1;
    mutable std::string mLastMode;
    QPointF mCursor;
    uint64_t mHoverTick = 0;
};

void registerIcons(ext::Context& ctx)
{
    const std::pair<const char*, const char*> icons[] = {
        {"puppeteer.move", "arrows-move"},         {"puppeteer.rotate", "rotate-clockwise"}, {"puppeteer.inherit", "rotate-2"},
        {"puppeteer.scale", "arrows-maximize"},    {"puppeteer.toon", "resize"},             {"puppeteer.settings", "settings"},
        {"puppeteer.help", "help"},                {"puppeteer.world", "world"},             {"puppeteer.local", "cube"},
        {"puppeteer.screen", "device-desktop"},    {"puppeteer.pivot", "focus-centered"},    {"puppeteer.discrete", "grid-dots"},
        {"puppeteer.sticky", "target-arrow"},      {"puppeteer.sticky2nd", "vector-bezier"}, {"puppeteer.keepDirection", "direction"},
        {"puppeteer.foot", "shoe"},                {"puppeteer.footAlign", "line"},          {"puppeteer.pole", "target"},
        {"puppeteer.proxy", "puzzle"},             {"puppeteer.forward", "arrow-down-tail"}, {"puppeteer.backward", "arrow-up-tail"},
        {"puppeteer.ik", "hand-grab"},             {"puppeteer.fullBody", "man"},            {"puppeteer.default", "wand"},
        {"puppeteer.free", "feather"},             {"puppeteer.bidirectional", "arrows-down-up"},
    };
    for (const auto& [id, tabler] : icons)
        overlayIcons::add(ctx, id, {.tabler = tabler});
}

void registerSpinnerActions(ext::Context& ctx)
{
    const char* cat = "Puppeteer Spinner";
    auto flag = [&](const char* id, const char* label, bool Settings::* member, const char* doc) {
        actions::add(ctx, {.id = id, .label = label, .category = cat,
                           .run =
                               [member] {
                                   settings().*member = !(settings().*member);
                                   changed();
                               },
                           .doc = doc});
    };
    actions::add(ctx, {.id = "puppeteer.spinner.move", .label = "Spinner Move", .category = cat, .run = [] { setTool(Tool::Move); },
                       .doc = "The Spinner moves the selection."});
    actions::add(ctx, {.id = "puppeteer.spinner.rotate", .label = "Spinner Rotate", .category = cat,
                       .run = [] { toolOrAlternative(Tool::Rotate); },
                       .doc = "The Spinner rotates the selection; again to toggle Inherit Rotate."});
    actions::add(ctx, {.id = "puppeteer.spinner.scale", .label = "Spinner Scale", .category = cat,
                       .run = [] { toolOrAlternative(Tool::Scale); },
                       .doc = "The Spinner scales the selection; again to toggle Toon Scale."});
    actions::add(ctx, {.id = "puppeteer.spinner.inheritRotate", .label = "Inherit Rotate", .category = cat,
                       .run =
                           [] {
                               settings().inherit = !settings().inherit;
                               setTool(Tool::Rotate);
                           },
                       .doc = "Rotate with the children keeping their world orientation, or back to plain rotate."});
    actions::add(ctx, {.id = "puppeteer.spinner.toonScale", .label = "Toon Scale", .category = cat,
                       .run =
                           [] {
                               settings().toon = !settings().toon;
                               setTool(Tool::Scale);
                           },
                       .doc = "Scale one axis with the other two compensating (squash and stretch), or back to plain scale."});
    for (auto [id, label, space] : {std::tuple{"puppeteer.spinner.local", "Spinner Local Space", Space::Local},
                                    {"puppeteer.spinner.world", "Spinner World Space", Space::World},
                                    {"puppeteer.spinner.screen", "Spinner Screen Space", Space::Screen}})
        actions::add(ctx, {.id = id, .label = label, .category = cat,
                           .run =
                               [space] {
                                   settings().space = space;
                                   changed();
                               },
                           .doc = "The axes the Spinner's zones drag along."});
    actions::add(ctx, {.id = "puppeteer.spinner.cycleSpace", .label = "Cycle Spinner Space", .category = cat,
                       .run =
                           [] {
                               settings().space = Space((int(settings().space) + 1) % 3);
                               changed();
                           },
                       .doc = "The Spinner's axes: local, world, screen."});
    actions::add(ctx, {.id = "puppeteer.spinner.summonDown", .label = "Summon Spinner", .category = cat,
                       .run =
                           [] {
                               summon().held = true;
                               viewportTools::requestRedraw();
                           },
                       .doc = "While held, the Spinner follows the cursor; it stays where the key is let go."});
    actions::add(ctx, {.id = "puppeteer.spinner.summonUp", .label = "Summon Spinner Release", .category = cat,
                       .run =
                           [] {
                               summon().held = false;
                               changed();
                           },
                       .doc = "Ends Summon Spinner: the Spinner stays where it is."});
    flag("puppeteer.spinner.discrete", "Discrete Steps", &Settings::discrete, "Spinner drags move, turn and scale in fixed steps.");
    flag("puppeteer.spinner.stickyAim", "Sticky Aim", &Settings::stickyAim, "Rigging: orients follow the bones as joints move.");
    flag("puppeteer.spinner.pivot", "Pivot", &Settings::pivot, "Rigging: the Spinner moves joints without their children.");
    flag("puppeteer.strings.trail", "Motion Trail", &Settings::trail,
         "The selected joint's motion trail over the time range, or around now: drag a frame of it to move the joint there.");
    flag("puppeteer.spinner.autoPose", "Auto Pose", &Settings::autoPose,
         "Full-body moves let the auto-poser place the rest of the body, as a person would move to reach.");
}
} // namespace

void registerSpinner(ext::Context& ctx)
{
    registerIcons(ctx);
    registerSpinnerActions(ctx);
    gizmos::add(ctx, {.id = kSpinnerGizmo, .label = "Spinner", .create = [] { return std::make_unique<SpinnerGizmo>(); }});
    // Puppeteer's modes show the View Switcher unless a viewport has chosen otherwise.
    auto modeViews = [] {
        const std::string& m = modes::current();
        viewportDisplay::setViewSwitcherDefault(m == kRigMode || m == kAnimateMode);
    };
    modeViews();
    const events::SubscriptionId sub = events::subscribe(events::Event::ModeChanged, [modeViews](const events::EventData&) { modeViews(); }, "Puppeteer");
    ctx.track("puppeteer:viewSwitcher", [sub] {
        events::unsubscribe(sub);
        viewportDisplay::setViewSwitcherDefault(false);
    });
}
} // namespace puppeteer
