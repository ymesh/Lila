// Fill: generated motion between a character's keys, keyed back as ordinary keys marked "generated" in the Stacker.

#include "motion/PuppeteerMotion.h"

#include "anim/AnimOps.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Motion.h>
#include <lila/Node.h>
#include <lila/Rig.h>

#include <algorithm>
#include <cmath>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::motionfill
{
namespace
{
namespace sa = lila::scene::anim;
namespace mo = lila::motion;
using T = ArgType;
using anim::Mark;
using anim::Marks;
using anim::TimeWarp;

Graph& graph() { return Graph::current(); }
double sceneFps() { return std::max(1.0, sa::playback().fps); }

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

/** One gap to fill, between two of the animator's keys. */
struct Gap
{
    double from = 0.0, to = 0.0;
};

/** A generated stretch on the character's joints, sampled at 30 Hz. */
struct Filled
{
    std::vector<retarget::Pose> poses;
    int tokens = 0;
    int generations = 0;
};

/** Generated SOMA frames from the last context pose to the first target pose: exactly frames long when fixed. */
std::vector<mo::Pose> generate(mo::Engine& engine, const std::array<mo::Pose, 4>& context, const std::vector<mo::Pose>& target, int frames,
                               uint16_t modelTokens, glm::vec2 heading, int& tokensOut, int& generations, std::vector<mo::Contacts>& contacts)
{
    mo::Player player(context.back());
    player.seed(context);
    std::vector<mo::Pose> out{context.back()};
    contacts.assign(1, mo::Contacts{});
    if (frames <= 0)
    {
        // The model chooses the length.
        const mo::Request r = player.request(target, modelTokens, heading);
        mo::Result res = engine.generate(std::span(&r, 1)).front();
        if (!res.error.empty())
            throw std::runtime_error(res.error);
        tokensOut = res.tokens;
        ++generations;
        contacts = res.contacts;
        return std::move(res.frames);
    }
    // Long gaps go in steps: each generation heads for the target but only its first part is kept.
    int remaining = frames;
    while (remaining > 0)
    {
        const bool last = remaining <= mo::kMaxFrames - 1;
        const int tokens = last ? mo::tokensForFrames(remaining + 1) : mo::kMaxTokens;
        const mo::Request r = player.request(target, mo::tokenMask(tokens, tokens), heading);
        mo::Result res = engine.generate(std::span(&r, 1)).front();
        if (!res.error.empty())
            throw std::runtime_error(res.error);
        tokensOut = res.tokens;
        ++generations;
        const int length = int(res.frames.size()) - 1;
        const int keep = last ? length : std::clamp(remaining - 30, 12, 40);
        player.play(std::move(res), length - keep);
        for (int k = 0; k < keep; ++k)
        {
            mo::Contacts c{};
            out.push_back(player.step(&c));
            contacts.push_back(c);
        }
        remaining -= keep;
        if (last)
            break;
    }
    return out;
}

/** The character pose at fraction u (0..1) of a stretch, between its 30 Hz samples. */
retarget::Pose sampleAt(const std::vector<retarget::Pose>& poses, double u)
{
    const double x = std::clamp(u, 0.0, 1.0) * double(poses.size() - 1);
    const size_t i = std::min(size_t(x), poses.size() - 1), k = std::min(i + 1, poses.size() - 1);
    const double t = x - double(i);
    retarget::Pose p = poses[i];
    for (size_t j = 0; j < p.rotation.size(); ++j)
    {
        p.rotation[j] = glm::slerp(poses[i].rotation[j], poses[k].rotation[j], t);
        p.translate[j] = glm::mix(poses[i].translate[j], poses[k].translate[j], t);
    }
    return p;
}

json runFill(const Args& a)
{
    Graph& g = graph();
    const std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError("motionFill: select a character or name a node in it");
    Node* node = g.find(objs.front());
    if (!node)
        throw CommandError("motionFill: no node named '" + objs.front() + "'");
    mo::Engine* engine = mo::Engine::shared();
    if (!engine)
        throw CommandError("motionFill: " + mo::Engine::status().message);

    return guarded("motionFill", [&] {
        const anim::Character ch = anim::resolveCharacter(g, node->id());
        std::vector<NodeId> curves = anim::characterCurves(g, ch);
        Marks marks = anim::marksOf(g, ch);
        std::vector<double> keys;
        for (double t : anim::keyTimesOf(g, curves))
            if (marks.get(t) != Mark::Generated)
                keys.push_back(t);
        if (keys.size() < 2)
            throw std::invalid_argument("the character needs two keys to fill between");

        // Which gaps: from/to times, else the one around the current time, else all with all.
        std::vector<Gap> gaps;
        const double now = a.has("time") ? a.get<double>("time") : g.time();
        const double lo = a.has("from") ? a.get<double>("from") : a.get<bool>("all", false) ? keys.front() : now;
        const double hi = a.has("to") ? a.get<double>("to") : a.get<bool>("all", false) ? keys.back() : now;
        for (size_t i = 0; i + 1 < keys.size(); ++i)
        {
            const bool around = lo == hi ? (keys[i] <= lo + 1e-6 && lo < keys[i + 1] - 1e-6) : (keys[i + 1] > lo + 1e-6 && keys[i] < hi - 1e-6);
            if (around)
                gaps.push_back({keys[i], keys[i + 1]});
        }
        if (gaps.empty())
            throw std::invalid_argument(lo == hi ? "the time is not between two keys" : "no keys in that range");

        const std::string timing = a.get<std::string>("timing", "keys");
        if (timing != "keys" && timing != "model")
            throw std::invalid_argument("timing is keys (keep the key times) or model (the model chooses each gap's length)");
        const int every = int(std::clamp<int64_t>(a.get<int64_t>("every", 2), 1, 30));
        const int targetPoses = int(std::clamp<int64_t>(a.get<int64_t>("targetPoses", 1), 1, 4));
        const uint32_t variant = uint32_t(a.get<int64_t>("variant", 0));
        const uint16_t modelTokens = mo::tokenMask(int(a.get<int64_t>("minTokens", 6)), int(a.get<int64_t>("maxTokens", 16)));
        if (!modelTokens)
            throw std::invalid_argument("minTokens to maxTokens allows no length");
        const double fps = sceneFps();

        // The skeleton under the character root carries the motion.
        NodeId joint = ch.root;
        if (!rig::isJoint(*g.node(joint)))
            for (NodeId id : ch.nodes)
                if (rig::isJoint(*g.node(id)))
                {
                    joint = id;
                    break;
                }
        const mo::CharacterMap map = mo::CharacterMap::forJoint(g, joint);
        const retarget::SceneSkeleton& sk = map.scene();
        const retarget::Pose rest = retarget::currentPose(g, sk);
        // Joints above the pelvis (a root) keep the animator's own motion unless root motion is asked for.
        std::vector<bool> above(sk.joints.size(), false);
        if (const int pelvis = sk.skeleton.find(map.rig().pelvis); pelvis >= 0 && !a.get<bool>("rootMotion", false))
            for (int j = sk.skeleton.parent[size_t(pelvis)]; j >= 0; j = sk.skeleton.parent[size_t(j)])
                above[size_t(j)] = true;

        json report = json::array();
        int keyed = 0;
        for (size_t gi = 0; gi < gaps.size(); ++gi)
        {
            Gap gap = gaps[gi];
            // Refilling replaces what an earlier fill keyed here.
            for (double t : anim::keyTimesOf(g, curves))
                if (t > gap.from + 1e-6 && t < gap.to - 1e-6 && marks.get(t) == Mark::Generated)
                {
                    anim::removeKeysAt(g, curves, t);
                    marks.set(t, Mark::None);
                }
            curves = anim::characterCurves(g, ch);

            std::array<mo::Pose, 4> context;
            for (int k = 0; k < 4; ++k)
                context[size_t(k)] = map.sampleSoma(g, gap.from - double(3 - k) / mo::kSampleRate);
            std::vector<mo::Pose> target;
            for (int k = 0; k < targetPoses; ++k)
                target.push_back(map.sampleSoma(g, gap.to + double(k) / mo::kSampleRate));

            const int frames = timing == "keys" ? int(std::lround((gap.to - gap.from) * mo::kSampleRate)) : 0;
            int tokens = 0, generations = 0;
            std::vector<mo::Contacts> contacts;
            const std::vector<mo::Pose> soma =
                generate(*engine, context, target, frames, modelTokens, mo::variantHeading(variant + uint32_t(gi) * 7u), tokens, generations, contacts);
            if (soma.size() < 2)
                throw std::runtime_error("the model returned no motion");

            if (timing == "model")
            {
                const double end = gap.from + double(soma.size() - 1) / mo::kSampleRate;
                const double delta = end - gap.to;
                if (std::abs(delta) > 1e-6)
                {
                    TimeWarp w;
                    w.knots = {{gap.from, gap.from}, {gap.to, end}};
                    anim::warpCurves(g, curves, w);
                    marks.warp(w);
                    for (Gap& later : gaps)
                        if (later.from >= gap.to - 1e-6)
                            later.from = w.map(later.from), later.to = w.map(later.to);
                    gap.to = end;
                    sa::Playback& pb = sa::playback();
                    if (pb.maxTime < end)
                        pb.maxTime = end;
                }
                curves = anim::characterCurves(g, ch);
            }

            mo::MapOptions mapOpts;
            mapOpts.rootMotion = a.get<bool>("rootMotion", false);
            mapOpts.plantFeet = a.get<bool>("plantFeet", true);
            const retarget::Result solved = map.toCharacter(soma, contacts, mapOpts);

            // Keys on the scene's frame grid, every few frames, never on the animator's own keys.
            anim::KeyWriter writer(g, ch.nodes);
            const double step = double(every) / fps;
            const double first = std::ceil(gap.from * fps - 1e-6) / fps;
            std::vector<double> times;
            for (double t = first; t < gap.to - 0.5 / fps; t += step)
                if (t > gap.from + 0.5 / fps)
                    times.push_back(t);
            for (double t : times)
            {
                const retarget::Pose p = sampleAt(solved.poses, (t - gap.from) / (gap.to - gap.from));
                json pose = json::object();
                for (size_t j = 0; j < sk.joints.size(); ++j)
                {
                    if (above[j])
                        continue;
                    const glm::dquat q = glm::normalize(p.rotation[j]);
                    json e = {{"rotate", {q.x, q.y, q.z, q.w}}};
                    if (glm::length(p.translate[j] - rest.translate[j]) > 1e-4 || int(j) == sk.skeleton.find(map.rig().pelvis))
                        e["translate"] = {p.translate[j].x, p.translate[j].y, p.translate[j].z};
                    pose[g.node(sk.joints[j])->name()] = e;
                }
                writer.write(pose, t, {"translate", "rotate"});
                marks.set(t, Mark::Generated);
            }
            keyed += writer.commit();
            curves = anim::characterCurves(g, ch);
            report.push_back({{"from", gap.from}, {"to", gap.to}, {"frames", int(soma.size()) - 1}, {"tokens", tokens},
                              {"generations", generations}, {"keys", times.size()}});
        }
        anim::setMarks(g, ch, marks);
        return json{{"gaps", report}, {"channelKeys", keyed}, {"device", engine->info().device}, {"variant", variant}};
    });
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "motionFill",
        .doc = "Fill between a character's keys with generated motion: the poses before a key lead in, the next key is the target, "
               "and the result is keyed as ordinary keys marked generated (the animator's keys stay exact). Filling again "
               "replaces earlier generated keys. Returns each gap filled.",
        .positionalDoc = "a node in the character (default the selection)",
        .flags = {
            {"time", "t", T::Float, nullptr, "Fill the gap around this time (default: the current time)."},
            {"from", "fr", T::Float, nullptr, "Fill every gap from this time..."},
            {"to", "to", T::Float, nullptr, "...to this time."},
            {"all", "a", T::Bool, false, "Fill every gap."},
            {"timing", "tm", T::String, "keys", "keys: keep the key times. model: the model chooses each gap's length and later keys move.",
             {"keys", "model"}},
            {"minTokens", "mn", T::Int, int64_t(6), "With model timing, the shortest gap in tokens of 4 frames at 30 fps (6..16).", {}, {}, 6.0, 16.0},
            {"maxTokens", "mx", T::Int, int64_t(16), "With model timing, the longest gap (6..16).", {}, {}, 6.0, 16.0},
            {"every", "e", T::Int, int64_t(2), "Frames between generated keys (2 keys on twos).", {}, {}, 1.0, 30.0},
            {"targetPoses", "tp", T::Int, int64_t(1), "Poses from the next key on the model aims for: more carries its motion in.", {}, {}, 1.0, 4.0},
            {"variant", "v", T::Int, int64_t(0), "Other values give other motion through the same keys.", {}, {}, 0.0},
            {"plantFeet", "pf", T::Bool, true, "Hold feet still where the model predicts contact."},
            {"rootMotion", "rm", T::Bool, false, "Move the root joint under the pelvis too."},
        },
        .run = runFill,
        .examples = {"motionFill('Manny', all=True)", "motionFill('Manny', time=1.5, variant=2)", "motionFill('Manny', all=True, timing='model')"},
    });
}
} // namespace puppeteer::motionfill
