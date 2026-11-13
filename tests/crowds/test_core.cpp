// The crowd runtime without the model: a stand-in generator moves each request's last context pose to its first target
// in a straight line, so scheduling, avoidance, behaviours and determinism can be checked anywhere, fast.

#include <doctest/doctest.h>

#include "CrowdCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace crowds;
using namespace lila::motion;

namespace
{
class LineGenerator : public Generator
{
public:
    std::vector<size_t> batches;

    std::vector<Result> generate(std::span<const Request> requests) override
    {
        batches.push_back(requests.size());
        std::vector<Result> out;
        for (const Request& r : requests)
        {
            const Pose& from = r.context.back();
            const Pose& to = r.target.front();
            int tokens = kMinTokens;
            while (tokens < kMaxTokens && !((r.tokens >> (tokens - kMinTokens)) & 1))
                ++tokens;
            const int frames = framesForTokens(tokens);
            Result res;
            res.tokens = tokens;
            for (int f = 0; f < frames; ++f)
            {
                const float w = float(f) / float(frames - 1);
                Pose p = from;
                for (size_t j = 0; j < p.size(); ++j)
                {
                    p[j].q = glm::normalize(glm::slerp(from[j].q, to[j].q, w));
                    p[j].p = glm::mix(from[j].p, to[j].p, w);
                }
                res.frames.push_back(std::move(p));
                res.contacts.push_back({1.0f, 1.0f, 0.0f, 0.0f});
            }
            out.push_back(std::move(res));
        }
        return out;
    }
    std::string device() const override { return "test"; }
};

/** Largest joint turn between two poses, degrees. */
double turnBetween(const Pose& a, const Pose& b)
{
    double turn = 0.0;
    for (size_t j = 0; j < a.size(); ++j)
        turn = std::max(turn, double(glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a[j].q, b[j].q)))))));
    return turn;
}

Setup wanderers(int count, float spacing, uint32_t seed = 3)
{
    Setup s;
    s.groups.push_back(Group{.name = "a", .behaviour = Behaviour::Wander, .center = {0, 0}, .radius = 12.0f, .pauseMin = 0.5f, .pauseMax = 1.5f});
    const int side = int(std::ceil(std::sqrt(double(count))));
    for (int i = 0; i < count; ++i)
        s.agents.push_back(AgentSpec{.start = {float(i % side) * spacing - 6.0f, float(i / side) * spacing - 6.0f}, .heading = 0.3f * float(i),
                                     .seed = uint32_t(i * 17 + 1), .speed = 0.9f + 0.2f * float(i % 3) / 2.0f});
    s.options.seed = seed;
    s.options.detail = Detail::Generated;
    return s;
}

void run(Runtime& rt, std::vector<double>& state, int from, int to, double fps = 30.0)
{
    for (int f = from; f < to; ++f)
        rt.step(state, 1.0 / fps, double(f + 1) / fps, {});
}

double closest(const Runtime& rt, const std::vector<double>& state)
{
    double best = 1e9;
    const size_t n = rt.setup().agents.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
        {
            const AgentState& a = rt.agent(state, i);
            const AgentState& b = rt.agent(state, j);
            best = std::min(best, std::hypot(a.x - b.x, a.z - b.z) - rt.setup().agents[i].radius - rt.setup().agents[j].radius);
        }
    return best;
}
} // namespace

TEST_CASE("crowd: requests wait for each other so batches fill")
{
    LineGenerator gen;
    Setup s = wanderers(64, 1.5f);
    s.options.batch = 16;
    Runtime rt(s, GaitLibrary::shipped(), &gen);
    std::vector<double> state;
    rt.reset(state);
    run(rt, state, 0, 300);
    REQUIRE(gen.batches.size() > 4);
    CHECK(*std::max_element(gen.batches.begin(), gen.batches.end()) <= 16);
    size_t total = 0;
    for (size_t b : gen.batches)
        total += b;
    const double fill = double(total) / double(gen.batches.size());
    MESSAGE("batches " << gen.batches.size() << ", mean fill " << fill << ", generations " << rt.stats().generations);
    CHECK(fill >= 8.0);
    CHECK(rt.stats().generations == int64_t(total));
    // Nobody ran past its motion waiting for a batch: every generated agent still has motion left or is holding.
    for (size_t i = 0; i < s.agents.size(); ++i)
    {
        const AgentState& a = rt.agent(state, i);
        if (MotionKind(int(a.motion)) == MotionKind::Clip)
            CHECK(a.t <= a.transition + 2.0 / 30.0 + 1e-9);
    }
}

TEST_CASE("crowd: a batch of one runs when an agent would otherwise run out")
{
    LineGenerator gen;
    Setup s = wanderers(1, 1.0f);
    Runtime rt(s, GaitLibrary::shipped(), &gen);
    std::vector<double> state;
    rt.reset(state);
    run(rt, state, 0, 120);
    REQUIRE(!gen.batches.empty());
    CHECK(std::all_of(gen.batches.begin(), gen.batches.end(), [](size_t b) { return b == 1; }));
    const AgentState& a = rt.agent(state, 0);
    CHECK(std::hypot(a.x - s.agents[0].start.x, a.z - s.agents[0].start.y) > 0.5);
}

TEST_CASE("crowd: the same setup gives the same crowd, and a resumed state continues identically")
{
    LineGenerator g1, g2;
    Setup s = wanderers(40, 1.2f);
    Runtime a(s, GaitLibrary::shipped(), &g1), b(s, GaitLibrary::shipped(), &g2);
    std::vector<double> sa, sb;
    a.reset(sa);
    b.reset(sb);
    run(a, sa, 0, 90);
    std::vector<double> mid = sa;
    run(a, sa, 90, 200);
    run(b, sb, 0, 200);
    REQUIRE(sa.size() == sb.size());
    CHECK(std::memcmp(sa.data(), sb.data(), sa.size() * sizeof(double)) == 0);

    // Stepping again from frame 90 (as the cache does after an edit) drops the later clips and makes the same ones.
    run(a, mid, 90, 200);
    CHECK(std::memcmp(mid.data(), sa.data(), sa.size() * sizeof(double)) == 0);
    CHECK(a.clipCount() == size_t(sa[0]));

    // A different seed gives a different crowd.
    Setup other = s;
    other.options.seed = 99;
    LineGenerator g3;
    Runtime c(other, GaitLibrary::shipped(), &g3);
    std::vector<double> sc;
    c.reset(sc);
    run(c, sc, 0, 200);
    CHECK(std::memcmp(sc.data(), sa.data(), sa.size() * sizeof(double)) != 0);
}

TEST_CASE("crowd: avoidance keeps agents walking through each other apart")
{
    // Two lines walking head on through each other, on cycles so avoidance steers them directly.
    Setup s;
    s.groups.push_back(Group{.name = "east", .behaviour = Behaviour::GoTo, .goal = {12, 0}});
    s.groups.push_back(Group{.name = "west", .behaviour = Behaviour::GoTo, .goal = {-12, 0}});
    for (int i = 0; i < 6; ++i)
    {
        s.agents.push_back(AgentSpec{.start = {-10.0f, float(i) - 2.5f}, .heading = 1.57f, .seed = uint32_t(i + 1), .group = 0});
        s.agents.push_back(AgentSpec{.start = {10.0f, float(i) - 2.4f}, .heading = -1.57f, .seed = uint32_t(i + 50), .group = 1});
    }
    s.options.detail = Detail::Cycles;
    for (bool avoid : {false, true})
    {
        s.options.avoidance = avoid;
        Runtime rt(s, GaitLibrary::shipped(), nullptr);
        std::vector<double> state;
        rt.reset(state);
        double worst = 1e9;
        for (int f = 0; f < 30 * 22; ++f)
        {
            rt.step(state, 1.0 / 30.0, double(f + 1) / 30.0, {});
            worst = std::min(worst, closest(rt, state));
        }
        MESSAGE("avoidance " << avoid << ": closest gap " << worst);
        if (avoid)
            CHECK(worst > -0.12);
        else
            CHECK(worst < -0.3);
        // They still get where they were going.
        int arrived = 0;
        for (size_t i = 0; i < s.agents.size(); ++i)
        {
            const AgentState& a = rt.agent(state, i);
            const glm::vec2 goal = s.groups[size_t(s.agents[i].group)].goal;
            arrived += std::hypot(a.x - goal.x, a.z - goal.y) < 4.0 ? 1 : 0;
        }
        CHECK(arrived >= int(s.agents.size()) - 1);
    }
}

TEST_CASE("crowd: orca keeps clear of a static obstacle")
{
    orca::Obstacle box{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    orca::Agent a{.position = {-4, 0.05}, .velocity = {1.3, 0}, .preferred = {1.3, 0}, .radius = 0.3, .maxSpeed = 1.5};
    glm::dvec2 p = a.position;
    double worst = 1e9;
    for (int k = 0; k < 30 * 8; ++k)
    {
        a.position = p;
        a.preferred = glm::normalize(glm::dvec2(4, 0) - p) * 1.3;
        const glm::dvec2 v = orca::solve({a}, {box}, {}).front();
        a.velocity = v;
        p += v / 30.0;
        worst = std::min(worst, orca::polygonDistance(box, p));
    }
    CHECK(worst > 0.25);
    CHECK(p.x > 3.0);
}

TEST_CASE("crowd: lanes route along the network")
{
    Network net;
    net.lanes.push_back({.points = {{0, 0}, {10, 0}}});
    net.lanes.push_back({.points = {{10, 0}, {10, 10}}});
    net.lanes.push_back({.points = {{10, 10}, {0, 10}}});
    net.lanes.push_back({.points = {{50, 50}, {60, 50}}}); // not joined
    net.connect();
    const std::vector<glm::vec2> r = net.route({1, 0.5f}, {1, 9.5f});
    REQUIRE(r.size() >= 4);
    // Round by (10, 0) and (10, 10), not straight across.
    bool corner1 = false, corner2 = false;
    for (const glm::vec2& p : r)
    {
        corner1 |= glm::length(p - glm::vec2(10, 0)) < 0.1f;
        corner2 |= glm::length(p - glm::vec2(10, 10)) < 0.1f;
    }
    CHECK(corner1);
    CHECK(corner2);
}

TEST_CASE("crowd: a queue lines up at its spacing and moves up as the head is served")
{
    LineGenerator gen;
    Setup s;
    s.groups.push_back(Group{.name = "q", .behaviour = Behaviour::Queue, .path = {{0, 0}, {0, 10}}, .spacing = 1.0f, .serve = 14.0f, .then = Behaviour::Wander});
    s.groups.back().radius = 5.0f;
    s.groups.back().center = {6, 10};
    for (int i = 0; i < 5; ++i)
        s.agents.push_back(AgentSpec{.start = {3.0f + float(i), 2.0f}, .seed = uint32_t(i + 3), .slot = i});
    s.options.detail = Detail::Generated;
    Runtime rt(s, GaitLibrary::shipped(), &gen);
    std::vector<double> state;
    rt.reset(state);
    run(rt, state, 0, 30 * 12);
    for (int i = 0; i < 5; ++i)
    {
        const AgentState& a = rt.agent(state, size_t(i));
        CHECK(std::abs(a.x) < 0.6);
        CHECK(std::abs(a.z - (10.0 - double(i))) < 0.6);
    }
    run(rt, state, 30 * 12, 30 * 18);
    // One served: the second is now at the head.
    const AgentState& second = rt.agent(state, 1);
    CHECK(std::abs(second.z - 10.0) < 0.6);
    CHECK(rt.label(state, 0) != "queue");
}

TEST_CASE("crowd: detail keeps far agents on cycles and the budget caps generation")
{
    LineGenerator gen;
    Setup s = wanderers(30, 2.0f);
    s.options.detail = Detail::Auto;
    s.options.focusInput = 0;
    s.options.generatedRadius = 4.0;
    s.options.generatedMax = 5;
    Runtime rt(s, GaitLibrary::shipped(), &gen);
    std::vector<double> state;
    rt.reset(state);
    const glm::vec2 focus(-6, -6);
    for (int f = 0; f < 150; ++f)
        rt.step(state, 1.0 / 30.0, double(f + 1) / 30.0, std::span(&focus, 1));
    CHECK(rt.stats().generatedAgents <= 5);
    CHECK(rt.stats().cycleAgents >= 20);
    CHECK(rt.stats().generations > 0);

    // Without an engine every agent plays cycles, and the runtime says why.
    Runtime none(s, GaitLibrary::shipped(), nullptr, "the model is missing");
    std::vector<double> st;
    none.reset(st);
    for (int f = 0; f < 60; ++f)
        none.step(st, 1.0 / 30.0, double(f + 1) / 30.0, std::span(&focus, 1));
    CHECK(none.stats().generations == 0);
    CHECK(none.stats().note == "the model is missing");
    Pose p;
    CHECK(none.pose(st, 3, p));
    CHECK(p.size() == SomaSkeleton::shipped().size());
}

TEST_CASE("crowd: gait cycles loop seamlessly, one per gait, a frame per tick")
{
    Setup s;
    s.groups.push_back(Group{.name = "a", .behaviour = Behaviour::Stand});
    s.agents.push_back(AgentSpec{.start = {0.0f, 0.0f}});
    Runtime rt(s, GaitLibrary::shipped(), nullptr);
    REQUIRE(rt.cycles().size() == GaitLibrary::shipped().gaits.size());
    for (const Cycle& c : rt.cycles())
    {
        INFO(c.style << " " << c.period);
        REQUIRE(c.frames.size() > 8);
        CHECK(c.period == doctest::Approx(double(c.frames.size()) / 30.0));
        double turn = 0.0;
        for (size_t f = 1; f < c.frames.size(); ++f)
            turn = std::max(turn, turnBetween(c.frames[f - 1], c.frames[f]));
        CHECK(turnBetween(c.frames.back(), c.frames.front()) <= turn * 1.25 + 0.5);
        CHECK(std::abs(c.frames.back()[0].p.y - c.frames.front()[0].p.y) < 0.02f);
    }
}
