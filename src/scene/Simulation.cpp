#include "scene/Simulation.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"
#include "scene/SplineChain.h"
#include "scene/Take.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

namespace lila::scene::sim
{
namespace
{
constexpr int kMinPasses = 2;

struct Sim
{
    std::string nodeType; // the solver type it was made from
    std::unique_ptr<Solver> solver;
    Setup setup;
    uint64_t params = 0;
    bool loop = false;
    int64_t loopStart = 0, loopEnd = 0;
    /** Index 0 is the start frame (or the loop's first frame). */
    std::vector<std::shared_ptr<const Frame>> frames;
    uint64_t validGeneration = 0;
    Info info;
};

/** Simulations of the graph below, by node. Ordered, so anything iterating it does so the same way every run. */
std::map<NodeId, Sim>& sims()
{
    // Never destroyed: solvers can come from extension DLLs, which unload before this would.
    static auto* s = new std::map<NodeId, Sim>;
    return *s;
}

Graph* gGraph = nullptr;
int gListener = 0;
uint64_t gGeneration = 1;
/** Set while the framework itself moves time or mutes layers, so those edits don't invalidate anything. */
bool gSampling = false;

void watch(Graph& g)
{
    if (gGraph == &g)
        return;
    if (gGraph && gListener)
        gGraph->removeListener(gListener);
    sims().clear();
    gGraph = &g;
    gListener = g.addListener(
        [](Change c, NodeId id) {
            if (gSampling)
                return;
            switch (c)
            {
            case Change::Selection:
                return;
            case Change::Cleared:
                sims().clear();
                break;
            case Change::Deleted:
                sims().erase(id);
                break;
            default:
                break;
            }
            ++gGeneration;
        },
        true);
}

const SolverType* solverTypeFor(const Node& n)
{
    for (const NodeType* cur = &n.type(); cur; cur = cur->base)
        if (const SolverType* s = solverTypes().find(cur->name); s && !s->matches)
            return s;
    for (const SolverType* s : solverTypes().all())
        if (s->matches && s->matches(n))
            return s;
    return nullptr;
}

Sim& simFor(Graph& g, NodeId node)
{
    watch(g);
    Node* n = g.node(node);
    if (!n)
        throw std::invalid_argument("no such node");
    const SolverType* type = solverTypeFor(*n);
    if (!type)
        throw std::invalid_argument(n->path() + " is not a simulation");
    Sim& s = sims()[node];
    if (!s.solver || s.nodeType != type->nodeType)
    {
        s = Sim{};
        s.nodeType = type->nodeType;
        s.solver = type->create(g, node);
        if (!s.solver)
            throw std::invalid_argument(n->path() + ": its solver could not be made");
    }
    return s;
}

double fps() { return std::max(anim::playback().fps, 1e-6); }

/** Seconds of step `index` after startFrame, from integers so a step's time never depends on how it was reached. */
double stepSeconds(int64_t startFrame, int substeps, int64_t index)
{
    return double(startFrame * substeps + index) / (double(substeps) * fps());
}

int64_t frameAt(double seconds) { return int64_t(std::llround(seconds * fps())); }

std::vector<std::string> recordings(Graph& g, NodeId node)
{
    Node* n = g.node(node);
    int a = n ? n->type().attrIndex(kRecordingsAttr) : -1;
    if (a < 0 || n->type().attrs[size_t(a)].type != PlugType::String)
        return {};
    json j = json::parse(n->as<std::string>(a), nullptr, false);
    std::vector<std::string> out;
    if (j.is_array())
        for (const json& e : j)
            if (e.is_string())
                out.push_back(e.get<std::string>());
    return out;
}

/**
 * Moves time for sampling and puts it back. Recorded layers of the simulation are muted meanwhile, with undo off, so
 * they never feed back into their own inputs.
 */
class SampleScope
{
public:
    SampleScope(Graph& g, NodeId node, const Setup& setup) : mGraph(g), mTime(g.time()), mReads(setup.reads)
    {
        for (const auto& [dst, src] : g.connections())
            if (g.isHeld(dst))
                throw std::runtime_error("a changed value isn't keyed yet; key it or change the time before simulating");
        gSampling = true;
        UndoStack& undo = UndoStack::get();
        mUndo = undo.enabled();
        undo.setEnabled(false);
        for (const std::string& name : recordings(g, node))
            if (Node* l = g.find(name); l && anim::isLayer(*l) && !l->as<bool>(anim::LayerNode::kMute))
            {
                g.setValue({l->id(), anim::LayerNode::kMute}, true);
                mMuted.push_back(l->id());
            }
    }
    ~SampleScope()
    {
        for (NodeId l : mMuted)
            if (mGraph.node(l))
                mGraph.setValue({l, anim::LayerNode::kMute}, false);
        mGraph.setTime(mTime, false);
        mGraph.evaluate();
        UndoStack::get().setEnabled(mUndo);
        gSampling = false;
    }

    void at(Solver& solver, double seconds, std::vector<double>& inputs)
    {
        mGraph.setTime(seconds, false);
        if (mReads.empty())
            mGraph.evaluate();
        else
            mGraph.evaluateUpstream(mReads);
        inputs.clear();
        solver.sample(mGraph, inputs);
    }

private:
    Graph& mGraph;
    double mTime;
    std::vector<NodeId> mReads;
    bool mUndo = true;
    std::vector<NodeId> mMuted;
};

uint64_t hashValue(uint64_t v, uint64_t h) { return hashBytes(&v, sizeof v, h); }

/** Largest slot difference over its tolerance: <= 1 means the states match. */
double residual(const std::vector<Slot>& layout, const std::vector<double>& a, const std::vector<double>& b, const Tolerances& tol)
{
    constexpr double kDeg = 180.0 / 3.14159265358979323846;
    double worst = 0.0;
    size_t i = 0;
    for (const Slot& s : layout)
    {
        double sq = 0.0;
        for (uint32_t k = 0; k < s.count && i + k < a.size() && i + k < b.size(); ++k)
            sq += (a[i + k] - b[i + k]) * (a[i + k] - b[i + k]);
        double d = std::sqrt(sq), limit = 1.0;
        switch (s.channel)
        {
        case Channel::Position: limit = tol.position; break;
        case Channel::Velocity: limit = tol.velocity; break;
        case Channel::Rotation: d *= kDeg; limit = tol.rotation; break;
        case Channel::AngularVelocity: d *= kDeg; limit = tol.angularVelocity; break;
        case Channel::Other: limit = tol.other; break;
        case Channel::Ignore: d = 0.0; break;
        }
        worst = std::max(worst, d / std::max(limit, 1e-12));
        i += s.count;
    }
    return worst;
}

size_t stateSize(const Setup& s)
{
    size_t n = 0;
    for (const Slot& slot : s.layout)
        n += slot.count;
    return n;
}

std::shared_ptr<Frame> makeFrame(Sim& s, int64_t frame, uint64_t hash, const std::vector<double>& state, const std::vector<double>& inputs)
{
    auto f = std::make_shared<Frame>();
    f->frame = frame;
    f->hash = hash;
    f->state = state;
    s.solver->pose(inputs, state, f->world);
    return f;
}

/** Holds each pose `stepped` frames, counting from the first frame. */
void applyStepped(Sim& s)
{
    const int n = std::max(1, s.setup.stepped);
    if (n == 1)
        return;
    for (size_t k = 0; k < s.frames.size(); ++k)
        if (k % size_t(n) != 0)
        {
            auto f = std::make_shared<Frame>(*s.frames[k]);
            f->world = s.frames[k - k % size_t(n)]->world;
            s.frames[k] = std::move(f);
        }
}

std::vector<NodeId> withAncestors(Graph& g, const std::vector<NodeId>& nodes)
{
    std::set<NodeId> seen;
    std::vector<NodeId> out;
    for (NodeId n : nodes)
        for (NodeId cur = n; cur != kNoNode; cur = g.parent(cur))
            if (seen.insert(cur).second)
                out.push_back(cur);
    return out;
}

/** Reads the node again: a new setup or loop range throws the cache away. */
void configure(Graph& g, NodeId node, Sim& s, LoopMode mode)
{
    Setup su = s.solver->configure(g);
    if (!su.loops)
        mode = LoopMode::Off;
    su.substeps = std::max(1, su.substeps);
    su.stepped = std::max(1, su.stepped);
    bool loop = false;
    int64_t ls = 0, le = 0;
    if (mode != LoopMode::Off)
    {
        std::vector<NodeId> driving = su.outputs;
        driving.insert(driving.end(), su.sources.begin(), su.sources.end());
        std::optional<std::pair<int64_t, int64_t>> r = loopRange(g, withAncestors(g, driving));
        if (r && r->second > r->first)
        {
            loop = true;
            ls = r->first;
            le = r->second;
        }
        else if (mode == LoopMode::On)
        {
            const anim::Playback& p = anim::playback();
            ls = frameAt(p.minTime);
            le = frameAt(p.maxTime);
            loop = le > ls;
        }
    }
    uint64_t h = hashValue(su.paramsHash, 0xcbf29ce484222325ull);
    for (int64_t v : {su.startFrame, int64_t(su.substeps), int64_t(su.stepped), int64_t(loop), ls, le})
        h = hashValue(uint64_t(v), h);
    double rate = fps();
    h = hashBytes(&rate, sizeof rate, h);
    for (const Slot& slot : su.layout)
        h = hashValue((uint64_t(slot.channel) << 32) | slot.count, h);
    for (NodeId o : su.outputs)
        h = hashValue(o, h);
    for (const std::string& r : recordings(g, node))
        h = hashBytes(r.data(), r.size(), h);
    if (h != s.params)
    {
        s.frames.clear();
        s.info.steps = 0;
        s.params = h;
    }
    s.setup = std::move(su);
    s.loop = loop;
    s.loopStart = ls;
    s.loopEnd = le;
}

Tolerances tolerances() { return {}; }

void simulateLoop(Graph& g, NodeId node, Sim& s)
{
    LILA_ZONE();

    const int sub = s.setup.substeps;
    const int64_t period = s.loopEnd - s.loopStart;
    const int64_t steps = period * sub;
    if (s.validGeneration == gGeneration && int64_t(s.frames.size()) == period + 1)
        return;
    const double dt = 1.0 / (double(sub) * fps());
    const double periodSeconds = double(period) / fps();

    // The whole period is sampled once; a looping animation's end is its start, so step `steps` reuses step 0.
    std::vector<std::vector<double>> inputs(static_cast<size_t>(steps));
    uint64_t h = s.params;
    {
        SampleScope scope(g, node, s.setup);
        for (int64_t i = 0; i < steps; ++i)
        {
            scope.at(*s.solver, stepSeconds(s.loopStart, sub, i), inputs[size_t(i)]);
            h = hashDoubles(inputs[size_t(i)], h);
        }
    }
    s.validGeneration = gGeneration;
    if (!s.frames.empty() && s.frames.back()->hash == h)
        return;
    s.frames.clear();
    s.info.lastInvalidated = s.loopStart;

    const size_t n = stateSize(s.setup);
    std::vector<double> state(n, 0.0);
    s.solver->reset(inputs[0], state);
    if (!s.setup.variableState)
        state.resize(n, 0.0);
    auto pass = [&](const std::function<void(int64_t step)>& after) {
        for (int64_t i = 1; i <= steps; ++i)
        {
            const int64_t wrapped = i % steps;
            StepInfo at{.index = wrapped, .seconds = stepSeconds(s.loopStart, sub, wrapped), .dt = dt, .period = periodSeconds};
            s.solver->step(inputs[size_t(wrapped)], state, at);
            ++s.info.steps;
            if (after)
                after(i);
        }
    };

    const int maxPasses = std::max(kMinPasses, PrefStore::get().value<int>("general", kMaxPassesPref, 16));
    s.info.converged = false;
    s.info.passes = 0;
    for (int p = 0; p < maxPasses; ++p)
    {
        std::vector<double> boundary = state;
        pass({});
        s.info.passes = p + 1;
        if (p + 1 >= kMinPasses && residual(s.setup.layout, boundary, state, tolerances()) <= 1.0)
        {
            s.info.converged = true;
            break;
        }
    }

    // The recorded pass: frame k holds the state after k frames of steps.
    std::vector<std::vector<double>> states{state};
    pass([&](int64_t i) {
        if (i % sub == 0)
            states.push_back(state);
    });
    s.info.residual = residual(s.setup.layout, states.front(), states.back(), tolerances());

    // Whatever the seam still differs by fades in over the last frames, so the last frame lands on the first.
    const int64_t fade = std::clamp<int64_t>(PrefStore::get().value<int64_t>("general", kCrossfadePref, 6), 1, period);
    std::vector<double> delta(n);
    for (size_t k = 0; k < n; ++k)
        delta[k] = states.front()[k] - states.back()[k];
    for (int64_t f = period - fade; f < period; ++f)
    {
        const double x = double(f - (period - fade)) / double(fade);
        const double w = x * x * (3.0 - 2.0 * x);
        for (size_t k = 0; k < n; ++k)
            states[size_t(f)][k] += delta[k] * w;
    }
    states.back() = states.front();

    uint64_t fh = s.params;
    for (int64_t f = 0; f <= period; ++f)
    {
        const std::vector<double>& in = inputs[size_t((f * sub) % steps)];
        fh = hashDoubles(in, fh);
        s.frames.push_back(makeFrame(s, s.loopStart + f, f == period ? h : fh, states[size_t(f)], in));
    }
    applyStepped(s);
    // Bitwise: the last frame is the first.
    auto last = std::make_shared<Frame>(*s.frames.front());
    last->frame = s.loopEnd;
    last->hash = h;
    s.frames.back() = std::move(last);
    if (!s.info.converged)
        log::write(LogLevel::Warning, g.node(node)->path() + ": the loop did not settle in " + std::to_string(s.info.passes) +
                                          " passes; the seam's residual was cross-faded over " + std::to_string(fade) + " frames");
}

/** Validates the cache up to `upTo` frames past the start, then simulates whatever is missing. */
void simulateLinear(Graph& g, NodeId node, Sim& s, int64_t upTo)
{
    LILA_ZONE();

    const int sub = s.setup.substeps;
    const int64_t start = s.setup.startFrame;
    const double dt = 1.0 / (double(sub) * fps());
    const size_t n = stateSize(s.setup);
    std::vector<double> inputs;

    if (s.validGeneration != gGeneration && !s.frames.empty() && !s.solver->inputsMayHaveChanged(g))
        s.validGeneration = gGeneration;
    const bool validate = s.validGeneration != gGeneration && !s.frames.empty();
    if (!validate && int64_t(s.frames.size()) > upTo)
        return;

    SampleScope scope(g, node, s.setup);
    s.info.lastInvalidated = -1;
    if (validate)
    {
        // Re-sample the inputs up to the frame asked for; the cache stands until the first frame they hash differently.
        const int64_t check = std::min<int64_t>(int64_t(s.frames.size()) - 1, upTo);
        size_t keep = 0;
        scope.at(*s.solver, stepSeconds(start, sub, 0), inputs);
        uint64_t h = hashDoubles(inputs, s.params);
        if (h == s.frames[0]->hash)
        {
            keep = 1;
            for (int64_t k = 1; k <= check; ++k)
            {
                for (int j = 1; j <= sub; ++j)
                {
                    scope.at(*s.solver, stepSeconds(start, sub, (k - 1) * sub + j), inputs);
                    h = hashDoubles(inputs, h);
                }
                if (h != s.frames[size_t(k)]->hash)
                    break;
                keep = size_t(k) + 1;
            }
        }
        // Frames past the ones checked are unverified, so they go too.
        if (keep < s.frames.size())
        {
            if (keep <= size_t(check))
                s.info.lastInvalidated = start + int64_t(keep);
            s.frames.resize(keep);
        }
        s.validGeneration = gGeneration;
    }
    s.validGeneration = gGeneration;

    // Stepped output reads back to the hold frame, which must still be the pose simulated, not the held copy.
    std::vector<double> state;
    if (s.frames.empty())
    {
        scope.at(*s.solver, stepSeconds(start, sub, 0), inputs);
        state.assign(n, 0.0);
        s.solver->reset(inputs, state);
        if (!s.setup.variableState)
            state.resize(n, 0.0);
        s.frames.push_back(makeFrame(s, start, hashDoubles(inputs, s.params), state, inputs));
    }
    // Long runs (jumping far ahead, or the first play) show progress and can be cancelled; the frames made so far stay cached.
    const int64_t todo = upTo + 1 - int64_t(s.frames.size());
    std::optional<progress::Scope> bar;
    if (todo >= 8)
        bar.emplace("Simulating", double(todo), progress::Cancellable);
    for (int64_t k = int64_t(s.frames.size()); k <= upTo; ++k)
    {
        if (bar)
        {
            bar->checkCancel();
            bar->setStatus("Frame " + std::to_string(start + k));
        }
        state = s.frames.back()->state;
        uint64_t h = s.frames.back()->hash;
        for (int j = 1; j <= sub; ++j)
        {
            const int64_t index = (k - 1) * sub + j;
            scope.at(*s.solver, stepSeconds(start, sub, index), inputs);
            StepInfo at{.index = index, .seconds = stepSeconds(start, sub, index), .dt = dt};
            s.solver->step(inputs, state, at);
            ++s.info.steps;
            h = hashDoubles(inputs, h);
        }
        auto f = makeFrame(s, start + k, h, state, inputs);
        const int64_t hold = std::max(1, s.setup.stepped);
        if (k % hold != 0)
            f->world = s.frames[size_t(k - k % hold)]->world;
        s.frames.push_back(std::move(f));
        if (bar)
            bar->step();
    }
}

void fillInfo(Sim& s)
{
    Info& i = s.info;
    i.loop = s.loop;
    i.loopStart = s.loopStart;
    i.loopEnd = s.loopEnd;
    i.startFrame = s.loop ? s.loopStart : s.setup.startFrame;
    i.substeps = s.setup.substeps;
    i.cachedFrom = i.startFrame;
    i.cachedTo = i.startFrame + int64_t(s.frames.size()) - 1;
    i.cacheHash = s.frames.empty() ? 0 : s.frames.back()->hash;
    i.outputs = s.setup.outputs;
    if (!s.loop)
    {
        i.converged = true;
        i.passes = 0;
        i.residual = 0.0;
    }
}

int64_t indexFor(const Sim& s, int64_t frame)
{
    if (s.loop)
    {
        const int64_t period = s.loopEnd - s.loopStart;
        if (frame == s.loopEnd)
            return period;
        int64_t k = (frame - s.loopStart) % period;
        return k < 0 ? k + period : k;
    }
    return std::max<int64_t>(0, frame - s.setup.startFrame);
}

std::shared_ptr<const Frame> run(Graph& g, NodeId node, int64_t frame, LoopMode mode)
{
    // A long run pumps events for its progress card; a redraw or idle timer must not start the same one again inside it.
    static std::set<NodeId> running;
    if (!running.insert(node).second)
        throw std::runtime_error("already simulating");
    struct Leave
    {
        NodeId n;
        ~Leave() { running.erase(n); }
    } leave{node};
    Sim& s = simFor(g, node);
    try
    {
        configure(g, node, s, mode);
        if (s.loop)
            simulateLoop(g, node, s);
        else
            simulateLinear(g, node, s, indexFor(s, frame));
        s.info.error.clear();
    }
    catch (const std::exception& e)
    {
        s.info.error = e.what();
        fillInfo(s);
        throw;
    }
    fillInfo(s);
    return s.frames[size_t(std::min<int64_t>(indexFor(s, frame), int64_t(s.frames.size()) - 1))];
}

std::string hex(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

uint64_t splitmix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

double lattice(uint64_t seed, int64_t cell)
{
    // 53 random bits to [-1, 1].
    return double(splitmix(seed ^ splitmix(uint64_t(cell))) >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}
} // namespace

Registry<SolverType>& solverTypes()
{
    static Registry<SolverType> r;
    return r;
}

void registerSolverType(SolverType type)
{
    std::string id = type.nodeType;
    solverTypes().add(id, std::move(type));
}

const SolverType* registerSolverType(ext::Context& ctx, SolverType type)
{
    const std::string id = type.nodeType;
    if (solverTypes().find(id))
    {
        ctx.log(LogLevel::Error, "simulation solver for '" + id + "' is already registered");
        return nullptr;
    }
    solverTypes().add(id, std::move(type), ctx.owner());
    const std::string owner = ctx.owner();
    ctx.track("simSolver:" + id, [id, owner] {
        for (auto it = sims().begin(); it != sims().end();)
            it = it->second.nodeType == id ? sims().erase(it) : std::next(it);
        if (const std::string* o = solverTypes().ownerOf(id); o && *o == owner)
            solverTypes().remove(id);
    });
    return solverTypes().find(id);
}

double framesPerSecond() { return fps(); }

int64_t frameOf(double seconds) { return frameAt(seconds); }

bool isSimulation(const Node& n) { return solverTypeFor(n) != nullptr; }

bool sampling() { return gSampling; }

std::vector<NodeId> simulations(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && isSimulation(*n))
            out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<const Frame> simulate(Graph& g, NodeId node, int64_t frame, LoopMode loop) { return run(g, node, frame, loop); }

std::shared_ptr<const Frame> cached(Graph& g, NodeId node, int64_t frame)
{
    watch(g);
    auto it = sims().find(node);
    if (it == sims().end() || it->second.frames.empty() || it->second.validGeneration != gGeneration)
        return nullptr;
    const int64_t k = indexFor(it->second, frame);
    return k < int64_t(it->second.frames.size()) ? it->second.frames[size_t(k)] : nullptr;
}

bool extendable(Graph& g, NodeId node, int64_t frame, int64_t within)
{
    watch(g);
    auto it = sims().find(node);
    if (it == sims().end() || it->second.frames.empty() || it->second.validGeneration != gGeneration)
        return false;
    return indexFor(it->second, frame) < int64_t(it->second.frames.size()) + within;
}

void clearCache(Graph& g, NodeId node)
{
    watch(g);
    if (node == kNoNode)
        sims().clear();
    else
        sims().erase(node);
}

Info info(Graph& g, NodeId node)
{
    Sim& s = simFor(g, node);
    fillInfo(s);
    return s.info;
}

std::optional<std::pair<int64_t, int64_t>> loopRange(Graph& g, const std::vector<NodeId>& nodes)
{
    const std::set<NodeId> ids(nodes.begin(), nodes.end());
    for (NodeId t : anim::activeTakes(g))
    {
        Node& take = *g.node(t);
        if (!take.as<bool>(anim::TakeNode::kLoop))
            continue;
        const std::string& root = take.as<std::string>(anim::TakeNode::kRoot);
        const std::vector<NodeId> rootIds = root.empty() ? std::vector<NodeId>{} : g.findAll(root);
        if (root.empty() || (rootIds.size() == 1 && ids.contains(rootIds[0])))
            return std::pair{frameAt(take.as<double>(anim::TakeNode::kStart)), frameAt(take.as<double>(anim::TakeNode::kEnd))};
    }
    std::vector<double> loops = anim::loopKeyTimes(g, nodes);
    if (loops.empty())
        return std::nullopt;
    // The period runs from the first key of the looping curves to the latest loop key.
    double first = loops.back();
    for (NodeId n : nodes)
        for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, n))
            if (Node* c = g.node(ap.curve); c && anim::curveLoops(*c))
                if (std::vector<double> times = anim::curveKeyTimes(*c); !times.empty())
                    first = std::min(first, times.front());
    return std::pair{frameAt(first), frameAt(loops.back())};
}

double noise(uint64_t seed, double x, int64_t period)
{
    const double fl = std::floor(x);
    int64_t c0 = int64_t(fl), c1 = c0 + 1;
    if (period > 0)
    {
        c0 = ((c0 % period) + period) % period;
        c1 = ((c1 % period) + period) % period;
    }
    const double t = x - fl;
    const double s = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    const double a = lattice(seed, c0), b = lattice(seed, c1);
    return a + (b - a) * s;
}

glm::dvec3 noise3(uint64_t seed, double x, int64_t period)
{
    return {noise(splitmix(seed + 1), x, period), noise(splitmix(seed + 2), x, period), noise(splitmix(seed + 3), x, period)};
}

uint64_t hashBytes(const void* data, size_t size, uint64_t h)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

namespace
{
struct Channels
{
    glm::dvec3 translate{0.0}, rotate{0.0}, scale{1.0};
};

glm::dmat3 rotationOnly(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
        r[i] /= std::max(glm::length(r[i]), 1e-300);
    return r;
}

/** The channel values that pose a transform at world, given its parent's world: translate, rotate and scale, no pivots. */
Channels channelsFor(const Node& n, const glm::dmat4& parentWorld, const glm::dmat4& world, const glm::dvec3& rotateNear)
{
    const auto& t = static_cast<const Transform&>(n);
    glm::dmat4 local = glm::inverse(parentWorld) * world;
    Channels c;
    c.translate = glm::dvec3(local[3]);
    glm::dmat3 m(local);
    for (int i = 0; i < 3; ++i)
    {
        c.scale[i] = glm::length(m[i]);
        m[i] /= std::max(c.scale[i], 1e-300);
    }
    const glm::dmat3 r = glm::transpose(glm::dmat3(t.preRotation())) * m * glm::transpose(glm::dmat3(t.rotateAxisMatrix()));
    c.rotate = eulerNear(r, RotateOrder(n.as<int64_t>(Transform::kRotateOrder)), rotateNear);
    return c;
}

/** Keys removed greedily while the curve stays within tol of every original key's value. */
anim::Curve reduce(const anim::Curve& in, double tol)
{
    anim::Curve c = in;
    for (size_t i = 1; i + 1 < c.keys.size();)
    {
        anim::Curve trial = c;
        trial.keys.erase(trial.keys.begin() + std::ptrdiff_t(i));
        bool ok = true;
        for (const anim::Key& k : in.keys)
            if (std::abs(trial.evaluate(k.time) - k.value) > tol)
            {
                ok = false;
                break;
            }
        if (ok)
            c = std::move(trial);
        else
            ++i;
    }
    return c;
}

anim::RotationCurve reduce(const anim::RotationCurve& in, double degrees)
{
    const double limit = degrees * 3.14159265358979323846 / 180.0;
    anim::RotationCurve c = in;
    for (size_t i = 1; i + 1 < c.keys.size();)
    {
        anim::RotationCurve trial = c;
        trial.keys.erase(trial.keys.begin() + std::ptrdiff_t(i));
        bool ok = true;
        for (const anim::RotKey& k : in.keys)
        {
            glm::dquat d = glm::inverse(k.q) * trial.evaluateQuat(k.time);
            if (glm::length(anim::quatLog(d)) * 2.0 > limit)
            {
                ok = false;
                break;
            }
        }
        if (ok)
            c = std::move(trial);
        else
            ++i;
    }
    return c;
}
} // namespace

NodeId record(Graph& g, NodeId node, const RecordOptions& o)
{
    LILA_ZONE();
    // Keying re-evaluates as it goes; skinned meshes nothing here reads wait for the evaluation after.
    Graph::DeferMeshes deferMeshes;

    Sim& s = simFor(g, node);
    configure(g, node, s, o.loop);
    const anim::Playback& pb = anim::playback();
    int64_t start = o.start.value_or(s.loop ? s.loopStart : frameAt(pb.minTime));
    int64_t end = o.end.value_or(s.loop ? s.loopEnd : frameAt(pb.maxTime));
    if (end < start)
        std::swap(start, end);

    run(g, node, end, o.loop);
    std::vector<std::shared_ptr<const Frame>> frames;
    for (int64_t f = start; f <= end; ++f)
        frames.push_back(s.frames[size_t(std::min<int64_t>(indexFor(s, f), int64_t(s.frames.size()) - 1))]);
    const std::vector<NodeId> outputs = s.setup.outputs;
    const int hold = std::max(1, s.setup.stepped);

    std::vector<size_t> chosen;
    for (size_t i = 0; i < outputs.size(); ++i)
    {
        Node* n = g.node(outputs[i]);
        if (!n || !n->type().isA("transform"))
            continue;
        if (o.nodes.empty() || std::find(o.nodes.begin(), o.nodes.end(), n->name()) != o.nodes.end() ||
            std::find(o.nodes.begin(), o.nodes.end(), n->path()) != o.nodes.end())
            chosen.push_back(i);
    }
    if (chosen.empty())
        throw std::invalid_argument("nothing to record");

    // The channel values each output needs at each frame, worked out before any key changes the scene.
    std::map<NodeId, size_t> outputIndex;
    for (size_t i = 0; i < outputs.size(); ++i)
        outputIndex[outputs[i]] = i;
    std::vector<std::vector<Channels>> want(frames.size(), std::vector<Channels>(chosen.size()));
    std::vector<std::array<bool, 3>> moves(chosen.size(), {false, false, false});
    // Joints a spline chain lays out are turned through their splineJoint's offset: their own rotate is overridden.
    std::vector<PlugRef> redirect(chosen.size(), PlugRef{kNoNode, -1, -1});
    for (size_t c = 0; c < chosen.size(); ++c)
        if (const NodeId sj = spline::splineJointOf(g, outputs[chosen[c]]))
            redirect[c] = {sj, g.node(sj)->type().attrIndex("offset"), -1};
    const double savedTime = g.time();
    {
        SampleScope scope(g, node, s.setup);
        for (size_t f = 0; f < frames.size(); ++f)
        {
            g.setTime(double(start + int64_t(f)) / fps());
            // Only the outputs' channels and parents are read: skinned meshes nothing reads stay dirty.
            g.evaluateUpstream(outputs);
            for (size_t c = 0; c < chosen.size(); ++c)
            {
                const size_t i = chosen[c];
                Node& n = *g.node(outputs[i]);
                const NodeId parent = g.parent(outputs[i]);
                glm::dmat4 parentWorld = n.as<glm::dmat4>(Transform::kParentMatrix);
                if (auto it = outputIndex.find(parent); it != outputIndex.end())
                    parentWorld = frames[f]->world[it->second];
                else
                    // Unsimulated joints between outputs ride on the nearest simulated ancestor.
                    for (NodeId a = g.parent(parent); a; a = g.parent(a))
                        if (auto up = outputIndex.find(a); up != outputIndex.end())
                        {
                            parentWorld = frames[f]->world[up->second] * glm::inverse(g.node(a)->as<glm::dmat4>(Transform::kWorldMatrix)) * parentWorld;
                            break;
                        }
                if (redirect[c].node)
                {
                    const glm::dvec3 offsetNow = std::get<glm::dvec3>(g.value(redirect[c]));
                    const glm::dmat3 local = glm::transpose(rotationOnly(parentWorld)) * rotationOnly(frames[f]->world[i]);
                    Channels ch;
                    ch.rotate = offsetNow;
                    if (auto k = spline::offsetFor(g, outputs[i], local))
                        ch.rotate = k->value;
                    want[f][c] = ch;
                    moves[c][1] = moves[c][1] || glm::length(ch.rotate - offsetNow) > 1e-9;
                    continue;
                }
                const glm::dvec3 now = n.as<glm::dvec3>(Transform::kRotate);
                Channels ch = channelsFor(n, parentWorld, frames[f]->world[i], now);
                want[f][c] = ch;
                const glm::dvec3 tNow = n.as<glm::dvec3>(Transform::kTranslate), sNow = n.as<glm::dvec3>(Transform::kScale);
                moves[c][0] = moves[c][0] || glm::length(ch.translate - tNow) > 1e-9;
                const RotateOrder order = RotateOrder(n.as<int64_t>(Transform::kRotateOrder));
                moves[c][1] = moves[c][1] || std::abs(glm::dot(anim::eulerToQuat(ch.rotate, order), anim::eulerToQuat(now, order))) < 1.0 - 1e-15;
                moves[c][2] = moves[c][2] || glm::length(ch.scale - sNow) > 1e-9;
            }
        }
    }

    std::string layerName = o.layer.empty() ? g.node(node)->name() + "_sim" : o.layer;
    const NodeId layer = anim::createLayer(g, layerName, anim::LayerMode::Additive);
    layerName = g.node(layer)->path();
    if (int a = g.node(node)->type().attrIndex(kRecordingsAttr); a >= 0)
    {
        std::vector<std::string> list = recordings(g, node);
        list.push_back(layerName);
        g.setValue({node, a}, json(list).dump());
    }

    const NodeId previous = anim::activeLayer(g);
    const std::string previousName = previous ? g.node(previous)->path() : std::string(anim::kBaseLayerName);
    ops::execute("setActiveAnimLayer", {{"args", {layerName}}});
    struct Restore
    {
        Graph& g;
        double time;
        std::string active;
        ~Restore()
        {
            try
            {
                ops::execute("setActiveAnimLayer", {{"args", {active}}});
            }
            catch (const std::exception&)
            {
            }
            g.setTime(time);
            g.evaluate();
        }
    } restore{g, savedTime, previousName};

    const std::optional<anim::Tangent> tangent = hold > 1 ? std::optional(anim::Tangent::Step) : std::nullopt;
    for (size_t f = 0; f < frames.size(); ++f)
    {
        if (hold > 1 && f % size_t(hold) != 0 && f + 1 != frames.size())
            continue;
        const double t = double(start + int64_t(f)) / fps();
        g.setTime(t);
        // One evaluation per frame, of what the outputs' keys read: keys at one time on the layer don't change what is below it.
        g.evaluateUpstream(outputs);
        anim::KeyBatch batch;
        for (size_t c = 0; c < chosen.size(); ++c)
        {
            const NodeId id = outputs[chosen[c]];
            const Channels& ch = want[f][c];
            if (redirect[c].node)
            {
                if (o.rotate && moves[c][1])
                    for (int k = 0; k < 3; ++k)
                        anim::setKey(g, {redirect[c].node, redirect[c].attr, k}, t, ch.rotate[k], tangent, tangent);
                continue;
            }
            for (int k = 0; k < 3; ++k)
            {
                if (o.translate && moves[c][0])
                    anim::setKey(g, {id, Transform::kTranslate, k}, t, ch.translate[k], tangent, tangent);
                if (moves[c][2])
                    anim::setKey(g, {id, Transform::kScale, k}, t, ch.scale[k], tangent, tangent);
            }
            if (o.rotate && moves[c][1])
                for (int k = 0; k < 3; ++k)
                    anim::setKey(g, {id, Transform::kRotate, k}, t, ch.rotate[k], tangent, tangent);
        }
    }

    // The layer's curves: reduce them, and loop them with the simulation.
    std::vector<NodeId> curves;
    for (size_t c = 0; c < chosen.size(); ++c)
        for (int attr : {int(Transform::kTranslate), int(Transform::kRotate), int(Transform::kScale)})
        {
            if (redirect[c].node && attr != int(Transform::kRotate))
                continue;
            const PlugRef plug = redirect[c].node ? redirect[c] : PlugRef{outputs[chosen[c]], attr, -1};
            const NodeId blend = anim::blendFor(g, plug, layer);
            if (!blend)
                continue;
            if (auto whole = g.source({blend, anim::BlendNode::kInputB, -1}))
            {
                if (Node* cn = g.node(whole->node); cn && anim::isRotationCurve(*cn))
                {
                    if (o.tolerance > 0.0 && hold == 1)
                        anim::setRotationCurve(g, cn->id(), reduce(anim::rotationCurveOf(*cn), o.rotationTolerance));
                    curves.push_back(cn->id());
                }
                continue;
            }
            for (int k = 0; k < 3; ++k)
                if (auto src = g.source({blend, anim::BlendNode::kInputB, k}))
                    if (Node* cn = g.node(src->node); cn && anim::isCurve(*cn))
                    {
                        if (o.tolerance > 0.0 && hold == 1)
                            anim::setCurve(g, cn->id(), reduce(anim::curveOf(*cn), o.tolerance));
                        curves.push_back(cn->id());
                    }
        }
    if (s.loop && start == s.loopStart && end == s.loopEnd && !curves.empty())
        anim::setCurvesLoop(g, curves, true);
    return layer;
}

namespace
{
LoopMode loopArg(const Args& a, const char* cmd)
{
    const std::string m = a.get<std::string>("loop", "auto");
    if (m == "auto")
        return LoopMode::Auto;
    if (m == "on")
        return LoopMode::On;
    if (m == "off")
        return LoopMode::Off;
    throw CommandError(std::string(cmd) + ": loop is auto, on or off");
}

std::vector<NodeId> simArgs(Graph& g, const Args& a, const char* cmd, bool allWhenNone)
{
    std::vector<NodeId> out;
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (isSimulation(*n))
            out.push_back(n->id());
        else if (!a.positional.empty())
            throw CommandError(std::string(cmd) + ": " + name + " is not a simulation");
    }
    if (out.empty() && allWhenNone)
        out = simulations(g);
    return out;
}

json infoJson(Graph& g, NodeId id, const Info& i)
{
    return {{"node", g.node(id)->path()}, {"loop", i.loop}, {"loopStart", i.loopStart}, {"loopEnd", i.loopEnd},
            {"converged", i.converged}, {"passes", i.passes}, {"residual", i.residual}, {"startFrame", i.startFrame},
            {"substeps", i.substeps}, {"steps", i.steps}, {"cachedFrom", i.cachedFrom}, {"cachedTo", i.cachedTo},
            {"lastInvalidated", i.lastInvalidated}, {"cacheHash", hex(i.cacheHash)}, {"error", i.error}};
}

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

int64_t currentFrame(Graph& g) { return frameAt(g.time()); }
} // namespace

void registerCommands()
{
    using T = ArgType;
    const FlagDef loopFlag{"loop", "lp", T::String, "auto", "auto (the active take's loop flag, else loop keys) | on | off", {"auto", "on", "off"}};

    ops::registerCommand({
        .name = "simulate",
        .doc = "Simulate up to a frame (default the current one) from the start frame or the cache, the same answer however "
               "the frame is reached. Looping animation is simulated as a periodic loop. Returns each simulation's info.",
        .positionalDoc = "simulation nodes (default the selected ones, else all)",
        .flags = {
            {"frame", "f", T::Int, nullptr, "Frame to simulate to. Default: the current frame."},
            {"positions", "pos", T::Bool, false, "Also return the outputs' world positions at that frame."},
            loopFlag,
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            const LoopMode mode = loopArg(a, "simulate");
            for (NodeId id : simArgs(g, a, "simulate", true))
            {
                const int64_t frame = a.has("frame") ? a.get<int64_t>("frame") : currentFrame(g);
                std::shared_ptr<const Frame> f = guarded("simulate", [&] { return simulate(g, id, frame, mode); });
                json j = infoJson(g, id, info(g, id));
                if (a.get<bool>("positions"))
                {
                    json pos = json::object();
                    const Setup& su = sims()[id].setup;
                    for (size_t i = 0; i < su.outputs.size() && i < f->world.size(); ++i)
                        if (Node* n = g.node(su.outputs[i]))
                            pos[n->path()] = {f->world[i][3].x, f->world[i][3].y, f->world[i][3].z};
                    j["frame"] = frame;
                    j["positions"] = pos;
                }
                out.push_back(j);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "recordSimulation",
        .doc = "Bake a simulation onto a new additive animation layer (one per recording): mute it to compare, delete it to "
               "revert, lower its weight to tone it down. Returns the layer.",
        .positionalDoc = "simulation node (default the selected one)",
        .flags = {
            {"start", "s", T::Int, nullptr, "First frame. Default: the loop period, else the playback range."},
            {"end", "e", T::Int, nullptr, "Last frame."},
            {"tolerance", "tol", T::Float, 0.0, "0 keys every frame; above it, keys go while the curves stay this close.", {}, {}, 0.0},
            {"rotationTolerance", "rtol", T::Float, 0.1, "Degrees, for rotation keys when tolerance is above 0.", {}, {}, 0.0},
            {"joints", "j", T::List, nullptr, "Outputs to key. Default: all."},
            {"layer", "l", T::String, nullptr, "Layer name. Default: <node>_sim."},
            {"translate", "t", T::Bool, true, "Key translation."},
            {"rotate", "r", T::Bool, true, "Key rotation."},
            loopFlag,
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> ids = simArgs(g, a, "recordSimulation", false);
            if (ids.empty())
            {
                std::vector<NodeId> all = simulations(g);
                if (all.size() != 1)
                    throw CommandError("recordSimulation: name the simulation");
                ids = all;
            }
            if (ids.size() != 1)
                throw CommandError("recordSimulation: name one simulation");
            RecordOptions o;
            if (a.has("start"))
                o.start = a.get<int64_t>("start");
            if (a.has("end"))
                o.end = a.get<int64_t>("end");
            o.tolerance = a.get<double>("tolerance", 0.0);
            o.rotationTolerance = a.get<double>("rotationTolerance", 0.1);
            if (a.has("joints"))
                o.nodes = a.flags["joints"].get<std::vector<std::string>>();
            o.layer = a.get<std::string>("layer");
            o.translate = a.get<bool>("translate", true);
            o.rotate = a.get<bool>("rotate", true);
            o.loop = loopArg(a, "recordSimulation");
            NodeId layer = guarded("recordSimulation", [&] { return record(g, ids[0], o); });
            return g.node(layer)->path();
        },
    });

    ops::registerCommand({
        .name = "clearSimCache",
        .doc = "Throw away simulation caches, so the next simulate starts from the start frame.",
        .positionalDoc = "simulation nodes (default all)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
            {
                clearCache(g);
                return nullptr;
            }
            for (NodeId id : simArgs(g, a, "clearSimCache", false))
                clearCache(g, id);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "simulationInfo",
        .doc = "A simulation's state: loop range and convergence (passes, residual), step count, cached frames and the "
               "cache's input hash.",
        .positionalDoc = "simulation nodes (default the selected ones, else all)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId id : simArgs(g, a, "simulationInfo", true))
                out.push_back(infoJson(g, id, guarded("simulationInfo", [&] { return info(g, id); })));
            return out;
        },
    });
}
} // namespace lila::scene::sim
