#include "scene/Ghosts.h"

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Poses.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace lila::scene::ghosts
{
namespace
{
constexpr double kTicksPerSecond = 60000.0;
constexpr size_t kMaxFrames = 600;

int64_t ticks(double t) { return std::llround(t * kTicksPerSecond); }

double fps()
{
    double f = anim::playback().fps;
    return f > 0.0 ? f : 30.0;
}
} // namespace

Settings Settings::fromPrefs()
{
    const PrefStore& p = PrefStore::get();
    Settings s;
    s.enabled = p.value<bool>("general", kEnabled, false);
    s.keys = p.value<std::string>("general", kMode, "keys") != "frames";
    s.before = std::clamp(p.value<int>("general", kBefore, 2), 0, 50);
    s.after = std::clamp(p.value<int>("general", kAfter, 2), 0, 50);
    s.step = std::clamp(p.value<int>("general", kStep, 1), 1, 1000);
    s.opacity = std::clamp(p.value<double>("general", kOpacity, 0.3), 0.0, 1.0);
    const std::string show = p.value<std::string>("general", kShow, "mesh");
    s.mesh = show != "skeleton";
    s.skeleton = show != "mesh";
    s.range = p.value<bool>("general", kRange, true);
    return s;
}

std::vector<GhostTime> rangeGhostTimes(const Settings& s, double now, const std::vector<double>& keys, double fps, const anim::TimeSelection& range)
{
    std::vector<GhostTime> out;
    if (!range.active)
        return out;
    if (fps <= 0.0)
        fps = 30.0;
    const double eps = 0.5 / fps;
    const double from = anim::frameOf(range.start) - range.before(), to = anim::frameOf(range.end) + range.after();
    std::vector<double> times;
    if (s.keys)
    {
        for (double t : keys)
            if (anim::frameOf(t) >= from - 1e-6 && anim::frameOf(t) <= to + 1e-6)
                times.push_back(t);
    }
    else
        for (double f = std::ceil(from - 1e-6); f <= to + 1e-6; f += std::max(1, s.step))
            times.push_back(anim::secondsOf(f));
    std::erase_if(times, [&](double t) { return std::abs(t - now) < eps || anim::timeSelectionWeight(range, t) <= 0.0; });
    if (times.size() > size_t(kMaxRangeGhosts))
    {
        std::vector<double> picked;
        for (int i = 0; i < kMaxRangeGhosts; ++i)
            picked.push_back(times[size_t(std::lround(double(i) * double(times.size() - 1) / double(kMaxRangeGhosts - 1)))]);
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        times = std::move(picked);
    }
    const int before = int(std::count_if(times.begin(), times.end(), [&](double t) { return t < now; }));
    for (int i = 0; i < int(times.size()); ++i)
        out.push_back({times[size_t(i)], i < before ? i - before : i - before + 1});
    return out;
}

std::vector<GhostTime> ghostTimes(const Settings& s, double now, const std::vector<double>& keys, double fps)
{
    std::vector<GhostTime> out;
    if (fps <= 0.0)
        fps = 30.0;
    const double eps = 0.5 / fps;
    if (!s.keys)
    {
        const double dt = double(std::max(1, s.step)) / fps;
        for (int k = 1; k <= s.before; ++k)
            out.push_back({now - k * dt, -k});
        for (int k = 1; k <= s.after; ++k)
            out.push_back({now + k * dt, k});
        return out;
    }
    const auto step = ptrdiff_t(std::max(1, s.step));
    // Last key before now and first key after it, outside half a frame.
    const auto before = std::lower_bound(keys.begin(), keys.end(), now - eps) - keys.begin() - 1;
    const auto after = std::upper_bound(keys.begin(), keys.end(), now + eps) - keys.begin();
    for (int k = 1; k <= s.before; ++k)
    {
        ptrdiff_t i = before - (k - 1) * step;
        if (i < 0)
            break;
        out.push_back({keys[size_t(i)], -k});
    }
    for (int k = 1; k <= s.after; ++k)
    {
        ptrdiff_t i = after + (k - 1) * step;
        if (i >= ptrdiff_t(keys.size()))
            break;
        out.push_back({keys[size_t(i)], k});
    }
    return out;
}

Cache::Cache(Graph& g) : mGraph(g)
{
    mListener = g.addListener([this](Change c, NodeId id) {
        switch (c)
        {
        case Change::Value:
            // Curves, layers and blends are DG nodes; a DAG value (posing without a key) leaves the ghosts alone.
            if (Node* n = mGraph.node(id); n && !n->type().dag)
                invalidate();
            break;
        case Change::Selection:
        case Change::Renamed: break;
        default:
            // Hierarchy, connections, nodes coming or going: the targets may differ too.
            mTargetsDirty = true;
            invalidate();
            break;
        }
    });
}

Cache::~Cache()
{
    mGraph.removeListener(mListener);
}

void Cache::invalidate()
{
    mFrames.clear();
    mKeysValid = false;
}

void Cache::refreshTargets()
{
    if (!mTargetsDirty && mGraph.selection() == mSelection)
        return;
    mTargetsDirty = false;
    mSelection = mGraph.selection();
    std::vector<NodeId> roots;
    auto addRoot = [&](NodeId id) {
        if (std::find(roots.begin(), roots.end(), id) == roots.end())
            roots.push_back(id);
    };
    for (NodeId id : mSelection)
    {
        Node* n = mGraph.node(id);
        if (!n)
            continue;
        if (rig::isJoint(*n))
            addRoot(poses::characterOf(mGraph, id).root);
        else
            addRoot(id);
        // A skinned mesh brings its skeleton.
        if (n->type().isA("mesh"))
            if (NodeId skin = skin::find(mGraph, id); skin != kNoNode)
                for (const skin::Influence& inf : skin::influences(mGraph, skin))
                    if (inf.joint != kNoNode)
                    {
                        addRoot(poses::characterOf(mGraph, inf.joint).root);
                        break;
                    }
    }
    std::unordered_set<NodeId> seen;
    mTargets.clear();
    for (NodeId r : roots)
    {
        if (r == kNoNode)
            continue;
        if (seen.insert(r).second)
            mTargets.push_back(r);
        for (NodeId d : mGraph.descendants(r))
            if (seen.insert(d).second)
                mTargets.push_back(d);
    }
    mMeshes.clear();
    std::unordered_set<NodeId> meshSeen;
    for (NodeId id : mTargets)
        if (mGraph.node(id)->type().isA("mesh") && meshSeen.insert(id).second)
            mMeshes.push_back(id);
    for (NodeId id : mGraph.nodes())
    {
        if (meshSeen.contains(id) || !mGraph.node(id)->type().isA("mesh"))
            continue;
        NodeId skin = skin::find(mGraph, id);
        if (skin == kNoNode)
            continue;
        for (const skin::Influence& inf : skin::influences(mGraph, skin))
            if (inf.joint != kNoNode && seen.contains(inf.joint))
            {
                meshSeen.insert(id);
                mMeshes.push_back(id);
                break;
            }
    }
    invalidate();
}

bool Cache::anyHeld() const
{
    for (const auto& [dst, src] : mGraph.connections())
        if (mGraph.isHeld(dst))
            return true;
    return false;
}

std::shared_ptr<const Frame> Cache::capture(double t)
{
    // The caller has set the time and evaluated.
    auto f = std::make_shared<Frame>();
    f->time = t;
    for (NodeId id : mMeshes)
    {
        Node* n = mGraph.node(id);
        if (!n || !effectivelyVisible(mGraph, id))
            continue;
        MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (m)
            f->meshes.push_back({id, std::move(m), n->as<glm::dmat4>(Transform::kWorldMatrix)});
    }
    for (NodeId id : mTargets)
    {
        Node* n = mGraph.node(id);
        NodeId parent = mGraph.parent(id);
        if (!n || !rig::isJoint(*n) || parent == kNoNode || !rig::isJoint(*mGraph.node(parent)))
            continue;
        f->bones.push_back({glm::dvec3(mGraph.node(parent)->as<glm::dmat4>(Transform::kWorldMatrix)[3]),
                            glm::dvec3(n->as<glm::dmat4>(Transform::kWorldMatrix)[3])});
    }
    ++mCaptures;
    return f;
}

std::vector<Ghost> Cache::ghosts(const Settings& s)
{
    LILA_ZONE();

    std::vector<Ghost> out;
    if (!s.enabled || (!s.mesh && !s.skeleton))
        return out;
    refreshTargets();
    if (mTargets.empty())
        return out;
    if (!mKeysValid)
    {
        mKeys = anim::keyTimes(mGraph, mTargets);
        mKeysValid = true;
    }
    const double now = mGraph.time();
    std::vector<GhostTime> times = timesNow(s);
    std::vector<double> missing;
    for (const auto& [t, offset] : times)
        if (!mFrames.contains(ticks(t)) && !mPreview.contains(ticks(t)))
            missing.push_back(t);
    if (!missing.empty() && !anyHeld())
    {
        if (mFrames.size() + missing.size() > kMaxFrames)
            mFrames.clear();
        for (double t : missing)
        {
            mGraph.setTime(t);
            mGraph.evaluate();
            mFrames[ticks(t)] = capture(t);
        }
        mGraph.setTime(now);
        mGraph.evaluate();
    }
    const int before = int(std::count_if(times.begin(), times.end(), [](const GhostTime& g) { return g.second < 0; }));
    const int after = int(times.size()) - before;
    const anim::TimeSelection& range = anim::timeSelection();
    const bool spread = s.range && range.active;
    for (const auto& [t, offset] : times)
    {
        Ghost g{.offset = offset, .count = std::max(1, offset < 0 ? before : after), .weight = spread ? anim::timeSelectionWeight(range, t) : 1.0};
        if (auto p = mPreview.find(ticks(t)); p != mPreview.end())
            g.frame = p->second;
        else if (auto it = mFrames.find(ticks(t)); it != mFrames.end())
            g.frame = it->second;
        if (g.frame)
            out.push_back(std::move(g));
    }
    return out;
}

std::vector<GhostTime> Cache::shownTimes(const Settings& s)
{
    if (!s.enabled || (!s.mesh && !s.skeleton))
        return {};
    refreshTargets();
    if (mTargets.empty())
        return {};
    if (!mKeysValid)
    {
        mKeys = anim::keyTimes(mGraph, mTargets);
        mKeysValid = true;
    }
    return timesNow(s);
}

std::vector<GhostTime> Cache::timesNow(const Settings& s) const
{
    if (const anim::TimeSelection& range = anim::timeSelection(); s.range && range.active)
        return rangeGhostTimes(s, mGraph.time(), mKeys, fps(), range);
    return ghostTimes(s, mGraph.time(), mKeys, fps());
}

std::shared_ptr<const Frame> Cache::captureNow(double t)
{
    refreshTargets();
    // Interactive edits defer the meshes; a ghost needs them as they are at t.
    mGraph.evaluateUpstream(mMeshes);
    return capture(t);
}

void Cache::setPreview(std::vector<std::shared_ptr<const Frame>> frames)
{
    mPreview.clear();
    for (std::shared_ptr<const Frame>& f : frames)
        if (f)
            mPreview[ticks(f->time)] = std::move(f);
}

Cache& cache()
{
    static Cache c(Graph::current());
    return c;
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "ghosting",
        .doc = "Animation ghosting of the selected characters: their meshes or skeletons at the previous and next keys, "
               "or every step frames. Sets the given options (user prefs) and returns them all.",
        .flags = {
            {"enable", "e", T::Bool, nullptr, "Show ghosts."},
            {"mode", "m", T::String, nullptr, "Ghost at keys, or at frames step apart.", {"keys", "frames"}},
            {"before", "b", T::Int, nullptr, "Ghosts before the current time.", {}, {}, 0.0, 50.0},
            {"after", "a", T::Int, nullptr, "Ghosts after the current time.", {}, {}, 0.0, 50.0},
            {"step", "st", T::Int, nullptr, "Frames apart (frames mode) or every nth key (keys mode).", {}, {}, 1.0, 1000.0},
            {"opacity", "o", T::Float, nullptr, "Opacity of the nearest ghost; farther ones fade.", {}, {}, 0.0, 1.0},
            {"show", "sh", T::String, nullptr, "What to draw.", {"mesh", "skeleton", "both"}},
            {"beforeColor", "bc", T::String, nullptr, "sRGB #rrggbb tint of ghosts before now."},
            {"afterColor", "ac", T::String, nullptr, "sRGB #rrggbb tint of ghosts after now."},
            {"range", "rg", T::Bool, nullptr, "While the time slider has a range selected, spread the ghosts across it and its falloff."},
            {"refresh", "r", T::Bool, false, "Recapture every ghost."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            PrefStore& p = PrefStore::get();
            if (a.has("enable"))
                p.set("general", kEnabled, a.get<bool>("enable"));
            if (a.has("mode"))
            {
                std::string m = a.get<std::string>("mode");
                if (m != "keys" && m != "frames")
                    throw CommandError("ghosting: mode is keys or frames");
                p.set("general", kMode, m);
            }
            if (a.has("show"))
            {
                std::string s = a.get<std::string>("show");
                if (s != "mesh" && s != "skeleton" && s != "both")
                    throw CommandError("ghosting: show is mesh, skeleton or both");
                p.set("general", kShow, s);
            }
            using Pair = std::pair<const char*, const char*>;
            for (const auto& [flag, key] : {Pair{"before", kBefore}, Pair{"after", kAfter}, Pair{"step", kStep}})
                if (a.has(flag))
                    p.set("general", key, std::max(flag == std::string("step") ? 1 : 0, a.get<int>(flag)));
            if (a.has("opacity"))
                p.set("general", kOpacity, std::clamp(a.get<double>("opacity"), 0.0, 1.0));
            if (a.has("range"))
                p.set("general", kRange, a.get<bool>("range"));
            for (const auto& [flag, key] : {Pair{"beforeColor", kBeforeColor}, Pair{"afterColor", kAfterColor}})
                if (a.has(flag))
                    p.set("general", key, a.get<std::string>(flag));
            if (a.get<bool>("refresh"))
                cache().invalidate();
            Settings s = Settings::fromPrefs();
            return {{"enable", s.enabled},
                    {"mode", s.keys ? "keys" : "frames"},
                    {"before", s.before},
                    {"after", s.after},
                    {"step", s.step},
                    {"opacity", s.opacity},
                    {"show", s.mesh && s.skeleton ? "both" : s.mesh ? "mesh" : "skeleton"},
                    {"beforeColor", p.value<std::string>("general", kBeforeColor, kDefaultBeforeColor)},
                    {"afterColor", p.value<std::string>("general", kAfterColor, kDefaultAfterColor)},
                    {"range", s.range}};
        },
    });
}
} // namespace lila::scene::ghosts
