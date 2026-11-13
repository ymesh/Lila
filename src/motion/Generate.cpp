#include "motion/Generate.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "motion/Character.h"
#include "retarget/Retarget.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <stdexcept>

namespace lila::motion
{
namespace fs = std::filesystem;

namespace
{
constexpr float kPi = std::numbers::pi_v<float>;

float wrap(float a)
{
    while (a > kPi)
        a -= 2.0f * kPi;
    while (a < -kPi)
        a += 2.0f * kPi;
    return a;
}

glm::vec2 ground(const Pose& p) { return {p[0].p.x, p[0].p.z}; }

/** A polyline with arc length. */
struct Path
{
    std::vector<glm::vec2> pts;
    std::vector<float> at; // arc length at each point

    explicit Path(const std::vector<glm::vec2>& p) : pts(p)
    {
        at.assign(pts.size(), 0.0f);
        for (size_t i = 1; i < pts.size(); ++i)
            at[i] = at[i - 1] + glm::length(pts[i] - pts[i - 1]);
    }
    float length() const { return at.empty() ? 0.0f : at.back(); }
    glm::vec2 point(float s) const
    {
        if (pts.size() == 1)
            return pts[0];
        s = std::clamp(s, 0.0f, length());
        for (size_t i = 1; i < pts.size(); ++i)
            if (s <= at[i] || i + 1 == pts.size())
            {
                const float seg = std::max(at[i] - at[i - 1], 1e-6f);
                return glm::mix(pts[i - 1], pts[i], std::clamp((s - at[i - 1]) / seg, 0.0f, 1.0f));
            }
        return pts.back();
    }
    glm::vec2 tangent(float s) const
    {
        if (pts.size() < 2)
            return {0.0f, 1.0f};
        const glm::vec2 d = point(std::min(s + 0.05f, length())) - point(std::max(s - 0.05f, 0.0f));
        return glm::length(d) > 1e-6f ? glm::normalize(d) : glm::normalize(pts.back() - pts.front() + glm::vec2(1e-6f, 0.0f));
    }
    /** Arc length of the point nearest p, searching from at least `from` on so loops don't send it back. */
    float project(glm::vec2 p, float from) const
    {
        float best = from, bestD = 1e30f;
        for (size_t i = 1; i < pts.size(); ++i)
        {
            if (at[i] < from - 0.25f)
                continue;
            const glm::vec2 a = pts[i - 1], d = pts[i] - a;
            const float len2 = std::max(glm::dot(d, d), 1e-9f);
            const float t = std::clamp(glm::dot(p - a, d) / len2, 0.0f, 1.0f);
            const float dist = glm::length(a + d * t - p);
            if (dist < bestD)
                bestD = dist, best = at[i - 1] + t * std::sqrt(len2);
        }
        return std::max(best, from);
    }
};

float headingFromDir(glm::vec2 d) { return std::atan2(d.x, d.y); }

/** Two-bone IK of a SOMA limb onto a world transform, blended by weight. */
void pinLimb(Pose& local, const std::array<const char*, 3>& names, const Xform& goal, float weight)
{
    const SomaRig& sr = somaRig();
    std::vector<int> joints;
    for (const char* n : names)
        joints.push_back(sr.skeleton.find(n));
    retarget::Pose rp = toRetargetPose(local);
    std::vector<glm::dmat4> world = retarget::worldOf(sr.skeleton, rp);
    retarget::Pose solved = rp;
    retarget::solveChainIk(sr.skeleton, solved, world, joints, glm::dvec3(goal.p) * 100.0, glm::dquat(goal.q));
    for (int j : joints)
        rp.rotation[size_t(j)] = glm::slerp(rp.rotation[size_t(j)], solved.rotation[size_t(j)], double(weight));
    local = fromRetargetPose(rp);
}

const std::array<std::pair<uint8_t, std::array<const char*, 3>>, 4> kPinLimbs = {{
    {kPinLeftHand, {"LeftArm", "LeftForeArm", "LeftHand"}},
    {kPinRightHand, {"RightArm", "RightForeArm", "RightHand"}},
    {kPinLeftFoot, {"LeftLeg", "LeftShin", "LeftFoot"}},
    {kPinRightFoot, {"RightLeg", "RightShin", "RightFoot"}},
}};
} // namespace

// Player ------------------------------------------------------------------------------------------------------------

Player::Player(const Pose& pose) : mHistory(kPosesPerToken, pose) {}

void Player::seed(std::span<const Pose> recent)
{
    if (recent.empty())
        return;
    mHistory.clear();
    for (size_t i = recent.size() > size_t(kPosesPerToken) ? recent.size() - kPosesPerToken : 0; i < recent.size(); ++i)
        mHistory.push_back(recent[i]);
    while (mHistory.size() < size_t(kPosesPerToken))
        mHistory.insert(mHistory.begin(), mHistory.front());
}

Request Player::request(std::vector<Pose> target, uint16_t tokens, glm::vec2 heading) const
{
    Request r;
    r.context = context();
    r.target = std::move(target);
    r.tokens = tokens;
    r.heading = heading;
    return r;
}

void Player::play(Result result, int transitionEarly, int skip)
{
    mPlaying = std::move(result);
    const int count = int(mPlaying.frames.size());
    mFrame = std::clamp(1 + skip, 0, count);
    mTransition = std::max(1, count - 1 - std::max(0, transitionEarly));
}

const Pose& Player::step(Contacts* contacts)
{
    Contacts c{};
    if (mFrame < int(mPlaying.frames.size()))
    {
        mHistory.push_back(mPlaying.frames[size_t(mFrame)]);
        c = mPlaying.contacts[size_t(mFrame)];
        ++mFrame;
    }
    else
    {
        mHistory.push_back(mHistory.back());
        if (!mContacts.empty())
            c = mContacts.back();
    }
    mHistory.erase(mHistory.begin());
    mRecorded.push_back(mHistory.back());
    mContacts.push_back(c);
    if (contacts)
        *contacts = c;
    return mHistory.back();
}

int Player::framesToTransition() const { return mPlaying.frames.empty() ? 0 : mTransition - (mFrame - 1); }
int Player::framesLeft() const { return std::max(0, int(mPlaying.frames.size()) - mFrame); }
const Pose& Player::current() const { return mHistory.back(); }

std::array<Pose, kPosesPerToken> Player::context() const
{
    std::array<Pose, kPosesPerToken> c;
    for (size_t i = 0; i < c.size(); ++i)
        c[i] = mHistory[i];
    return c;
}

void Player::clearRecording()
{
    mRecorded.clear();
    mContacts.clear();
}

// Storage -----------------------------------------------------------------------------------------------------------

json poseToJson(const Pose& pose)
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    json q = json::array();
    for (const char* n : modelJointNames())
    {
        const glm::quat r = pose[size_t(s.find(n))].q;
        for (float v : {r.x, r.y, r.z, r.w})
            q.push_back(std::round(v * 1e5f) / 1e5f);
    }
    const glm::vec3 p = pose[0].p;
    return {{"root", {std::round(p.x * 1e4f) / 1e4f, std::round(p.y * 1e4f) / 1e4f, std::round(p.z * 1e4f) / 1e4f}}, {"q", q}};
}

Pose poseFromJson(const json& j, const SomaSkeleton& s)
{
    Pose p = s.reference;
    const json& q = j.at("q");
    if (!q.is_array() || q.size() != size_t(kModelJoints) * 4)
        throw std::runtime_error("a pose holds 30 rotations");
    for (int m = 0; m < kModelJoints; ++m)
    {
        const int idx = s.find(modelJointNames()[size_t(m)]);
        p[size_t(idx)].q = glm::normalize(glm::quat(q[size_t(m * 4 + 3)].get<float>(), q[size_t(m * 4)].get<float>(), q[size_t(m * 4 + 1)].get<float>(),
                                                    q[size_t(m * 4 + 2)].get<float>()));
    }
    const json& r = j.at("root");
    p[0].p = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
    return p;
}

const Gait* GaitLibrary::find(std::string_view style, double speed) const
{
    const Gait* best = nullptr;
    for (const Gait& g : gaits)
        if (g.style == style && !g.windows.empty() && (!best || (speed > 0.0 && std::abs(g.speed - speed) < std::abs(best->speed - speed))))
            best = &g;
    return best;
}

std::vector<std::string> GaitLibrary::styles() const
{
    std::vector<std::string> out;
    for (const Gait& g : gaits)
        if (std::find(out.begin(), out.end(), g.style) == out.end())
            out.push_back(g.style);
    return out;
}

json GaitLibrary::toJson() const
{
    json gs = json::array();
    for (const Gait& g : gaits)
    {
        json ws = json::array();
        for (const GaitWindow& w : g.windows)
        {
            json ps = json::array();
            for (const Pose& p : w.poses)
                ps.push_back(poseToJson(p));
            ws.push_back({{"phase", w.phase}, {"poses", ps}});
        }
        json gj = {{"name", g.name}, {"style", g.style}, {"speed", g.speed}, {"period", g.period}, {"windows", ws}};
        if (!g.cycle.empty())
        {
            json cs = json::array();
            for (const Pose& p : g.cycle)
                cs.push_back(poseToJson(p));
            gj["cycle"] = std::move(cs);
        }
        gs.push_back(std::move(gj));
    }
    json out = {{"lila", "motionGaits"}, {"version", 1}, {"provenance", provenance}, {"idle", poseToJson(idle)}, {"gaits", gs}};
    if (!crouchIdle.empty())
        out["idleCrouch"] = poseToJson(crouchIdle);
    return out;
}

GaitLibrary GaitLibrary::fromJson(const json& j)
{
    try
    {
        GaitLibrary lib;
        lib.provenance = j.value("provenance", "");
        lib.idle = poseFromJson(j.at("idle"));
        if (j.contains("idleCrouch"))
            lib.crouchIdle = poseFromJson(j["idleCrouch"]);
        for (const json& g : j.at("gaits"))
        {
            Gait gait;
            gait.name = g.at("name").get<std::string>();
            gait.style = g.at("style").get<std::string>();
            gait.speed = g.at("speed").get<double>();
            gait.period = g.value("period", 1.0);
            for (const json& w : g.at("windows"))
            {
                GaitWindow win;
                win.phase = w.value("phase", 0.0);
                const json& ps = w.at("poses");
                if (ps.size() != size_t(kPosesPerToken))
                    throw std::runtime_error("a gait window holds four poses");
                for (size_t k = 0; k < ps.size(); ++k)
                    win.poses[k] = poseFromJson(ps[k]);
                gait.windows.push_back(std::move(win));
            }
            if (g.contains("cycle"))
                for (const json& p : g["cycle"])
                    gait.cycle.push_back(poseFromJson(p));
            lib.gaits.push_back(std::move(gait));
        }
        return lib;
    }
    catch (const json::exception& e)
    {
        throw std::runtime_error(std::string("not a gait library: ") + e.what());
    }
}

FootStrikes footStrikes(const std::vector<Pose>& frames)
{
    FootStrikes out;
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const int feet[2] = {s.find("LeftFoot"), s.find("RightFoot")};
    std::vector<float> h[2];
    for (const Pose& p : frames)
    {
        const Pose w = s.world(p);
        for (int k = 0; k < 2; ++k)
            h[k].push_back(w[size_t(feet[k])].p.y);
    }
    for (int k = 0; k < 2; ++k)
    {
        if (feet[k] < 0 || h[k].size() < 3)
            continue;
        std::vector<float> sorted = h[k];
        std::sort(sorted.begin(), sorted.end());
        // Percentiles, so a stumble or a raised foot at the clip's ends doesn't set the range.
        const double lo = sorted[sorted.size() / 20], hi = sorted[sorted.size() - 1 - sorted.size() / 20];
        if (hi - lo < 1e-3)
            continue;
        const double down = lo + 0.25 * (hi - lo), up = lo + 0.5 * (hi - lo);
        std::vector<double>& strikes = k == 0 ? out.left : out.right;
        std::vector<double>& lifts = k == 0 ? out.leftLifts : out.rightLifts;
        bool armed = h[k][0] > up;
        for (size_t f = 1; f < h[k].size(); ++f)
        {
            const double a = h[k][f - 1], b = h[k][f];
            if (a <= down && b > down)
                lifts.push_back(double(f - 1) + (down - a) / (b - a));
            if (b > up)
                armed = true;
            if (armed && a > down && b <= down)
            {
                strikes.push_back(double(f - 1) + (a - down) / (a - b));
                armed = false;
            }
        }
    }
    return out;
}

double cycleFrames(const FootStrikes& strikes)
{
    std::vector<double> gaps;
    for (const std::vector<double>* foot : {&strikes.left, &strikes.right})
        for (size_t i = 1; i < foot->size(); ++i)
            gaps.push_back((*foot)[i] - (*foot)[i - 1]);
    if (gaps.empty())
        return 0.0;
    std::vector<double> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    // A missed strike makes one gap twice as long; the gaps near the median are the cycle.
    double sum = 0.0;
    int count = 0;
    for (double g : gaps)
        if (std::abs(g - median) <= 0.2 * median)
        {
            sum += g;
            ++count;
        }
    return sum / double(count);
}

std::vector<Pose> loopCycle(const std::vector<Pose>& frames, double start, double length)
{
    std::vector<Pose> out;
    if (frames.empty() || !(length > 0.0))
        return out;
    const int n = std::max(2, int(std::lround(length)));
    auto at = [&](double t) {
        t = std::clamp(t, 0.0, double(frames.size() - 1));
        const size_t i0 = size_t(std::floor(t)), i1 = std::min(i0 + 1, frames.size() - 1);
        const float w = float(t - double(i0));
        Pose p = frames[i0];
        for (size_t j = 0; j < p.size(); ++j)
        {
            p[j].q = glm::normalize(glm::slerp(frames[i0][j].q, frames[i1][j].q, w));
            p[j].p = glm::mix(frames[i0][j].p, frames[i1][j].p, w);
        }
        return p;
    };
    for (int i = 0; i <= n; ++i)
        out.push_back(at(start + length * double(i) / double(n)));

    const glm::quat back = glm::angleAxis(-headingOf(out[0]), glm::vec3(0, 1, 0));
    const glm::vec3 origin(out[0][0].p.x, 0.0f, out[0][0].p.z);
    for (Pose& p : out)
    {
        const glm::vec3 r = back * (p[0].p - origin);
        p[0].p = glm::vec3(r.x, p[0].p.y, r.z);
        p[0].q = glm::normalize(back * p[0].q);
    }
    const glm::vec2 travel(out[size_t(n)][0].p.x, out[size_t(n)][0].p.z);
    for (int i = 0; i <= n; ++i)
    {
        const float u = float(i) / float(n);
        out[size_t(i)][0].p.x -= travel.x * u;
        out[size_t(i)][0].p.z -= travel.y * u;
    }
    const Pose& first = out[0];
    const Pose last = out[size_t(n)];
    for (size_t j = 0; j < first.size(); ++j)
    {
        glm::quat d = glm::normalize(first[j].q * glm::conjugate(last[j].q));
        if (d.w < 0.0f)
            d = -d;
        const glm::vec3 dp = first[j].p - last[j].p;
        for (int i = 1; i < n; ++i)
        {
            const float u = float(i) / float(n);
            Xform& x = out[size_t(i)][j];
            x.q = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), d, u) * x.q);
            x.p += dp * u;
        }
    }
    out.pop_back();
    return out;
}

GaitLibrary GaitLibrary::load(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot open " + paths::toUtf8(file));
    return fromJson(json::parse(in));
}

const GaitLibrary& GaitLibrary::shipped()
{
    static const GaitLibrary lib = load(paths::resourcesDir() / "motion" / "gaits.json");
    return lib;
}

std::vector<Pose> gaitTarget(const Gait& gait, const GaitWindow& window, glm::vec2 at, float facing, double speed)
{
    const float scale = gait.speed > 1e-3 && speed > 0.0 ? float(speed / gait.speed) : 1.0f;
    const glm::quat turn = glm::angleAxis(facing, glm::vec3(0, 1, 0));
    const glm::vec3 origin = window.poses[0][0].p;
    std::vector<Pose> out;
    for (const Pose& p : window.poses)
    {
        Pose q = p;
        glm::vec3 off = p[0].p - origin;
        off.x *= scale, off.z *= scale;
        const glm::vec3 r = turn * glm::vec3(off.x, 0.0f, off.z);
        q[0].p = glm::vec3(at.x + r.x, p[0].p.y, at.y + r.z);
        q[0].q = glm::normalize(turn * p[0].q);
        out.push_back(std::move(q));
    }
    return out;
}

json Spot::toJson() const
{
    json ts = json::array();
    for (const SpotTarget& t : targets)
    {
        json ps = json::array();
        for (const Pose& p : t.poses)
            ps.push_back(poseToJson(p));
        int lo = kMaxTokens, hi = kMinTokens;
        for (int k = kMinTokens; k <= kMaxTokens; ++k)
            if ((t.tokens >> (k - kMinTokens)) & 1)
                lo = std::min(lo, k), hi = std::max(hi, k);
        json pins = json::array();
        for (const auto& [bit, pin] : {std::pair{kPinLeftHand, "leftHand"}, {kPinRightHand, "rightHand"}, {kPinLeftFoot, "leftFoot"}, {kPinRightFoot, "rightFoot"}})
            if (t.pins & bit)
                pins.push_back(pin);
        ts.push_back({{"name", t.name}, {"poses", ps}, {"transitionEarly", t.transitionEarly}, {"tokens", {lo, hi}}, {"pins", pins}, {"hold", t.hold}});
    }
    return {{"lila", "motionSpot"}, {"version", 1}, {"name", name}, {"provenance", provenance}, {"targets", ts}};
}

Spot Spot::fromJson(const json& j)
{
    try
    {
        if (j.value("lila", "") != "motionSpot")
            throw std::runtime_error("not a spot file");
        Spot s;
        s.name = j.value("name", "");
        s.provenance = j.value("provenance", "");
        for (const json& t : j.at("targets"))
        {
            SpotTarget st;
            st.name = t.value("name", "");
            for (const json& p : t.at("poses"))
                st.poses.push_back(poseFromJson(p));
            if (st.poses.empty() || st.poses.size() > size_t(kPosesPerToken))
                throw std::runtime_error("a target holds 1 to 4 poses");
            st.transitionEarly = t.value("transitionEarly", 0);
            if (t.contains("tokens"))
                st.tokens = tokenMask(t["tokens"][0].get<int>(), t["tokens"][1].get<int>());
            for (const json& p : t.value("pins", json::array()))
            {
                const std::string n = p.get<std::string>();
                st.pins |= n == "leftHand" ? kPinLeftHand : n == "rightHand" ? kPinRightHand : n == "leftFoot" ? kPinLeftFoot : n == "rightFoot" ? kPinRightFoot : 0;
            }
            st.hold = t.value("hold", 0);
            s.targets.push_back(std::move(st));
        }
        return s;
    }
    catch (const json::exception& e)
    {
        throw std::runtime_error(std::string("not a spot file: ") + e.what());
    }
}

Spot Spot::load(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot open " + paths::toUtf8(file));
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error(paths::toUtf8(file) + " is not JSON");
    Spot s = fromJson(j);
    if (s.name.empty())
        s.name = paths::toUtf8(file.stem());
    return s;
}

void Spot::save(const fs::path& file) const
{
    std::ofstream out(file);
    if (!out)
        throw std::runtime_error("cannot write " + paths::toUtf8(file));
    out << toJson().dump(1);
}

std::vector<std::pair<std::string, fs::path>> shippedSpots()
{
    std::vector<std::pair<std::string, fs::path>> out;
    const fs::path dir = paths::resourcesDir() / "motion" / "spots";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".lilaspot")
            out.push_back({paths::toUtf8(e.path().stem()), e.path()});
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<PlacedTarget> placeSpot(const Spot& spot, glm::vec2 at, float heading)
{
    const glm::quat turn = glm::angleAxis(heading, glm::vec3(0, 1, 0));
    std::vector<PlacedTarget> out;
    for (const SpotTarget& t : spot.targets)
    {
        PlacedTarget p{.transitionEarly = t.transitionEarly, .tokens = t.tokens, .pins = t.pins, .hold = t.hold,
                       .label = spot.name + (t.name.empty() ? std::string() : ": " + t.name)};
        for (const Pose& pose : t.poses)
        {
            Pose q = pose;
            const glm::vec3 r = turn * glm::vec3(pose[0].p.x, 0.0f, pose[0].p.z);
            q[0].p = glm::vec3(at.x + r.x, pose[0].p.y, at.y + r.z);
            q[0].q = glm::normalize(turn * pose[0].q);
            p.poses.push_back(std::move(q));
        }
        out.push_back(std::move(p));
    }
    return out;
}

glm::vec2 variantHeading(uint32_t variant)
{
    if (variant == 0)
        return {1.0f, 0.0f};
    const float a = float(variant) * 2.39996323f; // golden angle
    return {std::cos(a), std::sin(a)};
}

// Journeys ----------------------------------------------------------------------------------------------------------

std::vector<Clip> followPaths(Engine& engine, std::span<const Journey> journeys, const GaitLibrary& gaits)
{
    LILA_ZONE();
    enum class Phase
    {
        Walk,
        Target,
        Settle,
        Done,
    };
    struct Agent
    {
        const Journey* j = nullptr;
        Player player;
        size_t leg = 0;
        int target = -1; // index of the target being generated toward
        Phase phase = Phase::Walk;
        float along = 0.0f;
        double cycle = 0.0;
        int hold = 0;
        bool held = false;
        const Gait* gait = nullptr;
        glm::vec2 facing{0.0f};
        Clip clip;
        std::vector<std::pair<int, const PlacedTarget*>> pins;
    };

    std::vector<Agent> agents;
    agents.reserve(journeys.size());
    for (const Journey& j : journeys)
    {
        Agent a{.j = &j, .player = Player(j.start)};
        if (!j.before.empty())
        {
            std::vector<Pose> seed = j.before;
            seed.push_back(j.start);
            a.player.seed(seed);
        }
        a.gait = gaits.find(j.options.style, j.options.speed);
        if (!a.gait)
            throw std::invalid_argument("no gait for the style '" + j.options.style + "'");
        a.facing = j.options.facing;
        if (glm::length(a.facing) < 1e-6f)
        {
            const Leg* first = j.legs.empty() ? nullptr : &j.legs.front();
            a.facing = first && first->path.size() > 1 ? Path(first->path).tangent(0.0f) : glm::vec2(std::sin(headingOf(j.start)), std::cos(headingOf(j.start)));
        }
        a.facing = glm::normalize(a.facing);
        // Strafing faces across the path, so its sideways step runs along it.
        if (glm::length(j.options.facing) < 1e-6f && j.options.style == "strafeLeft")
            a.facing = {-a.facing.y, a.facing.x};
        else if (glm::length(j.options.facing) < 1e-6f && j.options.style == "strafeRight")
            a.facing = {a.facing.y, -a.facing.x};
        a.phase = j.legs.empty() ? Phase::Done : j.legs.front().path.size() > 1 ? Phase::Walk : Phase::Target;
        if (a.phase == Phase::Done)
            a.clip.error = "nothing to do: no path or targets";
        agents.push_back(std::move(a));
    }

    // What an agent generates toward next, if anything; advances its state.
    struct Next
    {
        std::vector<Pose> target;
        uint16_t tokens = kDefaultTokens;
        int early = 0;
    };
    auto decide = [&](Agent& a) -> std::optional<Next> {
        const PathOptions& o = a.j->options;
        const double speed = o.speed > 0.0 ? o.speed : a.gait->speed;
        for (int guard = 0; guard < 8; ++guard)
        {
            if (a.leg >= a.j->legs.size())
            {
                a.phase = Phase::Done;
                return std::nullopt;
            }
            const Leg& leg = a.j->legs[a.leg];
            if (a.phase == Phase::Walk)
            {
                const Path path(leg.path);
                a.along = path.project(ground(a.player.current()), a.along);
                const float ahead = float(speed * o.lookahead);
                const bool last = a.leg + 1 == a.j->legs.size();
                if (path.length() - a.along <= ahead * 1.1f)
                {
                    if (!leg.targets.empty())
                    {
                        a.phase = Phase::Target;
                        a.target = -1;
                        continue;
                    }
                    if (last && o.stop)
                    {
                        a.phase = Phase::Settle;
                        const glm::vec2 end = path.point(path.length());
                        const glm::vec2 dir = o.style.starts_with("strafe") ? a.facing : o.style == "backward" ? -path.tangent(path.length()) : path.tangent(path.length());
                        return Next{.target = {placed(gaits.rest(o.style), headingFromDir(dir), end)}, .tokens = kDefaultTokens, .early = 0};
                    }
                    if (!last)
                    {
                        ++a.leg;
                        a.along = 0.0f;
                        a.phase = a.j->legs[a.leg].path.size() > 1 ? Phase::Walk : Phase::Target;
                        a.target = -1;
                        continue;
                    }
                    // A last leg with no stop keeps its pace to the end, then lets the last motion play out.
                    if (last && path.length() - a.along < 0.3f)
                    {
                        a.phase = Phase::Done;
                        return std::nullopt;
                    }
                }
                const float s = std::min(a.along + ahead, path.length());
                const glm::vec2 tan = path.tangent(s);
                const glm::vec2 dir = o.style.starts_with("strafe") ? a.facing : o.style == "backward" ? -tan : tan;
                const double framesAhead = o.lookahead * kSampleRate;
                const double phase = std::fmod(a.cycle + framesAhead / (kSampleRate * std::max(a.gait->period, 0.1)), 1.0);
                const GaitWindow* win = &a.gait->windows.front();
                double bestD = 2.0;
                for (const GaitWindow& w : a.gait->windows)
                {
                    const double d = std::min(std::abs(w.phase - phase), 1.0 - std::abs(w.phase - phase));
                    if (d < bestD)
                        bestD = d, win = &w;
                }
                return Next{.target = gaitTarget(*a.gait, *win, path.point(s), headingFromDir(dir), speed), .tokens = o.tokens, .early = o.transitionEarly};
            }
            if (a.phase == Phase::Target)
            {
                if (a.target >= 0 && !a.held)
                {
                    const PlacedTarget& reached = leg.targets[size_t(a.target)];
                    a.clip.reached.push_back({int(a.player.recorded().size()), reached.label});
                    if (reached.pins)
                        a.pins.push_back({int(a.player.recorded().size()), &reached});
                    if (reached.hold > 0)
                    {
                        a.hold = reached.hold;
                        a.held = true;
                        return std::nullopt;
                    }
                }
                a.held = false;
                ++a.target;
                if (a.target < int(leg.targets.size()))
                {
                    const PlacedTarget& t = leg.targets[size_t(a.target)];
                    return Next{.target = t.poses, .tokens = t.tokens, .early = t.transitionEarly};
                }
                ++a.leg;
                a.along = 0.0f;
                a.target = -1;
                if (a.leg >= a.j->legs.size())
                {
                    a.phase = Phase::Done;
                    return std::nullopt;
                }
                a.phase = a.j->legs[a.leg].path.size() > 1 ? Phase::Walk : Phase::Target;
                continue;
            }
            if (a.phase == Phase::Settle)
            {
                a.phase = Phase::Done;
                return std::nullopt;
            }
            return std::nullopt;
        }
        return std::nullopt;
    };

    int frame = 0;
    int maxFrames = 0;
    for (const Journey& j : journeys)
        maxFrames = std::max(maxFrames, j.options.maxFrames);
    while (frame < maxFrames)
    {
        std::vector<Request> batch;
        std::vector<std::pair<size_t, int>> who; // agent, transition early
        bool any = false;
        for (size_t i = 0; i < agents.size(); ++i)
        {
            Agent& a = agents[i];
            if (a.phase == Phase::Done)
            {
                // Settled agents play out their last generation before they stop.
                continue;
            }
            any = true;
            if (a.hold > 0 || a.player.framesToTransition() > 0)
                continue;
            if (auto next = decide(a))
            {
                batch.push_back(a.player.request(std::move(next->target), next->tokens, variantHeading(a.j->options.variant + uint32_t(a.clip.generations))));
                who.push_back({i, next->early});
            }
        }
        if (!any)
            break;
        if (!batch.empty())
        {
            std::vector<Result> res = engine.generate(batch);
            for (size_t k = 0; k < res.size(); ++k)
            {
                Agent& a = agents[who[k].first];
                if (!res[k].error.empty())
                {
                    a.clip.error = res[k].error;
                    a.phase = Phase::Done;
                    continue;
                }
                ++a.clip.generations;
                a.player.play(std::move(res[k]), who[k].second);
            }
        }
        for (Agent& a : agents)
        {
            const bool finishing = a.phase == Phase::Done && a.player.framesLeft() > 0;
            if (a.phase == Phase::Done && !finishing)
                continue;
            a.player.step();
            a.cycle = std::fmod(a.cycle + 1.0 / (kSampleRate * std::max(a.gait->period, 0.1)), 1.0);
            if (a.hold > 0)
                --a.hold;
        }
        ++frame;
    }

    std::vector<Clip> out;
    for (Agent& a : agents)
    {
        a.clip.frames = a.player.recorded();
        a.clip.contacts = a.player.recordedContacts();
        if (frame >= maxFrames && a.phase != Phase::Done && a.clip.error.empty())
            a.clip.error = "stopped after " + std::to_string(maxFrames) + " frames before the end of the path";
        // Hands and feet ease onto pinned targets over the frames leading in, and stay through the hold.
        for (const auto& [at, t] : a.pins)
        {
            const SomaSkeleton& s = SomaSkeleton::shipped();
            const Pose goalWorld = s.world(t->poses.front());
            constexpr int kLead = 8;
            for (int f = std::max(0, at - kLead); f < std::min(int(a.clip.frames.size()), at + 1 + t->hold); ++f)
            {
                const float w = f >= at ? 1.0f : glm::smoothstep(0.0f, 1.0f, float(f - (at - kLead)) / float(kLead));
                for (const auto& [bit, limb] : kPinLimbs)
                    if (t->pins & bit)
                        pinLimb(a.clip.frames[size_t(f)], limb, goalWorld[size_t(s.find(limb[2]))], w);
            }
        }
        out.push_back(std::move(a.clip));
    }
    return out;
}
// Live driving ------------------------------------------------------------------------------------------------------

LiveDriver::LiveDriver(Engine& engine, const Pose& start, std::span<const Pose> before, const GaitLibrary& gaits)
    : mEngine(engine), mGaits(gaits), mPlayer(start)
{
    if (!before.empty())
    {
        std::vector<Pose> seed(before.begin(), before.end());
        seed.push_back(start);
        mPlayer.seed(seed);
    }
}

LiveDriver::~LiveDriver() { wait(); }

void LiveDriver::setInput(const DriveInput& input) { mInput = input; }

bool LiveDriver::wantsGeneration() const
{
    if (mPending.valid())
        return false;
    const float mag = glm::length(mInput.move);
    if (mag < 0.1f)
        return !mStill;
    if (mStill || mPlayer.framesToTransition() <= mLatency)
        return true;
    if (mFrame - mLastGen < 8)
        return false;
    const float was = glm::length(mAsked.move);
    const float turn = was > 1e-3f ? std::acos(std::clamp(glm::dot(mInput.move / mag, mAsked.move / was), -1.0f, 1.0f)) : kPi;
    return turn > 0.45f || std::abs(mag - was) > 0.25f || mInput.style != mAsked.style || glm::length(mInput.facing - mAsked.facing) > 0.2f;
}

Request LiveDriver::nextRequest()
{
    const Pose& now = mPlayer.current();
    const glm::vec2 at = ground(now);
    const float mag = glm::length(mInput.move);
    mAsked = mInput;
    if (mag < 0.1f)
    {
        mStill = true;
        mPendingEarly = 0;
        return mPlayer.request({placed(mGaits.rest(mAsked.style), headingOf(now), at)});
    }
    mStill = false;
    const Gait* gait = mGaits.find(mInput.style);
    if (!gait)
        gait = mGaits.find("walk");
    if (!gait)
        throw std::runtime_error("the gait library has no walk");
    const double speed = gait->speed * double(std::clamp(mag, 0.3f, 1.0f));
    const glm::vec2 dir = mInput.move / mag;
    glm::vec2 face = glm::length(mInput.facing) > 1e-3f ? glm::normalize(mInput.facing) : dir;
    if (mInput.style == "backward" && glm::length(mInput.facing) <= 1e-3f)
        face = -dir;
    else if (mInput.style == "strafeLeft" && glm::length(mInput.facing) <= 1e-3f)
        face = {-dir.y, dir.x};
    else if (mInput.style == "strafeRight" && glm::length(mInput.facing) <= 1e-3f)
        face = {dir.y, -dir.x};
    constexpr double kLookahead = 0.9;
    const double phase = std::fmod(mCycle + kLookahead / std::max(gait->period, 0.1), 1.0);
    const GaitWindow* win = &gait->windows.front();
    double best = 2.0;
    for (const GaitWindow& w : gait->windows)
        if (const double d = std::min(std::abs(w.phase - phase), 1.0 - std::abs(w.phase - phase)); d < best)
            best = d, win = &w;
    mPendingEarly = 10;
    return mPlayer.request(gaitTarget(*gait, *win, at + dir * float(speed * kLookahead), headingFromDir(face), speed), tokenMask(7, 11));
}

void LiveDriver::collect(bool block)
{
    if (!mPending.valid())
        return;
    if (!block && mPending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    Result r = mPending.get();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mSent).count();
    mLatency = std::clamp(int(std::ceil(ms / (1000.0 / kSampleRate))) + 1, 1, 20);
    if (!r.error.empty())
    {
        mError = r.error;
        return;
    }
    ++mGenerations;
    mPlayer.play(std::move(r), mPendingEarly, mFrame - mRequested);
}

void LiveDriver::wait() { collect(true); }

const Pose& LiveDriver::tick()
{
    collect(false);
    if (wantsGeneration())
    {
        try
        {
            const Request req = nextRequest();
            mRequested = mFrame;
            mLastGen = mFrame;
            mSent = std::chrono::steady_clock::now();
            if (mSync)
            {
                Result r = mEngine.generate(std::span(&req, 1)).front();
                if (r.error.empty())
                {
                    ++mGenerations;
                    mPlayer.play(std::move(r), mPendingEarly);
                }
                else
                    mError = r.error;
            }
            else
            {
                Engine* engine = &mEngine;
                mPending = std::async(std::launch::async, [engine, req] { return engine->generate(std::span(&req, 1)).front(); });
            }
        }
        catch (const std::exception& e)
        {
            mError = e.what();
        }
    }
    const Pose& p = mPlayer.step();
    ++mFrame;
    const Gait* gait = mGaits.find(mAsked.style);
    mCycle = std::fmod(mCycle + 1.0 / (kSampleRate * std::max(gait ? gait->period : 1.0, 0.1)), 1.0);
    return p;
}
} // namespace lila::motion
