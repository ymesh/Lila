#include "scene/Anim.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Events.h"
#include "core/PrefStore.h"
#include "core/Progress.h"
#include "core/Profile.h"
#include "scene/AnimKeyOps.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"
#include "scene/Spaces.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>

namespace lila::scene::anim
{
namespace
{
constexpr double kInf = std::numeric_limits<double>::infinity();

struct State
{
    Playback playback;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    PlaybackDriver driver;
    int direction = 0;
    std::optional<bool> autoKey;
};

State& state()
{
    static State s;
    return s;
}

/** Clipboard of copyKey; times relative to the earliest copied key. */
struct ClipChannel
{
    std::string attr;
    std::vector<Key> keys;
    /** A rotation curve's keys instead (attr is then the whole rotate, e.g. "rotate"). */
    std::vector<RotKey> rotKeys;
    bool rotation = false;
};
std::vector<ClipChannel>& clipboard()
{
    static std::vector<ClipChannel> c;
    return c;
}

using rigutil::guarded;

bool stepped(const AttrDef& a)
{
    return a.type == PlugType::Bool || a.type == PlugType::Enum;
}

double currentValue(Graph& g, const PlugRef& p)
{
    g.evaluate();
    const PlugValue& v = g.storedValue(p);
    if (p.component >= 0)
        return std::get<glm::dvec3>(v)[p.component];
    return std::visit(
        [](const auto& x) -> double {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_arithmetic_v<T>)
                return double(x);
            else
                return 0.0;
        },
        v);
}

struct Target
{
    PlugRef plug; // invalid when a curve node was named directly
    NodeId curve = kNoNode;
};

std::vector<std::string> attrList(const Args& a)
{
    std::vector<std::string> out;
    if (!a.has("attribute"))
        return out;
    const json& v = a.flags["attribute"];
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& x : v)
            if (x.is_string())
                out.push_back(x.get<std::string>());
    return out;
}

/** Objects or plugs from the args (default the selection), expanded to scalar channels. */
std::vector<Target> targets(Graph& g, const Args& a, bool animatedOnly)
{
    std::vector<Target> out;
    auto add = [&](const PlugRef& p) {
        NodeId c = curveFor(g, p);
        if (animatedOnly && !c)
            return;
        for (const Target& t : out)
            if (t.plug == p)
                return;
        out.push_back({p, c});
    };
    auto expand = [&](const PlugRef& p) {
        if (p.component < 0 && g.attr(p).type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                add({p.node, p.attr, c});
        else
            add(p);
    };
    std::vector<std::string> attrs = attrList(a);
    for (const std::string& obj : a.objects())
    {
        if (obj.find('.') != std::string::npos)
        {
            expand(guarded([&] { return g.plug(obj); }));
            continue;
        }
        Node* n = g.find(obj);
        if (!n)
            throw CommandError("no node named '" + obj + "'");
        if (isAnyCurve(*n))
        {
            out.push_back({{}, n->id()});
            continue;
        }
        if (!attrs.empty())
        {
            for (const std::string& at : attrs)
                expand(guarded([&] { return g.plug(obj + "." + at); }));
            continue;
        }
        if (animatedOnly)
            for (const AnimatedPlug& ap : animatedPlugs(g, n->id()))
                add(ap.plug);
        else
            for (const PlugRef& p : keyablePlugs(g, n->id()))
                add(p);
    }
    return out;
}

/** time=t or time=(start, end), None for an open end. Inclusive, with a little slack for float frames. */
std::pair<double, double> timeRange(const Args& a)
{
    if (!a.has("time"))
        return {-kInf, kInf};
    const json& t = a.flags["time"];
    auto num = [](const json& v, double fallback) { return v.is_number() ? v.get<double>() : fallback; };
    if (t.is_number())
        return {t.get<double>() - kTimeEps, t.get<double>() + kTimeEps};
    if (t.is_array() && t.size() == 1)
        return {num(t[0], -kInf) - kTimeEps, num(t[0], kInf) + kTimeEps};
    if (t.is_array() && t.size() == 2)
        return {num(t[0], -kInf) - kTimeEps, num(t[1], kInf) + kTimeEps};
    throw CommandError("time must be a number or (start, end)");
}

Tangent tangentArg(const Args& a, const char* flag)
{
    return guarded([&] { return tangentFromName(a.get<std::string>(flag)); });
}

double degreesOf(double slope)
{
    return std::atan(slope) * 180.0 / 3.14159265358979323846;
}

/** Locks or breaks key i's handles. Locking a key whose sides differ fixes both to their average. */
void unify(Curve& c, size_t i, bool lock)
{
    Key& k = c.keys[i];
    k.broken = !lock;
    const double in = c.inSlope(i), out = c.outSlope(i);
    if (!lock || (k.in == k.out && std::abs(in - out) < 1e-9))
        return;
    k.in = k.out = Tangent::Fixed;
    k.inAngle = k.outAngle = degreesOf(0.5 * (in + out));
}

void unify(RotationCurve& rc, size_t i, bool lock)
{
    const glm::dvec3 in = glm::degrees(rc.inVelocity(i)), out = glm::degrees(rc.outVelocity(i));
    RotKey& k = rc.keys[i];
    k.broken = !lock;
    if (!lock || (k.in == k.out && glm::length(in - out) < 1e-9))
        return;
    k.in = k.out = Tangent::Fixed;
    k.inVelocity = k.outVelocity = 0.5 * (in + out);
}

int copyKeys(Graph& g, const Args& a)
{
    LILA_ZONE();

    auto [t0, t1] = timeRange(a);
    std::vector<ClipChannel> clip;
    double start = kInf;
    int count = 0;
    std::vector<NodeId> rotationsDone;
    for (const Target& t : targets(g, a, true))
    {
        if (isRotationCurve(*g.node(t.curve)))
        {
            if (std::find(rotationsDone.begin(), rotationsDone.end(), t.curve) != rotationsDone.end())
                continue;
            rotationsDone.push_back(t.curve);
            ClipChannel ch{t.plug.valid() ? g.attr(t.plug).name : g.node(t.curve)->name(), {}, {}, true};
            for (const RotKey& k : rotationCurveOf(*g.node(t.curve)).keys)
                if (k.time >= t0 && k.time <= t1)
                {
                    ch.rotKeys.push_back(k);
                    start = std::min(start, k.time);
                }
            count += int(ch.rotKeys.size());
            if (!ch.rotKeys.empty())
                clip.push_back(std::move(ch));
            continue;
        }
        ClipChannel ch{t.plug.valid() ? channelName(g, t.plug) : g.node(t.curve)->name(), {}};
        for (const Key& k : curveOf(*g.node(t.curve)).keys)
            if (k.time >= t0 && k.time <= t1)
            {
                ch.keys.push_back(k);
                start = std::min(start, k.time);
            }
        count += int(ch.keys.size());
        if (!ch.keys.empty())
            clip.push_back(std::move(ch));
    }
    if (clip.empty())
        return 0;
    for (ClipChannel& ch : clip)
    {
        for (Key& k : ch.keys)
            k.time -= start;
        for (RotKey& k : ch.rotKeys)
            k.time -= start;
    }
    clipboard() = std::move(clip);
    return count;
}

void notify()
{
    auto listeners = state().listeners;
    for (auto& [id, fn] : listeners)
        fn();
}
/** Marks or clears breakdown on a scalar or rotation curve's keys in [t0, t1]. Returns how many changed. */
int setBreakdown(Graph& g, NodeId curve, double t0, double t1, bool on)
{
    const Node* n = g.node(curve);
    if (!n)
        return 0;
    int changed = 0;
    auto mark = [&](auto& keys) {
        for (auto& k : keys)
            if (k.time >= t0 - kTimeEps && k.time <= t1 + kTimeEps && k.breakdown != on)
            {
                k.breakdown = on;
                ++changed;
            }
    };
    if (isRotationCurve(*n))
    {
        RotationCurve rc = rotationCurveOf(*n);
        mark(rc.keys);
        if (changed)
            setRotationCurve(g, curve, rc);
    }
    else if (isCurve(*n))
    {
        Curve c = curveOf(*n);
        mark(c.keys);
        if (changed)
            setCurve(g, curve, c);
    }
    return changed;
}
} // namespace

const char* curveTypeFor(const AttrDef& a)
{
    if (a.type == PlugType::Vec3 || a.type == PlugType::Float)
    {
        const std::string& n = a.name;
        if (n.starts_with("rotate") && n.find("Pivot") == std::string::npos)
            return "animCurveTA";
        if (n.find("translate") != std::string::npos || n.find("Translate") != std::string::npos)
            return "animCurveTL";
    }
    return "animCurveTU";
}

std::string channelName(Graph& g, const PlugRef& p)
{
    const AttrDef& a = g.attr(p);
    return p.component >= 0 ? a.childNames[p.component] : a.name;
}

json Playback::toJson() const
{
    return {{"minTime", minTime}, {"maxTime", maxTime}, {"animationStartTime", animStart}, {"animationEndTime", animEnd},
            {"fps", fps}, {"loop", loop}, {"playbackSpeed", std::string(speedName(speed))}, {"everyFrame", playsEveryFrame(speed)},
            {"speedMultiplier", customSpeed}};
}

void Playback::fromJson(const json& j)
{
    if (!j.is_object())
        return;
    minTime = j.value("minTime", minTime);
    maxTime = j.value("maxTime", maxTime);
    animStart = j.value("animationStartTime", animStart);
    animEnd = j.value("animationEndTime", animEnd);
    fps = std::max(1.0, j.value("fps", fps));
    loop = j.value("loop", loop);
    maxTime = std::max(maxTime, minTime);
    animStart = std::min(animStart, minTime);
    animEnd = std::max(animEnd, maxTime);
}

Playback& playback()
{
    return state().playback;
}

TimeSelection& timeSelection()
{
    static TimeSelection s;
    return s;
}

double timeSelectionWeight(const TimeSelection& s, double seconds)
{
    if (!s.active)
        return 0.0;
    const double frame = frameOf(seconds), from = frameOf(s.start), to = frameOf(s.end);
    const double outside = frame < from ? from - frame : frame > to ? frame - to : 0.0;
    if (outside <= 1e-9)
        return 1.0;
    if (s.falloff <= 0.0 || outside >= s.falloff + 1.0)
        return 0.0;
    // Smoothstep from the selection's edge to a frame past the falloff, so the last eased frame still moves a little.
    const double x = 1.0 - outside / (s.falloff + 1.0);
    return x * x * (3.0 - 2.0 * x);
}

void playbackChanged()
{
    notify();
}

int addPlaybackListener(std::function<void()> fn)
{
    State& s = state();
    s.listeners.emplace(s.nextListener, std::move(fn));
    return s.nextListener++;
}

void removePlaybackListener(int id)
{
    state().listeners.erase(id);
}

double frameOf(double seconds)
{
    return seconds * playback().fps;
}

double secondsOf(double frame)
{
    return frame / playback().fps;
}

double snapToFrame(double seconds)
{
    return secondsOf(std::round(frameOf(seconds)));
}

void setPlaybackDriver(PlaybackDriver driver)
{
    state().driver = std::move(driver);
}

void play(int direction)
{
    setHeldTime(Graph::current(), std::nullopt);
    State& s = state();
    const bool wasStopped = s.direction == 0;
    s.direction = direction >= 0 ? 1 : -1;
    if (s.driver.start)
        s.driver.start(s.direction);
    notify();
    if (wasStopped)
        events::send({.type = events::Event::PlaybackStarted});
}

void stop()
{
    State& s = state();
    if (s.direction == 0)
        return;
    s.direction = 0;
    if (s.driver.stop)
        s.driver.stop();
    notify();
    events::send({.type = events::Event::PlaybackStopped});
}

int playDirection()
{
    return state().direction;
}

bool autoKey()
{
    State& s = state();
    if (!s.autoKey)
        s.autoKey = PrefStore::get().value<bool>("anim", "/autoKey", false);
    return *s.autoKey;
}

void setAutoKey(bool on)
{
    state().autoKey = on;
    PrefStore::get().set("anim", "/autoKey", on);
    notify();
}

NodeId curveFor(Graph& g, const PlugRef& plug)
{
    PlugRef in = keyInput(g, plug);
    auto src = g.source(in);
    // A rotation curve drives the whole rotate and stands for each of its components.
    if (!src && in.component >= 0)
        if (auto whole = g.source({in.node, in.attr, -1}); whole && g.node(whole->node) && isRotationCurve(*g.node(whole->node)))
            src = whole;
    if (!src)
        return kNoNode;
    Node* n = g.node(src->node);
    return n && isAnyCurve(*n) ? n->id() : kNoNode;
}

std::vector<PlugRef> keyablePlugs(Graph& g, NodeId id)
{
    std::vector<PlugRef> out;
    Node* n = g.node(id);
    if (!n)
        return out;
    const auto& attrs = n->type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
    {
        const AttrDef& a = attrs[i];
        // The space channel shows (and keys) only on nodes that have spaces.
        const bool space = i == Transform::kSpace && n->type().isA("transform") && spaces::hasSpaces(g, id);
        if (!(a.flags & AttrKeyable) || ((a.flags & AttrHidden) && !space) || !(a.flags & AttrInput))
            continue;
        if (a.type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                out.push_back({id, i, c});
        else if (a.type == PlugType::Float || a.type == PlugType::Int || a.type == PlugType::Bool || a.type == PlugType::Enum)
            out.push_back({id, i, -1});
    }
    return out;
}

std::vector<AnimatedPlug> animatedPlugs(Graph& g, NodeId id)
{
    std::vector<AnimatedPlug> out;
    Node* n = g.node(id);
    if (!n)
        return out;
    for (int i = 0; i < int(n->type().attrs.size()); ++i)
        for (int c = -1; c < 3; ++c)
        {
            if (c >= 0 && n->type().attrs[i].type != PlugType::Vec3)
                break;
            PlugRef p{id, i, c};
            NodeId curve = curveFor(g, p);
            // A rotation curve is listed once per component, not for the whole plug.
            if (curve && !(c < 0 && n->type().attrs[i].type == PlugType::Vec3 && isRotationCurve(*g.node(curve))))
                out.push_back({p, curve});
        }
    return out;
}

bool hasKeyAt(const Node& curve, double t)
{
    for (double k : curveKeyTimes(curve))
        if (std::abs(k - t) <= 1e-6)
            return true;
    return false;
}

std::vector<double> keyTimes(Graph& g, const std::vector<NodeId>& nodes)
{
    LILA_ZONE();

    std::vector<double> out;
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        std::vector<NodeId> curves;
        if (isAnyCurve(*n))
            curves.push_back(id);
        for (const AnimatedPlug& ap : animatedPlugs(g, id))
            curves.push_back(ap.curve);
        for (NodeId c : curves)
            for (double t : curveKeyTimes(*g.node(c)))
                out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::abs(a - b) <= kTimeEps; }), out.end());
    return out;
}

NodeId setKey(Graph& g, const PlugRef& plug, double time, double value, std::optional<Tangent> in, std::optional<Tangent> out)
{
    LILA_ZONE();

    const AttrDef& a = g.attr(plug);
    if (plug.component < 0 && a.type == PlugType::Vec3)
        throw std::invalid_argument("key the components of " + g.plugName(plug));
    if (!(a.flags & AttrInput))
        throw std::invalid_argument(g.plugName(plug) + " is an output");

    if (auto rq = setLayeredRotationKey(g, plug, time, value, in, out))
        return *rq;
    if (keysAsRotation(g, plug))
    {
        // One component of a rotation key: the rest of the key keeps what is already there.
        PlugRef whole{plug.node, plug.attr, -1};
        NodeId rq = curveFor(g, plug);
        glm::dvec3 e = std::get<glm::dvec3>(g.value(whole));
        if (rq)
        {
            RotationCurve rc = rotationCurveOf(*g.node(rq));
            const bool now = std::abs(time - g.time()) <= kTimeEps;
            if (int k = rc.find(time); k >= 0 && !(now && g.isHeld(whole)))
                e = rc.keys[size_t(k)].euler;
            else if (!now)
                e = rc.evaluate(time);
        }
        e[plug.component] = value;
        return setRotationKey(g, whole, time, e, in, out);
    }

    // On a layered channel the key lands on the active layer's input, converted so the plug reads value.
    PlugRef keyed = plug;
    std::string layeredName;
    if (auto r = redirectKey(g, plug, value))
    {
        // An offset blend's inputB is its one control value: posing on its layer moves it at every frame together.
        if (const auto* bn = dynamic_cast<const BlendNode*>(g.node(r->plug.node)); bn && bn->staticInputB() && r->plug.attr == BlendNode::kInputB)
        {
            g.setValue(r->plug, r->value);
            return r->plug.node;
        }
        keyed = r->plug;
        value = r->value;
        layeredName = r->curveName;
    }
    NodeId curve = curveFor(g, keyed);
    Curve cv;
    if (!curve)
    {
        if (g.source(keyed) || (keyed.component >= 0 && g.source({keyed.node, keyed.attr, -1})))
            throw std::invalid_argument(g.plugName(plug) + " is driven by a connection");
        std::string name = layeredName.empty() ? g.node(plug.node)->name() + "_" + channelName(g, plug) : layeredName;
        std::replace(name.begin(), name.end(), ':', '_');
        curve = g.createNode(*types::find(curveTypeFor(a)), name, kNoNode, false)->id();
    }
    else
        cv = curveOf(*g.node(curve));

    bool fresh = cv.find(time) < 0;
    size_t i = cv.insert({.time = time, .value = value});
    if (fresh && stepped(a))
        cv.keys[i].out = Tangent::Step;
    if (in)
        cv.keys[i].in = *in;
    if (out)
        cv.keys[i].out = *out;
    setCurve(g, curve, cv);
    if (!g.source(keyed))
        g.connect({curve, CurveNode::kOutput, -1}, keyed);
    return curve;
}

int removeKeys(Graph& g, NodeId curve, double t0, double t1)
{
    LILA_ZONE();

    if (!g.node(curve))
        return 0;
    if (isRotationCurve(*g.node(curve)))
    {
        RotationCurve rc = rotationCurveOf(*g.node(curve));
        size_t before = rc.keys.size();
        std::erase_if(rc.keys, [&](const RotKey& k) { return k.time >= t0 && k.time <= t1; });
        int removed = int(before - rc.keys.size());
        if (removed == 0)
            return 0;
        if (rc.keys.empty())
            g.deleteNode(curve);
        else
            setRotationCurve(g, curve, rc);
        return removed;
    }
    Curve cv = curveOf(*g.node(curve));
    size_t before = cv.keys.size();
    std::erase_if(cv.keys, [&](const Key& k) { return k.time >= t0 && k.time <= t1; });
    int removed = int(before - cv.keys.size());
    if (removed == 0)
        return 0;
    if (cv.keys.empty())
        g.deleteNode(curve);
    else
        setCurve(g, curve, cv);
    return removed;
}

void registerCommands()
{
    using T = ArgType;
    registerTypes();
    registerKeyOpCommands();

    {
        State& s = state();
        s.playback.fps = std::max(1.0, PrefStore::get().value<double>("anim", "/fps", 30.0));
        s.playback.customSpeed = std::clamp(PrefStore::get().value<double>("anim", "/playbackSpeedMultiplier", 1.0), kMinCustomSpeed,
                                            kMaxCustomSpeed);
        std::string speed = PrefStore::get().value<std::string>("anim", "/playbackSpeed", "");
        if (auto named = speedFromName(speed))
            s.playback.speed = *named;
        else if (PrefStore::get().value<bool>("anim", "/everyFrame", false))
            s.playback.speed = PlaybackSpeed::EveryFrameMaxRealTime;
    }

    Graph::current().setDrivenSetHook([](Graph& g, const PlugRef& dst, const PlugRef& src, double v) {
        Node* s = g.node(src.node);
        if (!s || !(isCurve(*s) || isBlend(*s)))
            return Graph::DrivenEdit::Reject;
        // Interactive drags run with undo off; they hold, and the committing set keys.
        if (autoKey() && UndoStack::get().enabled())
        {
            setKey(g, dst, g.time(), v);
            return Graph::DrivenEdit::Handled;
        }
        return Graph::DrivenEdit::Hold;
    });

    registerRotationCommands();

    const FlagDef attrFlag{"attribute", "at", T::Any, nullptr, "Attribute name or list of names (translate keys all three)."};
    const FlagDef timeFlag{"time", "t", T::Any, nullptr, "Seconds, or (start, end) with None for an open end. Default: all keys."};

    ops::registerCommand({
        .name = "timeRange",
        .doc = "The time slider's selected range (Shift + drag on it), which edits across frames act on, such as Strings "
               "poses. Give start and end to select, clear to drop it, falloff for the frames edits ease out over either "
               "side. Returns {active, start, end, falloff}, times in seconds.",
        .flags = {
            {"start", "s", T::Float, nullptr, "Seconds."},
            {"end", "e", T::Float, nullptr, "Seconds."},
            {"falloff", "fo", T::Float, nullptr, "Frames edits ease out over either side."},
            {"clear", "c", T::Bool, false, "Drop the selection."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            TimeSelection& s = timeSelection();
            if (a.get<bool>("clear", false))
                s.active = false;
            if (a.has("start") || a.has("end"))
            {
                const double from = a.get<double>("start", a.get<double>("end", 0.0)), to = a.get<double>("end", from);
                s.start = std::min(from, to);
                s.end = std::max(from, to);
                s.active = true;
            }
            if (a.has("falloff"))
                s.falloff = std::max(0.0, a.get<double>("falloff"));
            playbackChanged();
            return {{"active", s.active}, {"start", s.start}, {"end", s.end}, {"falloff", s.falloff}};
        },
    });

    ops::registerCommand({
        .name = "setKeyframe",
        .doc = "Key channels at a time. With no objects, keys every keyable channel of the selection at the current "
               "time (the S key). Objects may be nodes, plugs ('cube.ty') or curve nodes. Returns the number of keys set.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"time", "t", T::Float, nullptr, "Seconds. Default: the current time."},
            {"value", "v", T::Float, nullptr, "Value. Default: the channel's current value."},
            {"inTangentType", "itt", T::String, nullptr, "auto | spline | linear | flat | step | clamped"},
            {"outTangentType", "ott", T::String, nullptr, "auto | spline | linear | flat | step | clamped"},
            {"breakdown", "bd", T::Bool, false, "Set breakdown keys."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            double time = a.get<double>("time", g.time());
            std::vector<NodeId> keyed;
            std::optional<Tangent> in, out;
            if (a.has("inTangentType"))
                in = tangentArg(a, "inTangentType");
            if (a.has("outTangentType"))
                out = tangentArg(a, "outTangentType");
            int count = 0;
            const std::vector<Target> ts = targets(g, a, false);
            // Rotation channels key as one rotation per rotate, read before any key releases a held value.
            std::vector<PlugRef> rotationsKeyed;
            for (const Target& t : ts)
            {
                if (!t.plug.valid() || !keysAsRotation(g, t.plug))
                    continue;
                PlugRef whole{t.plug.node, t.plug.attr, -1};
                if (std::find(rotationsKeyed.begin(), rotationsKeyed.end(), whole) != rotationsKeyed.end())
                    continue;
                rotationsKeyed.push_back(whole);
                g.evaluate();
                glm::dvec3 e = std::get<glm::dvec3>(g.value(whole));
                if (t.curve && std::abs(time - g.time()) > kTimeEps)
                    e = rotationCurveOf(*g.node(t.curve)).evaluate(time);
                for (const Target& o : ts)
                    if (o.plug.node == whole.node && o.plug.attr == whole.attr && o.plug.component >= 0)
                    {
                        if (a.has("value"))
                            e[o.plug.component] = a.get<double>("value");
                        ++count;
                    }
                keyed.push_back(guarded([&] { return setRotationKey(g, whole, time, e, in, out); }));
            }
            for (const Target& t : ts)
            {
                if (t.plug.valid() && std::find(rotationsKeyed.begin(), rotationsKeyed.end(), PlugRef{t.plug.node, t.plug.attr, -1}) !=
                                          rotationsKeyed.end())
                    continue;
                if (!t.plug.valid() && isRotationCurve(*g.node(t.curve)))
                {
                    RotationCurve rc = rotationCurveOf(*g.node(t.curve));
                    size_t i = rc.insert(rc.makeKey(time, rc.evaluate(time)));
                    if (in)
                        rc.keys[i].in = *in;
                    if (out)
                        rc.keys[i].out = *out;
                    setRotationCurve(g, t.curve, rc);
                    keyed.push_back(t.curve);
                    ++count;
                    continue;
                }
                if (!t.plug.valid())
                {
                    Curve cv = curveOf(*g.node(t.curve));
                    double v = a.has("value") ? a.get<double>("value") : cv.evaluate(time);
                    size_t i = cv.insert({.time = time, .value = v});
                    if (in)
                        cv.keys[i].in = *in;
                    if (out)
                        cv.keys[i].out = *out;
                    setCurve(g, t.curve, cv);
                    keyed.push_back(t.curve);
                    ++count;
                    continue;
                }
                double v;
                if (a.has("value"))
                    v = a.get<double>("value");
                else if (t.curve && std::abs(time - g.time()) > kTimeEps)
                    v = curveValueAt(*g.node(t.curve), t.plug.component, time);
                else
                    v = currentValue(g, t.plug);
                keyed.push_back(guarded([&] { return setKey(g, t.plug, time, v, in, out); }));
                ++count;
            }
            if (a.get<bool>("breakdown"))
                for (NodeId c : keyed)
                    setBreakdown(g, c, time, time, true);
            return count;
        },
    });

    ops::registerCommand({
        .name = "mute",
        .doc = "Mute animated channels: each holds its value at the current time until unmuted (disable=True), keys kept. "
               "query=True returns whether the first is muted.",
        .positionalDoc = "objects",
        .flags = {attrFlag,
                  {"disable", "d", T::Bool, false, "Unmute."},
                  {"query", "q", T::Bool, false, "Return whether the first channel is muted."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> curves;
            for (const Target& t : targets(g, a, true))
                if (t.curve && std::find(curves.begin(), curves.end(), t.curve) == curves.end())
                    curves.push_back(t.curve);
            if (a.get<bool>("query"))
                return !curves.empty() && g.node(curves[0])->as<bool>(CurveNode::kMute);
            const bool on = !a.get<bool>("disable");
            int count = 0;
            for (NodeId c : curves)
            {
                // Scalar and rotation curves number mute and muteTime alike.
                const int mute = g.node(c)->type().attrIndex("mute"), at = g.node(c)->type().attrIndex("muteTime");
                if (mute < 0 || g.node(c)->as<bool>(mute) == on)
                    continue;
                if (on)
                    g.setValue({c, at}, g.time());
                g.setValue({c, mute}, on);
                ++count;
            }
            return count;
        },
    });

    ops::registerCommand({
        .name = "breakdown",
        .doc = "Turn keys into breakdowns (or back, with value=False) on channels, at a time or in a range; all keys by default. "
               "query=True lists the breakdown times of the first channel. Returns how many keys changed.",
        .positionalDoc = "objects",
        .flags = {attrFlag, timeFlag,
                  {"value", "v", T::Bool, true, "True makes breakdowns, False plain keys."},
                  {"query", "q", T::Bool, false, "Return the first channel's breakdown key times."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto [t0, t1] = timeRange(a);
            std::vector<NodeId> curves;
            for (const Target& t : targets(g, a, true))
                if (t.curve && std::find(curves.begin(), curves.end(), t.curve) == curves.end())
                    curves.push_back(t.curve);
            if (a.get<bool>("query"))
            {
                json out = json::array();
                if (curves.empty())
                    return out;
                const Node& n = *g.node(curves[0]);
                auto list = [&](const auto& keys) {
                    for (const auto& k : keys)
                        if (k.breakdown)
                            out.push_back(k.time);
                };
                if (isRotationCurve(n))
                    list(rotationCurveOf(n).keys);
                else
                    list(curveOf(n).keys);
                return out;
            }
            int count = 0;
            for (NodeId c : curves)
                count += setBreakdown(g, c, t0, t1, a.get<bool>("value"));
            return count;
        },
    });

    ops::registerCommand({
        .name = "cutKey",
        .doc = "Remove keys (all, or those in time) and copy them for pasteKey. A curve left with no keys is deleted "
               "and the channel keeps its value. Returns the number removed.",
        .positionalDoc = "objects",
        .flags = {attrFlag, timeFlag, {"clear", "cl", T::Bool, false, "Don't copy to the clipboard."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto [t0, t1] = timeRange(a);
            std::vector<Target> ts = targets(g, a, true);
            if (!a.get<bool>("clear"))
                copyKeys(g, a);
            int removed = 0;
            for (const Target& t : ts)
                removed += removeKeys(g, t.curve, t0, t1);
            return removed;
        },
    });

    ops::registerCommand({
        .name = "copyKey",
        .doc = "Copy keys (all, or those in time) to the key clipboard. Returns the number copied.",
        .positionalDoc = "objects",
        .flags = {attrFlag, timeFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            return copyKeys(Graph::current(), a);
        },
    });

    ops::registerCommand({
        .name = "pasteKey",
        .doc = "Paste copied keys starting at time (default the current time). One copied channel pastes onto every "
               "target; several match by channel name. option: merge (replace keys at the same times) or replace "
               "(clear the pasted span first). Returns the number pasted.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"time", "t", T::Float, nullptr, "Seconds. Default: the current time."},
            {"option", "o", T::String, "merge", "merge | replace"},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const auto& clip = clipboard();
            if (clip.empty())
                throw CommandError("pasteKey: nothing copied");
            double at = a.get<double>("time", g.time());
            std::string option = a.get<std::string>("option", "merge");
            if (option != "merge" && option != "replace")
                throw CommandError("pasteKey: option is merge or replace");
            int count = 0;
            std::vector<PlugRef> rotationsDone;
            for (const Target& t : targets(g, a, false))
            {
                const ClipChannel* ch = nullptr;
                std::string name = t.plug.valid() ? channelName(g, t.plug) : g.node(t.curve)->name();
                for (const ClipChannel& c : clip)
                    if (clip.size() == 1 || c.attr == name || (c.rotation && t.plug.valid() && g.attr(t.plug).name == c.attr))
                        ch = &c;
                if (!ch)
                    continue;
                // Rotation keys onto a rotation channel, whole; onto Euler channels, as their Euler values.
                bool toRotation = t.plug.valid() ? keysAsRotation(g, t.plug) : isRotationCurve(*g.node(t.curve));
                if (toRotation)
                {
                    PlugRef whole{t.plug.node, t.plug.attr, -1};
                    if (t.plug.valid() && std::find(rotationsDone.begin(), rotationsDone.end(), whole) != rotationsDone.end() && ch->rotation)
                        continue;
                    if (t.plug.valid())
                        rotationsDone.push_back(whole);
                    NodeId curve = t.curve;
                    if (!curve && ch->rotation)
                        curve = guarded([&] { return setRotationKey(g, whole, at + ch->rotKeys[0].time, ch->rotKeys[0].euler); });
                    else if (!curve)
                        curve = guarded([&] { return setKey(g, t.plug, at + ch->keys[0].time, ch->keys[0].value); });
                    RotationCurve rc = rotationCurveOf(*g.node(curve));
                    if (ch->rotation)
                    {
                        double span = ch->rotKeys.back().time;
                        if (option == "replace")
                            std::erase_if(rc.keys, [&](const RotKey& k) { return k.time >= at - kTimeEps && k.time <= at + span + kTimeEps; });
                        for (RotKey k : ch->rotKeys)
                        {
                            k.time += at;
                            if (int i = rc.find(k.time); i >= 0)
                                rc.keys[size_t(i)] = k;
                            else
                                rc.insert(k);
                            ++count;
                        }
                    }
                    else
                    {
                        int axis = std::max(0, t.plug.component);
                        for (const Key& k : ch->keys)
                        {
                            double time = at + k.time;
                            int i = rc.find(time);
                            glm::dvec3 e = i >= 0 ? rc.keys[size_t(i)].euler : rc.evaluate(time);
                            e[axis] = k.value;
                            if (i >= 0)
                                rc.setEuler(size_t(i), e);
                            else
                            {
                                RotKey rk = rc.makeKey(time, e);
                                rk.in = k.in == Tangent::Fixed ? Tangent::Auto : k.in;
                                rk.out = k.out == Tangent::Fixed ? Tangent::Auto : k.out;
                                rc.insert(rk);
                            }
                            ++count;
                        }
                    }
                    setRotationCurve(g, curve, rc);
                    continue;
                }
                std::vector<Key> fromRotation;
                if (ch->rotation)
                {
                    if (!t.plug.valid() || t.plug.component < 0)
                        continue;
                    for (const RotKey& rk : ch->rotKeys)
                        fromRotation.push_back({.time = rk.time, .value = rk.euler[t.plug.component],
                                                .in = rk.in == Tangent::Fixed ? Tangent::Auto : rk.in,
                                                .out = rk.out == Tangent::Fixed ? Tangent::Auto : rk.out});
                }
                const std::vector<Key>& keys = ch->rotation ? fromRotation : ch->keys;
                NodeId curve = t.curve;
                if (!curve)
                    curve = guarded([&] { return setKey(g, t.plug, at + keys[0].time, keys[0].value); });
                Curve cv = curveOf(*g.node(curve));
                double span = keys.back().time;
                if (option == "replace")
                    std::erase_if(cv.keys, [&](const Key& k) { return k.time >= at - kTimeEps && k.time <= at + span + kTimeEps; });
                for (Key k : keys)
                {
                    k.time += at;
                    if (int i = cv.find(k.time); i >= 0)
                        cv.keys[size_t(i)] = k;
                    else
                        cv.insert(k);
                    ++count;
                }
                setCurve(g, curve, cv);
            }
            return count;
        },
    });

    ops::registerCommand({
        .name = "keyframe",
        .doc = "Query or edit keys. query=True returns key times (valueChange=True: values; keyCount=True: the "
               "count). Without query, moves the keys in time by timeChange and/or sets valueChange; relative=True "
               "offsets instead of setting.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            timeFlag,
            {"query", "q", T::Bool, false, ""},
            {"keyCount", "kc", T::Bool, false, "Query the number of keys."},
            {"valueChange", "vc", T::Any, nullptr, "Value to set (or offset), or True with query."},
            {"timeChange", "tc", T::Any, nullptr, "Time to set (or offset), or True with query."},
            {"relative", "r", T::Bool, false, "Offset instead of set."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto [t0, t1] = timeRange(a);
            std::vector<Target> ts = targets(g, a, true);
            if (a.get<bool>("query"))
            {
                bool values = a.has("valueChange") && a.flags["valueChange"] == true;
                json out = json::array();
                int count = 0;
                for (const Target& t : ts)
                {
                    if (isRotationCurve(*g.node(t.curve)))
                    {
                        for (const RotKey& k : rotationCurveOf(*g.node(t.curve)).keys)
                            if (k.time >= t0 && k.time <= t1)
                            {
                                ++count;
                                if (!values)
                                    out.push_back(k.time);
                                else if (t.plug.component >= 0)
                                    out.push_back(k.euler[t.plug.component]);
                                else
                                    out.push_back({k.euler.x, k.euler.y, k.euler.z});
                            }
                        continue;
                    }
                    for (const Key& k : curveOf(*g.node(t.curve)).keys)
                        if (k.time >= t0 && k.time <= t1)
                        {
                            ++count;
                            out.push_back(values ? k.value : k.time);
                        }
                }
                return a.get<bool>("keyCount") ? json(count) : out;
            }
            bool tc = a.has("timeChange"), vc = a.has("valueChange");
            if (!tc && !vc)
                throw CommandError("keyframe: give timeChange and/or valueChange, or query=True");
            if ((tc && !a.flags["timeChange"].is_number()) || (vc && !a.flags["valueChange"].is_number()))
                throw CommandError("keyframe: timeChange and valueChange take numbers");
            bool rel = a.get<bool>("relative");
            int count = 0;
            // A rotation key moves in time as a whole, once per curve, after every axis has had its value change.
            std::vector<NodeId> rotationsToMove;
            for (const Target& t : ts)
            {
                if (!isRotationCurve(*g.node(t.curve)))
                    continue;
                RotationCurve rc = rotationCurveOf(*g.node(t.curve));
                int changed = 0;
                for (size_t i = 0; i < rc.keys.size(); ++i)
                {
                    if (rc.keys[i].time < t0 || rc.keys[i].time > t1)
                        continue;
                    ++changed;
                    if (!vc)
                        continue;
                    glm::dvec3 e = rc.keys[i].euler;
                    double v = a.get<double>("valueChange");
                    for (int axis = 0; axis < 3; ++axis)
                        if (t.plug.component == axis || t.plug.component < 0)
                            e[axis] = rel ? e[axis] + v : v;
                    rc.setEuler(i, e);
                }
                if (vc && changed)
                    setRotationCurve(g, t.curve, rc);
                if (tc && std::find(rotationsToMove.begin(), rotationsToMove.end(), t.curve) == rotationsToMove.end())
                    rotationsToMove.push_back(t.curve);
                count += vc ? changed : 0;
            }
            for (NodeId c : rotationsToMove)
            {
                RotationCurve rc = rotationCurveOf(*g.node(c));
                std::vector<RotKey> moved, kept;
                for (const RotKey& k : rc.keys)
                {
                    if (k.time < t0 || k.time > t1)
                        kept.push_back(k);
                    else
                    {
                        RotKey m = k;
                        m.time = rel ? m.time + a.get<double>("timeChange") : a.get<double>("timeChange");
                        moved.push_back(m);
                    }
                }
                if (moved.empty())
                    continue;
                count += vc ? 0 : int(moved.size());
                rc.keys = kept;
                for (const RotKey& m : moved)
                {
                    if (int i = rc.find(m.time); i >= 0)
                        rc.keys[size_t(i)] = m;
                    else
                        rc.insert(m);
                }
                setRotationCurve(g, c, rc);
            }
            for (const Target& t : ts)
            {
                if (isRotationCurve(*g.node(t.curve)))
                    continue;
                Curve cv = curveOf(*g.node(t.curve));
                std::vector<Key> moved, kept;
                for (const Key& k : cv.keys)
                {
                    if (k.time < t0 || k.time > t1)
                    {
                        kept.push_back(k);
                        continue;
                    }
                    Key m = k;
                    if (tc)
                        m.time = rel ? m.time + a.get<double>("timeChange") : a.get<double>("timeChange");
                    if (vc)
                        m.value = rel ? m.value + a.get<double>("valueChange") : a.get<double>("valueChange");
                    moved.push_back(m);
                }
                if (moved.empty())
                    continue;
                count += int(moved.size());
                // Moved keys land on top of whatever was at their new time.
                cv.keys = kept;
                for (const Key& m : moved)
                {
                    if (int i = cv.find(m.time); i >= 0)
                        cv.keys[size_t(i)] = m;
                    else
                        cv.insert(m);
                }
                setCurve(g, t.curve, cv);
            }
            return count;
        },
    });

    ops::registerCommand({
        .name = "keyTangent",
        .doc = "Query or set tangents of keys (all, or those in time). Setting an angle makes that side fixed. "
               "query=True returns [{time, in, out, inAngle, outAngle, inWeight, outWeight}].",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            timeFlag,
            {"query", "q", T::Bool, false, ""},
            {"inTangentType", "itt", T::String, nullptr, "auto | spline | linear | flat | step | clamped | fixed"},
            {"outTangentType", "ott", T::String, nullptr, "auto | spline | linear | flat | step | clamped | fixed"},
            {"inAngle", "ia", T::Float, nullptr, "Degrees."},
            {"outAngle", "oa", T::Float, nullptr, "Degrees."},
            {"inWeight", "iw", T::Float, nullptr, "Handle length, 1 = a third of the segment.", {}, {}, 0.0, 3.0},
            {"outWeight", "ow", T::Float, nullptr, "Handle length, 1 = a third of the segment.", {}, {}, 0.0, 3.0},
            {"weightedTangents", "wt", T::Bool, nullptr, "Make the curves weighted or not."},
            {"lock", "l", T::Bool, nullptr, "True unifies each key's in and out handles (they move together), False breaks them."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto [t0, t1] = timeRange(a);
            std::vector<Target> ts = targets(g, a, true);
            if (a.get<bool>("query"))
            {
                json out = json::array();
                for (const Target& t : ts)
                {
                    if (isRotationCurve(*g.node(t.curve)))
                    {
                        // Angles are those of the Euler channel the target names, as the graph editor draws it.
                        RotationCurve rc = rotationCurveOf(*g.node(t.curve));
                        int axis = std::max(0, t.plug.component);
                        for (size_t i = 0; i < rc.keys.size(); ++i)
                        {
                            const RotKey& k = rc.keys[i];
                            if (k.time < t0 || k.time > t1)
                                continue;
                            out.push_back({{"time", k.time}, {"in", tangentName(k.in)}, {"out", tangentName(k.out)},
                                           {"inAngle", degreesOf(rc.eulerSlope(i, axis, false))},
                                           {"outAngle", degreesOf(rc.eulerSlope(i, axis, true))}, {"inWeight", k.inWeight},
                                           {"outWeight", k.outWeight}});
                        }
                        continue;
                    }
                    Curve cv = curveOf(*g.node(t.curve));
                    for (size_t i = 0; i < cv.keys.size(); ++i)
                    {
                        const Key& k = cv.keys[i];
                        if (k.time < t0 || k.time > t1)
                            continue;
                        out.push_back({{"time", k.time}, {"in", tangentName(k.in)}, {"out", tangentName(k.out)},
                                       {"inAngle", degreesOf(cv.inSlope(i))}, {"outAngle", degreesOf(cv.outSlope(i))},
                                       {"inWeight", k.inWeight}, {"outWeight", k.outWeight}});
                    }
                }
                return out;
            }
            std::optional<Tangent> in, out;
            if (a.has("inTangentType"))
                in = tangentArg(a, "inTangentType");
            if (a.has("outTangentType"))
                out = tangentArg(a, "outTangentType");
            int count = 0;
            for (const Target& t : ts)
            {
                if (!isRotationCurve(*g.node(t.curve)))
                    continue;
                RotationCurve rc = rotationCurveOf(*g.node(t.curve));
                const RotationCurve before = rc;
                int axis = std::max(0, t.plug.component);
                // The key-frame angular velocity giving these Euler rates (degrees per second) at the key.
                auto velocityFor = [&](size_t i, bool outSide, double angle) {
                    glm::dvec3 rates;
                    for (int c = 0; c < 3; ++c)
                        rates[c] = before.eulerSlope(i, c, outSide);
                    rates[axis] = std::tan(glm::radians(std::clamp(angle, -89.999, 89.999)));
                    constexpr double h = 1e-6;
                    const glm::dvec3& e = before.keys[i].euler;
                    glm::dquat q0 = eulerToQuat(e, rc.order), q1 = eulerToQuat(e + rates * h, rc.order);
                    return glm::degrees(quatLog(glm::inverse(q0) * q1) / h);
                };
                if (a.has("weightedTangents"))
                    rc.weighted = a.get<bool>("weightedTangents");
                for (size_t i = 0; i < rc.keys.size(); ++i)
                {
                    RotKey& k = rc.keys[i];
                    if (k.time < t0 || k.time > t1)
                        continue;
                    ++count;
                    if (in)
                    {
                        if (*in == Tangent::Fixed && k.in != Tangent::Fixed)
                            k.inVelocity = glm::degrees(before.inVelocity(i));
                        k.in = *in;
                    }
                    if (out)
                    {
                        if (*out == Tangent::Fixed && k.out != Tangent::Fixed)
                            k.outVelocity = glm::degrees(before.outVelocity(i));
                        k.out = *out;
                    }
                    if (a.has("inAngle"))
                    {
                        k.inVelocity = velocityFor(i, false, a.get<double>("inAngle"));
                        k.in = Tangent::Fixed;
                    }
                    if (a.has("outAngle"))
                    {
                        k.outVelocity = velocityFor(i, true, a.get<double>("outAngle"));
                        k.out = Tangent::Fixed;
                    }
                    if (a.has("inWeight"))
                        k.inWeight = std::clamp(a.get<double>("inWeight"), 0.0, 3.0);
                    if (a.has("outWeight"))
                        k.outWeight = std::clamp(a.get<double>("outWeight"), 0.0, 3.0);
                    if (a.has("lock"))
                        unify(rc, i, a.get<bool>("lock"));
                }
                setRotationCurve(g, t.curve, rc);
            }
            for (const Target& t : ts)
            {
                if (isRotationCurve(*g.node(t.curve)))
                    continue;
                Curve cv = curveOf(*g.node(t.curve));
                Curve before = cv;
                if (a.has("weightedTangents"))
                    cv.weighted = a.get<bool>("weightedTangents");
                for (size_t i = 0; i < cv.keys.size(); ++i)
                {
                    Key& k = cv.keys[i];
                    if (k.time < t0 || k.time > t1)
                        continue;
                    ++count;
                    // Fixing a side freezes the slope it has now.
                    if (in)
                    {
                        if (*in == Tangent::Fixed && k.in != Tangent::Fixed)
                            k.inAngle = degreesOf(before.inSlope(i));
                        k.in = *in;
                    }
                    if (out)
                    {
                        if (*out == Tangent::Fixed && k.out != Tangent::Fixed)
                            k.outAngle = degreesOf(before.outSlope(i));
                        k.out = *out;
                    }
                    if (a.has("inAngle"))
                    {
                        k.in = Tangent::Fixed;
                        k.inAngle = std::clamp(a.get<double>("inAngle"), -89.999, 89.999);
                    }
                    if (a.has("outAngle"))
                    {
                        k.out = Tangent::Fixed;
                        k.outAngle = std::clamp(a.get<double>("outAngle"), -89.999, 89.999);
                    }
                    if (a.has("inWeight"))
                        k.inWeight = std::clamp(a.get<double>("inWeight"), 0.0, 3.0);
                    if (a.has("outWeight"))
                        k.outWeight = std::clamp(a.get<double>("outWeight"), 0.0, 3.0);
                    if (a.has("lock"))
                        unify(cv, i, a.get<bool>("lock"));
                }
                setCurve(g, t.curve, cv);
            }
            return count;
        },
    });

    ops::registerCommand({
        .name = "setInfinity",
        .doc = "Curve behaviour outside its keys: constant, linear, cycle, cycleRelative (cycle with offset), "
               "oscillate. query=True returns [{curve, pre, post}].",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"preInfinite", "pri", T::String, nullptr, "Before the first key."},
            {"postInfinite", "poi", T::String, nullptr, "After the last key."},
            {"query", "q", T::Bool, false, ""},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            std::vector<NodeId> rotationsDone;
            for (const Target& t : targets(g, a, true))
            {
                if (isRotationCurve(*g.node(t.curve)))
                {
                    if (std::find(rotationsDone.begin(), rotationsDone.end(), t.curve) != rotationsDone.end())
                        continue;
                    rotationsDone.push_back(t.curve);
                    RotationCurve rc = rotationCurveOf(*g.node(t.curve));
                    if (a.get<bool>("query"))
                    {
                        out.push_back({{"curve", g.node(t.curve)->path()}, {"pre", infinityName(rc.pre)}, {"post", infinityName(rc.post)}});
                        continue;
                    }
                    if (a.has("preInfinite"))
                        rc.pre = guarded([&] { return infinityFromName(a.get<std::string>("preInfinite")); });
                    if (a.has("postInfinite"))
                        rc.post = guarded([&] { return infinityFromName(a.get<std::string>("postInfinite")); });
                    setRotationCurve(g, t.curve, rc);
                    continue;
                }
                Curve cv = curveOf(*g.node(t.curve));
                if (a.get<bool>("query"))
                {
                    out.push_back({{"curve", g.node(t.curve)->path()}, {"pre", infinityName(cv.pre)}, {"post", infinityName(cv.post)}});
                    continue;
                }
                if (a.has("preInfinite"))
                    cv.pre = guarded([&] { return infinityFromName(a.get<std::string>("preInfinite")); });
                if (a.has("postInfinite"))
                    cv.post = guarded([&] { return infinityFromName(a.get<std::string>("postInfinite")); });
                setCurve(g, t.curve, cv);
            }
            return a.get<bool>("query") ? out : json(nullptr);
        },
    });

    ops::registerCommand({
        .name = "findKeyframe",
        .doc = "Time of the next, previous, first or last key on the objects (default the selection), or None. "
               "next and previous wrap around.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"which", "w", T::String, "next", "next | previous | first | last"},
            {"time", "t", T::Float, nullptr, "Search from here. Default: the current time."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<double> times;
            for (const Target& t : targets(g, a, true))
                for (double k : curveKeyTimes(*g.node(t.curve)))
                    times.push_back(k);
            if (times.empty())
                return nullptr;
            std::sort(times.begin(), times.end());
            std::string which = a.get<std::string>("which", "next");
            double now = a.get<double>("time", g.time());
            if (which == "first")
                return times.front();
            if (which == "last")
                return times.back();
            if (which == "next")
            {
                for (double t : times)
                    if (t > now + kTimeEps)
                        return t;
                return times.front();
            }
            if (which == "previous")
            {
                for (auto it = times.rbegin(); it != times.rend(); ++it)
                    if (*it < now - kTimeEps)
                        return *it;
                return times.back();
            }
            throw CommandError("findKeyframe: which is next, previous, first or last");
        },
    });

    ops::registerCommand({
        .name = "cachedPlayback",
        .doc = "Keeps every evaluated frame in memory, so playing back over frames already seen restores them instead of "
               "recomputing. Any change to the scene drops the cache. fill evaluates the whole playback range now. Returns "
               "{enabled, frames, bytes}.",
        .flags = {
            {"state", "st", T::Bool, nullptr, "Turn cached playback on or off; remembered as a preference."},
            {"fill", "f", T::Bool, false, "Evaluate every frame of the playback range into the cache (turns it on)."},
            {"clear", "cl", T::Bool, false, "Drop every cached frame."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (!a.get<bool>("query"))
            {
                if (a.has("state") || a.get<bool>("fill"))
                {
                    const bool on = a.get<bool>("fill") || a.get<bool>("state");
                    g.setCachedPlayback(on);
                    PrefStore::get().set("general", kCachedPlaybackPref, on);
                }
                if (a.get<bool>("clear"))
                    g.clearPlaybackCache();
                if (a.get<bool>("fill"))
                {
                    const Playback& p = playback();
                    const int64_t first = int64_t(std::lround(frameOf(p.minTime))), last = int64_t(std::lround(frameOf(p.maxTime)));
                    const double was = g.time();
                    try
                    {
                        progress::Scope bar("Caching playback", double(last - first + 1), progress::Cancellable);
                        for (int64_t f = first; f <= last; ++f)
                        {
                            bar.checkCancel();
                            bar.setStatus("Frame " + std::to_string(f));
                            g.setTime(secondsOf(double(f)), false);
                            g.evaluate();
                            bar.step();
                        }
                    }
                    catch (const progress::Cancelled&)
                    {
                    }
                    g.setTime(was, false);
                    g.evaluate();
                    events::send({.type = events::Event::TimeChanged, .time = was});
                }
            }
            return {{"enabled", g.cachedPlayback()}, {"frames", g.playbackCacheTimes().size()}, {"bytes", g.playbackCacheBytes()}};
        },
    });

    ops::registerCommand({
        .name = "playbackOptions",
        .doc = "Query or set playback: ranges in seconds, fps (default 30), loop, playbackSpeed (realTime, half, twice, "
               "everyFrame, everyFrameMaxRealTime, custom), speedMultiplier for custom; speed is a user pref, not saved in "
               "the scene. Returns the settings.",
        .flags = {
            {"minTime", "min", T::Float, nullptr, "Playback start, seconds."},
            {"maxTime", "max", T::Float, nullptr, "Playback end, seconds."},
            {"animationStartTime", "ast", T::Float, nullptr, "Range slider start, seconds."},
            {"animationEndTime", "aet", T::Float, nullptr, "Range slider end, seconds."},
            {"fps", "fps", T::Float, nullptr, "Frames per second.", {}, {}, 1.0},
            {"loop", "l", T::Any, nullptr, "True/'continuous' to loop, False/'once' to stop at the end."},
            {"playbackSpeed", "ps", T::String, nullptr,
             "realTime (drops frames to keep pace), half, twice, everyFrame (as fast as it evaluates), everyFrameMaxRealTime or "
             "custom (real time times speedMultiplier)."},
            {"speedMultiplier", "sm", T::Float, nullptr,
             "Real-time multiplier of the custom speed, e.g. 0.25 or 1.5. Selects custom unless playbackSpeed is given.", {}, {}, kMinCustomSpeed, kMaxCustomSpeed},
            {"everyFrame", "ef", T::Bool, nullptr, "True plays every frame up to fps (everyFrameMaxRealTime), False real time."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Playback& p = playback();
            if (a.get<bool>("query"))
                return p.toJson();
            json j = p.toJson();
            for (const char* f : {"minTime", "maxTime", "animationStartTime", "animationEndTime", "fps"})
                if (a.has(f))
                    j[f] = a.flags[f];
            if (a.has("loop"))
            {
                const json& l = a.flags["loop"];
                if (l.is_boolean())
                    j["loop"] = l.get<bool>();
                else if (l.is_string() && (l == "continuous" || l == "once"))
                    j["loop"] = l == "continuous";
                else
                    throw CommandError("playbackOptions: loop is True/False or 'continuous'/'once'");
            }
            if (a.has("fps") && a.get<double>("fps") <= 0.0)
                throw CommandError("playbackOptions: fps must be positive");
            // Moving the inner range past the outer one drags the outer along, as the range slider does.
            if (a.has("minTime") && !a.has("animationStartTime"))
                j["animationStartTime"] = std::min(j["animationStartTime"].get<double>(), j["minTime"].get<double>());
            if (a.has("maxTime") && !a.has("animationEndTime"))
                j["animationEndTime"] = std::max(j["animationEndTime"].get<double>(), j["maxTime"].get<double>());
            std::optional<PlaybackSpeed> speed;
            if (a.has("playbackSpeed"))
            {
                speed = speedFromName(a.get<std::string>("playbackSpeed"));
                if (!speed)
                    throw CommandError(
                        "playbackOptions: playbackSpeed is realTime, half, twice, everyFrame, everyFrameMaxRealTime or custom");
            }
            else if (a.has("speedMultiplier"))
                speed = PlaybackSpeed::Custom;
            else if (a.has("everyFrame"))
                speed = a.get<bool>("everyFrame") ? PlaybackSpeed::EveryFrameMaxRealTime : PlaybackSpeed::RealTime;
            std::optional<double> multiplier;
            if (a.has("speedMultiplier"))
            {
                multiplier = a.get<double>("speedMultiplier");
                if (!(*multiplier >= kMinCustomSpeed && *multiplier <= kMaxCustomSpeed))
                    throw CommandError(std::format("playbackOptions: speedMultiplier is between {} and {}", kMinCustomSpeed, kMaxCustomSpeed));
            }
            p.fromJson(j);
            if (multiplier && *multiplier != p.customSpeed)
            {
                p.customSpeed = *multiplier;
                PrefStore::get().set("anim", "/playbackSpeedMultiplier", *multiplier);
            }
            if (speed && *speed != p.speed)
            {
                p.speed = *speed;
                PrefStore::get().set("anim", "/playbackSpeed", std::string(speedName(*speed)));
            }
            playbackChanged();
            return p.toJson();
        },
    });

    ops::registerCommand({
        .name = "play",
        .doc = "Start playback (forward=False plays backwards), or stop it with state=False. query=True returns "
               "whether it is playing.",
        .flags = {
            {"forward", "f", T::Bool, true, ""},
            {"state", "st", T::Bool, true, "False stops."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("query"))
                return playDirection() != 0;
            if (!a.get<bool>("state", true))
                stop();
            else
                play(a.get<bool>("forward", true) ? 1 : -1);
            return playDirection() != 0;
        },
    });

    ops::registerCommand({
        .name = "stop",
        .doc = "Stop playback.",
        .undoable = false,
        .run = [](const Args&) -> json {
            stop();
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "autoKeyframe",
        .doc = "Auto key: when on, changing an animated channel keys it at the current time. With no flags, queries.",
        .flags = {{"state", "st", T::Bool, nullptr, "On or off."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("state"))
                setAutoKey(a.get<bool>("state"));
            return autoKey();
        },
    });
}
} // namespace lila::scene::anim
