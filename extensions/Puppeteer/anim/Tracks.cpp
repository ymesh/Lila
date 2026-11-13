#include "Tracks.h"

#include "Common.h"
#include "Puppeteer.h"

#include <lila/Command.h>
#include <lila/Modes.h>
#include <lila/Node.h>
#include <lila/Relocate.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::anim
{
using T = ArgType;

namespace
{
namespace sa = lila::scene::anim;

enum TracksAttr
{
    kCharacter,
    kTarget,
    kEnabled,
    kOutWeight,
};

class TracksNode : public Node
{
public:
    void compute(const EvalContext&) override { setOutput(kOutWeight, as<bool>(kEnabled) ? 1.0 : 0.0); }
};

struct RouteState
{
    std::optional<NodeId> forced;
    bool explicitKey = false;
    int filling = 0;
};
RouteState& routeState()
{
    static RouteState s;
    return s;
}

const Graph* gCacheGraph = nullptr;
uint64_t gCacheStructure = ~uint64_t(0);
std::map<NodeId, NodeId> gTracksByNode;

/** Every character node to its controlTracks node, rebuilt when nodes come or go. */
const std::map<NodeId, NodeId>& tracksByNode(Graph& g)
{
    if (gCacheGraph == &g && gCacheStructure == g.structure())
        return gTracksByNode;
    gTracksByNode.clear();
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || n->type().name != kTracksType)
            continue;
        Node* root = named(g, n->as<std::string>(kCharacter));
        if (!root)
            continue;
        try
        {
            for (NodeId c : resolveCharacter(g, root->id()).nodes)
                gTracksByNode[c] = id;
        }
        catch (const std::exception&)
        {
        }
    }
    gCacheGraph = &g;
    gCacheStructure = g.structure();
    return gTracksByNode;
}

NodeId tracksNodeOf(Graph& g, NodeId node)
{
    const auto& m = tracksByNode(g);
    auto it = m.find(node);
    return it == m.end() || !g.node(it->second) ? kNoNode : it->second;
}

NodeId ensureTracksNode(Graph& g, const Character& ch)
{
    if (NodeId id = tracksNode(g, ch))
        return id;
    const NodeType* t = types::find(kTracksType);
    if (!t)
        throw std::invalid_argument("control tracks are unavailable");
    Node* n = g.createNode(*t, g.node(ch.root)->name() + "_tracks");
    g.setValue({n->id(), kCharacter, -1}, g.path(ch.root));
    return n->id();
}

bool isTransformNode(const Node& n) { return n.type().isA("transform"); }

KeyTarget targetOf(Graph& g, NodeId tracks)
{
    return tracks ? KeyTarget(g.node(tracks)->as<int64_t>(kTarget)) : KeyTarget::Character;
}

/**
 * The key router: keys on a blend or layer go straight to it; a Mixer layer the animator made active wins; otherwise
 * the key target decides, Control sending a control's edits to its track (made on the first edit).
 */
std::optional<sa::KeyRoute> route(Graph& g, const PlugRef& member, bool write)
{
    Node* n = g.node(member.node);
    if (!n)
        return std::nullopt;
    if (sa::isBlend(*n) || sa::isLayer(*n))
        return sa::KeyRoute{};
    if (routeState().forced)
        return sa::KeyRoute{*routeState().forced, false};
    if (!isTransformNode(*n))
        return std::nullopt;
    const NodeId active = sa::activeLayer(g);
    const bool activeTrack = active && isTrack(g, active);
    if (active && !activeTrack)
        return std::nullopt;
    const NodeId tracks = tracksNodeOf(g, member.node);
    if (targetOf(g, tracks) != KeyTarget::Control)
        return activeTrack ? std::optional<sa::KeyRoute>(sa::KeyRoute{}) : std::nullopt;
    NodeId track = trackOf(g, member.node);
    if (!track)
    {
        if (!write)
            return sa::KeyRoute{kNoNode, !routeState().explicitKey};
        track = ensureTrack(g, member.node);
    }
    return sa::KeyRoute{track, !routeState().explicitKey && trackCurves(g, track).empty()};
}

/** Auto key in Animate mode on the character keys keys the whole character at that time. */
void onAutoKey(Graph& g, const PlugRef& plug, double t)
{
    if (routeState().filling > 0 || routeState().forced || lila::ui::modes::current() != kAnimateMode)
        return;
    if (sa::keyLayer(g, plug) != kNoNode)
        return;
    try
    {
        const Character ch = resolveCharacter(g, plug.node);
        if (keyTarget(g, ch) == KeyTarget::Control)
            return;
        ++routeState().filling;
        fillCharacterKey(g, ch, t);
        --routeState().filling;
    }
    catch (const std::exception&)
    {
        routeState().filling = 0;
    }
}

const std::vector<int> kTrackAttrs = {Transform::kTranslate, Transform::kRotate};

glm::dvec3 vec(Graph& g, const PlugRef& member)
{
    const PlugValue v = g.value(member);
    if (std::holds_alternative<glm::dvec3>(v))
        return std::get<glm::dvec3>(v);
    return glm::dvec3(std::holds_alternative<double>(v) ? std::get<double>(v) : 0.0);
}
} // namespace

NodeId tracksNode(Graph& g, const Character& ch)
{
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().name == kTracksType && named(g, n->as<std::string>(kCharacter)) == g.node(ch.root))
            return id;
    return kNoNode;
}

KeyTarget keyTarget(Graph& g, const Character& ch) { return targetOf(g, tracksNode(g, ch)); }

void setKeyTarget(Graph& g, const Character& ch, KeyTarget t)
{
    if (keyTarget(g, ch) == t)
        return;
    g.setValue({ensureTracksNode(g, ch), kTarget, -1}, int64_t(t));
}

bool tracksEnabled(Graph& g, const Character& ch)
{
    const NodeId id = tracksNode(g, ch);
    return !id || g.node(id)->as<bool>(kEnabled);
}

void setTracksEnabled(Graph& g, const Character& ch, bool on)
{
    if (tracksEnabled(g, ch) != on)
        g.setValue({ensureTracksNode(g, ch), kEnabled, -1}, on);
}

bool isTrack(Graph& g, NodeId layer)
{
    Node* n = g.node(layer);
    if (!n || !sa::isLayer(*n))
        return false;
    auto src = g.source({layer, sa::LayerNode::kWeight, -1});
    return src && g.node(src->node) && g.node(src->node)->type().name == kTracksType;
}

NodeId trackControl(Graph& g, NodeId track)
{
    for (const PlugRef& m : sa::layerMembers(g, track))
        return m.node;
    return kNoNode;
}

NodeId trackOf(Graph& g, NodeId control)
{
    for (int attr : kTrackAttrs)
        for (NodeId b : sa::blendChain(g, {control, attr, -1}))
            if (NodeId l = sa::layerOf(g, b); l && isTrack(g, l))
                return l;
    return kNoNode;
}

NodeId ensureTrack(Graph& g, NodeId control)
{
    if (NodeId t = trackOf(g, control))
        return t;
    const Character ch = resolveCharacter(g, control);
    const NodeId tracks = ensureTracksNode(g, ch);
    const std::string name = poses::stripNamespace(g.node(control)->name()) + "_track";
    // A track whose blends are parked with another take is this control's track in that take: this take reuses it.
    NodeId layer = kNoNode;
    for (NodeId l : sa::layers(g))
        if (auto src = g.source({l, sa::LayerNode::kWeight, -1}); src && src->node == tracks && g.node(l)->name() == name &&
                                                                 sa::layerMembers(g, l).empty())
            layer = l;
    if (!layer)
    {
        layer = sa::createLayer(g, name, sa::LayerMode::Additive);
        g.connect({tracks, kOutWeight, -1}, {layer, sa::LayerNode::kWeight, -1});
    }
    for (int attr : kTrackAttrs)
        sa::addMember(g, layer, {control, attr, -1});
    return layer;
}

std::vector<NodeId> tracksOf(Graph& g, const Character& ch)
{
    const NodeId tracks = tracksNode(g, ch);
    std::vector<NodeId> out;
    if (!tracks)
        return out;
    // Tracks with no members here belong to another take.
    for (NodeId l : sa::layers(g))
        if (auto src = g.source({l, sa::LayerNode::kWeight, -1}); src && src->node == tracks && trackControl(g, l))
            out.push_back(l);
    return out;
}

std::vector<NodeId> trackCurves(Graph& g, NodeId track)
{
    std::vector<NodeId> out;
    for (const PlugRef& m : sa::layerMembers(g, track))
    {
        const NodeId b = sa::blendFor(g, m, track);
        if (!b)
            continue;
        for (int c = -1; c < 3; ++c)
            if (auto src = g.source({b, sa::BlendNode::kInputB, c}))
                if (Node* n = g.node(src->node); n && sa::isAnyCurve(*n) && std::find(out.begin(), out.end(), n->id()) == out.end())
                    out.push_back(n->id());
    }
    return out;
}

RouteScope::RouteScope(NodeId layer) : mPrevious(routeState().forced) { routeState().forced = layer; }
RouteScope::~RouteScope() { routeState().forced = mPrevious; }

ExplicitKeyScope::ExplicitKeyScope() : mPrevious(routeState().explicitKey) { routeState().explicitKey = true; }
ExplicitKeyScope::~ExplicitKeyScope() { routeState().explicitKey = mPrevious; }

int fillCharacterKey(Graph& g, const Character& ch, double t)
{
    int made = 0;
    for (NodeId c : characterCurves(g, ch, false))
    {
        Node* n = g.node(c);
        if (!n || sa::hasKeyAt(*n, t))
            continue;
        if (sa::isRotationCurve(*n))
        {
            sa::RotationCurve rc = sa::rotationCurveOf(*n);
            if (rc.keys.empty())
                continue;
            rc.insert(rc.makeKey(t, rc.evaluate(t)));
            sa::setRotationCurve(g, c, rc);
        }
        else
        {
            sa::Curve cv = sa::curveOf(*n);
            if (cv.keys.empty())
                continue;
            cv.insert({.time = t, .value = sa::curveValueAt(*n, 0, t)});
            sa::setCurve(g, c, cv);
        }
        ++made;
    }
    return made;
}

void deleteTrack(Graph& g, NodeId track)
{
    if (g.node(track)->as<bool>(sa::LayerNode::kSolo))
        ops::execute("editAnimLayer", {{"args", {g.node(track)->path()}}, {"solo", false}});
    for (const PlugRef& m : sa::layerMembers(g, track))
        sa::removeMember(g, track, m);
    // Blends of parked takes still hang off the layer.
    for (const PlugRef& d : g.destinations({track, sa::LayerNode::kOutWeight, -1}))
        if (g.node(d.node) && sa::isBlend(*g.node(d.node)))
            sa::removeBlend(g, d.node, true);
    g.deleteNode(track);
}

MergeResult mergeTrack(Graph& g, NodeId track)
{
    MergeResult res;
    const NodeId control = trackControl(g, track);
    if (!control)
    {
        deleteTrack(g, track);
        return res;
    }
    res.control = g.node(control)->path();
    const Character ch = resolveCharacter(g, control);
    const std::vector<PlugRef> members = sa::layerMembers(g, track);

    // The control's own character keys and the track's: an offset leaves the curve's shape, a keyed track adds its times.
    Character own = ch;
    own.nodes = {control};
    std::vector<double> times = keyTimesOf(g, characterCurves(g, own, false));
    for (double t : keyTimesOf(g, trackCurves(g, track)))
        times.push_back(t);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return sameTime(a, b); }), times.end());
    const double saved = g.time();
    if (times.empty())
        times.push_back(saved);

    const double fps = std::max(1.0, sa::playback().fps);
    std::vector<double> frames;
    for (long long f = std::llround(std::ceil(times.front() * fps - 1e-6)), last = std::llround(std::floor(times.back() * fps + 1e-6)); f <= last; ++f)
        frames.push_back(double(f) / fps);
    auto sample = [&](const std::vector<double>& at) {
        std::vector<std::vector<glm::dvec3>> out;
        for (double t : at)
        {
            g.setTime(t, false);
            g.evaluate();
            std::vector<glm::dvec3> row;
            for (const PlugRef& m : members)
                row.push_back(vec(g, m));
            out.push_back(std::move(row));
        }
        return out;
    };
    const auto before = sample(frames);
    const auto atKeys = sample(times);

    deleteTrack(g, track);
    {
        RouteScope base(kNoNode);
        auto keyAt = [&](double t, const std::vector<glm::dvec3>& pose) {
            for (size_t m = 0; m < members.size(); ++m)
                for (int c = 0; c < 3; ++c)
                    sa::setKey(g, {members[m].node, members[m].attr, c}, t, pose[m][c]);
        };
        for (size_t k = 0; k < times.size(); ++k)
            keyAt(times[k], atKeys[k]);

        const RotateOrder order = rotateOrderOf(g, control);
        std::vector<double> rotateError(frames.size(), 0.0), translateError(frames.size(), 0.0);
        std::vector<char> dirty(frames.size(), 1);
        std::vector<double> keyed = times;
        auto exceeds = [&](size_t f) { return rotateError[f] > kMergeRotateTolerance || translateError[f] > kMergeTranslateTolerance; };
        auto score = [&](size_t f) { return rotateError[f] / kMergeRotateTolerance + translateError[f] / kMergeTranslateTolerance; };
        for (;;)
        {
            for (size_t f = 0; f < frames.size(); ++f)
            {
                if (!dirty[f])
                    continue;
                dirty[f] = 0;
                g.setTime(frames[f], false);
                g.evaluate();
                rotateError[f] = translateError[f] = 0.0;
                for (size_t m = 0; m < members.size(); ++m)
                {
                    const glm::dvec3 now = vec(g, members[m]);
                    if (members[m].attr == Transform::kRotate)
                    {
                        const glm::dquat a = sa::eulerToQuat(before[f][m], order), b = sa::eulerToQuat(now, order);
                        const double dot = std::min(1.0, std::abs(glm::dot(a, b)));
                        rotateError[f] = std::max(rotateError[f], glm::degrees(2.0 * std::acos(dot)));
                    }
                    else
                        translateError[f] = std::max(translateError[f], glm::length(before[f][m] - now));
                }
            }
            // The worst frame of each span between keys that leaves the sum gets a key at the summed pose.
            std::map<size_t, size_t> worst;
            for (size_t f = 0; f < frames.size(); ++f)
            {
                if (!exceeds(f))
                    continue;
                const auto span = size_t(std::upper_bound(keyed.begin(), keyed.end(), frames[f]) - keyed.begin());
                auto [it, added] = worst.try_emplace(span, f);
                if (!added && score(f) > score(it->second))
                    it->second = f;
            }
            std::vector<double> adding;
            for (const auto& [span, f] : worst)
                if (std::ranges::none_of(keyed, [&](double k) { return sameTime(k, frames[f]); }))
                {
                    keyAt(frames[f], before[f]);
                    adding.push_back(frames[f]);
                }
            if (adding.empty())
                break;
            res.addedKeys += int(adding.size());
            keyed.insert(keyed.end(), adding.begin(), adding.end());
            std::sort(keyed.begin(), keyed.end());
            // A new key moves its neighbours' tangents, so the spans two keys either side need measuring again.
            for (double t : adding)
            {
                const auto i = std::lower_bound(keyed.begin(), keyed.end(), t) - keyed.begin();
                const double lo = i >= 2 ? keyed[size_t(i - 2)] : -1e300;
                const double hi = size_t(i + 2) < keyed.size() ? keyed[size_t(i + 2)] : 1e300;
                for (size_t f = 0; f < frames.size(); ++f)
                    if (frames[f] >= lo && frames[f] <= hi)
                        dirty[f] = 1;
            }
        }
        res.keys = int(keyed.size());
        for (size_t f = 0; f < frames.size(); ++f)
        {
            res.rotateError = std::max(res.rotateError, rotateError[f]);
            res.translateError = std::max(res.translateError, translateError[f]);
        }
    }
    g.setTime(saved, false);
    g.evaluate();
    return res;
}

namespace
{
/** A track named directly, or the track of a control. */
NodeId trackArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (isTrack(g, n->id()))
        return n->id();
    if (NodeId t = trackOf(g, n->id()))
        return t;
    throw CommandError(std::string(cmd) + ": " + n->path() + " has no control track");
}

std::vector<NodeId> trackArgs(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& o : objectArgs(a))
        if (NodeId t = trackArg(g, o, cmd); std::find(out.begin(), out.end(), t) == out.end())
            out.push_back(t);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select controls with tracks or name them");
    return out;
}

Character characterArg(Graph& g, const Args& a, const char* cmd)
{
    const std::vector<std::string> objs = objectArgs(a);
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": select a character or name a node in one");
    Node* n = g.find(objs.front());
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + objs.front() + "'");
    try
    {
        return resolveCharacter(g, n->id());
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

const char* targetName(KeyTarget t) { return t == KeyTarget::Control ? "control" : "character"; }

json trackJson(Graph& g, NodeId track)
{
    const Node& l = *g.node(track);
    const NodeId control = trackControl(g, track);
    const std::vector<double> keys = keyTimesOf(g, trackCurves(g, track));
    const double fps = std::max(1.0, sa::playback().fps);
    json frames = json::array();
    for (double t : keys)
        frames.push_back(t * fps);
    return {{"track", l.path()},
            {"control", control ? g.node(control)->path() : std::string()},
            {"offset", keys.empty()},
            {"keys", frames},
            {"mute", l.as<bool>(sa::LayerNode::kMute)},
            {"solo", l.as<bool>(sa::LayerNode::kSolo)}};
}

/** Joints of the character with a channel held by an unkeyed edit (posed, not keyed yet). */
std::vector<NodeId> heldNodes(Graph& g, const Character& ch)
{
    std::vector<NodeId> out;
    for (NodeId id : ch.nodes)
        for (const PlugRef& p : sa::keyablePlugs(g, id))
            if (g.isHeld(p) || g.isHeld({p.node, p.attr, -1}))
            {
                out.push_back(id);
                break;
            }
    return out;
}

json namesOf(Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        out.push_back(g.path(id));
    return out;
}
} // namespace

void registerTracks(ext::Context& ctx)
{
    NodeType t{.name = kTracksType,
               .doc = "Puppeteer's control tracks for one character: where posing and keys go, and the switch that turns "
                      "every track on or off.",
               .icon = "stack-2",
               .create = [] { return std::make_unique<TracksNode>(); }};
    const uint32_t stored = AttrInput | AttrStorable;
    t.ownAttrs = {
        {.name = "character", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "The character's root joint."},
        {.name = "target", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored, .enumNames = {"character", "control"},
         .doc = "character: posing keys the character keys. control: it goes to the posed control's track."},
        {.name = "enabled", .type = PlugType::Bool, .defaultValue = true, .flags = stored, .doc = "Every track on or off."},
        {.name = "outWeight", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrOutput | AttrHidden, .doc = "Drives the tracks' weight."},
    };
    lila::scene::relocate::registerNeutral(t.name);
    types::registerType(ctx, std::move(t));

    sa::setKeyRouter(route);
    sa::setAutoKeyListener(onAutoKey);
    ctx.track("puppeteer:keyRouter", [] {
        sa::setKeyRouter({});
        sa::setAutoKeyListener({});
        gCacheGraph = nullptr;
        gTracksByNode.clear();
    });

    ctx.registerCommand({
        .name = "keyTarget",
        .doc = "Where a character's posing and keys go: character (the character keys; tracks stay on top) or control "
               "(the posed control's own track, made on the first edit). Returns the target.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"target", "tg", T::String, nullptr, "character | control", {"character", "control"}},
                  {"toggle", "tgl", T::Bool, false, "Switch to the other target."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Character ch = characterArg(g, a, "keyTarget");
            KeyTarget want = keyTarget(g, ch);
            if (a.get<bool>("toggle", false))
                want = want == KeyTarget::Control ? KeyTarget::Character : KeyTarget::Control;
            else if (a.has("target"))
                want = a.get<std::string>("target") == "control" ? KeyTarget::Control : KeyTarget::Character;
            setKeyTarget(g, ch, want);
            return targetName(want);
        },
    });

    ctx.registerCommand({
        .name = "characterKey",
        .doc = "Set Key for a character. Key target character: every control the character animates is keyed (plus the "
               "named or selected controls, and every joint's rotate on a first key), so one key holds the whole pose. "
               "Key target control: the controls' tracks are keyed, an offset becoming a keyed track. Returns the target "
               "and the channel keys set.",
        .positionalDoc = "controls (default the selection)",
        .flags = {{"time", "t", T::Float, nullptr, "Seconds. Default: the current time."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Character ch = characterArg(g, a, "characterKey");
            const double t = a.get<double>("time", g.time());
            std::vector<NodeId> controls;
            for (const std::string& o : objectArgs(a))
                if (Node* n = g.find(o); n && isTransformNode(*n))
                    controls.push_back(n->id());
            json call = {{"time", t}};
            if (keyTarget(g, ch) == KeyTarget::Control)
            {
                ExplicitKeyScope keying;
                call["args"] = namesOf(g, controls);
                call["attribute"] = json::array({"translate", "rotate"});
                const json n = ops::execute("setKeyframe", call);
                return {{"target", "control"}, {"keys", n}};
            }
            const bool first = keyTimesOf(g, characterCurves(g, ch, false)).empty();
            std::vector<NodeId> nodes = controls;
            for (NodeId id : heldNodes(g, ch))
                if (std::find(nodes.begin(), nodes.end(), id) == nodes.end())
                    nodes.push_back(id);
            int keys = 0;
            if (!nodes.empty())
            {
                call["args"] = namesOf(g, nodes);
                keys += ops::execute("setKeyframe", call).get<int>();
            }
            if (first)
            {
                std::vector<NodeId> joints;
                for (NodeId id : ch.nodes)
                    if (Node* n = g.node(id); n && rig::isJoint(*n) && std::find(nodes.begin(), nodes.end(), id) == nodes.end())
                        joints.push_back(id);
                if (!joints.empty())
                    keys += ops::execute("setKeyframe", {{"args", namesOf(g, joints)}, {"attribute", "rotate"}, {"time", t}}).get<int>();
                if (std::find(nodes.begin(), nodes.end(), ch.root) == nodes.end())
                    keys += ops::execute("setKeyframe", {{"args", {g.path(ch.root)}}, {"attribute", "translate"}, {"time", t}}).get<int>();
            }
            keys += fillCharacterKey(g, ch, t);
            return {{"target", "character"}, {"keys", keys}};
        },
    });

    ctx.registerCommand({
        .name = "controlTrack",
        .doc = "Give controls a track of their own: an offset (one value over the whole take) until keyed. Returns the tracks.",
        .positionalDoc = "controls (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (const std::string& o : objectArgs(a))
            {
                Node* n = g.find(o);
                if (!n || !isTransformNode(*n))
                    throw CommandError("controlTrack: " + o + " is not a control");
                const NodeId t = [&] {
                    try
                    {
                        return ensureTrack(g, n->id());
                    }
                    catch (const std::invalid_argument& e)
                    {
                        throw CommandError(std::string("controlTrack: ") + e.what());
                    }
                }();
                out.push_back(g.node(t)->path());
            }
            if (out.empty())
                throw CommandError("controlTrack: select the controls");
            return out;
        },
    });

    ctx.registerCommand({
        .name = "listControlTracks",
        .doc = "A character's key target, whether its tracks are on, and each track: its control, whether it is an "
               "offset, its key frames, mute and solo.",
        .positionalDoc = "a node in the character (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Character ch = characterArg(g, a, "listControlTracks");
            json tracks = json::array();
            for (NodeId l : tracksOf(g, ch))
                tracks.push_back(trackJson(g, l));
            return {{"target", targetName(keyTarget(g, ch))}, {"enabled", tracksEnabled(g, ch)}, {"tracks", tracks}};
        },
    });

    ctx.registerCommand({
        .name = "editControlTrack",
        .doc = "Mute or solo control tracks (named, or the tracks of the named or selected controls). Returns them.",
        .positionalDoc = "tracks or controls (default the selection)",
        .flags = {{"mute", "mu", T::Bool, nullptr, "Turn the track off; the character keys stay."},
                  {"solo", "so", T::Bool, nullptr, "Mute every other layer."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId t : trackArgs(g, a, "editControlTrack"))
            {
                json call = {{"args", {g.node(t)->path()}}};
                for (const char* f : {"mute", "solo"})
                    if (a.has(f))
                        call[f] = a.get<bool>(f);
                ops::execute("editAnimLayer", call);
                out.push_back(trackJson(g, t));
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "controlTracksOn",
        .doc = "Turn every control track of a character off or on, without changing the character keys or each "
               "track's own mute. Returns the state.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"state", "st", T::Bool, nullptr, "On or off. Default: toggle."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const Character ch = characterArg(g, a, "controlTracksOn");
            const bool on = a.has("state") ? a.get<bool>("state") : !tracksEnabled(g, ch);
            setTracksEnabled(g, ch, on);
            return on;
        },
    });

    ctx.registerCommand({
        .name = "mergeControlTrack",
        .doc = "Merge control tracks into the character keys: each control is keyed at its own keys and its track's "
               "keys with the summed pose, plus keys wherever the curve would leave the sum between them, and the track "
               "goes. Returns per track the keys, the keys added between, and the largest difference left from the "
               "layered result on any frame (degrees, units).",
        .positionalDoc = "tracks or controls (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId t : trackArgs(g, a, "mergeControlTrack"))
            {
                const MergeResult r = mergeTrack(g, t);
                if (r.rotateError > kMergeRotateTolerance || r.translateError > kMergeTranslateTolerance)
                    log::write(LogLevel::Warning, "mergeControlTrack: " + r.control + " differs from its layered motion by up to " +
                                                      std::to_string(r.rotateError) + " degrees and " + std::to_string(r.translateError) +
                                                      " units with every frame keyed");
                out.push_back({{"control", r.control},
                               {"keys", r.keys},
                               {"addedKeys", r.addedKeys},
                               {"rotateError", r.rotateError},
                               {"translateError", r.translateError}});
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "deleteControlTrack",
        .doc = "Delete control tracks; the character keys underneath stay as they are. Returns how many went.",
        .positionalDoc = "tracks or controls (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<NodeId> ts = trackArgs(g, a, "deleteControlTrack");
            for (NodeId t : ts)
                deleteTrack(g, t);
            return ts.size();
        },
    });
}
} // namespace puppeteer::anim
