// Puppeteer's animation commands: the Stacker's key operations, Cycle Maker, the walk generator, and the Anim Bank's
// take operations that the base take commands don't cover. The panels in AnimPanels.cpp only call these.

#include "AnimCommands.h"
#include "AnimOps.h"
#include "Tracks.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/Relocate.h>

#include <cmath>
#include <fstream>
#include <limits>
#include <memory>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::anim
{
using T = ArgType;

namespace
{
namespace sa = lila::scene::anim;

Graph& graph() { return Graph::current(); }
double fps() { return std::max(1.0, sa::playback().fps); }

template <class F> auto guarded(const char* cmd, F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** The character a command acts on and its keys (the character keys, or one control track's), read once. */
struct Stack
{
    Character ch;
    /** The control track worked on; kNoNode for the character keys. */
    NodeId track = kNoNode;
    /** The curves whose keys are the rows. */
    std::vector<NodeId> curves;
    /** The curves a retime moves: a track's own, or the character keys with every track following. */
    std::vector<NodeId> warp;
    std::shared_ptr<RouteScope> route;
    std::vector<double> keys;
    double loop = std::numeric_limits<double>::quiet_NaN();

    bool looping() const { return !keys.empty() && !std::isnan(loop) && sameTime(keys.back(), loop); }
    bool isLoopKey(size_t i) const { return looping() && i + 1 == keys.size(); }
    /** Keys before the loop key. */
    size_t cycleKeys() const { return looping() ? keys.size() - 1 : keys.size(); }
};

Stack stackOf(Graph& g, NodeId node, const char* cmd, NodeId track = kNoNode)
{
    Stack s;
    s.ch = guarded(cmd, [&] { return resolveCharacter(g, node); });
    s.track = track;
    s.curves = track ? trackCurves(g, track) : characterCurves(g, s.ch, false);
    s.warp = track ? s.curves : characterCurves(g, s.ch, true);
    s.keys = keyTimesOf(g, s.curves);
    s.loop = loopTimeOf(g, s.curves);
    return s;
}

Stack stackArg(const Args& a, const char* cmd)
{
    Graph& g = graph();
    const std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": select a character or name a node in one");
    Node* n = g.find(objs.front());
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + objs.front() + "'");
    NodeId track = kNoNode;
    if (a.has("track"))
    {
        Node* t = g.find(a.get<std::string>("track"));
        if (!t)
            throw CommandError(std::string(cmd) + ": no node named '" + a.get<std::string>("track") + "'");
        track = isTrack(g, t->id()) ? t->id() : trackOf(g, t->id());
        if (!track)
            throw CommandError(std::string(cmd) + ": " + t->path() + " has no control track");
    }
    Stack s = stackOf(g, n->id(), cmd, track);
    // Reads and writes stay on the keys this stack lists, whatever the key target says.
    const NodeId active = sa::activeLayer(g);
    s.route = std::make_shared<RouteScope>(track ? track : active && !isTrack(g, active) ? active : kNoNode);
    return s;
}

size_t indexOf(const json& j, const Stack& s, const char* cmd)
{
    if (!j.is_number_integer() || j.get<int64_t>() < 0 || size_t(j.get<int64_t>()) >= s.keys.size())
        throw CommandError(std::string(cmd) + ": no key " + j.dump() + " (the character has " + std::to_string(s.keys.size()) + " keys)");
    return size_t(j.get<int64_t>());
}

/** The "keys" flag: one index or a list, sorted and unique. Empty when required is false and it isn't given. */
std::vector<size_t> keysArg(const Args& a, const Stack& s, const char* cmd, bool required = true)
{
    std::vector<size_t> out;
    if (!a.has("keys"))
    {
        if (required)
            throw CommandError(std::string(cmd) + ": give keys (key indices, 0 first)");
        return out;
    }
    const json& k = a.flags.at("keys");
    if (k.is_array())
        for (const json& e : k)
            out.push_back(indexOf(e, s, cmd));
    else
        out.push_back(indexOf(k, s, cmd));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

char axisArg(const Args& a)
{
    const std::string s = a.get<std::string>("axis", "x");
    return s.empty() ? 'x' : s[0];
}

/** Moves the playback end and the active take's end along with the last key when they sat on it. */
void followRange(Graph& g, const Stack& s, const TimeWarp& w)
{
    if (s.keys.empty() || s.track)
        return;
    const double oldEnd = s.keys.back(), newEnd = w.map(oldEnd);
    if (sameTime(oldEnd, newEnd))
        return;
    sa::Playback& pb = sa::playback();
    if (std::abs(pb.maxTime - oldEnd) < 1e-4)
    {
        pb.maxTime = newEnd;
        pb.animEnd = std::max(pb.animEnd, newEnd);
        sa::playbackChanged();
    }
    for (NodeId t : sa::activeTakes(g))
        if (auto src = g.source({t, sa::TakeNode::kRoot, -1}); src && src->node == s.ch.root &&
                                                               std::abs(g.node(t)->as<double>(sa::TakeNode::kEnd) - oldEnd) < 1e-4)
            g.setValue({t, sa::TakeNode::kEnd}, newEnd);
}

/** Retimes the stack's curves, and for the character keys their marks and the range too. */
void applyWarp(Graph& g, const Stack& s, const TimeWarp& w)
{
    warpCurves(g, s.warp, w);
    if (!s.track)
    {
        Marks m = marksOf(g, s.ch);
        m.warp(w);
        setMarks(g, s.ch, m);
    }
    followRange(g, s, w);
}

/** Marks of the stack: the character's, none for a track. */
Marks stackMarks(Graph& g, const Stack& s) { return s.track ? Marks{} : marksOf(g, s.ch); }

/** The nodes whose poses a stack reads and writes: the character, or a track's control. */
std::vector<NodeId> scopeOf(Graph& g, const Stack& s)
{
    if (!s.track)
        return s.ch.nodes;
    const NodeId c = trackControl(g, s.track);
    return c ? std::vector<NodeId>{c} : std::vector<NodeId>{};
}

/** Channels a stack writes: everything, or a track's members. */
std::vector<std::string> channelsOf(const Stack& s)
{
    return s.track ? std::vector<std::string>{"translate", "rotate"} : std::vector<std::string>{};
}

json stackJson(Graph& g, const Stack& s)
{
    const double f = fps();
    json rowsJson = json::array();
    for (const Row& r : rows(s.keys, s.loop, stackMarks(g, s)))
        rowsJson.push_back({{"index", r.index}, {"time", r.time}, {"frame", r.time * f}, {"gap", r.gap * f}, {"loop", r.loop},
                            {"mark", markName(r.mark)}});
    return {{"character", s.ch.id},
            {"root", g.node(s.ch.root)->path()},
            {"fps", f},
            {"total", totalTime(s.keys) * f},
            {"loop", s.looping()},
            {"track", s.track ? g.node(s.track)->path() : std::string()},
            {"rows", rowsJson}};
}

/** Copied keys: each pose and the seconds from the copied key before it. */
struct Clip
{
    std::string character;
    std::vector<std::pair<json, double>> keys;
    /** Copied from a control track: the poses are what the track adds. */
    bool track = false;
};
Clip& clipboard()
{
    static Clip c;
    return c;
}

/** Track keys add to a pose and character keys are one, so neither pastes onto the other. */
void checkClip(const Stack& s, const char* cmd)
{
    if (clipboard().track != (s.track != kNoNode))
        throw CommandError(std::string(cmd) + (clipboard().track ? ": the copied keys are a control track's; paste them onto a track"
                                                                 : ": the copied keys are character keys; paste them onto the character keys"));
}

const FlagDef kKeysFlag{"keys", "k", T::Any, nullptr, "Key index (0 first) or a list of them, as stackerKeys numbers them."};
const FlagDef kAxisFlag{"axis", "ax", T::String, "x", "Axis the mirror plane faces.", {"x", "y", "z"}};
const FlagDef kTrackFlag{"track", "tr", T::String, nullptr, "A control (or its track): work on its control track's keys instead of the character keys."};

void registerStackerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "stackerKeys",
        .doc = "A character's keys as the Stacker lists them: every key across its joints and layers, in order, with its "
               "frame, the gap to the next key (frames), its breakdown/extreme mark, and which is the loop key.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kTrackFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            return stackJson(g, stackArg(a, "stackerKeys"));
        },
    });

    ctx.registerCommand({
        .name = "stackerSetGap",
        .doc = "Set the gap (frames) from a key to the next: the key's span stretches to fit and every later key moves "
               "with the difference. Returns the keys.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"index", "i", T::Int, nullptr, "The key whose gap changes."}, {"gap", "g", T::Float, nullptr, "New gap, frames."}, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerSetGap");
            if (!a.has("index") || !a.has("gap"))
                throw CommandError("stackerSetGap: give index and gap");
            const size_t i = indexOf(a.flags.at("index"), s, "stackerSetGap");
            applyWarp(g, s, guarded("stackerSetGap", [&] { return retimeGap(s.keys, i, a.get<double>("gap") / fps()); }));
            return stackJson(g, stackOf(g, s.ch.root, "stackerSetGap", s.track));
        },
    });

    ctx.registerCommand({
        .name = "stackerNudgeGap",
        .doc = "Lengthen (by > 0) or shorten the gaps after keys by whole frames, retiming everything after; gaps stay at "
               "least a frame. Returns the keys.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag, {"by", "b", T::Float, 1.0, "Frames to add; negative shortens."}, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerNudgeGap");
            const std::vector<size_t> keys = keysArg(a, s, "stackerNudgeGap");
            const double f = fps();
            // Last first: an edit only moves keys after it, so earlier gaps keep their original key times.
            for (auto it = keys.rbegin(); it != keys.rend(); ++it)
            {
                if (*it + 1 >= s.keys.size())
                    continue;
                const double gap = std::max(1.0, std::round(gapAfter(s.keys, *it) * f) + a.get<double>("by", 1.0)) / f;
                const TimeWarp w = retimeGap(s.keys, *it, gap);
                applyWarp(g, s, w);
                s.keys = w.apply(s.keys);
            }
            return stackJson(g, stackOf(g, s.ch.root, "stackerNudgeGap", s.track));
        },
    });

    ctx.registerCommand({
        .name = "stackerCreateKey",
        .doc = "Make a key after a key, holding that key's pose, one gap later (the gap before it by default); keys after "
               "it move later by the same gap. Returns the new key's index.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"index", "i", T::Int, nullptr, "The key to follow. Default: the last key before the loop key."},
                  {"gap", "g", T::Float, nullptr, "Frames after it. Default: the gap before it."}, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerCreateKey");
            if (s.keys.empty())
                throw CommandError("stackerCreateKey: the character has no keys yet; set one first");
            const size_t i = a.has("index") ? indexOf(a.flags.at("index"), s, "stackerCreateKey") : s.cycleKeys() - 1;
            if (s.isLoopKey(i))
                throw CommandError("stackerCreateKey: the loop key ends the cycle; pick the key before it");
            const double gap = a.has("gap") ? a.get<double>("gap") / fps() : lastGap(s.keys, i, 10.0 / fps());
            if (!(gap > kTimeEps))
                throw CommandError("stackerCreateKey: the gap must be longer than zero");
            const json pose = poseAt(g, scopeOf(g, s), s.keys[i]);
            applyWarp(g, s, openAfter(s.keys, i, gap));
            KeyWriter w(g, scopeOf(g, s));
            w.write(pose, s.keys[i] + gap, channelsOf(s));
            w.commit();
            return i + 1;
        },
    });

    ctx.registerCommand({
        .name = "stackerDeleteKey",
        .doc = "Delete keys with their timing: every later key moves earlier by the deleted key's gap. The loop key "
               "can't be deleted (turn the loop off first). Returns the keys.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerDeleteKey");
            const std::vector<size_t> keys = keysArg(a, s, "stackerDeleteKey");
            for (size_t i : keys)
                if (s.isLoopKey(i))
                    throw CommandError("stackerDeleteKey: key " + std::to_string(i) + " is the loop key; turn the loop off first");
            for (auto it = keys.rbegin(); it != keys.rend(); ++it)
            {
                removeKeysAt(g, s.curves, s.keys[*it]);
                if (!s.track)
                {
                    Marks m = marksOf(g, s.ch);
                    m.erase(s.keys[*it]);
                    setMarks(g, s.ch, m);
                }
                const TimeWarp w = deleteWithTiming(s.keys, *it);
                applyWarp(g, s, w);
                s.keys.erase(s.keys.begin() + long(*it));
                s.keys = w.apply(s.keys);
            }
            return stackJson(g, stackOf(g, s.ch.root, "stackerDeleteKey", s.track));
        },
    });

    ctx.registerCommand({
        .name = "stackerMarkKey",
        .doc = "Mark keys as breakdowns or extremes (or clear the mark); marks move with their keys.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag, {"mark", "m", T::String, "breakdown", "breakdown | extreme | none", {"breakdown", "extreme", "none"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerMarkKey");
            if (s.track)
                throw CommandError("stackerMarkKey: marks are for the character keys, not a control track");
            const Mark mark = guarded("stackerMarkKey", [&] { return markFromName(a.get<std::string>("mark", "breakdown")); });
            Marks m = marksOf(g, s.ch);
            const std::vector<size_t> keys = keysArg(a, s, "stackerMarkKey");
            for (size_t i : keys)
                m.set(s.keys[i], mark);
            setMarks(g, s.ch, m);
            return keys.size();
        },
    });

    ctx.registerCommand({
        .name = "stackerSelect",
        .doc = "Key indices for a quick select: all keys, the first half or the second half of the cycle (the loop key "
               "is never picked).",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"which", "w", T::String, "all", "all | firstHalf | secondHalf", {"all", "firstHalf", "secondHalf"}}, kTrackFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Stack s = stackArg(a, "stackerSelect");
            const std::string w = a.get<std::string>("which", "all");
            const QuickSelect q = w == "firstHalf" ? QuickSelect::FirstHalf : w == "secondHalf" ? QuickSelect::SecondHalf : QuickSelect::All;
            return quickSelect(s.keys.size(), s.looping(), q);
        },
    });

    ctx.registerCommand({
        .name = "stackerCopyKeys",
        .doc = "Copy keys' poses (and the gaps between them) for stackerPasteKeys and stackerInsertKeys. Returns how many.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag, kTrackFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerCopyKeys");
            Clip c{s.ch.id, {}, s.track != kNoNode};
            const std::vector<size_t> keys = keysArg(a, s, "stackerCopyKeys");
            for (size_t k = 0; k < keys.size(); ++k)
                c.keys.push_back({poseAt(g, scopeOf(g, s), s.keys[keys[k]]), k == 0 ? 0.0 : s.keys[keys[k]] - s.keys[keys[k - 1]]});
            clipboard() = std::move(c);
            return keys.size();
        },
    });

    ctx.registerCommand({
        .name = "stackerPasteKeys",
        .doc = "Paste copied poses onto keys, in order, repeating the copied keys when pasting onto more. Returns the "
               "channel keys set.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerPasteKeys");
            if (clipboard().keys.empty())
                throw CommandError("stackerPasteKeys: nothing copied; run stackerCopyKeys first");
            checkClip(s, "stackerPasteKeys");
            const std::vector<size_t> keys = keysArg(a, s, "stackerPasteKeys");
            KeyWriter w(g, scopeOf(g, s));
            for (size_t k = 0; k < keys.size(); ++k)
                w.write(clipboard().keys[k % clipboard().keys.size()].first, s.keys[keys[k]], channelsOf(s));
            return w.commit();
        },
    });

    ctx.registerCommand({
        .name = "stackerInsertKeys",
        .doc = "Insert the copied keys after a key, with the gaps they were copied with (the first one gap after it), "
               "moving later keys along. Returns the new keys' indices.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"index", "i", T::Int, nullptr, "The key to insert after. Default: the last key before the loop key."}, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerInsertKeys");
            const Clip& c = clipboard();
            if (c.keys.empty())
                throw CommandError("stackerInsertKeys: nothing copied; run stackerCopyKeys first");
            checkClip(s, "stackerInsertKeys");
            if (s.keys.empty())
                throw CommandError("stackerInsertKeys: the character has no keys yet");
            const size_t i = a.has("index") ? indexOf(a.flags.at("index"), s, "stackerInsertKeys") : s.cycleKeys() - 1;
            if (s.isLoopKey(i))
                throw CommandError("stackerInsertKeys: the loop key ends the cycle; pick the key before it");
            std::vector<double> at;
            double t = s.keys[i] + lastGap(s.keys, i, 10.0 / fps());
            for (size_t k = 0; k < c.keys.size(); ++k)
            {
                if (k > 0)
                    t += std::max(c.keys[k].second, 1.0 / fps());
                at.push_back(t);
            }
            // Room for the inserted keys plus the gap that followed key i.
            applyWarp(g, s, openAfter(s.keys, i, at.back() - s.keys[i]));
            KeyWriter w(g, scopeOf(g, s));
            json out = json::array();
            for (size_t k = 0; k < c.keys.size(); ++k)
            {
                w.write(c.keys[k].first, at[k], channelsOf(s));
                out.push_back(i + 1 + k);
            }
            w.commit();
            return out;
        },
    });

    ctx.registerCommand({
        .name = "stackerMirrorKeys",
        .doc = "Mirror the pose at keys: left and right swap, centre joints reflect across the mirror plane. On a "
               "control track the track's keys go, mirrored, to the opposite control's track. Returns the channel keys set.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag, kAxisFlag, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerMirrorKeys");
            const std::vector<size_t> keys = keysArg(a, s, "stackerMirrorKeys");
            std::vector<json> mirrored;
            for (size_t i : keys)
                mirrored.push_back(mirrorPose(g, s.ch, poseAt(g, scopeOf(g, s), s.keys[i]), axisArg(a)));
            std::vector<NodeId> scope = s.ch.nodes;
            std::unique_ptr<RouteScope> onto;
            if (s.track)
            {
                // Deltas mirror like poses (the same per-channel maths), onto the other side's own track.
                const NodeId control = trackControl(g, s.track);
                Node* other = nullptr;
                if (control)
                    for (NodeId id : s.ch.nodes)
                        if (poses::stripNamespace(g.node(id)->name()) == poses::mirrorNodeName(poses::stripNamespace(g.node(control)->name())))
                            other = g.node(id);
                if (!other)
                    throw CommandError("stackerMirrorKeys: " + (control ? g.node(control)->path() : std::string("the track")) + " has no opposite control");
                const NodeId to = other->id() == control ? s.track : guarded("stackerMirrorKeys", [&] { return ensureTrack(g, other->id()); });
                onto = std::make_unique<RouteScope>(to);
                scope = {other->id()};
            }
            KeyWriter w(g, scope);
            for (size_t k = 0; k < keys.size(); ++k)
                w.write(mirrored[k], s.keys[keys[k]], channelsOf(s));
            return w.commit();
        },
    });

    ctx.registerCommand({
        .name = "stackerResetKeys",
        .doc = "Reset keys to the character's bind or ref pose (any library pose), optionally only some channels. "
               "Animated channels are keyed; others just take the value. Returns the channel keys set.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kKeysFlag,
                  {"pose", "p", T::String, "bind", "Library pose: bind, ref or any other.", {}},
                  {"channels", "ch", T::List, nullptr, "Channels: translate, rotate, scale or attribute names. Default: all."}, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerResetKeys");
            if (s.track)
            {
                // A track resets to adding nothing.
                const NodeId control = trackControl(g, s.track);
                if (!control)
                    return 0;
                const json zero = {{poses::stripNamespace(g.node(control)->name()), {{"translate", {0, 0, 0}}, {"rotate", {0, 0, 0}}}}};
                KeyWriter w(g, scopeOf(g, s));
                for (size_t i : keysArg(a, s, "stackerResetKeys"))
                    w.write(zero, s.keys[i]);
                return w.commit();
            }
            const poses::Pose p = guarded("stackerResetKeys", [&] { return libraryPose(s.ch, a.get<std::string>("pose", "bind")); });
            std::vector<std::string> channels;
            if (a.has("channels"))
                for (const json& c : a.flags.at("channels"))
                    channels.push_back(c.get<std::string>());
            KeyWriter w(g, s.ch.nodes, {.absolute = true, .keyMissing = false});
            for (size_t i : keysArg(a, s, "stackerResetKeys"))
                w.write(p.nodes, s.keys[i], channels);
            return w.commit();
        },
    });

    ctx.registerCommand({
        .name = "stackerLoopKey",
        .doc = "Turn the character's loop key on (a key one gap after the last that mirrors the first, curves cycling) "
               "or off. Returns the loop key frame, or None when off.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {{"state", "st", T::Bool, nullptr, "On or off. Default: toggle."},
                  {"gap", "g", T::Float, nullptr, "Frames from the last key to a new loop key. Default: the last gap."}, kTrackFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "stackerLoopKey");
            const bool on = a.has("state") ? a.get<bool>("state") : !s.looping();
            if (!on)
            {
                std::vector<NodeId> looping;
                for (NodeId c : s.curves)
                    if (g.node(c) && sa::curveLoops(*g.node(c)))
                        looping.push_back(c);
                sa::setCurvesLoop(g, looping, false);
                return nullptr;
            }
            if (s.looping())
                return s.loop * fps();
            if (s.keys.empty())
                throw CommandError("stackerLoopKey: the character has no keys to loop");
            const double gap = a.has("gap") ? a.get<double>("gap") / fps() : lastGap(s.keys, s.keys.size() - 1, 10.0 / fps());
            const double t = sa::setCurvesLoop(g, s.curves, true, s.keys.back() + gap);
            if (std::isnan(t))
                return nullptr;
            if (s.track)
                return t * fps();
            sa::Playback& pb = sa::playback();
            pb.maxTime = std::max(t, pb.minTime);
            pb.animEnd = std::max(pb.animEnd, pb.maxTime);
            sa::playbackChanged();
            return t * fps();
        },
    });
}

void registerCycleCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "cycleStep",
        .doc = "Cycle Step: build the second step of a cycle from the first. The keys in the first half are mirrored "
               "(left and right swapped, centre reflected) half a cycle later, replacing any keys there. The cycle ends "
               "at the loop key, or end. Returns how many keys were made and replaced.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {kAxisFlag, {"end", "e", T::Float, nullptr, "Cycle end frame when the character has no loop key."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "cycleStep");
            if (s.keys.empty())
                throw CommandError("cycleStep: the character has no keys");
            double end = s.looping() ? s.loop : std::numeric_limits<double>::quiet_NaN();
            if (a.has("end"))
                end = a.get<double>("end") / fps();
            if (std::isnan(end))
                throw CommandError("cycleStep: turn the loop key on or give end");
            const StepPlan plan = guarded("cycleStep", [&] { return cycleStepPlan(s.keys, s.keys.front(), end); });
            std::vector<json> poses;
            for (const StepCopy& c : plan.copies)
                poses.push_back(mirrorPose(g, s.ch, poseAt(g, s.ch.nodes, c.from), axisArg(a)));
            Marks m = marksOf(g, s.ch);
            for (double t : plan.replaced)
            {
                removeKeysAt(g, s.curves, t);
                m.erase(t);
            }
            for (const StepCopy& c : plan.copies)
                m.set(c.to, m.get(c.from));
            setMarks(g, s.ch, m);
            KeyWriter w(g, s.ch.nodes);
            for (size_t k = 0; k < plan.copies.size(); ++k)
                w.write(poses[k], plan.copies[k].to);
            w.commit();
            return {{"made", plan.copies.size()}, {"replaced", plan.replaced.size()}};
        },
    });

    ctx.registerCommand({
        .name = "cycleSide",
        .doc = "Cycle Side: give the opposite side of the selected joints their motion half a cycle later, mirrored. Key k "
               "of the n keys before the loop key takes the pose of key (k + n/2) mod n, so n must be even. Returns the "
               "channel keys set.",
        .positionalDoc = "joints (default the selection)",
        .flags = {kAxisFlag},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("cycleSide: select the joints to mirror");
            std::vector<NodeId> joints;
            for (const std::string& o : objs)
            {
                Node* n = g.find(o);
                if (!n)
                    throw CommandError("cycleSide: no node named '" + o + "'");
                joints.push_back(n->id());
            }
            Stack s = stackOf(g, joints.front(), "cycleSide");
            if (!s.looping())
                throw CommandError("cycleSide: turn the loop key on first; the cycle ends at it");
            const size_t n = s.cycleKeys();
            const std::vector<size_t> src = guarded("cycleSide", [&] { return cycleSideMap(n); });
            std::vector<json> poses;
            for (size_t k = 0; k < n; ++k)
                poses.push_back(mirrorPose(g, s.ch, poseAt(g, joints, s.keys[src[k]]), axisArg(a)));
            KeyWriter w(g, s.ch.nodes);
            for (size_t k = 0; k < n; ++k)
                w.write(poses[k], s.keys[k]);
            return w.commit();
        },
    });

    ctx.registerCommand({
        .name = "walkCycle",
        .doc = "Generate a looping walk or run for a character whose joints carry semantic labels (from its rig "
               "template, or UE5-style names: pelvis, spine_01, thigh_l, calf_l, foot_l, ball_l, upperarm_l ...). Keys "
               "one cycle into a new take, in place, from frame 0 to the loop key. Returns the take, frames and joints.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {
            {"run", "r", T::Bool, false, "A run: shorter contact, a flight phase, the bounce inverted."},
            {"stride", "sl", T::Float, 50.0, "cm each foot travels under the body while planted.", {}, {}, 0.0},
            {"speed", "sp", T::Float, 0.0, "cm/s; above zero it sets the cycle length from the stride.", {}, {}, 0.0},
            {"cycleLength", "cl", T::Float, 32.0, "Frames per cycle (two steps) when speed is zero.", {}, {}, 4.0},
            {"bounce", "bo", T::Float, 3.0, "cm the pelvis rises and falls.", {}, {}, 0.0},
            {"stepHeight", "sh", T::Float, 10.0, "cm the swinging foot lifts.", {}, {}, 0.0},
            {"armSwing", "as", T::Float, 20.0, "Degrees each arm swings either way.", {}, {}, 0.0},
            {"lean", "le", T::Float, 4.0, "Degrees the spine leans forward."},
            {"sway", "sw", T::Float, 2.0, "cm the pelvis shifts over the planted foot.", {}, {}, 0.0},
            {"twist", "tw", T::Float, 6.0, "Degrees the pelvis turns with the legs.", {}, {}, 0.0},
            {"keysPerStep", "kps", T::Int, int64_t(4), "Keys per step.", {}, {}, 1.0, 64.0},
            {"name", "n", T::String, nullptr, "Take name. Default: Walk or Run."},
            {"restPose", "rp", T::String, nullptr, "Library pose to start from. Default: ref, else bind, else the current pose."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "walkCycle");
            WalkParams p;
            p.run = a.get<bool>("run", false);
            p.stride = a.get<double>("stride", 50.0);
            p.speed = a.get<double>("speed", 0.0);
            p.cycleFrames = a.get<double>("cycleLength", 32.0);
            p.bounce = a.get<double>("bounce", 3.0);
            p.stepHeight = a.get<double>("stepHeight", 10.0);
            p.armSwing = a.get<double>("armSwing", 20.0);
            p.lean = a.get<double>("lean", 4.0);
            p.sway = a.get<double>("sway", 2.0);
            p.twist = a.get<double>("twist", 6.0);
            p.keysPerStep = int(std::clamp<int64_t>(a.get<int64_t>("keysPerStep", 4), 1, 64));
            p.fps = fps();
            if (p.stride < 0.0 || p.speed < 0.0)
                throw CommandError("walkCycle: stride and speed can't be negative");
            const WalkResult r = guarded("walkCycle", [&] { return generateWalk(g, s.ch, p, a.get<std::string>("name"), a.get<std::string>("restPose")); });
            return {{"take", r.take}, {"frames", r.frames}, {"keys", r.keys}, {"joints", r.joints}};
        },
    });

    ctx.registerCommand({
        .name = "characterLabels",
        .doc = "The semantic labels of a character's joints (pelvis, thigh_l ...), from its rig template or by name, as "
               "{joint: label}.",
        .positionalDoc = "a node in the character (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Stack s = stackArg(a, "characterLabels");
            json out = json::object();
            for (const auto& [id, label] : jointLabels(g, s.ch))
                out[g.node(id)->path()] = label;
            return out;
        },
    });
}

NodeId takeArg(Graph& g, const Args& a, const char* cmd)
{
    if (a.positional.size() != 1 || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": name one take");
    Node* n = g.find(a.positional[0].get<std::string>());
    if (!n || !sa::isTake(*n))
        throw CommandError(std::string(cmd) + ": no take named '" + a.positional[0].get<std::string>() + "'");
    return n->id();
}

/** The part of a plug name after the node: "attr" or "attr.component" spelled as the graph spells it. */
std::string attrPart(Graph& g, const PlugRef& p)
{
    const std::string full = g.plugName(p);
    const size_t dot = full.find('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

void registerBankCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "bankDuplicateTake",
        .doc = "Copy a take as a new, inactive take named with the next free _NN suffix (Walk -> Walk_01). Returns its name.",
        .positionalDoc = "take",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId t = takeArg(g, a, "bankDuplicateTake");
            std::vector<std::string> names;
            for (NodeId o : sa::takes(g))
                names.push_back(g.node(o)->name());
            for (NodeId id : g.nodes())
                names.push_back(g.node(id)->name());
            const std::string name = numberedName(g.node(t)->name(), names);
            return g.node(sa::duplicateTake(g, t, name))->path();
        },
    });

    ctx.registerCommand({
        .name = "exportTake",
        .doc = "Write a take (its settings and curves) to a .lilatake JSON file. Layer blend chains are not written; "
               "merge layers first. Returns the file, the curves written and the channels skipped.",
        .positionalDoc = "take",
        .flags = {{"path", "p", T::String, nullptr, "File to write."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId t = takeArg(g, a, "exportTake");
            if (!a.has("path"))
                throw CommandError("exportTake: give path");
            const Node& n = *g.node(t);
            auto root = g.source({t, sa::TakeNode::kRoot, -1});
            json curves = json::array();
            int skipped = 0;
            for (const auto& [dst, src] : sa::takeConnections(g, t))
            {
                Node* c = g.node(src.node);
                Node* d = g.node(dst.node);
                if (!c || !d || !sa::isAnyCurve(*c))
                {
                    ++skipped;
                    continue;
                }
                json attrs = json::object();
                for (size_t i = 0; i < c->type().attrs.size(); ++i)
                {
                    const AttrDef& def = c->type().attrs[i];
                    if ((def.flags & AttrStorable) && !(def.flags & AttrOutput))
                        attrs[def.name] = toJson(c->value(int(i)));
                }
                curves.push_back({{"node", poses::stripNamespace(d->name())},
                                  {"plug", attrPart(g, dst)},
                                  {"output", attrPart(g, src)},
                                  {"type", c->type().name},
                                  {"attrs", attrs}});
            }
            const json file = {{"format", "lilatake"},
                               {"version", 1},
                               {"name", n.name()},
                               {"root", root ? poses::stripNamespace(g.node(root->node)->name()) : std::string()},
                               {"start", n.as<double>(sa::TakeNode::kStart)},
                               {"end", n.as<double>(sa::TakeNode::kEnd)},
                               {"fps", n.as<double>(sa::TakeNode::kFps)},
                               {"loop", n.as<bool>(sa::TakeNode::kLoop)},
                               {"folder", n.as<std::string>(sa::TakeNode::kFolder)},
                               {"curves", curves}};
            const std::string path = a.get<std::string>("path");
            std::ofstream out(path, std::ios::binary);
            if (!out)
                throw CommandError("exportTake: can't write " + path);
            out << file.dump(1);
            if (!out)
                throw CommandError("exportTake: writing " + path + " failed");
            return {{"path", path}, {"curves", curves.size()}, {"skipped", skipped}};
        },
    });

    ctx.registerCommand({
        .name = "importTake",
        .doc = "Read a .lilatake file as a new take on its character (matched by node names without namespace). The "
               "take it was active for stays active unless activate. Returns the new take's name.",
        .positionalDoc = "file",
        .flags = {{"name", "n", T::String, nullptr, "Take name. Default: the file's."},
                  nodeFlag({"root", "r", T::String, nullptr, "Character root to put it on. Default: the file's."}, "transform"),
                  {"activate", "a", T::Bool, false, "Switch to the new take."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("importTake: give the file");
            const std::string path = a.positional[0].get<std::string>();
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw CommandError("importTake: can't read " + path);
            const json file = json::parse(in, nullptr, false);
            if (!file.is_object() || file.value("format", std::string()) != "lilatake")
                throw CommandError("importTake: " + path + " is not a .lilatake file");
            const std::string rootName = a.get<std::string>("root", file.value("root", std::string()));
            Node* root = rootName.empty() ? nullptr : g.find(rootName);
            if (!rootName.empty() && !root)
                throw CommandError("importTake: no character root named '" + rootName + "'");

            std::map<std::string, NodeId> byName;
            std::vector<NodeId> scope;
            if (root)
            {
                const Character ch = guarded("importTake", [&] { return resolveCharacter(g, root->id()); });
                adoptIntoTake(g, ch);
                scope = ch.nodes;
            }
            else
                scope = g.nodes();
            for (NodeId id : scope)
                byName.emplace(poses::stripNamespace(g.node(id)->name()), id);

            NodeId before = kNoNode;
            for (NodeId t : sa::activeTakes(g))
                if (auto src = g.source({t, sa::TakeNode::kRoot, -1}); (src ? src->node : kNoNode) == (root ? root->id() : kNoNode))
                    before = t;
            sa::TakeSettings s{root ? root->path() : std::string(), file.value("start", 0.0), file.value("end", 4.0),
                               file.value("fps", sa::playback().fps), file.value("loop", false)};
            const NodeId t = guarded("importTake", [&] { return sa::createTake(g, a.get<std::string>("name", file.value("name", std::string("Take1"))), s); });
            g.setValue({t, sa::TakeNode::kFolder}, normalFolder(file.value("folder", std::string())));
            sa::setActiveTake(g, t);
            int missing = 0;
            std::string firstError;
            for (const json& c : file.value("curves", json::array()))
            {
                auto node = byName.find(c.value("node", std::string()));
                const NodeType* type = types::find(c.value("type", std::string()));
                if (node == byName.end() || !type)
                {
                    if (missing++ == 0)
                        firstError = node == byName.end() ? "no node " + c.value("node", std::string()) : "no curve type " + c.value("type", std::string());
                    continue;
                }
                const std::string nodeName = g.node(node->second)->name();
                PlugRef dst, out;
                Node* curve = nullptr;
                try
                {
                    dst = g.plug(g.path(node->second) + "." + c.value("plug", std::string()));
                    curve = g.createNode(*type, nodeName + "_" + c.value("plug", std::string()), kNoNode, false);
                    // Keys last: they are read in the curve's rotate order, which has to be set first.
                    const json attrs = c.value("attrs", json::object());
                    for (const auto& [attrName, v] : attrs.items())
                        if (const int i = curve->type().attrIndex(attrName); i >= 0 && !v.is_null() && attrName != "keys")
                            g.setValue({curve->id(), i, -1}, fromJson(curve->type().attrs[size_t(i)], v));
                    if (const int i = curve->type().attrIndex("keys"); i >= 0 && attrs.contains("keys"))
                        g.setValue({curve->id(), i, -1}, fromJson(curve->type().attrs[size_t(i)], attrs["keys"]));
                    out = g.plug(curve->path() + "." + c.value("output", std::string("output")));
                    g.connect(out, dst, true);
                }
                catch (const std::exception& e)
                {
                    if (curve)
                        g.deleteNode(curve->id());
                    if (missing++ == 0)
                        firstError = e.what();
                }
            }
            if (!a.get<bool>("activate", false) && before)
                sa::setActiveTake(g, before);
            if (missing > 0)
                lila::log::write(LogLevel::Warning, "importTake: " + std::to_string(missing) + " channels had no match in the scene" +
                                                        (firstError.empty() ? std::string() : " (" + firstError + ")"));
            return g.node(t)->path();
        },
    });
}

bool registerMarksType(ext::Context& ctx)
{
    NodeType t{.name = kMarksType,
               .doc = "Puppeteer's breakdown and extreme marks for one character's keys, kept by key time.",
               .icon = "bookmark",
               .create = [] { return std::make_unique<Node>(); }};
    const uint32_t stored = AttrInput | AttrStorable;
    t.ownAttrs = {
        {.name = "character", .type = PlugType::String, .defaultValue = std::string(), .flags = stored, .doc = "The character's root joint."},
        {.name = "marks", .type = PlugType::String, .defaultValue = std::string("[]"), .flags = stored | AttrHidden,
         .doc = "[[seconds, \"breakdown\" | \"extreme\"], ...]"},
    };
    lila::scene::relocate::registerNeutral(t.name);
    return types::registerType(ctx, std::move(t)) != nullptr;
}
} // namespace

void registerAnimCommands(ext::Context& ctx)
{
    registerMarksType(ctx);
    registerTracks(ctx);
    registerStackerCommands(ctx);
    registerCycleCommands(ctx);
    registerBankCommands(ctx);
}
} // namespace puppeteer::anim
