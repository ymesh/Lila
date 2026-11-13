#include "motion/Scene.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "retarget/Bvh.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>

namespace lila::motion
{
using namespace scene;
namespace fs = std::filesystem;

namespace
{
constexpr const char* kPrefs = "motion";

Graph& graph() { return Graph::current(); }

template <class F> auto guarded(const char* cmd, F&& f)
{
    try
    {
        return f();
    }
    catch (const CommandError&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

Node& named(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

int attrOf(const Node& n, const char* name)
{
    const int a = n.type().attrIndex(name);
    if (a < 0)
        throw std::invalid_argument(n.path() + " has no " + name);
    return a;
}

glm::dmat4 worldOf(Graph& g, NodeId n)
{
    g.evaluate();
    return g.node(n)->as<glm::dmat4>(Transform::kWorldMatrix);
}

/** The joint a character node names: itself, or the first joint under it (a Puppeteer character, a group). */
NodeId jointFor(Graph& g, NodeId node)
{
    std::vector<NodeId> stack{node};
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        if (rig::isJoint(*g.node(id)))
            return id;
        const auto& kids = g.children(id);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back(*it);
    }
    // A character node that points at its skeleton through a connection.
    for (const char* attr : {"skeleton", "root"})
        if (const int a = g.node(node)->type().attrIndex(attr); a >= 0)
            if (const auto src = g.source({node, a, -1}))
                if (const Node* s = g.node(src->node); s && rig::isJoint(*s))
                    return src->node;
    throw std::invalid_argument(g.node(node)->path() + " has no skeleton");
}

Device deviceFromName(const std::string& s)
{
    if (s == "gpu")
        return Device::Gpu;
    if (s == "cpu")
        return Device::Cpu;
    if (s == "auto" || s.empty())
        return Device::Auto;
    throw CommandError("motionDevice: device is auto, gpu or cpu");
}

const char* deviceName(Device d) { return d == Device::Gpu ? "gpu" : d == Device::Cpu ? "cpu" : "auto"; }

json statusJson(const Status& st)
{
    return {{"ready", st.ready},
            {"device", st.device},
            {"message", st.message},
            {"model", paths::toUtf8(st.model)},
            {"runtime", st.runtimeVersion},
            {"preference", deviceName(Engine::sharedDevice())}};
}

Engine& engineOrThrow(const char* cmd)
{
    Engine* e = Engine::shared();
    if (!e)
        throw CommandError(std::string(cmd) + ": " + Engine::status().message);
    return *e;
}

std::vector<Waypoint> waypointsArg(Graph& g, const json& j, const char* cmd)
{
    std::vector<Waypoint> out;
    if (!j.is_array())
        throw CommandError(std::string(cmd) + ": path is a list of points [x, y, z], curve names and spot names");
    for (const json& e : j)
    {
        if (e.is_string())
            out.push_back(named(e.get<std::string>(), cmd).id());
        else if (e.is_array() && e.size() >= 2)
            out.push_back(glm::dvec3(e[0].get<double>(), e.size() > 2 ? e[1].get<double>() : 0.0, e[e.size() > 2 ? 2 : 1].get<double>()));
        else
            throw CommandError(std::string(cmd) + ": path entry " + e.dump() + " is not a point or a node name");
    }
    (void)g;
    return out;
}

void loadDevicePref()
{
    const std::string d = PrefStore::get().value<std::string>(kPrefs, "/device", "auto");
    Engine::setSharedDevice(d == "gpu" ? Device::Gpu : d == "cpu" ? Device::Cpu : Device::Auto);
}
} // namespace

// Spots ---------------------------------------------------------------------------------------------------------------

Spot spotOf(Graph& g, NodeId node)
{
    const Node* n = g.node(node);
    if (!n || !n->type().isA(kSpotType))
        throw std::invalid_argument((n ? n->path() : std::string("that")) + " is not a spot");
    const std::string data = n->as<std::string>(attrOf(*n, "spot"));
    if (data.empty())
        return Spot{.name = n->name()};
    Spot s = Spot::fromJson(json::parse(data));
    if (s.name.empty())
        s.name = n->name();
    return s;
}

void setSpot(Graph& g, NodeId node, const Spot& spot)
{
    const Node& n = *g.node(node);
    g.setValue({node, attrOf(n, "spot"), -1}, spot.toJson().dump());
}

NodeId createSpot(Graph& g, const Spot& spot, const glm::dvec3& position, double yaw, const std::string& name)
{
    const NodeType* t = types::find(kSpotType);
    Node* n = g.createNode(*t, name.empty() ? (spot.name.empty() ? std::string("spot") : spot.name) : name);
    g.setValue({n->id(), Transform::kTranslate, -1}, position);
    g.setValue({n->id(), Transform::kRotate, -1}, glm::dvec3(0.0, yaw, 0.0));
    setSpot(g, n->id(), spot);
    return n->id();
}

std::pair<glm::vec2, float> groundFrame(Graph& g, NodeId node)
{
    const glm::dmat4 w = worldOf(g, node);
    const glm::dvec3 z = glm::dvec3(w[2]);
    return {glm::vec2(float(w[3].x / kCmPerMeter), float(w[3].z / kCmPerMeter)), float(std::atan2(z.x, z.z))};
}

SpotTarget captureTarget(Graph& g, const CharacterMap& map, NodeId spot, double t, int poses)
{
    const auto [at, heading] = groundFrame(g, spot);
    const glm::quat back = glm::angleAxis(-heading, glm::vec3(0, 1, 0));
    SpotTarget out;
    for (int k = 0; k < std::clamp(poses, 1, kPosesPerToken); ++k)
    {
        Pose p = map.sampleSoma(g, t + double(k) / kSampleRate);
        const glm::vec3 r = back * glm::vec3(p[0].p.x - at.x, 0.0f, p[0].p.z - at.y);
        p[0].p = glm::vec3(r.x, p[0].p.y, r.z);
        p[0].q = glm::normalize(back * p[0].q);
        out.poses.push_back(std::move(p));
    }
    return out;
}

std::vector<Leg> legsFrom(Graph& g, const std::vector<Waypoint>& waypoints, glm::vec2 start)
{
    std::vector<Leg> legs;
    Leg cur;
    cur.path.push_back(start);
    auto add = [&](glm::vec2 p) {
        if (cur.path.empty() || glm::length(cur.path.back() - p) > 1e-3f)
            cur.path.push_back(p);
    };
    for (const Waypoint& w : waypoints)
    {
        if (const auto* p = std::get_if<glm::dvec3>(&w))
        {
            add({float(p->x / kCmPerMeter), float(p->z / kCmPerMeter)});
            continue;
        }
        const NodeId id = std::get<NodeId>(w);
        const Node& n = *g.node(id);
        if (n.type().isA(kSpotType))
        {
            const auto [at, heading] = groundFrame(g, id);
            std::vector<PlacedTarget> targets = placeSpot(spotOf(g, id), at, heading);
            if (targets.empty())
                throw std::invalid_argument(n.path() + " has no targets yet");
            add({targets.front().poses.front()[0].p.x, targets.front().poses.front()[0].p.z});
            cur.targets = std::move(targets);
            const Pose& last = cur.targets.back().poses.front();
            legs.push_back(std::move(cur));
            cur = Leg{};
            cur.path.push_back({last[0].p.x, last[0].p.z});
            continue;
        }
        const int ws = n.type().attrIndex("worldSpace");
        if (ws < 0)
            throw std::invalid_argument(n.path() + " is not a point, curve or spot");
        g.evaluate();
        for (const curve::Spline& s : curve::splinesOf(g.value({id, ws, -1})))
            for (const glm::dvec3& p : curve::tessellate(s).points)
                add({float(p.x / kCmPerMeter), float(p.z / kCmPerMeter)});
    }
    if (cur.path.size() > 1 || legs.empty())
        legs.push_back(std::move(cur));
    return legs;
}

retarget::RunReport keyClip(Graph& g, const CharacterMap& map, const Clip& clip, const KeyOptions& o)
{
    LILA_ZONE();
    const retarget::Result res = map.toCharacter(clip.frames, clip.contacts, o.map);
    const retarget::Pose now = retarget::currentPose(g, map.scene());
    retarget::RunOptions ro;
    ro.take = o.take;
    ro.lockChains = o.lockFeet;
    return retarget::keyResult(g, map.scene(), map.rig(), now, res, o.start, kSampleRate, ro);
}

// Gait library building -----------------------------------------------------------------------------------------------

namespace
{
/** The clip's poses with the first frame's root on the origin facing +Z. */
std::vector<Pose> normalised(const std::vector<Pose>& frames, size_t first)
{
    const float h = headingOf(frames[first]);
    const glm::quat back = glm::angleAxis(-h, glm::vec3(0, 1, 0));
    const glm::vec3 o(frames[first][0].p.x, 0.0f, frames[first][0].p.z);
    std::vector<Pose> out;
    for (size_t f = first; f < frames.size(); ++f)
    {
        Pose p = frames[f];
        const glm::vec3 r = back * (p[0].p - o);
        p[0].p = glm::vec3(r.x, p[0].p.y, r.z);
        p[0].q = glm::normalize(back * p[0].q);
        out.push_back(std::move(p));
    }
    return out;
}

/** Lowers the hips by drop meters and bends the legs to keep the feet where they were (a crouched gait from a walk). */
Pose crouched(const Pose& p, float drop)
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const SomaRig& sr = somaRig();
    const Pose w = s.world(p);
    Pose out = p;
    out[0].p.y -= drop;
    const int chest = s.find("Spine2");
    out[size_t(chest)].q = glm::normalize(out[size_t(chest)].q * glm::angleAxis(0.35f, glm::vec3(0, 0, 1)));
    retarget::Pose rp = toRetargetPose(out);
    std::vector<glm::dmat4> world = retarget::worldOf(sr.skeleton, rp);
    for (const char* side : {"Left", "Right"})
    {
        const std::string pre = side;
        std::vector<int> joints = {sr.skeleton.find(pre + "Leg"), sr.skeleton.find(pre + "Shin"), sr.skeleton.find(pre + "Foot")};
        const Xform& foot = w[size_t(joints[2])];
        retarget::solveChainIk(sr.skeleton, rp, world, joints, glm::dvec3(foot.p) * kCmPerMeter, glm::dquat(foot.q));
    }
    return fromRetargetPose(rp);
}
} // namespace

GaitLibrary buildGaits(Graph& g, const json& spec, std::vector<std::string>& notes)
{
    GaitLibrary lib;
    lib.provenance = spec.value("provenance", "");
    const int windowsPer = spec.value("windows", 6);
    auto sampleClip = [&](const fs::path& file, double scale) {
        retarget::bvh::ImportOptions io;
        io.scale = scale;
        io.setPlayback = false;
        const retarget::bvh::ImportResult r = retarget::bvh::import(g, file, io);
        const CharacterMap map = CharacterMap::forJoint(g, r.joints.front());
        const retarget::Frames f = retarget::sample(g, map.scene(), r.start, r.end, kSampleRate);
        std::vector<Pose> poses;
        for (const auto& w : f.world)
            poses.push_back(map.toSoma(w));
        g.deleteNode(r.root);
        return poses;
    };
    for (const json& e : spec.at("gaits"))
    {
        const fs::path file = paths::fromUtf8(e.at("file").get<std::string>());
        std::vector<Pose> poses = sampleClip(file, e.value("scale", 1.0));
        const size_t n = poses.size();
        const size_t a = size_t(double(n) * e.value("from", 0.25)), b = size_t(double(n) * e.value("to", 0.75));
        if (b <= a + 8)
        {
            notes.push_back(paths::toUtf8(file) + ": too short");
            continue;
        }
        Gait gait;
        gait.name = e.value("name", paths::toUtf8(file.stem()));
        gait.style = e.at("style").get<std::string>();
        const FootStrikes strikes = footStrikes(poses);
        double length = e.value("period", 0.0) * kSampleRate;
        std::string how = length > 0.0 ? "cycle from the spec" : "cycle from " + std::to_string(strikes.left.size() + strikes.right.size()) + " foot strikes";
        if (length <= 0.0)
            length = cycleFrames(strikes);
        double start = -1.0;
        if (length <= 0.0 && !strikes.left.empty() && !strikes.right.empty())
        {
            // One step per foot (a side step): the cycle runs from the first foot leaving the ground to the other landing.
            const bool leftFirst = strikes.left.front() < strikes.right.front();
            const double land = leftFirst ? strikes.left.front() : strikes.right.front();
            const double otherLand = leftFirst ? strikes.right.front() : strikes.left.front();
            for (double t : leftFirst ? strikes.leftLifts : strikes.rightLifts)
                if (t < land)
                    start = t;
            if (start >= 0.0)
            {
                length = otherLand - start;
                how = std::string("one step per foot: cycle from the ") + (leftFirst ? "left" : "right") + " foot leaving the ground to the " +
                      (leftFirst ? "right" : "left") + " landing";
            }
        }
        if (length <= 0.0)
        {
            notes.push_back(gait.name + ": skipped, no foot strikes found to measure its cycle from");
            continue;
        }
        // Phase 0 is a left strike: the one nearest the middle of the steady part with a whole cycle after it in the clip.
        for (const std::vector<double>* foot : {&strikes.left, &strikes.right})
        {
            if (start >= 0.0)
                break;
            const double mid = 0.5 * (double(a) + double(b) - length);
            for (double t : *foot)
                if (t + length <= double(n - 1) && (start < 0.0 || std::abs(t - mid) < std::abs(start - mid)))
                    start = t;
            if (start >= 0.0)
            {
                if (foot == &strikes.right)
                    how += ", phase 0 at a right strike (no left one fits)";
                break;
            }
        }
        if (start < 0.0)
        {
            notes.push_back(gait.name + ": skipped, a whole cycle (" + std::to_string(length / kSampleRate).substr(0, 4) + " s) doesn't fit after any strike");
            continue;
        }
        gait.period = length / kSampleRate;
        auto rootAt = [&](double t) {
            const size_t i0 = size_t(t), i1 = std::min(i0 + 1, n - 1);
            return glm::mix(poses[i0][0].p, poses[i1][0].p, float(t - double(i0)));
        };
        const glm::vec3 d = rootAt(start + length) - rootAt(start);
        gait.speed = std::max(0.05, double(glm::length(glm::vec2(d.x, d.z))) / gait.period);
        const float drop = e.value("crouch", 0.0f);
        gait.cycle = loopCycle(poses, start, length);
        if (drop > 0.0f)
            for (Pose& p : gait.cycle)
                p = crouched(p, drop);
        for (int k = 0; k < windowsPer; ++k)
        {
            const size_t from = size_t(std::lround(start + length * double(k) / double(windowsPer)));
            if (from + kPosesPerToken > n)
                break;
            std::vector<Pose> win = normalised(std::vector<Pose>(poses.begin() + long(from), poses.begin() + long(from + kPosesPerToken)), 0);
            GaitWindow w;
            w.phase = double(k) / double(windowsPer);
            for (size_t i = 0; i < size_t(kPosesPerToken); ++i)
                w.poses[i] = drop > 0.0f ? crouched(win[i], drop) : win[i];
            gait.windows.push_back(std::move(w));
        }
        notes.push_back(gait.name + ": " + std::to_string(gait.speed).substr(0, 4) + " m/s, cycle " + std::to_string(gait.period).substr(0, 5) + " s, " + how);
        lib.gaits.push_back(std::move(gait));
    }
    if (spec.contains("idle"))
    {
        const json& e = spec["idle"];
        std::vector<Pose> poses = sampleClip(paths::fromUtf8(e.at("file").get<std::string>()), e.value("scale", 1.0));
        const size_t f = std::min(poses.size() - 1, size_t(double(poses.size()) * e.value("at", 0.5)));
        lib.idle = normalised(poses, f).front();
    }
    else
        lib.idle = SomaSkeleton::shipped().reference;
    lib.crouchIdle = crouched(lib.idle, spec.value("crouchIdle", 0.35f));
    return lib;
}

// Types and commands ------------------------------------------------------------------------------------------------

void registerTypes()
{
    if (types::find(kSpotType))
        return;
    NodeType spot;
    spot.name = kSpotType;
    spot.icon = "map-pin";
    spot.base = types::find("locator");
    spot.dag = true;
    spot.doc = "A spot: target poses around an object (sit, vault, duck, step up, pick up) that a character performs when "
               "its path reaches it. Move and turn the spot to place the interaction.";
    spot.ownAttrs = {
        {.name = "spot", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The spot's targets as JSON (motionSpotTarget edits them)."},
        {.name = "file", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "The .lilaspot asset it was made from or saved to."},
    };
    spot.create = [] { return std::make_unique<Transform>(); };
    types::registerType(std::move(spot));
}

void registerCommands()
{
    using T = ArgType;
    static bool done = false;
    if (done)
        return;
    done = true;
    registerTypes();
    loadDevicePref();
    PrefStore::get().onChanged([](const std::string& file) {
        if (file == kPrefs)
            loadDevicePref();
    });

    ops::registerCommand({
        .name = "motionStatus",
        .doc = "Load the motion model if needed and report what it runs on: the device (GPU through DirectML, or CPU, with the "
               "reason when it fell back), the model file, the runtime version, or what is missing.",
        .flags = {{"reload", "r", T::Bool, false, "Load the model again (after replacing the file)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("reload", false))
                Engine::resetShared();
            return statusJson(Engine::status());
        },
    });

    ops::registerCommand({
        .name = "motionDevice",
        .doc = "Choose where motion generates: auto (the GPU where it works, else the CPU), gpu or cpu. Saved as a preference; "
               "the model reloads on next use.",
        .flags = {{"device", "d", T::String, "auto", "auto, gpu or cpu.", {"auto", "gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string d = a.get<std::string>("device", "auto");
            Engine::setSharedDevice(deviceFromName(d));
            PrefStore::get().set(kPrefs, "/device", d);
            return {{"preference", d}};
        },
    });

    ops::registerCommand({
        .name = "motionGaits",
        .doc = "List the locomotion styles and their gaits (speed in m/s, cycle in seconds) motionFollowPath can use, and where "
               "their poses came from.",
        .undoable = false,
        .run = [](const Args&) -> json {
            return guarded("motionGaits", [] {
                const GaitLibrary& lib = GaitLibrary::shipped();
                json gs = json::array();
                for (const Gait& gait : lib.gaits)
                    gs.push_back({{"name", gait.name}, {"style", gait.style}, {"speed", gait.speed}, {"period", gait.period}, {"windows", gait.windows.size()}});
                return json{{"styles", lib.styles()}, {"gaits", gs}, {"provenance", lib.provenance}};
            });
        },
    });

    ops::registerCommand({
        .name = "motionFollowPath",
        .doc = "Generate a character walking, running, strafing or crouching along a path, performing any spots on it, and key "
               "the motion into a new take. The path is points, curves and spots in order; the character starts as posed at "
               "the start time.",
        .positionalDoc = "the character (a joint of its skeleton, or the node above it)",
        .flags = {
            {"path", "p", T::List, nullptr, "Waypoints in order: points [x, y, z] (cm), curve names, motionSpot names."},
            {"style", "st", T::String, "walk", "walk, run, crouch, strafeLeft, strafeRight or backward (motionGaits lists them)."},
            {"speed", "sp", T::Float, 0.0, "m/s; 0 uses the gait's own.", {}, {}, 0.0, 8.0},
            {"stop", "s", T::Bool, true, "Come to rest at the end."},
            {"facing", "f", T::Vec3, nullptr, "Strafe styles face this direction; default: the way the path starts."},
            {"variant", "v", T::Int, int64_t(0), "Other values give other, equally valid motion.", {}, {}, 0.0},
            {"lookahead", "la", T::Float, 0.9, "Seconds of travel each target sits ahead: lower follows tighter.", {}, {}, 0.3, 2.0},
            {"take", "t", T::String, "Path", "Take to key into; one of that name is replaced. Empty keys the current animation."},
            {"start", "sa", T::Float, nullptr, "Seconds; default the playback start."},
            {"rootMotion", "rm", T::Bool, true, "The root joint follows the pelvis on the ground (for engine root motion)."},
            {"plantFeet", "pf", T::Bool, true, "Hold feet still where the model predicts contact."},
            {"lockFeet", "lf", T::Bool, true, "Key the foot lock of the character's IK chains where feet are planted."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("motionFollowPath: select a character or name it");
            if (!a.has("path"))
                throw CommandError("motionFollowPath: give a path");
            Engine& engine = engineOrThrow("motionFollowPath");
            return guarded("motionFollowPath", [&] {
                const NodeId joint = jointFor(g, named(objs.front(), "motionFollowPath").id());
                const double start = a.has("start") ? a.get<double>("start") : anim::playback().minTime;
                const CharacterMap map = CharacterMap::forJoint(g, joint);
                Journey j;
                j.start = map.sampleSoma(g, start);
                for (int k = 3; k >= 1; --k)
                    j.before.push_back(map.sampleSoma(g, start - double(k) / kSampleRate));
                j.legs = legsFrom(g, waypointsArg(g, a.flags["path"], "motionFollowPath"), {j.start[0].p.x, j.start[0].p.z});
                j.options.style = a.get<std::string>("style", "walk");
                j.options.speed = a.get<double>("speed", 0.0);
                j.options.stop = a.get<bool>("stop", true);
                j.options.variant = uint32_t(a.get<int64_t>("variant", 0));
                j.options.lookahead = a.get<double>("lookahead", 0.9);
                if (a.has("facing"))
                {
                    const auto f = a.flags["facing"];
                    j.options.facing = {f[0].get<float>(), f[2].get<float>()};
                }
                const Clip clip = followPaths(engine, std::span(&j, 1)).front();
                if (clip.frames.empty())
                    throw std::invalid_argument(clip.error.empty() ? "no motion was generated" : clip.error);
                KeyOptions ko;
                ko.take = a.get<std::string>("take", "Path");
                ko.start = start;
                ko.map.rootMotion = a.get<bool>("rootMotion", true);
                ko.map.plantFeet = a.get<bool>("plantFeet", true);
                ko.lockFeet = a.get<bool>("lockFeet", true);
                const retarget::RunReport rep = keyClip(g, map, clip, ko);
                json reached = json::array();
                for (const auto& [f, label] : clip.reached)
                    reached.push_back({{"frame", f}, {"time", start + double(f) / kSampleRate}, {"target", label}});
                json out = rep.toJson();
                out["generations"] = clip.generations;
                out["reached"] = reached;
                out["device"] = engine.info().device;
                if (!clip.error.empty())
                    out["warning"] = clip.error;
                return out;
            });
        },
        .examples = {"motionFollowPath('Manny', path=[[0, 0, 400], [300, 0, 700]], style='run')",
                     "motionFollowPath('Manny', path=['pathCurve1', 'chairSpot'], take='SitDown')"},
    });

    ops::registerCommand({
        .name = "motionSpot",
        .doc = "Make a spot: a starter interaction Lila ships (by name), a .lilaspot file, or an empty one to capture poses into. "
               "Returns the node, or with list the starters.",
        .flags = {
            {"preset", "pr", T::String, nullptr, "A shipped spot by name (list shows them)."},
            {"file", "f", T::String, nullptr, "A .lilaspot file."},
            {"name", "n", T::String, nullptr, "Node name."},
            {"position", "p", T::Vec3, nullptr, "World position, cm."},
            {"yaw", "y", T::Float, 0.0, "Degrees about Y: the way the spot faces."},
            {"list", "l", T::Bool, false, "List the shipped spots instead."},
        },
        .run = [](const Args& a) -> json {
            if (a.get<bool>("list", false))
            {
                json l = json::array();
                for (const auto& [name, file] : shippedSpots())
                {
                    json e = {{"name", name}, {"file", paths::toUtf8(file)}};
                    try
                    {
                        const Spot s = Spot::load(file);
                        e["targets"] = s.targets.size();
                        e["provenance"] = s.provenance;
                    }
                    catch (const std::exception& ex)
                    {
                        e["error"] = ex.what();
                    }
                    l.push_back(e);
                }
                return l;
            }
            Graph& g = graph();
            return guarded("motionSpot", [&] {
                Spot s;
                std::string file;
                if (a.has("preset"))
                {
                    const std::string want = a.get<std::string>("preset");
                    for (const auto& [name, f] : shippedSpots())
                        if (name == want)
                            file = paths::toUtf8(f);
                    if (file.empty())
                        throw std::invalid_argument("no shipped spot named '" + want + "'");
                }
                else if (a.has("file"))
                    file = a.get<std::string>("file");
                if (!file.empty())
                    s = Spot::load(paths::fromUtf8(file));
                glm::dvec3 pos(0.0);
                if (a.has("position"))
                    pos = {a.flags["position"][0].get<double>(), a.flags["position"][1].get<double>(), a.flags["position"][2].get<double>()};
                const NodeId id = createSpot(g, s, pos, a.get<double>("yaw", 0.0), a.get<std::string>("name", ""));
                if (!file.empty())
                    g.setValue({id, g.node(id)->type().attrIndex("file"), -1}, file);
                return json{{"node", g.path(id)}, {"targets", s.targets.size()}};
            });
        },
        .examples = {"motionSpot(preset='Pick Up', position=[0, 0, 300])"},
    });

    ops::registerCommand({
        .name = "motionSpotTarget",
        .doc = "Add, change or remove a spot's targets. capture adds the character's pose at a time (and the next poses at 30 Hz "
               "for a moving arrival) relative to the spot. Returns the spot's targets.",
        .positionalDoc = "the motionSpot node",
        .flags = {
            nodeFlag({"capture", "c", T::String, nullptr, "Character to capture a target from."}, "node"),
            {"time", "t", T::Float, nullptr, "Seconds; default the current time."},
            {"poses", "ps", T::Int, int64_t(1), "Poses to capture, 1 to 4: more gives the arrival its velocity.", {}, {}, 1.0, 4.0},
            {"index", "i", T::Int, nullptr, "Target to change or remove; capture inserts before it (default: at the end)."},
            {"remove", "rm", T::Bool, false, "Remove the target at index."},
            {"name", "n", T::String, nullptr, "Target name."},
            {"transitionEarly", "te", T::Int, nullptr, "Frames before arriving the next target takes over: higher passes through.", {}, {}, 0.0, 20.0},
            {"minTokens", "mn", T::Int, nullptr, "Shortest approach, tokens of 4 frames (6..16).", {}, {}, 6.0, 16.0},
            {"maxTokens", "mx", T::Int, nullptr, "Longest approach, tokens of 4 frames (6..16).", {}, {}, 6.0, 16.0},
            {"pins", "pi", T::List, nullptr, "Hands and feet held on the pose as it arrives: leftHand, rightHand, leftFoot, rightFoot."},
            {"hold", "h", T::Int, nullptr, "Frames to stay in the pose.", {}, {}, 0.0, 300.0},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.empty())
                throw CommandError("motionSpotTarget: name the spot");
            const NodeId id = named(a.positional[0].get<std::string>(), "motionSpotTarget").id();
            return guarded("motionSpotTarget", [&] {
                Spot s = spotOf(g, id);
                int index = a.has("index") ? int(a.get<int64_t>("index")) : -1;
                if (a.has("capture"))
                {
                    const NodeId joint = jointFor(g, named(a.get<std::string>("capture"), "motionSpotTarget").id());
                    const CharacterMap map = CharacterMap::forJoint(g, joint);
                    SpotTarget t = captureTarget(g, map, id, a.has("time") ? a.get<double>("time") : g.time(), int(a.get<int64_t>("poses", 1)));
                    t.name = a.get<std::string>("name", "Target " + std::to_string(s.targets.size() + 1));
                    if (s.provenance.empty())
                        s.provenance = "Captured from " + g.node(map.scene().top)->name();
                    if (index < 0 || index > int(s.targets.size()))
                        index = int(s.targets.size());
                    s.targets.insert(s.targets.begin() + index, std::move(t));
                }
                else if (index < 0 || index >= int(s.targets.size()))
                    throw std::invalid_argument("no target " + std::to_string(index) + " (the spot has " + std::to_string(s.targets.size()) + ")");
                if (a.get<bool>("remove", false))
                    s.targets.erase(s.targets.begin() + index);
                else
                {
                    SpotTarget& t = s.targets[size_t(index)];
                    if (a.has("name") && !a.has("capture"))
                        t.name = a.get<std::string>("name");
                    if (a.has("transitionEarly"))
                        t.transitionEarly = int(a.get<int64_t>("transitionEarly"));
                    if (a.has("minTokens") || a.has("maxTokens"))
                    {
                        int lo = kMaxTokens, hi = kMinTokens;
                        for (int k = kMinTokens; k <= kMaxTokens; ++k)
                            if ((t.tokens >> (k - kMinTokens)) & 1)
                                lo = std::min(lo, k), hi = std::max(hi, k);
                        lo = int(a.get<int64_t>("minTokens", lo));
                        hi = int(a.get<int64_t>("maxTokens", hi));
                        if (hi < lo)
                            throw std::invalid_argument("maxTokens is below minTokens");
                        t.tokens = tokenMask(lo, hi);
                    }
                    if (a.has("pins"))
                    {
                        t.pins = 0;
                        for (const json& p : a.flags["pins"])
                        {
                            const std::string n = p.get<std::string>();
                            const uint8_t bit = n == "leftHand" ? kPinLeftHand : n == "rightHand" ? kPinRightHand : n == "leftFoot" ? kPinLeftFoot : n == "rightFoot" ? kPinRightFoot : 0;
                            if (!bit)
                                throw std::invalid_argument("pins are leftHand, rightHand, leftFoot, rightFoot");
                            t.pins |= bit;
                        }
                    }
                    if (a.has("hold"))
                        t.hold = int(a.get<int64_t>("hold"));
                }
                setSpot(g, id, s);
                json ts = s.toJson()["targets"];
                for (json& t : ts)
                    t.erase("poses");
                return json{{"spot", g.path(id)}, {"targets", ts}};
            });
        },
        .examples = {"motionSpotTarget('chairSpot', capture='Manny', time=2.0, poses=1, name='Sit')"},
    });

    ops::registerCommand({
        .name = "motionSpotSave",
        .doc = "Save a spot's targets as a .lilaspot asset to reuse in other scenes (motionSpot file=...).",
        .positionalDoc = "the motionSpot node",
        .flags = {{"file", "f", T::String, nullptr, "Where to write; default the spot's file."},
                  {"provenance", "pv", T::String, nullptr, "Where the poses came from, stored with the asset."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.empty())
                throw CommandError("motionSpotSave: name the spot");
            const NodeId id = named(a.positional[0].get<std::string>(), "motionSpotSave").id();
            return guarded("motionSpotSave", [&] {
                const int fa = g.node(id)->type().attrIndex("file");
                std::string file = a.get<std::string>("file", g.node(id)->as<std::string>(fa));
                if (file.empty())
                    throw std::invalid_argument("give a file");
                if (!file.ends_with(".lilaspot"))
                    file += ".lilaspot";
                Spot spot = spotOf(g, id);
                if (a.has("provenance"))
                    spot.provenance = a.get<std::string>("provenance");
                spot.save(paths::fromUtf8(file));
                g.setValue({id, fa, -1}, file);
                return json{{"file", file}};
            });
        },
    });

    ops::registerCommand({
        .name = "motionSample",
        .doc = "A character's motion as the motion model sees it: per frame at 30 fps, the world positions (cm) of SOMA joints, "
               "the heading and the ground point. For checking generated motion and building spots from clips.",
        .positionalDoc = "the character",
        .flags = {
            {"start", "s", T::Float, nullptr, "Seconds; default the playback start."},
            {"end", "e", T::Float, nullptr, "Seconds; default the playback end."},
            {"joints", "j", T::List, nullptr, "SOMA joint names (Hips, LeftHand, RightFoot ...); default Hips, hands and feet."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("motionSample: name the character");
            return guarded("motionSample", [&] {
                const NodeId joint = jointFor(g, named(objs.front(), "motionSample").id());
                const CharacterMap map = CharacterMap::forJoint(g, joint);
                const double start = a.has("start") ? a.get<double>("start") : anim::playback().minTime;
                const double end = a.has("end") ? a.get<double>("end") : anim::playback().maxTime;
                std::vector<std::string> names = {"Hips", "LeftHand", "RightHand", "LeftFoot", "RightFoot", "Head"};
                if (a.has("joints"))
                    names = a.flags["joints"].get<std::vector<std::string>>();
                const SomaSkeleton& s = SomaSkeleton::shipped();
                const retarget::Frames f = retarget::sample(g, map.scene(), start, end, kSampleRate);
                json frames = json::array();
                for (size_t k = 0; k < f.world.size(); ++k)
                {
                    const Pose p = map.toSoma(f.world[k]);
                    const Pose w = s.world(p);
                    json pos = json::object();
                    for (const std::string& n : names)
                        if (const int j = s.find(n); j >= 0)
                            pos[n] = {w[size_t(j)].p.x * kCmPerMeter, w[size_t(j)].p.y * kCmPerMeter, w[size_t(j)].p.z * kCmPerMeter};
                    frames.push_back({{"time", start + double(k) / kSampleRate}, {"heading", glm::degrees(headingOf(p))}, {"joints", pos}});
                }
                return json{{"frames", frames}, {"scale", map.scale()}};
            });
        },
    });

    ops::registerCommand({
        .name = "motionBuildGaits",
        .doc = "Build a gait library from motion capture (BVH): each clip retargeted to the model's skeleton, its speed and cycle "
               "measured, target windows sampled across one cycle. For your own mocap, or to rebuild Lila's.",
        .flags = {
            {"spec", "s", T::Any, nullptr,
             "{'provenance': text, 'windows': 6, 'gaits': [{'file', 'style', 'name', 'scale', 'from', 'to', 'crouch'}], 'idle': {'file', 'at'}}."},
            {"out", "o", T::String, nullptr, "The gaits .json to write."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (!a.has("spec") || !a.has("out"))
                throw CommandError("motionBuildGaits: give spec and out");
            return guarded("motionBuildGaits", [&] {
                std::vector<std::string> notes;
                const GaitLibrary lib = buildGaits(g, a.flags["spec"], notes);
                std::ofstream out(paths::fromUtf8(a.get<std::string>("out")));
                out << lib.toJson().dump();
                return json{{"gaits", lib.gaits.size()}, {"notes", notes}};
            });
        },
    });
}
} // namespace lila::motion
