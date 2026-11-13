#include "CrowdCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <queue>
#include <stdexcept>

namespace crowds
{
using namespace lila::motion;

namespace
{
constexpr float kPi = std::numbers::pi_v<float>;
constexpr double kRate = lila::motion::kSampleRate;

enum Intent : int
{
    kStill = 0,  // hold when the motion runs out
    kWalk = 1,   // walk along the desired heading
    kStop = 2,   // come to rest at (goalX, goalZ) facing `hold` radians
    kTurn = 3,   // shift on the spot to face `hold` radians
    kTarget = 4, // perform spot target `target`
};

enum Label : int
{
    kLabelStand,
    kLabelWalk,
    kLabelPause,
    kLabelQueue,
    kLabelIdle,
    kLabelSpot,
    kLabelFlee,
    kLabelChase,
    kLabelCaught,
    kLabelLeave,
};

const char* labelName(int l)
{
    switch (l)
    {
    case kLabelWalk: return "walk";
    case kLabelPause: return "pause";
    case kLabelQueue: return "queue";
    case kLabelIdle: return "idle";
    case kLabelSpot: return "spot";
    case kLabelFlee: return "flee";
    case kLabelChase: return "chase";
    case kLabelCaught: return "caught";
    case kLabelLeave: return "leave";
    default: return "stand";
    }
}

glm::vec2 dirOf(float heading) { return {std::sin(heading), std::cos(heading)}; }
float headingOfDir(glm::vec2 d) { return std::atan2(d.x, d.y); }

uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/** A polyline with arc length (meters). */
struct Polyline
{
    const std::vector<glm::vec2>& pts;
    std::vector<float> at;

    explicit Polyline(const std::vector<glm::vec2>& p) : pts(p)
    {
        at.assign(pts.size(), 0.0f);
        for (size_t i = 1; i < pts.size(); ++i)
            at[i] = at[i - 1] + glm::length(pts[i] - pts[i - 1]);
    }
    float length() const { return at.empty() ? 0.0f : at.back(); }
    glm::vec2 point(float s) const
    {
        if (pts.empty())
            return glm::vec2(0.0f);
        if (pts.size() == 1)
            return pts[0];
        s = std::clamp(s, 0.0f, length());
        const size_t i = size_t(std::upper_bound(at.begin(), at.end(), s) - at.begin());
        const size_t b = std::clamp<size_t>(i, 1, pts.size() - 1);
        const float seg = std::max(at[b] - at[b - 1], 1e-6f);
        return glm::mix(pts[b - 1], pts[b], std::clamp((s - at[b - 1]) / seg, 0.0f, 1.0f));
    }
    glm::vec2 tangent(float s) const
    {
        if (pts.size() < 2)
            return {0.0f, 1.0f};
        const glm::vec2 d = point(std::min(s + 0.1f, length())) - point(std::max(s - 0.1f, 0.0f));
        return glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec2(0.0f, 1.0f);
    }
    /** Arc length of the point nearest p, from at least `from - back` on, so loops don't send it back. */
    float project(glm::vec2 p, float from, float back = 0.5f) const
    {
        float best = from, bestD = std::numeric_limits<float>::max();
        for (size_t i = 1; i < pts.size(); ++i)
        {
            if (at[i] < from - back)
                continue;
            const glm::vec2 a = pts[i - 1], d = pts[i] - a;
            const float len2 = std::max(glm::dot(d, d), 1e-9f);
            const float t = std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f);
            const float dist = glm::length(a + d * t - p);
            if (dist < bestD)
                bestD = dist, best = at[i - 1] + t * std::sqrt(len2);
        }
        return std::max(best, from - back);
    }
};

bool insidePolygon(const std::vector<glm::vec2>& poly, glm::vec2 p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) && (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
            in = !in;
    return in;
}

int16_t packUnit(float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }
float unpackUnit(int16_t v) { return float(v) / 32767.0f; }

/** Default walking speed per style (m/s) before the agent's own multiplier; others use their gait's. */
double styleSpeed(const GaitLibrary& lib, const std::string& style)
{
    if (style == "walk")
        return 1.3;
    if (style == "run")
        return 3.6;
    const Gait* g = lib.find(style);
    return g ? g->speed : 1.3;
}
} // namespace

const char* behaviourName(Behaviour b)
{
    switch (b)
    {
    case Behaviour::Stand: return "stand";
    case Behaviour::Wander: return "wander";
    case Behaviour::GoTo: return "goTo";
    case Behaviour::FollowPath: return "followPath";
    case Behaviour::Queue: return "queue";
    case Behaviour::Idle: return "idle";
    case Behaviour::Spot: return "spot";
    case Behaviour::Flee: return "flee";
    case Behaviour::Chase: return "chase";
    }
    return "stand";
}

Behaviour behaviourFrom(const std::string& name)
{
    for (int k = 0; k <= int(Behaviour::Chase); ++k)
        if (name == behaviourName(Behaviour(k)))
            return Behaviour(k);
    throw std::invalid_argument("unknown behaviour '" + name + "' (stand, wander, goTo, followPath, queue, idle, spot, flee, chase)");
}

// Network -----------------------------------------------------------------------------------------------------------

void Network::connect(float tolerance)
{
    joins.assign(lanes.size() * 2, {});
    auto endPoint = [&](int e) { const Lane& l = lanes[size_t(e / 2)]; return (e & 1) ? l.points.back() : l.points.front(); };
    const int n = int(lanes.size()) * 2;
    for (int a = 0; a < n; ++a)
        for (int b = a + 1; b < n; ++b)
            if (a / 2 != b / 2 && glm::length(endPoint(a) - endPoint(b)) <= tolerance)
            {
                joins[size_t(a)].push_back(b);
                joins[size_t(b)].push_back(a);
            }
}

double Network::length() const
{
    double len = 0.0;
    for (const Lane& l : lanes)
        len += Polyline(l.points).length();
    return len;
}

std::pair<int, float> Network::nearest(glm::vec2 p) const
{
    int best = -1;
    float bestS = 0.0f, bestD = std::numeric_limits<float>::max();
    for (size_t i = 0; i < lanes.size(); ++i)
    {
        const Polyline pl(lanes[i].points);
        const float s = pl.project(p, 0.0f, 0.0f);
        const float d = glm::length(pl.point(s) - p);
        if (d < bestD)
            bestD = d, best = int(i), bestS = s;
    }
    return {best, bestS};
}

glm::vec2 Network::pointAt(double u, int* lane) const
{
    const double total = length();
    double want = std::clamp(u, 0.0, 0.999999) * total;
    for (size_t i = 0; i < lanes.size(); ++i)
    {
        const Polyline pl(lanes[i].points);
        if (want <= pl.length() || i + 1 == lanes.size())
        {
            if (lane)
                *lane = int(i);
            return pl.point(float(want));
        }
        want -= pl.length();
    }
    return glm::vec2(0.0f);
}

std::vector<glm::vec2> Network::route(glm::vec2 a, glm::vec2 b) const
{
    const auto [la, sa] = nearest(a);
    const auto [lb, sb] = nearest(b);
    if (la < 0 || lb < 0)
        return {a, b};
    auto lanePoints = [&](int lane, float from, float to) {
        const Polyline pl(lanes[size_t(lane)].points);
        std::vector<glm::vec2> out{pl.point(from)};
        if (from <= to)
        {
            for (size_t i = 0; i < pl.pts.size(); ++i)
                if (pl.at[i] > from && pl.at[i] < to)
                    out.push_back(pl.pts[i]);
        }
        else
        {
            for (size_t i = pl.pts.size(); i-- > 0;)
                if (pl.at[i] < from && pl.at[i] > to)
                    out.push_back(pl.pts[i]);
        }
        out.push_back(pl.point(to));
        return out;
    };
    if (la == lb)
    {
        std::vector<glm::vec2> out = lanePoints(la, sa, sb);
        out.insert(out.begin(), a);
        out.push_back(b);
        return out;
    }
    // Dijkstra over lane ends; arriving at an end of a lane means walking it from the other end.
    const int n = int(lanes.size()) * 2;
    std::vector<double> dist(size_t(n), std::numeric_limits<double>::max());
    std::vector<int> from(size_t(n), -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    const float lenA = Polyline(lanes[size_t(la)].points).length();
    for (int e : {la * 2, la * 2 + 1})
    {
        const double d = (e & 1) ? double(lenA - sa) : double(sa);
        dist[size_t(e)] = d;
        open.push({d, e});
    }
    while (!open.empty())
    {
        const auto [d, e] = open.top();
        open.pop();
        if (d > dist[size_t(e)])
            continue;
        if (e / 2 == lb)
            continue;
        for (int j : joins[size_t(e)])
        {
            // Entering lane j/2 at end j, leaving at its other end.
            const int other = j ^ 1;
            const double nd = d + (j / 2 == lb ? 0.0 : double(Polyline(lanes[size_t(j / 2)].points).length()));
            const int arrive = j / 2 == lb ? j : other;
            if (nd < dist[size_t(arrive)])
            {
                dist[size_t(arrive)] = nd;
                from[size_t(arrive)] = e;
                open.push({nd, arrive});
            }
        }
    }
    const float lenB = Polyline(lanes[size_t(lb)].points).length();
    int bestEnd = -1;
    double best = std::numeric_limits<double>::max();
    for (int e : {lb * 2, lb * 2 + 1})
    {
        if (dist[size_t(e)] == std::numeric_limits<double>::max())
            continue;
        const double d = dist[size_t(e)] + ((e & 1) ? double(lenB - sb) : double(sb));
        if (d < best)
            best = d, bestEnd = e;
    }
    if (bestEnd < 0)
        return {a, b};
    // Walk back: the chain of ends gives the lanes in reverse.
    std::vector<int> ends;
    for (int e = bestEnd; e >= 0; e = from[size_t(e)])
        ends.push_back(e);
    std::reverse(ends.begin(), ends.end());
    std::vector<glm::vec2> out{a};
    auto append = [&](const std::vector<glm::vec2>& pts) { out.insert(out.end(), pts.begin(), pts.end()); };
    // First lane: from sa to the end the chain starts from.
    append(lanePoints(la, sa, (ends.front() & 1) ? lenA : 0.0f));
    for (size_t k = 1; k + 1 < ends.size() + 1 && k < ends.size(); ++k)
    {
        const int e = ends[k];
        if (e / 2 == lb)
            break;
        const float len = Polyline(lanes[size_t(e / 2)].points).length();
        // e is the end it leaves by; it came in at the other.
        append((e & 1) ? lanePoints(e / 2, 0.0f, len) : lanePoints(e / 2, len, 0.0f));
    }
    append(lanePoints(lb, (bestEnd & 1) ? lenB : 0.0f, sb));
    out.push_back(b);
    // Drop repeats where lanes meet.
    std::vector<glm::vec2> clean;
    for (const glm::vec2& p : out)
        if (clean.empty() || glm::length(p - clean.back()) > 0.05f)
            clean.push_back(p);
    return clean;
}

// Runtime -----------------------------------------------------------------------------------------------------------

Runtime::Runtime(Setup setup, const GaitLibrary& gaits, Generator* gen, std::string why) : mSetup(std::move(setup)), mGaits(gaits), mGen(gen)
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    for (const char* n : modelJointNames())
        mModelJoints.push_back(s.find(n));
    if (!mGen)
        mStats.note = why.empty() ? "no motion engine: every agent plays gait cycles" : why;
    else if (mSetup.options.detail == Detail::Cycles)
        mStats.note = "detail is Cycles: no motion is generated";
    mStats.device = mGen ? mGen->device() : "none";
    mGroupSize.assign(std::max<size_t>(1, mSetup.groups.size()), 0);
    for (const AgentSpec& a : mSetup.agents)
        ++mGroupSize[size_t(std::clamp(a.group, 0, int(mGroupSize.size()) - 1))];
    if (mSetup.groups.empty())
        mSetup.groups.push_back(Group{.name = "crowd", .behaviour = Behaviour::Stand});

    // Gait cycles as measured from the gait's foot strikes: in place already, the root's sideways sway kept, its surge dropped.
    std::string uncycled;
    for (const Gait& g : gaits.gaits)
    {
        if (g.cycle.empty())
        {
            uncycled += (uncycled.empty() ? "" : ", ") + g.name;
            continue;
        }
        // Played a frame per 30 Hz tick, so the period is the frame count's and the speed keeps the stride.
        const double period = double(g.cycle.size()) / kRate;
        Cycle c{.style = g.style, .speed = g.speed * g.period / period, .period = period, .frames = g.cycle};
        for (Pose& p : c.frames)
            p[0].p.z = 0.0f;
        mCycles.push_back(std::move(c));
    }
    if (!uncycled.empty())
        mStats.note += (mStats.note.empty() ? "" : "; ") + ("no gait cycle in the library for " + uncycled + ": far agents use the other gaits' cycles");
}

Runtime::~Runtime() = default;

int Runtime::cycleFor(const std::string& style, double speed) const
{
    int best = -1;
    for (size_t c = 0; c < mCycles.size(); ++c)
        if (mCycles[c].style == style && (best < 0 || std::abs(mCycles[c].speed - speed) < std::abs(mCycles[size_t(best)].speed - speed)))
            best = int(c);
    if (best < 0)
        for (size_t c = 0; c < mCycles.size(); ++c)
            if (mCycles[c].style == "walk")
                return int(c);
    return std::max(best, 0);
}

AgentState& Runtime::at(std::vector<double>& state, size_t i) const
{
    return *reinterpret_cast<AgentState*>(state.data() + kHeaderDoubles + i * kAgentDoubles);
}

const AgentState& Runtime::agent(const std::vector<double>& state, size_t i) const
{
    return *reinterpret_cast<const AgentState*>(state.data() + kHeaderDoubles + i * kAgentDoubles);
}

uint32_t Runtime::hash(size_t agent, uint32_t salt, double count) const
{
    uint64_t h = mix(uint64_t(mSetup.options.seed) * 0x100000001B3ull);
    h = mix(h ^ mSetup.agents[agent].seed);
    h = mix(h ^ salt);
    h = mix(h ^ uint64_t(int64_t(count)));
    return uint32_t(h >> 32);
}

float Runtime::random(size_t agent, uint32_t salt, double count) const { return float(hash(agent, salt, count) >> 8) / float(1u << 24); }

int64_t Runtime::storeClip(Result&& r, int64_t id)
{
    auto c = std::make_shared<PackedClip>();
    c->frames = int(r.frames.size());
    c->root.reserve(size_t(c->frames) * 3);
    c->rotations.reserve(size_t(c->frames) * kModelJoints * 4);
    c->contacts.reserve(size_t(c->frames) * 4);
    for (size_t f = 0; f < r.frames.size(); ++f)
    {
        const Pose& p = r.frames[f];
        c->root.insert(c->root.end(), {p[0].p.x, p[0].p.y, p[0].p.z});
        for (int j : mModelJoints)
        {
            glm::quat q = glm::normalize(p[size_t(j)].q);
            if (q.w < 0.0f)
                q = -q;
            c->rotations.insert(c->rotations.end(), {packUnit(q.x), packUnit(q.y), packUnit(q.z), packUnit(q.w)});
        }
        const Contacts ct = f < r.contacts.size() ? r.contacts[f] : Contacts{};
        for (float v : ct)
            c->contacts.push_back(uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
    }
    if (size_t(id) != mClips.size())
        throw std::logic_error("crowd clip ids out of order");
    mClips.push_back(std::move(c));
    mRoutes.push_back(nullptr);
    return id;
}

int64_t Runtime::storeRoute(std::vector<glm::vec2> route, int64_t id)
{
    if (size_t(id) != mClips.size())
        throw std::logic_error("crowd route ids out of order");
    mClips.push_back(nullptr);
    mRoutes.push_back(std::make_shared<const std::vector<glm::vec2>>(std::move(route)));
    return id;
}

void Runtime::sampleClip(int64_t id, double t, Pose& out, Contacts* contacts) const
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    out = s.reference;
    if (id < 0 || size_t(id) >= mClips.size() || !mClips[size_t(id)])
        return;
    const PackedClip& c = *mClips[size_t(id)];
    const double f = std::clamp(t * kRate, 0.0, double(c.frames - 1));
    const int f0 = int(std::floor(f));
    const int f1 = std::min(f0 + 1, c.frames - 1);
    const float w = float(f - double(f0));
    auto quat = [&](int frame, int j) {
        const int16_t* q = &c.rotations[(size_t(frame) * kModelJoints + size_t(j)) * 4];
        return glm::quat(unpackUnit(q[3]), unpackUnit(q[0]), unpackUnit(q[1]), unpackUnit(q[2]));
    };
    for (int j = 0; j < kModelJoints; ++j)
        out[size_t(mModelJoints[size_t(j)])].q = glm::normalize(glm::slerp(quat(f0, j), quat(f1, j), w));
    const glm::vec3 r0(c.root[size_t(f0) * 3], c.root[size_t(f0) * 3 + 1], c.root[size_t(f0) * 3 + 2]);
    const glm::vec3 r1(c.root[size_t(f1) * 3], c.root[size_t(f1) * 3 + 1], c.root[size_t(f1) * 3 + 2]);
    out[0].p = glm::mix(r0, r1, w);
    if (contacts)
        for (int k = 0; k < 4; ++k)
            (*contacts)[size_t(k)] = glm::mix(float(c.contacts[size_t(f0) * 4 + size_t(k)]), float(c.contacts[size_t(f1) * 4 + size_t(k)]), w) / 255.0f;
}

void Runtime::cyclePose(const AgentState& a, Pose& out) const
{
    if (mCycles.empty() || a.cycle < 0 || size_t(a.cycle) >= mCycles.size())
    {
        out = placed(mGaits.idle, headingOfDir({float(a.hx), float(a.hz)}), {float(a.x), float(a.z)});
        return;
    }
    const Cycle& c = mCycles[size_t(a.cycle)];
    const double f = a.phase * double(c.frames.size());
    const size_t f0 = size_t(std::floor(f)) % c.frames.size(), f1 = (f0 + 1) % c.frames.size();
    const float w = float(f - std::floor(f));
    out = c.frames[f0];
    for (size_t j = 0; j < out.size(); ++j)
    {
        out[j].q = glm::normalize(glm::slerp(c.frames[f0][j].q, c.frames[f1][j].q, w));
        out[j].p = glm::mix(c.frames[f0][j].p, c.frames[f1][j].p, w);
    }
    const float heading = headingOfDir({float(a.hx), float(a.hz)});
    const glm::quat turn = glm::angleAxis(heading, glm::vec3(0, 1, 0));
    const glm::vec3 sway = turn * glm::vec3(out[0].p.x, 0.0f, 0.0f);
    out[0].q = glm::normalize(turn * out[0].q);
    out[0].p = glm::vec3(float(a.x) + sway.x, out[0].p.y, float(a.z) + sway.z);
}

bool Runtime::pose(const std::vector<double>& state, size_t i, Pose& out) const
{
    const AgentState& a = agent(state, i);
    if (a.alive < 0.5)
        return false;
    switch (MotionKind(int(a.motion)))
    {
    case MotionKind::Cycle:
        if (a.moving > 0.5)
            cyclePose(a, out);
        else
            out = placed(mGaits.idle, headingOfDir({float(a.hx), float(a.hz)}), {float(a.x), float(a.z)});
        return true;
    case MotionKind::Hold:
        if (a.clip < 0)
        {
            out = placed(mGaits.idle, headingOfDir({float(a.hx), float(a.hz)}), {float(a.x), float(a.z)});
            return true;
        }
        [[fallthrough]];
    case MotionKind::Clip:
        sampleClip(int64_t(a.clip), a.t, out);
        out[0].p.x += float(a.ox);
        out[0].p.z += float(a.oz);
        return true;
    }
    return false;
}

void Runtime::history(const AgentState& a, std::array<Pose, 4>& ctx) const
{
    const MotionKind kind = MotionKind(int(a.motion));
    for (int k = 0; k < 4; ++k)
    {
        const double back = double(3 - k) / kRate;
        Pose& p = ctx[size_t(k)];
        if (kind == MotionKind::Clip || (kind == MotionKind::Hold && a.clip >= 0))
        {
            const double tau = kind == MotionKind::Hold ? a.t : a.t - back;
            if (tau >= 0.0 || a.prevClip < 0)
                sampleClip(int64_t(a.clip), std::max(tau, 0.0), p);
            else
                sampleClip(int64_t(a.prevClip), std::max(a.prevT + tau, 0.0), p);
            p[0].p.x += float(a.ox);
            p[0].p.z += float(a.oz);
        }
        else if (kind == MotionKind::Cycle && a.moving > 0.5)
        {
            AgentState earlier = a;
            const Cycle& c = mCycles[size_t(std::clamp<double>(a.cycle, 0, double(mCycles.size() - 1)))];
            const double speed = std::hypot(a.vx, a.vz);
            earlier.phase = std::fmod(a.phase - back * (speed / std::max(c.speed, 0.1)) / std::max(c.period, 0.1) + 4.0, 1.0);
            earlier.x = a.x - a.vx * back;
            earlier.z = a.z - a.vz * back;
            cyclePose(earlier, p);
        }
        else
            p = placed(mGaits.idle, headingOfDir({float(a.hx), float(a.hz)}), {float(a.x), float(a.z)});
    }
}

Contacts Runtime::contacts(const std::vector<double>& state, size_t i) const
{
    const AgentState& a = agent(state, i);
    Contacts c{};
    if (a.alive < 0.5 || a.clip < 0)
        return c;
    const MotionKind k = MotionKind(int(a.motion));
    if (k == MotionKind::Cycle)
        return c;
    Pose scratch;
    sampleClip(int64_t(a.clip), a.t, scratch, &c);
    if (k == MotionKind::Hold)
        c = {1.0f, 1.0f, 1.0f, 1.0f};
    return c;
}

std::string Runtime::label(const std::vector<double>& state, size_t i) const
{
    const AgentState& a = agent(state, i);
    if (a.alive < 0.5)
        return "gone";
    return labelName(int(a.label));
}

std::vector<glm::vec2> Runtime::pathFor(const AgentState& a) const
{
    if (a.route >= 0 && size_t(a.route) < mRoutes.size() && mRoutes[size_t(a.route)])
        return *mRoutes[size_t(a.route)];
    return {glm::vec2(float(a.x), float(a.z)), glm::vec2(float(a.goalX), float(a.goalZ))};
}

void Runtime::reset(std::vector<double>& state)
{
    mClips.clear();
    mRoutes.clear();
    state.assign(stateSize(), 0.0);
    for (size_t i = 0; i < mSetup.agents.size(); ++i)
    {
        const AgentSpec& s = mSetup.agents[i];
        AgentState& a = at(state, i);
        a = AgentState{};
        a.alive = s.appear <= 0.0f ? 1.0 : 0.0;
        a.x = s.start.x;
        a.z = s.start.y;
        const glm::vec2 h = dirOf(s.heading);
        a.hx = a.px = a.dx = h.x;
        a.hz = a.pz = a.dz = h.y;
        a.motion = double(int(MotionKind::Hold));
        a.clip = -1;
        a.phase = random(i, 7, 0);
        a.along = s.along;
        a.task = 0;
        a.label = kLabelStand;
    }
}

namespace
{
/** Goals and points for one step. */
struct StepContext
{
    double time = 0.0;
    std::span<const glm::vec2> points;
};
} // namespace

bool Runtime::wantsGenerated(const AgentState& a, size_t i, std::span<const glm::vec2> points) const
{
    if (!mGen || mSetup.options.detail == Detail::Cycles)
        return false;
    // Interactions need generated motion wherever they are.
    if (int(a.task) >= 10)
        return true;
    if (mSetup.options.detail == Detail::Generated || mSetup.options.focusInput < 0 || size_t(mSetup.options.focusInput) >= points.size())
        return true;
    const glm::vec2 f = points[size_t(mSetup.options.focusInput)];
    const double d = glm::length(glm::vec2(float(a.x), float(a.z)) - f);
    const bool generated = MotionKind(int(a.motion)) != MotionKind::Cycle;
    // Hysteresis so agents on the edge don't flip every step.
    return d <= mSetup.options.generatedRadius * (generated ? 1.1 : 0.9);
    (void)i;
}

void Runtime::decide(AgentState& a, size_t i, double time, std::span<const glm::vec2> points, std::vector<double>& state)
{
    const AgentSpec& spec = mSetup.agents[i];
    const Group& grp = mSetup.groups[size_t(std::clamp(spec.group, 0, int(mSetup.groups.size()) - 1))];
    const glm::vec2 pos(float(a.x), float(a.z));
    const double speed = styleSpeed(mGaits, grp.style) * spec.speed;
    const float lead = float(std::max(1.0, speed * 0.8));
    const bool holding = MotionKind(int(a.motion)) == MotionKind::Hold;

    auto setIntent = [&](int intent) {
        if (int(a.intent) == intent)
            return;
        a.intent = double(intent);
        a.moving = intent == kWalk ? 1 : 0;
        if (MotionKind(int(a.motion)) == MotionKind::Cycle)
            return;
        if (holding && intent != kStill)
            a.replan = 1;
        else if (!holding && intent == kStop && a.t > mSetup.options.minPlay)
            a.replan = 1;
    };
    auto steerAlong = [&](const std::vector<glm::vec2>& path, bool stopAtEnd, float faceAtEnd, int label) {
        const Polyline pl(path);
        a.along = pl.project(pos, float(a.along));
        const float remaining = pl.length() - float(a.along);
        a.label = label;
        if (stopAtEnd && remaining <= lead * 1.1f)
        {
            const glm::vec2 end = pl.point(pl.length());
            a.goalX = end.x;
            a.goalZ = end.y;
            if (int(a.intent) != kStop && (int(a.intent) != kStill || (holding && glm::length(end - pos) > 0.3f)))
            {
                a.hold = faceAtEnd;
                setIntent(kStop);
            }
            return true;
        }
        const glm::vec2 aim = pl.point(std::min(float(a.along) + lead, pl.length()));
        const glm::vec2 d = aim - pos;
        if (glm::length(d) > 1e-3f)
        {
            const glm::vec2 n = glm::normalize(d);
            a.dx = n.x;
            a.dz = n.y;
        }
        if (int(a.intent) != kWalk)
            setIntent(kWalk);
        return false;
    };
    auto pickGoal = [&](bool network) {
        glm::vec2 g;
        const double n = a.count;
        if (network && !mSetup.network.empty())
            g = mSetup.network.pointAt(random(i, 11, n));
        else if (grp.area.size() >= 3)
        {
            g = grp.area[0];
            glm::vec2 lo = grp.area[0], hi = grp.area[0];
            for (const glm::vec2& p : grp.area)
                lo = glm::min(lo, p), hi = glm::max(hi, p);
            for (int k = 0; k < 16; ++k)
            {
                const glm::vec2 c(glm::mix(lo.x, hi.x, random(i, 12, n * 16 + k)), glm::mix(lo.y, hi.y, random(i, 13, n * 16 + k)));
                if (insidePolygon(grp.area, c))
                {
                    g = c;
                    break;
                }
            }
        }
        else
        {
            const float r = grp.radius * std::sqrt(random(i, 14, n));
            const float ang = 2.0f * kPi * random(i, 15, n) + grp.yaw;
            g = grp.center + glm::vec2(std::cos(ang), std::sin(ang)) * r;
        }
        a.goalX = g.x;
        a.goalZ = g.y;
        a.along = 0;
        a.route = -1;
        if (network && !mSetup.network.empty())
            a.route = double(storeRoute(mSetup.network.route(pos, g), int64_t(state[0]++)));
        a.count += 1;
    };
    auto point = [&](int input, glm::vec2 fallback) { return input >= 0 && size_t(input) < points.size() ? points[size_t(input)] : fallback; };
    auto stand = [&](int label) {
        a.label = label;
        if (int(a.intent) == kWalk)
        {
            const glm::vec2 v(float(a.vx), float(a.vz));
            const glm::vec2 stop = pos + v * 0.45f;
            a.goalX = stop.x;
            a.goalZ = stop.y;
            a.hold = headingOfDir({float(a.hx), float(a.hz)});
            setIntent(kStop);
        }
    };

    Behaviour b = grp.behaviour;
    // Behaviours that hand over keep the next one in task >= 100.
    if (int(a.task) >= 100)
        b = grp.then;

    switch (b)
    {
    case Behaviour::Stand: stand(kLabelStand); break;
    case Behaviour::Wander:
    {
        const int task = int(a.task) % 100;
        const int base = int(a.task) - task;
        if (task == 0)
        {
            pickGoal(grp.useNetwork);
            a.task = base + 1;
        }
        if (int(a.task) % 100 == 1)
        {
            if (steerAlong(pathFor(a), true, headingOfDir({float(a.hx), float(a.hz)}), kLabelWalk))
            {
                a.task = base + 2;
                a.timer = time + grp.pauseMin + (grp.pauseMax - grp.pauseMin) * random(i, 16, a.count);
            }
        }
        else if (int(a.task) % 100 == 2)
        {
            a.label = kLabelPause;
            if (time >= a.timer)
                a.task = base;
        }
        break;
    }
    case Behaviour::GoTo:
    {
        glm::vec2 goal = point(grp.input, grp.goal);
        // Agents sharing a goal spread over a disc so they don't all aim for one spot.
        const float spread = 0.45f * std::sqrt(float(std::max(1, mGroupSize[size_t(std::clamp(spec.group, 0, int(mGroupSize.size()) - 1))])));
        const float r = std::min(spread, 6.0f) * std::sqrt(random(i, 21, 0));
        const float ang = 2.0f * kPi * random(i, 22, 0) + grp.yaw;
        goal += glm::vec2(std::cos(ang), std::sin(ang)) * r;
        const bool moved = glm::length(goal - glm::vec2(float(a.goalX), float(a.goalZ))) > 2.0f;
        if (int(a.task) == 0 || (moved && int(a.task) == 2))
        {
            a.goalX = goal.x;
            a.goalZ = goal.y;
            a.along = 0;
            a.route = grp.useNetwork && !mSetup.network.empty() ? double(storeRoute(mSetup.network.route(pos, goal), int64_t(state[0]++))) : -1.0;
            a.task = 1;
        }
        else if (int(a.task) == 1 && a.route < 0)
        {
            a.goalX = goal.x;
            a.goalZ = goal.y;
        }
        if (int(a.task) == 1 && steerAlong(pathFor(a), true, headingOfDir({float(a.hx), float(a.hz)}), kLabelWalk))
            a.task = grp.then == Behaviour::Stand ? 2 : 100;
        else if (int(a.task) == 2)
            a.label = kLabelStand;
        break;
    }
    case Behaviour::FollowPath:
    {
        if (grp.path.size() < 2)
        {
            stand(kLabelStand);
            break;
        }
        const Polyline pl(grp.path);
        if (grp.loop)
        {
            // Twice round, so the lookahead runs on past the seam; along wraps after.
            std::vector<glm::vec2> twice = grp.path;
            if (glm::length(grp.path.front() - grp.path.back()) > 0.05f)
                twice.push_back(grp.path.front());
            const std::vector<glm::vec2> once = twice;
            twice.insert(twice.end(), once.begin() + 1, once.end());
            steerAlong(twice, false, 0.0f, kLabelWalk);
            const float closed = Polyline(once).length();
            if (a.along > closed)
                a.along -= closed;
        }
        else if (int(a.task) == 0 && steerAlong(grp.path, true, headingOfDir(pl.tangent(pl.length())), kLabelWalk))
            a.task = grp.then == Behaviour::Stand ? 1 : 100;
        break;
    }
    case Behaviour::Queue:
    {
        if (grp.path.size() < 2)
        {
            stand(kLabelStand);
            break;
        }
        const Polyline pl(grp.path);
        const int served = int(std::floor(time / std::max(grp.serve, 0.5f)));
        const int place = spec.slot - served;
        if (place < 0)
        {
            a.task = 100;
            a.count += 1;
            a.label = kLabelLeave;
            break;
        }
        const float s = std::max(0.0f, pl.length() - float(place) * grp.spacing);
        const glm::vec2 spot = pl.point(s);
        const float face = headingOfDir(pl.tangent(s));
        a.label = kLabelQueue;
        const float off = glm::length(spot - pos);
        if (off > 0.35f && (holding || int(a.intent) == kWalk))
        {
            a.goalX = spot.x;
            a.goalZ = spot.y;
            a.route = -1;
            a.along = 0;
            steerAlong({pos, spot}, true, face, kLabelQueue);
        }
        break;
    }
    case Behaviour::Idle:
    {
        a.label = kLabelIdle;
        if (int(a.intent) == kWalk)
            stand(kLabelIdle);
        if (int(a.intent) == kStill && holding && time >= a.timer)
        {
            // Turn toward the centre, or now and then toward another member of the group.
            glm::vec2 look = grp.center - pos;
            if (random(i, 31, a.count) < 0.4f)
            {
                const size_t other = size_t(hash(i, 32, a.count) % uint32_t(std::max<size_t>(1, mSetup.agents.size())));
                if (other != i && mSetup.agents[other].group == spec.group)
                    look = glm::vec2(float(agent(state, other).x), float(agent(state, other).z)) - pos;
            }
            if (glm::length(look) > 1e-3f)
            {
                a.hold = headingOfDir(look) + (random(i, 33, a.count) - 0.5f) * 0.5f;
                if (a.count > 0 || std::abs(std::remainder(float(a.hold) - headingOfDir({float(a.hx), float(a.hz)}), 2.0f * kPi)) > 0.3f)
                {
                    a.goalX = pos.x;
                    a.goalZ = pos.y;
                    setIntent(kTurn);
                }
            }
            a.count += 1;
            a.timer = time + 3.0 + 5.0 * random(i, 34, a.count);
        }
        break;
    }
    case Behaviour::Spot:
    {
        if (grp.spots.empty())
        {
            stand(kLabelStand);
            break;
        }
        const PlacedSpot& sp = grp.spots[size_t(spec.slot) % grp.spots.size()];
        a.label = kLabelSpot;
        if (int(a.task) == 0)
        {
            const glm::vec2 goal = sp.targets.empty() || sp.targets.front().poses.empty()
                                       ? sp.at
                                       : glm::vec2(sp.targets.front().poses.front()[0].p.x, sp.targets.front().poses.front()[0].p.z);
            a.goalX = goal.x;
            a.goalZ = goal.y;
            a.along = 0;
            a.route = grp.useNetwork && !mSetup.network.empty() ? double(storeRoute(mSetup.network.route(pos, goal), int64_t(state[0]++))) : -1.0;
            a.task = 1;
        }
        if (int(a.task) == 1)
        {
            const std::vector<glm::vec2> path = pathFor(a);
            const Polyline pl(path);
            a.along = pl.project(pos, float(a.along));
            if (pl.length() - float(a.along) <= lead * 1.1f && !sp.targets.empty())
            {
                // Walk into the first target: the interaction takes over from here.
                a.task = 10;
                a.target = 0;
                a.exact = 1;
                setIntent(kTarget);
            }
            else
                steerAlong(path, sp.targets.empty(), sp.heading, kLabelSpot);
        }
        else if (int(a.task) == 11 && grp.stay > 0.0f && time >= a.timer)
        {
            a.task = 100;
            a.exact = 0;
        }
        break;
    }
    case Behaviour::Flee:
    {
        const glm::vec2 threat = point(grp.input, grp.goal);
        const glm::vec2 away = pos - threat;
        const float d = glm::length(away);
        if (d < grp.range)
        {
            const glm::vec2 n = d > 1e-3f ? away / d : dirOf(float(i));
            a.dx = n.x;
            a.dz = n.y;
            a.label = kLabelFlee;
            if (int(a.intent) != kWalk)
                setIntent(kWalk);
        }
        else
            stand(kLabelStand);
        break;
    }
    case Behaviour::Chase:
    {
        glm::vec2 target = point(grp.input, grp.goal);
        if (grp.prey >= 0)
        {
            float best = std::numeric_limits<float>::max();
            for (size_t j = 0; j < mSetup.agents.size(); ++j)
            {
                const AgentState& o = agent(state, j);
                if (j == i || o.alive < 0.5 || mSetup.agents[j].group != grp.prey)
                    continue;
                const glm::vec2 op(float(o.x), float(o.z));
                if (glm::length(op - pos) < best)
                    best = glm::length(op - pos), target = op;
            }
        }
        const glm::vec2 to = target - pos;
        const float d = glm::length(to);
        if (d <= std::max(grp.range, 0.6f) * (int(a.task) == 2 ? 2.0f : 1.0f))
        {
            a.task = 2;
            stand(kLabelCaught);
        }
        else
        {
            a.task = 1;
            a.dx = to.x / d;
            a.dz = to.y / d;
            a.label = kLabelChase;
            if (int(a.intent) != kWalk)
                setIntent(kWalk);
        }
        break;
    }
    }
}

Request Runtime::makeRequest(AgentState& a, size_t i, int& early)
{
    const AgentSpec& spec = mSetup.agents[i];
    const Group& grp = mSetup.groups[size_t(std::clamp(spec.group, 0, int(mSetup.groups.size()) - 1))];
    Request r;
    history(a, r.context);
    r.heading = variantHeading(hash(i, 41, a.generations) & 0xffff);
    early = 0;
    const glm::vec2 pos(float(a.x), float(a.z));
    switch (int(a.intent))
    {
    case kTarget:
    {
        const PlacedSpot& sp = grp.spots[size_t(spec.slot) % grp.spots.size()];
        const auto& t = sp.targets[size_t(std::clamp<double>(a.target, 0, double(sp.targets.size() - 1)))];
        r.target = t.poses;
        r.tokens = t.tokens;
        early = t.transitionEarly;
        return r;
    }
    case kStop:
    case kTurn:
        r.target = {placed(mGaits.rest(grp.style), float(a.hold), {float(a.goalX), float(a.goalZ)})};
        return r;
    default:
    {
        const double speed = styleSpeed(mGaits, grp.style) * spec.speed;
        const Gait* g = mGaits.find(grp.style, speed);
        if (!g)
            g = mGaits.find("walk", speed);
        if (!g)
            throw std::runtime_error("the gait library has no walk");
        a.gait = double(g - mGaits.gaits.data());
        constexpr double kLookahead = 0.9;
        const double phase = std::fmod(a.phase + kLookahead / std::max(g->period, 0.1), 1.0);
        const GaitWindow* win = &g->windows.front();
        double best = 2.0;
        for (const GaitWindow& w : g->windows)
            if (const double d = std::min(std::abs(w.phase - phase), 1.0 - std::abs(w.phase - phase)); d < best)
                best = d, win = &w;
        const glm::vec2 dir = glm::normalize(glm::vec2(float(a.dx), float(a.dz)) + glm::vec2(1e-6f, 0.0f));
        glm::vec2 face = dir;
        if (grp.style == "backward")
            face = -dir;
        r.target = gaitTarget(*g, *win, pos + dir * float(speed * kLookahead), headingOfDir(face), speed);
        r.tokens = tokenMask(7, 11);
        early = 10;
        return r;
    }
    }
}

void Runtime::step(std::vector<double>& state, double dt, double time, std::span<const glm::vec2> points)
{
    LILA_ZONE();
    if (state.size() != stateSize())
        throw std::invalid_argument("crowd state does not match its agents");
    // Clips and routes past the state's counter belong to frames that were thrown away.
    const size_t next = size_t(state[0]);
    if (mClips.size() > next)
    {
        mClips.resize(next);
        mRoutes.resize(next);
    }
    state[1] += 1.0;
    const size_t n = mSetup.agents.size();
    const Options& o = mSetup.options;

    for (size_t i = 0; i < n; ++i)
    {
        AgentState& a = at(state, i);
        if (a.alive < 0.5)
        {
            if (mSetup.agents[i].appear <= time && a.count >= 0)
                a.alive = 1;
            else
                continue;
        }
        const MotionKind kind = MotionKind(int(a.motion));
        if (kind == MotionKind::Clip)
        {
            a.t += dt;
            const PackedClip* c = a.clip >= 0 && size_t(a.clip) < mClips.size() ? mClips[size_t(a.clip)].get() : nullptr;
            const double end = c ? double(c->frames - 1) / kRate : 0.0;
            if (a.t >= end && (a.replan < 0.5 && (int(a.intent) == kStill || a.settle > 0.5)))
            {
                a.t = end;
                a.motion = double(int(MotionKind::Hold));
                a.settle = 0;
                if (int(a.task) == 10)
                {
                    // Reached a spot target: hold it, then the next one.
                    const AgentSpec& spec = mSetup.agents[i];
                    const Group& grp = mSetup.groups[size_t(std::clamp(spec.group, 0, int(mSetup.groups.size()) - 1))];
                    const PlacedSpot& sp = grp.spots[size_t(spec.slot) % grp.spots.size()];
                    const auto& t = sp.targets[size_t(std::clamp<double>(a.target, 0, double(sp.targets.size() - 1)))];
                    a.timer = time + double(t.hold) / kRate;
                    if (a.target + 1 < double(sp.targets.size()))
                    {
                        a.target += 1;
                        a.intent = kTarget;
                        a.settle = 2; // waiting out the hold
                    }
                    else
                    {
                        a.task = 11;
                        a.timer = time + grp.stay;
                    }
                }
            }
        }
        if (a.settle > 1.5 && time >= a.timer)
        {
            a.settle = 0;
            a.replan = 1;
        }
        a.phase = std::fmod(a.phase + dt / std::max(a.gait >= 0 && size_t(a.gait) < mGaits.gaits.size() ? mGaits.gaits[size_t(a.gait)].period : 1.0, 0.1), 1.0);
        decide(a, i, time, points, state);
    }

    // Avoidance: preferred velocities from the behaviours, solved together.
    std::vector<orca::Agent> oa(n);
    std::vector<size_t> live;
    for (size_t i = 0; i < n; ++i)
    {
        const AgentState& a = agent(state, i);
        if (a.alive < 0.5)
            continue;
        live.push_back(i);
    }
    std::vector<orca::Agent> solveIn;
    solveIn.reserve(live.size());
    for (size_t i : live)
    {
        const AgentState& a = agent(state, i);
        const Group& grp = mSetup.groups[size_t(std::clamp(mSetup.agents[i].group, 0, int(mSetup.groups.size()) - 1))];
        const double speed = styleSpeed(mGaits, grp.style) * mSetup.agents[i].speed;
        orca::Agent x;
        x.position = {a.x, a.z};
        x.velocity = {a.vx, a.vz};
        x.radius = mSetup.agents[i].radius;
        x.maxSpeed = speed * 1.15;
        x.active = int(a.intent) == kWalk;
        x.preferred = x.active ? glm::dvec2(a.dx, a.dz) * speed : glm::dvec2(0.0);
        solveIn.push_back(x);
    }
    std::vector<glm::dvec2> solved;
    if (o.avoidance && !solveIn.empty())
    {
        orca::Options oo = o.orca;
        solved = orca::solve(solveIn, mSetup.obstacles, oo);
    }
    for (size_t k = 0; k < live.size(); ++k)
    {
        AgentState& a = at(state, live[k]);
        if (int(a.intent) != kWalk)
            continue;
        const glm::dvec2 v = solved.empty() ? solveIn[k].preferred : solved[k];
        if (glm::length(v) > 0.05)
        {
            const glm::dvec2 nv = glm::normalize(v);
            a.dx = nv.x;
            a.dz = nv.y;
        }
        if (MotionKind(int(a.motion)) == MotionKind::Cycle)
        {
            // Cycles are steered directly: ease toward the solved velocity, turn toward it.
            const double ease = 1.0 - std::exp(-dt / 0.3);
            a.vx += (v.x - a.vx) * ease;
            a.vz += (v.y - a.vz) * ease;
        }
    }

    // Detail: the nearest agents that want generated motion get it, up to the budget; the rest play cycles.
    // Interactions come first, wherever they are.
    std::vector<std::pair<double, size_t>> wanting;
    for (size_t i : live)
    {
        const AgentState& a = agent(state, i);
        if (!wantsGenerated(a, i, points))
            continue;
        double d = 0.0;
        if (int(a.task) >= 10 && int(a.task) < 100)
            d = -1.0;
        else if (o.focusInput >= 0 && size_t(o.focusInput) < points.size())
            d = glm::length(glm::dvec2(a.x, a.z) - glm::dvec2(points[size_t(o.focusInput)]));
        wanting.push_back({d, i});
    }
    std::sort(wanting.begin(), wanting.end());
    std::vector<uint8_t> generate(n, 0);
    for (size_t k = 0; k < wanting.size() && k < size_t(std::max(0, o.generatedMax)); ++k)
        generate[wanting[k].second] = 1;
    for (size_t i : live)
    {
        AgentState& a = at(state, i);
        const MotionKind kind = MotionKind(int(a.motion));
        if (kind == MotionKind::Cycle && generate[i])
        {
            // A walking cycle stays a cycle until its first generation, which starts from the cycle's poses.
            if (int(a.intent) != kWalk)
            {
                a.motion = double(int(MotionKind::Hold));
                a.clip = -1;
            }
            a.replan = int(a.intent) == kStill ? 0 : 1;
        }
        else if (kind != MotionKind::Cycle && !generate[i])
        {
            // Back to cycles at the end of what it plays, or at once while holding.
            const PackedClip* c = a.clip >= 0 && size_t(a.clip) < mClips.size() ? mClips[size_t(a.clip)].get() : nullptr;
            if (kind == MotionKind::Hold || !c || a.t >= a.transition)
            {
                a.motion = double(int(MotionKind::Cycle));
                a.cycle = double(cycleFor(mSetup.groups[size_t(std::clamp(mSetup.agents[i].group, 0, int(mSetup.groups.size()) - 1))].style,
                                          std::hypot(a.vx, a.vz)));
                a.ox = a.oz = 0;
                a.replan = 0;
                a.settle = 0;
            }
        }
    }

    // Scheduling: who generates this step.
    struct Due
    {
        int rank;
        double overshoot;
        size_t agent;
    };
    std::vector<Due> due;
    bool urgent = false;
    const double steerCos = std::cos(o.steerDegrees * std::numbers::pi / 180.0);
    for (size_t i : live)
    {
        AgentState& a = at(state, i);
        const MotionKind kind = MotionKind(int(a.motion));
        if (!mGen || o.detail == Detail::Cycles)
            break;
        if (kind == MotionKind::Cycle)
        {
            if (a.replan > 0.5 && generate[i])
                due.push_back({0, 1.0, i}), urgent = true;
            continue;
        }
        if (kind == MotionKind::Hold)
        {
            if (a.replan > 0.5 && a.settle < 1.5)
                due.push_back({0, 1.0, i}), urgent = true;
            continue;
        }
        if (a.settle > 0.5 && a.replan < 0.5)
            continue;
        const double early = a.exact > 0.5 ? 0.0 : o.lookahead;
        const double over = a.t - a.transition;
        if (over >= -dt || a.replan > 0.5)
        {
            due.push_back({0, over, i});
            urgent = true;
        }
        else if (over >= -early)
            due.push_back({1, over, i});
        else if (a.exact < 0.5 && int(a.intent) == kWalk && a.t > o.minPlay && a.dx * a.px + a.dz * a.pz < steerCos)
            due.push_back({2, over, i});
    }
    std::stable_sort(due.begin(), due.end(), [](const Due& x, const Due& y) { return x.rank != y.rank ? x.rank < y.rank : x.overshoot > y.overshoot; });
    const size_t batchSize = size_t(std::max(1, o.batch));
    if (!due.empty() && (urgent || due.size() >= batchSize))
    {
        if (due.size() > batchSize)
            due.resize(batchSize);
        std::vector<Request> batch;
        std::vector<std::pair<size_t, int>> who;
        for (const Due& d : due)
        {
            AgentState& a = at(state, d.agent);
            if (int(a.intent) == kStill)
            {
                // Nothing to do: play out and hold.
                a.settle = 1;
                a.replan = 0;
                continue;
            }
            if (int(a.intent) == kStop && MotionKind(int(a.motion)) == MotionKind::Hold &&
                glm::length(glm::dvec2(a.goalX - a.x, a.goalZ - a.z)) < 0.2)
            {
                a.intent = kStill;
                a.replan = 0;
                continue;
            }
            int early = 0;
            batch.push_back(makeRequest(a, d.agent, early));
            who.push_back({d.agent, early});
        }
        if (!batch.empty())
        {
            std::vector<Result> res;
            const auto t0 = std::chrono::steady_clock::now();
            res = mGen->generate(batch);
            mStats.inferenceMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++mStats.batches;
            if (batch.size() >= batchSize)
                ++mStats.fullBatches;
            for (size_t k = 0; k < who.size() && k < res.size(); ++k)
            {
                AgentState& a = at(state, who[k].first);
                if (!res[k].error.empty() || res[k].frames.size() < 2)
                {
                    // A failed request leaves the agent where it is.
                    a.replan = 0;
                    a.settle = 1;
                    continue;
                }
                ++mStats.generations;
                const int frames = int(res[k].frames.size());
                const MotionKind was = MotionKind(int(a.motion));
                a.prevClip = was == MotionKind::Clip ? a.clip : -1;
                a.prevT = a.t;
                a.clip = double(storeClip(std::move(res[k]), int64_t(state[0]++)));
                a.t = 0;
                a.transition = double(std::max(0, frames - 1 - who[k].second)) / kRate;
                a.motion = double(int(MotionKind::Clip));
                a.px = a.dx;
                a.pz = a.dz;
                a.replan = 0;
                a.settle = 0;
                a.ox = a.oz = 0;
                a.generations += 1;
                const int intent = int(a.intent);
                if (intent == kStop || intent == kTurn || intent == kTarget)
                {
                    // One generation reaches a stop, a turn or a target; after it, hold.
                    a.settle = 1;
                    if (intent != kTarget)
                        a.intent = kStill;
                }
            }
        }
    }

    // Where everyone is now.
    Pose p;
    int gen = 0, cyc = 0, alive = 0;
    for (size_t i : live)
    {
        AgentState& a = at(state, i);
        ++alive;
        const MotionKind kind = MotionKind(int(a.motion));
        if (kind == MotionKind::Cycle)
        {
            ++cyc;
            if (int(a.intent) != kWalk)
            {
                const double ease = 1.0 - std::exp(-dt / 0.2);
                a.vx -= a.vx * ease;
                a.vz -= a.vz * ease;
            }
            a.x += a.vx * dt;
            a.z += a.vz * dt;
            const double sp = std::hypot(a.vx, a.vz);
            a.moving = sp > 0.15 ? 1 : 0;
            if (sp > 0.15)
            {
                const double turn = 1.0 - std::exp(-dt / 0.25);
                const glm::dvec2 h = glm::normalize(glm::mix(glm::dvec2(a.hx, a.hz), glm::dvec2(a.vx, a.vz) / sp, turn) + glm::dvec2(1e-9, 0.0));
                a.hx = h.x;
                a.hz = h.y;
                const Cycle& c = mCycles[size_t(std::clamp<double>(a.cycle, 0, double(std::max<size_t>(1, mCycles.size()) - 1)))];
                a.phase = std::fmod(a.phase + dt * (sp / std::max(c.speed, 0.1)) / std::max(c.period, 0.1) - dt / std::max(c.period, 0.1) + 1.0, 1.0);
            }
            if (int(a.intent) == kStop || int(a.intent) == kTurn)
            {
                if (int(a.intent) == kTurn || sp < 0.2)
                {
                    const glm::vec2 h = dirOf(float(a.hold));
                    a.hx = h.x;
                    a.hz = h.y;
                    a.intent = kStill;
                }
            }
            if (int(a.intent) == kTarget)
                a.intent = kStill;
            continue;
        }
        ++gen;
        pose(state, i, p);
        const glm::dvec2 now(p[0].p.x, p[0].p.z);
        const glm::dvec2 instant = (now - glm::dvec2(a.x, a.z)) / std::max(dt, 1e-6);
        const double ease = 1.0 - std::exp(-dt / 0.15);
        a.vx += (instant.x - a.vx) * ease;
        a.vz += (instant.y - a.vz) * ease;
        a.x = now.x;
        a.z = now.y;
        const double sp = std::hypot(a.vx, a.vz);
        if (sp > 0.5)
        {
            a.hx = a.vx / sp;
            a.hz = a.vz / sp;
        }
        else if (kind == MotionKind::Hold && a.clip >= 0)
        {
            const glm::vec2 h = dirOf(headingOf(p));
            a.hx = h.x;
            a.hz = h.y;
        }
    }

    // Overlaps left after avoidance (generated motion can't swerve mid-clip): nudge them apart, a little each step.
    if (o.avoidance)
    {
        const double maxPush = 0.8 * dt;
        for (size_t k = 0; k < live.size(); ++k)
        {
            AgentState& a = at(state, live[k]);
            for (size_t m = k + 1; m < live.size(); ++m)
            {
                AgentState& b = at(state, live[m]);
                const glm::dvec2 d(b.x - a.x, b.z - a.z);
                const double want = mSetup.agents[live[k]].radius + mSetup.agents[live[m]].radius;
                const double l = glm::length(d);
                if (l >= want || std::abs(d.x) >= want || std::abs(d.y) >= want)
                    continue;
                const glm::dvec2 nrm = l > 1e-6 ? d / l : glm::dvec2(1.0, 0.0);
                const double push = std::min((want - l) * 0.5, maxPush);
                const bool aMoves = MotionKind(int(a.motion)) != MotionKind::Cycle, bMoves = MotionKind(int(b.motion)) != MotionKind::Cycle;
                (aMoves ? a.ox : a.x) -= nrm.x * push;
                (aMoves ? a.oz : a.z) -= nrm.y * push;
                (bMoves ? b.ox : b.x) += nrm.x * push;
                (bMoves ? b.oz : b.z) += nrm.y * push;
                if (aMoves)
                    a.x -= nrm.x * push, a.z -= nrm.y * push;
                if (bMoves)
                    b.x += nrm.x * push, b.z += nrm.y * push;
            }
            for (const orca::Obstacle& ob : mSetup.obstacles)
            {
                glm::dvec2 q;
                const double d = orca::polygonDistance(ob, {a.x, a.z}, &q);
                const double r = mSetup.agents[live[k]].radius;
                if (d >= r)
                    continue;
                glm::dvec2 out = glm::dvec2(a.x, a.z) - q;
                if (d < 0.0)
                    out = -out;
                if (glm::length(out) < 1e-9)
                    continue;
                out = glm::normalize(out) * std::min(r - d, maxPush * 2.0);
                const bool moves = MotionKind(int(a.motion)) != MotionKind::Cycle;
                if (moves)
                    a.ox += out.x, a.oz += out.y;
                a.x += out.x;
                a.z += out.y;
            }
        }
    }
    mStats.generatedAgents = gen;
    mStats.cycleAgents = cyc;
    mStats.alive = alive;
}
} // namespace crowds
