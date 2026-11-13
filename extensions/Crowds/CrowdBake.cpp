// Crowds out of the simulation: characters for the agents, each agent baked onto its own copy of its character, files
// per agent or combined, and Unreal (a Level Sequence of the baked agents).

#include "CrowdDraw.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Motion.h>
#include <lila/Paths.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace crowds
{
namespace fs = std::filesystem;

namespace
{
Graph& sceneGraph() { return Graph::current(); }

int attrIndex(const Node& n, const char* name) { return n.type().attrIndex(name); }

std::string joinPaths(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& x : v)
        s += (s.empty() ? "" : ", ") + x;
    return s;
}

/** Agent indices from a flag (ints), or every agent. */
std::vector<size_t> agentsArg(const Args& a, size_t count, const char* cmd)
{
    std::vector<size_t> out;
    if (!a.has("agents"))
    {
        for (size_t i = 0; i < count; ++i)
            out.push_back(i);
        return out;
    }
    for (const json& x : a.flags["agents"])
    {
        const int64_t i = x.get<int64_t>();
        if (i < 0 || size_t(i) >= count)
            throw CommandError(std::string(cmd) + ": no agent " + std::to_string(i) + " (the crowd has " + std::to_string(count) + ")");
        out.push_back(size_t(i));
    }
    return out;
}

struct Range
{
    int64_t start = 0, end = 0;
};

Range rangeArg(const Args& a, const CrowdView& v)
{
    Range r;
    r.start = a.has("start") ? a.get<int64_t>("start") : std::max<int64_t>(v.startFrame, sim::frameOf(anim::playback().minTime));
    r.end = a.has("end") ? a.get<int64_t>("end") : sim::frameOf(anim::playback().maxTime);
    if (r.end <= r.start)
        throw CommandError("the range is empty: end must come after start");
    return r;
}

/** An agent's motion over the range at 30 Hz, from the cached frames (interpolated between them). */
motion::Clip clipOf(Graph& g, NodeId crowd, const CrowdView& v, size_t agent, Range r, int& cycleFrames, int& firstAlive)
{
    const double fps = std::max(v.fps, 1e-6);
    const double t0 = double(r.start) / fps, t1 = double(r.end) / fps;
    motion::Clip clip;
    cycleFrames = 0;
    firstAlive = -1;
    motion::Pose a, b, last;
    for (int k = 0;; ++k)
    {
        const double t = t0 + double(k) / motion::kSampleRate;
        if (t > t1 + 1e-9)
            break;
        const double fr = t * fps;
        const int64_t f0 = int64_t(std::floor(fr + 1e-9));
        const double w = fr - double(f0);
        // The range was simulated before baking, so these are cache reads; simulate() would set the crowd up again each time.
        auto frameAt = [&](int64_t f) {
            std::shared_ptr<const sim::Frame> fr = sim::cached(g, crowd, f);
            return fr ? fr : sim::simulate(g, crowd, f, sim::LoopMode::Off);
        };
        std::shared_ptr<const sim::Frame> fa = frameAt(f0);
        std::shared_ptr<const sim::Frame> fb = w > 1e-6 ? frameAt(f0 + 1) : fa;
        const bool aliveA = agentPose(v, fa->state, agent, a);
        const bool aliveB = agentPose(v, fb->state, agent, b);
        if (aliveA && aliveB && w > 1e-6)
            for (size_t j = 0; j < a.size(); ++j)
            {
                a[j].q = glm::normalize(glm::slerp(a[j].q, b[j].q, float(w)));
                a[j].p = glm::mix(a[j].p, b[j].p, float(w));
            }
        if (aliveA)
        {
            if (firstAlive < 0)
                firstAlive = k;
            last = a;
        }
        if (MotionKind(int(v.runtime->agent(fa->state, agent).motion)) == MotionKind::Cycle)
            ++cycleFrames;
        clip.frames.push_back(aliveA ? a : last);
        clip.contacts.push_back(v.runtime->contacts(fa->state, agent));
    }
    // Before it appears, it stands where it first shows.
    if (firstAlive > 0)
        for (int k = 0; k < firstAlive; ++k)
            clip.frames[size_t(k)] = clip.frames[size_t(firstAlive)];
    return clip;
}

NodeId nodeNamed(Graph& g, const std::string& path)
{
    try
    {
        if (Node* n = g.find(path))
            return n->id();
    }
    catch (const std::exception&)
    {
    }
    return kNoNode;
}

struct Baked
{
    size_t agent = 0;
    NodeId root = kNoNode;
    int character = 0;
    int cycleFrames = 0;
    int frames = 0;
};

std::vector<Baked> bake(Graph& g, NodeId crowd, const Args& a, const char* cmd, json& report)
{
    std::shared_ptr<const CrowdView> v = viewOf(g, crowd);
    if (!v)
        throw CommandError(std::string(cmd) + ": " + sim::info(g, crowd).error);
    if (v->characters.empty())
        throw CommandError(std::string(cmd) + ": the crowd has no characters; set them with crowdCharacters");
    const Range r = rangeArg(a, *v);
    const std::vector<size_t> agents = agentsArg(a, v->runtime->setup().agents.size(), cmd);
    {
        progress::Scope sim("Simulating the crowd", 0.0, progress::Cancellable);
        sim::simulate(g, crowd, r.end + 1, sim::LoopMode::Off);
    }
    v = viewOf(g, crowd);
    // Every clip is read before the first edit: any edit marks the cache unverified until the crowd simulates again.
    struct Read
    {
        motion::Clip clip;
        int cycleFrames = 0;
    };
    std::vector<Read> reads;
    for (size_t i : agents)
    {
        Read rd;
        int firstAlive = -1;
        rd.clip = clipOf(g, crowd, *v, i, r, rd.cycleFrames, firstAlive);
        reads.push_back(std::move(rd));
    }
    const std::string base = g.node(crowd)->name();
    const NodeId group = g.createNode(*types::find("transform"), a.get<std::string>("group", base + "_baked"))->id();
    motion::KeyOptions ko;
    ko.start = double(r.start) / std::max(v->fps, 1e-6);
    ko.map.plantFeet = a.get<bool>("plantFeet", true);
    ko.lockFeet = a.get<bool>("lockFeet", true);
    ko.map.rootMotion = true;

    std::vector<Baked> out;
    progress::Scope bar("Baking crowd agents", double(agents.size()), progress::Cancellable);
    int totalCycle = 0;
    for (size_t n = 0; n < agents.size(); ++n)
    {
        const size_t i = agents[n];
        bar.checkCancel();
        bar.setStatus("Agent " + std::to_string(i));
        const int ch = std::clamp(v->runtime->setup().agents[i].character, 0, int(v->characters.size()) - 1);
        const NodeId src = nodeNamed(g, v->characters[size_t(ch)]);
        if (!src)
            throw CommandError(std::string(cmd) + ": character '" + v->characters[size_t(ch)] + "' is not in the scene");
        const json made = ops::execute("duplicate", {{"args", {g.path(src)}}, {"name", base + "_agent" + std::to_string(i)}});
        const std::string name = made.is_array() && !made.empty() ? made[0].get<std::string>() : made.get<std::string>();
        const NodeId root = nodeNamed(g, name);
        if (!root)
            throw CommandError(std::string(cmd) + ": duplicating " + g.path(src) + " made nothing");
        g.setParent(root, group);
        const NodeId joint = characterJoint(g, root);
        if (!joint)
            throw CommandError(std::string(cmd) + ": " + name + " has no skeleton");
        Baked b{.agent = i, .root = root, .character = ch, .cycleFrames = reads[n].cycleFrames};
        const motion::Clip& clip = reads[n].clip;
        b.frames = int(clip.frames.size());
        const motion::CharacterMap map = motion::CharacterMap::forJoint(g, joint);
        motion::keyClip(g, map, clip, ko);
        // Keys continue from where the copy stands (at the origin), so the copy moves to where its agent starts.
        if (!clip.frames.empty())
        {
            const motion::Pose& first = clip.frames.front();
            const double ground = v->ground.at(first[0].p.x, first[0].p.z);
            g.setValue({root, Transform::kTranslate, -1}, glm::dvec3(first[0].p.x * kCm, ground * kCm, first[0].p.z * kCm));
        }
        totalCycle += b.cycleFrames;
        out.push_back(b);
        bar.step();
    }
    json list = json::array();
    for (const Baked& b : out)
        list.push_back({{"agent", b.agent}, {"node", g.path(b.root)}, {"character", v->characters[size_t(b.character)]}, {"frames", b.frames},
                        {"cycleFrames", b.cycleFrames}});
    report["group"] = g.path(group);
    report["agents"] = list;
    report["start"] = r.start;
    report["end"] = r.end;
    report["device"] = v->runtime->stats().device;
    if (totalCycle > 0)
        report["warning"] = std::to_string(totalCycle) + " baked frames came from gait cycles (agents far from the focus or past the "
                                                         "generated budget); set detail to generated for generated motion throughout";
    return out;
}

fs::path folderArg(const std::string& p)
{
    fs::path f = paths::fromUtf8(p);
    std::error_code ec;
    fs::create_directories(f, ec);
    if (ec)
        throw CommandError("cannot make the folder " + p + ": " + ec.message());
    return f;
}

std::string assetName(std::string s)
{
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            c = '_';
    return s;
}
/** One agent group as a Mass crowd group: its share of the agents, walking speed (cm/s) and spread, pause and radius. */
struct MassGroup
{
    std::string name;
    int agents = 0;
    /** Agents per character the group wears. */
    std::map<int, int> wears;
    double speedSum = 0.0, speedMin = 1e9, speedMax = 0.0, radius = 0.0, pause = 2.0, style = 1.3;
};

/**
 * Each of the crowd's characters for Mass: its skeletal mesh, and its walk as an animation on that skeleton, cut from a
 * network agent wearing it while it walks; the root bone's travel is taken out in Unreal so the walk plays in place.
 * yaw turns the mesh so it faces Unreal's +X, the way Mass agents walk.
 */
json massCharacters(Graph& g, NodeId c, const CrowdView& v, const std::string& folder)
{
    LILA_ZONE();

    const Setup& su = v.runtime->setup();
    const fs::path cache = fs::temp_directory_path() / "lila_crowds" / assetName(g.node(c)->name());
    fs::create_directories(cache);
    const math::Vector f = math::toUnreal(math::Vector(0.0, 0.0, 1.0));
    const double yaw = -glm::degrees(std::atan2(f.y, f.x));
    json out = json::array();
    progress::Scope bar("Sending the crowd's characters to Unreal", double(v.characters.size()));
    for (size_t ch = 0; ch < v.characters.size(); ++ch)
    {
        bar.step();
        // An agent wearing it, walking a little after the start.
        int agent = -1;
        for (size_t i = 0; i < su.agents.size() && agent < 0; ++i)
            if (su.agents[i].character == int(ch) && su.groups[size_t(su.agents[i].group)].useNetwork)
                agent = int(i);
        const std::string name = assetName(g.node(nodeNamed(g, v.characters[ch]))->name());
        if (agent < 0)
        {
            out.push_back({{"name", name}, {"mesh", ""}, {"walk", ""}, {"yaw", yaw}});
            continue;
        }
        const int64_t start = v.startFrame + int64_t(v.fps), end = start + int64_t(v.fps * 2.0);
        const json baked = ops::execute("crowdBake", {{"args", {g.path(c)}}, {"agents", {agent}}, {"start", start}, {"end", end}});
        const std::string root = baked["agents"][0]["node"].get<std::string>();
        const std::string mesh = folder + "/SK_" + name, walk = folder + "/A_" + name + "_Walk";
        const fs::path meshFile = cache / ("SK_" + name + ".fbx"), walkFile = cache / ("A_" + name + "_Walk.fbx");
        ops::execute("exportFbx", {{"args", {paths::toUtf8(meshFile), root}}, {"kind", "skeletal"}, {"animation", false}});
        const json r = ops::execute("unrealImport", {{"args", {paths::toUtf8(meshFile)}}, {"contentPath", mesh}, {"kind", "skeletal"}, {"wait", true}});
        std::string skeleton = mesh + "_Skeleton";
        for (const json& as : r.value("assets", json::array()))
            if (const std::string p = as.is_string() ? as.get<std::string>() : as.value("path", ""); p.find("Skeleton") != std::string::npos)
                skeleton = p;
        ops::execute("exportFbx", {{"args", {paths::toUtf8(walkFile), root}}, {"kind", "animation"}, {"start", double(start)}, {"end", double(end)}});
        ops::execute("unrealImport", {{"args", {paths::toUtf8(walkFile)}}, {"contentPath", walk}, {"kind", "animation"}, {"skeleton", skeleton}, {"wait", true}});
        ops::execute("delete", {{"args", {baked["group"]}}});
        out.push_back({{"name", name}, {"mesh", mesh}, {"walk", walk}, {"yaw", yaw}});
    }
    return out;
}

/**
 * The crowd as a live Mass crowd in Unreal: its network's lanes (sidewalks, footpaths, curves, and the crossings and corners
 * joining them) with ground heights, and its network groups' shares, speeds, pauses and radii; LilaLinkCrowd builds the
 * ZoneGraph lanes, a lane-following StateTree and Mass entity config per group, and a spawner.
 */
json sendMass(Graph& g, NodeId c, const Args& a)
{
    LILA_ZONE();

    const char* cmd = "crowdSendToUnreal";
    std::shared_ptr<const CrowdView> v = viewOf(g, c);
    if (!v || !v->runtime)
        throw CommandError(std::string(cmd) + ": the crowd can't be set up: " + (v ? v->status : std::string("not a crowd")));
    const Setup& su = v->runtime->setup();
    const bool crossings = a.get<bool>("crossings", true);
    auto ueAt = [&](glm::vec2 p) {
        double y = v->ground.at(p.x, p.y);
        if (!std::isfinite(y))
            y = v->ground.fallback;
        const math::Vector u = math::toUnreal(math::Vector(p.x * kCm, y * kCm, p.y * kCm));
        return json::array({u.x, u.y, u.z});
    };
    json lanes = json::array();
    double walkway = 0.0;
    for (const Network::Lane& l : su.network.lanes)
    {
        if (l.crossing && !crossings)
            continue;
        json pts = json::array();
        for (size_t i = 0; i < l.points.size(); ++i)
        {
            pts.push_back(ueAt(l.points[i]));
            if (i > 0 && !l.crossing)
                walkway += glm::length(l.points[i] - l.points[i - 1]) * kCm;
        }
        json lane = {{"points", pts}, {"crossing", l.crossing}};
        if (l.phase >= 0 && l.phases > 0)
            lane["signal"] = {{"junction", l.junction}, {"phase", l.phase}, {"phases", l.phases}};
        lanes.push_back(lane);
    }
    if (lanes.empty())
        throw CommandError(std::string(cmd) + ": mass=True needs the crowd's network (a settlement's sidewalks or curves); set it in the crowd's network");

    std::vector<MassGroup> groups(su.groups.size());
    for (size_t k = 0; k < su.groups.size(); ++k)
    {
        const Group& gr = su.groups[k];
        groups[k].name = gr.name;
        groups[k].pause = 0.5 * (gr.pauseMin + gr.pauseMax);
        groups[k].style = gr.style == "run" ? 3.6 : 1.3;
    }
    for (const AgentSpec& s : su.agents)
        if (s.group >= 0 && size_t(s.group) < groups.size() && su.groups[size_t(s.group)].useNetwork)
        {
            MassGroup& m = groups[size_t(s.group)];
            ++m.agents;
            ++m.wears[s.character];
            m.speedSum += s.speed;
            m.speedMin = std::min(m.speedMin, double(s.speed));
            m.speedMax = std::max(m.speedMax, double(s.speed));
            m.radius += s.radius;
        }
    json groupsOut = json::array();
    int total = 0;
    for (const MassGroup& m : groups)
        if (m.agents > 0)
        {
            const double mean = m.speedSum / m.agents;
            json wears = json::array();
            for (const auto& [ch, n] : m.wears)
                wears.push_back({{"index", ch}, {"share", n}});
            groupsOut.push_back({{"characters", wears},
                                 {"name", m.name},
                                 {"share", m.agents},
                                 {"speed", m.style * mean * kCm},
                                 {"speedVariance", mean > 0.0 ? std::clamp((m.speedMax - m.speedMin) * 0.5 / mean, 0.0, 1.0) : 0.0},
                                 {"pause", m.pause},
                                 {"radius", m.radius / m.agents * kCm}});
            total += m.agents;
        }
    if (groupsOut.empty())
        throw CommandError(std::string(cmd) + ": mass=True sends the groups that walk the network; spawn one with crowdSpawn on the network first");

    const int count = int(a.get<int64_t>("count", total));
    json characters = json::array();
    if (a.get<bool>("characters", true) && !v->characters.empty() && !a.get<bool>("dryRun", false))
        characters = massCharacters(g, c, *v, a.get<std::string>("folder", "/Game/Crowds"));
    // Spawn points along both walking lanes of each walkway, a little more than needed so the spawner picks among them.
    const double gap = a.has("density") ? 10000.0 / std::max(0.01, a.get<double>("density"))
                                        : std::max(100.0, 2.0 * walkway / std::max(1, count) * 0.8);
    const std::string name = g.node(c)->name();
    const json params = {{"id", name},
                         {"label", name},
                         {"folder", a.get<std::string>("folder", "/Game/Crowds")},
                         {"lanes", lanes},
                         {"groups", groupsOut},
                         {"count", count},
                         {"minGap", std::max(100.0, gap * 0.7)},
                         {"maxGap", std::max(100.0, gap * 1.3)},
                         {"mesh", a.get<std::string>("mesh", "/Engine/BasicShapes/Cylinder.Cylinder")},
                         {"characters", characters},
                         {"actorDistance", a.get<double>("actorDistance", 5000.0)},
                         {"visibleDistance", a.get<double>("visibleDistance", 15000.0)},
                         {"maxActors", a.get<int64_t>("maxActors", 200)},
                         {"phaseSeconds", a.get<double>("phaseSeconds", 20.0)},
                         {"clearSeconds", a.get<double>("clearSeconds", 4.0)}};
    if (a.get<bool>("dryRun", false))
        return {{"params", params}};
    const json r = ops::execute("unrealCall", {{"args", {"setMassCrowd"}}, {"params", params}, {"timeoutMs", 300000}});
    if (!r.value("ok", false))
    {
        const std::string error = r.value("error", r.dump());
        if (error.find("setMassCrowd") != std::string::npos && error.find("nknown") != std::string::npos)
            throw CommandError(std::string(cmd) + ": enable the LilaLinkCrowd plugin in the Unreal project (Lila's ue/LilaLinkCrowd); it brings "
                                                  "MassCrowd, MassAI, StateTree and ZoneGraph");
        throw CommandError(std::string(cmd) + ": Unreal could not make the Mass crowd: " + error);
    }
    const json made = r.value("result", json::object());
    for (const json& w : made.value("warnings", json::array()))
        log::write(LogLevel::Warning, std::string(cmd) + ": " + w.get<std::string>());
    return {{"mass", made}, {"agents", count}, {"groups", groupsOut.size()}, {"lanes", lanes.size()}};
}

} // namespace

void setCharacters(Graph& g, NodeId crowd, const std::vector<std::string>& characters)
{
    std::vector<std::string> paths;
    for (const std::string& c : characters)
    {
        const NodeId id = nodeNamed(g, c);
        if (!id)
            throw CommandError("no node named '" + c + "'");
        const NodeId joint = characterJoint(g, id);
        if (!joint)
            throw CommandError(g.path(id) + " has no skeleton");
        // Characterises it (undoably) when it has no retarget rig yet, so drawing and bake can map motion onto it.
        motion::CharacterMap::forJoint(g, joint);
        paths.push_back(g.path(id));
    }
    g.setValue({crowd, attrIndex(*g.node(crowd), "characters"), -1}, joinPaths(paths));
}

void registerBake(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "crowdCharacters",
        .doc = "Set the characters a crowd's agents wear (skinned characters' root nodes); each is characterised for motion when "
               "it isn't yet. Agents keep their character index, so the order matters. Returns how each one draws.",
        .positionalDoc = "the crowd",
        .flags = {nodeFlag({"characters", "c", T::List, nullptr, "Character root nodes, in order."}, "node")},
        .run = [](const Args& a) -> json {
            Graph& g = sceneGraph();
            const NodeId c = crowdArg(g, a.objects(), "crowdCharacters");
            if (a.has("characters"))
            {
                std::vector<std::string> list;
                for (const json& x : a.flags["characters"])
                    list.push_back(x.get<std::string>());
                try
                {
                    setCharacters(g, c, list);
                }
                catch (const CommandError& e)
                {
                    throw CommandError(std::string("crowdCharacters: ") + e.what());
                }
            }
            json out = json::array();
            const Node& n = *g.node(c);
            std::string list = n.as<std::string>(attrIndex(n, "characters"));
            std::vector<std::string> names;
            for (size_t s = 0; s <= list.size();)
            {
                size_t e = list.find(',', s);
                if (e == std::string::npos)
                    e = list.size();
                std::string x = list.substr(s, e - s);
                x.erase(0, x.find_first_not_of(' '));
                if (!x.empty())
                    names.push_back(x);
                s = e + 1;
            }
            for (const std::string& ch : names)
            {
                std::shared_ptr<const CharacterParts> p = partsFor(g, ch);
                out.push_back({{"character", ch}, {"pieces", p->pieces.size()}, {"problem", p->problem}});
            }
            return out;
        },
        .examples = {"crowdCharacters('crowd', characters=['Manny', 'Quinn'])"},
    });

    ctx.registerCommand({
        .name = "crowdBake",
        .doc = "Bake crowd agents onto real characters: each agent gets its own copy of its character, keyed every frame (30 Hz) "
               "with its motion, feet planted where the motion has contact. The copies go under one group. Reports any frames "
               "that came from gait cycles rather than generated motion.",
        .positionalDoc = "the crowd",
        .flags = {
            {"agents", "a", T::List, nullptr, "Agent indices to bake; default every agent."},
            {"start", "st", T::Int, nullptr, "First frame; default the later of the crowd's start and the playback start."},
            {"end", "et", T::Int, nullptr, "Last frame; default the playback end."},
            {"group", "g", T::String, nullptr, "Name of the group the copies go under; default <crowd>_baked."},
            {"plantFeet", "pf", T::Bool, true, "Hold feet still where the motion has contact."},
            {"lockFeet", "lf", T::Bool, true, "Key the foot lock of the characters' IK chains where feet are planted."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = sceneGraph();
            const NodeId c = crowdArg(g, a.objects(), "crowdBake");
            json report;
            bake(g, c, a, "crowdBake", report);
            return report;
        },
        .examples = {"crowdBake('crowd', agents=[0, 1, 2])", "crowdBake('crowd', start=1, end=240)"},
    });

    ctx.registerCommand({
        .name = "crowdExport",
        .doc = "Bake crowd agents and export them: one FBX or glTF per agent into a folder, or every agent in one file. Returns the "
               "files written and the bake report.",
        .positionalDoc = "the crowd",
        .flags = {
            {"path", "p", T::String, nullptr, "Folder (one file per agent) or file (combined)."},
            {"format", "fm", T::String, "fbx", "fbx or gltf (.glb).", {"fbx", "gltf"}},
            {"combined", "cb", T::Bool, false, "Every agent in one file at path, each node named with its agent's prefix so names stay unique."},
            {"agents", "a", T::List, nullptr, "Agent indices; default every agent."},
            {"start", "st", T::Int, nullptr, "First frame."},
            {"end", "et", T::Int, nullptr, "Last frame."},
            {"keep", "k", T::Bool, true, "Keep the baked characters in the scene afterwards."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = sceneGraph();
            const NodeId c = crowdArg(g, a.objects(), "crowdExport");
            if (!a.has("path"))
                throw CommandError("crowdExport: give a path");
            const std::string fmt = a.get<std::string>("format", "fbx");
            const bool combined = a.get<bool>("combined", false);
            json report;
            const std::vector<Baked> baked = bake(g, c, a, "crowdExport", report);
            const std::string cmd = fmt == "gltf" ? "exportGltf" : "exportFbx";
            const std::string ext = fmt == "gltf" ? ".glb" : ".fbx";
            auto exportOne = [&](const fs::path& file, const json& objs) {
                json call = {{"args", json::array({paths::toUtf8(file)})}, {"start", double(report["start"].get<int64_t>())},
                             {"end", double(report["end"].get<int64_t>())}};
                for (const json& o : objs)
                    call["args"].push_back(o);
                if (fmt == "gltf")
                    call["binary"] = true;
                else
                    call["kind"] = "skeletal";
                ops::execute(cmd, call);
            };
            json files = json::array();
            if (combined)
            {
                // One file holds every agent, so names (joints included) carry the agent's so they stay unique in it.
                for (const Baked& b : baked)
                {
                    const std::string prefix = g.node(b.root)->name() + "_";
                    for (NodeId d : g.descendants(b.root))
                        g.rename(d, prefix + g.node(d)->name());
                }
                fs::path file = paths::fromUtf8(a.get<std::string>("path"));
                if (file.extension().empty())
                    file += ext;
                fs::create_directories(file.parent_path());
                exportOne(file, json::array({report["group"]}));
                files.push_back(paths::toUtf8(file));
            }
            else
            {
                const fs::path dir = folderArg(a.get<std::string>("path"));
                for (const Baked& b : baked)
                {
                    const fs::path file = dir / (assetName(g.node(b.root)->name()) + ext);
                    exportOne(file, json::array({g.path(b.root)}));
                    files.push_back(paths::toUtf8(file));
                }
            }
            if (!a.get<bool>("keep", true))
                ops::execute("delete", {{"args", {report["group"]}}});
            report["files"] = files;
            return report;
        },
        .examples = {"crowdExport('crowd', path='D:/out/crowd', format='fbx')", "crowdExport('crowd', path='D:/out/crowd.glb', format='gltf', combined=True)"},
    });

    ctx.registerCommand({
        .name = "crowdSendToUnreal",
        .doc = "Send a crowd to the connected Unreal editor as a Level Sequence: each character's skeletal mesh is imported once, each "
               "agent's baked animation becomes its own animation asset on that skeleton, and the sequence holds one skeletal mesh "
               "actor per agent playing it, placed in the open level. With mass=True it goes as a live Mass crowd instead: the "
               "network's sidewalks and crossings become ZoneGraph lanes, and the groups walking them a StateTree, Mass entity "
               "config and spawner each, so Unreal simulates them (needs the LilaLinkCrowd plugin).",
        .positionalDoc = "the crowd",
        .flags = {
            {"folder", "fo", T::String, "/Game/Crowds", "Content folder for the meshes, animations and the sequence."},
            {"sequence", "sq", T::String, nullptr, "Level Sequence name; default LS_<crowd>."},
            {"agents", "a", T::List, nullptr, "Agent indices; default every agent."},
            {"start", "st", T::Int, nullptr, "First frame."},
            {"end", "et", T::Int, nullptr, "Last frame."},
            {"keep", "k", T::Bool, false, "Keep the baked characters in the scene afterwards."},
            {"mass", "ms", T::Bool, false, "Send a live Mass crowd on the network's lanes instead of a baked sequence."},
            {"count", "n", T::Int, nullptr, "mass: agents to spawn; default as many as walk the network in Lila.", {}, {}, 0.0, 100000.0},
            {"density", "dn", T::Float, nullptr, "mass: spawn spacing as agents per 100 m of walking lane; default from count.", {}, {}, 0.01},
            {"crossings", "cx", T::Bool, true, "mass: include the crossings, so agents cross the roads there."},
            {"characters", "chs", T::Bool, true, "mass: agents show as the crowd's characters, walking, near the camera."},
            {"actorDistance", "ad", T::Float, 5000.0, "mass: within this many cm of the camera agents show as their characters.", {}, {}, 0.0},
            {"visibleDistance", "vd", T::Float, 15000.0, "mass: past this many cm agents aren't drawn (they still walk).", {}, {}, 0.0},
            {"maxActors", "mxa", T::Int, 200, "mass: at most this many agents show as characters at once.", {}, {}, 0.0},
            {"phaseSeconds", "ps", T::Float, 20.0, "mass: seconds each signal phase lasts; crossings at signals open in the phase that walks them.", {}, {}, 1.0},
            {"clearSeconds", "cs", T::Float, 4.0, "mass: seconds at the end of a walk phase when no one starts across.", {}, {}, 0.0},
            {"mesh", "me", T::String, "/Engine/BasicShapes/Cylinder.Cylinder", "mass: static mesh each agent shows as; empty for none."},
            {"dryRun", "dr", T::Bool, false, "mass: return what would be sent without sending."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = sceneGraph();
            const NodeId c = crowdArg(g, a.objects(), "crowdSendToUnreal");
            if (a.get<bool>("mass", false))
                return sendMass(g, c, a);
            const std::string folder = a.get<std::string>("folder", "/Game/Crowds");
            json report;
            std::vector<Baked> baked;
            {
                baked = bake(g, c, a, "crowdSendToUnreal", report);
            }
            const fs::path cache = fs::temp_directory_path() / "lila_crowds" / assetName(g.node(c)->name());
            fs::create_directories(cache);
            std::shared_ptr<const CrowdView> v = viewOf(g, c);
            // One skeletal mesh per character, from its first baked agent (bind pose), then an animation per agent.
            std::map<int, std::pair<std::string, std::string>> meshOf; // character -> mesh, skeleton asset
            json agents = json::array();
            progress::Scope bar("Sending the crowd to Unreal", double(baked.size()), progress::Cancellable);
            for (const Baked& b : baked)
            {
                bar.checkCancel();
                if (!meshOf.contains(b.character))
                {
                    const std::string name = "SK_" + assetName(g.node(nodeNamed(g, v->characters[size_t(b.character)]))->name());
                    const fs::path file = cache / (name + ".fbx");
                    ops::execute("exportFbx", {{"args", {paths::toUtf8(file), g.path(b.root)}}, {"kind", "skeletal"}, {"animation", false}});
                    const json r = ops::execute("unrealImport", {{"args", {paths::toUtf8(file)}}, {"contentPath", folder + "/" + name}, {"kind", "skeletal"}, {"wait", true}});
                    std::string mesh = folder + "/" + name, skeleton;
                    for (const json& as : r.value("assets", json::array()))
                    {
                        const std::string p = as.is_string() ? as.get<std::string>() : as.value("path", "");
                        if (p.find("Skeleton") != std::string::npos)
                            skeleton = p;
                    }
                    if (skeleton.empty())
                        skeleton = mesh + "_Skeleton";
                    meshOf[b.character] = {mesh, skeleton};
                }
                const auto& [mesh, skeleton] = meshOf[b.character];
                const std::string anim = "A_" + assetName(g.node(b.root)->name());
                const fs::path file = cache / (anim + ".fbx");
                ops::execute("exportFbx", {{"args", {paths::toUtf8(file), g.path(b.root)}},
                                           {"kind", "animation"},
                                           {"start", double(report["start"].get<int64_t>())},
                                           {"end", double(report["end"].get<int64_t>())}});
                ops::execute("unrealImport", {{"args", {paths::toUtf8(file)}}, {"contentPath", folder + "/" + anim}, {"kind", "animation"}, {"skeleton", skeleton}, {"wait", true}});
                agents.push_back({{"name", g.node(b.root)->name()}, {"mesh", mesh}, {"animation", folder + "/" + anim}});
                bar.step();
            }
            const std::string seq = a.get<std::string>("sequence", "LS_" + assetName(g.node(c)->name()));
            const json call = {{"args", {"createCrowdSequence"}},
                               {"params",
                                {{"path", folder + "/" + seq},
                                 {"fps", v->fps},
                                 {"frames", report["end"].get<int64_t>() - report["start"].get<int64_t>()},
                                 {"agents", agents}}},
                               {"timeoutMs", 600000}};
            const json r = ops::execute("unrealCall", call);
            if (!r.value("ok", false))
                throw CommandError("crowdSendToUnreal: Unreal could not make the sequence: " + r.value("error", r.dump()));
            if (!a.get<bool>("keep", false))
                ops::execute("delete", {{"args", {report["group"]}}});
            report["sequence"] = folder + "/" + seq;
            report["unreal"] = r.value("result", json());
            return report;
        },
        .examples = {"crowdSendToUnreal('crowd', folder='/Game/Crowds/Plaza')", "crowdSendToUnreal('crowd', mass=True, count=200)"},
    });
}
} // namespace crowds
