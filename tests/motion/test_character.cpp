#include <doctest/doctest.h>

#include "motion/Character.h"
#include "motion/Generate.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace lila::motion;

TEST_CASE("motion: the SOMA rig's chains are on its skeleton")
{
    const SomaRig& r = somaRig();
    CHECK(r.skeleton.size() == SomaSkeleton::shipped().size());
    for (const auto& c : r.rig.chains)
    {
        INFO(c.name);
        CHECK(r.skeleton.find(c.start) >= 0);
        CHECK(r.skeleton.find(c.end) >= 0);
    }
    CHECK(r.rig.chain("LeftLeg"));
    CHECK(r.rig.chain("RightArm"));
    // Standing on the floor, facing along +Z or -Z with the left hip to one side.
    CHECK(std::abs(r.rig.ground) < 10.0);
    CHECK(std::abs(r.rig.forward.y) < 1e-6);
}

TEST_CASE("motion: a pose round-trips through retarget data")
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const Pose back = fromRetargetPose(toRetargetPose(s.reference));
    for (size_t j = 0; j < s.size(); ++j)
    {
        CHECK(glm::length(back[j].p - s.reference[j].p) < 1e-5f);
        CHECK(std::abs(glm::dot(back[j].q, s.reference[j].q)) > 1.0f - 1e-6f);
    }
}

TEST_CASE("motion: SOMA mapped onto itself comes back as it went")
{
    const SomaRig& r = somaRig();
    const CharacterMap map(r.skeleton, r.rig, r.reference);
    CHECK(map.scale() == doctest::Approx(1.0).epsilon(0.01));
    const SomaSkeleton& s = SomaSkeleton::shipped();
    Pose bent = placed(s.reference, 0.7f, {1.0f, 2.0f});
    bent[size_t(s.find("LeftForeArm"))].q = glm::normalize(bent[size_t(s.find("LeftForeArm"))].q * glm::angleAxis(0.8f, glm::vec3(0, 0, 1)));
    const lila::retarget::Pose on = map.toCharacter(bent);
    const auto w = lila::retarget::worldOf(r.skeleton, on);
    const Pose want = s.world(bent);
    for (const char* j : {"Hips", "LeftHand", "RightFoot", "Head"})
    {
        INFO(j);
        const glm::dvec3 got = glm::dvec3(w[size_t(r.skeleton.find(j))][3]) / kCmPerMeter;
        CHECK(glm::length(got - glm::dvec3(want[size_t(s.find(j))].p)) < 0.02);
    }
    const Pose back = map.toSoma(w);
    CHECK(glm::length(s.world(back)[size_t(s.find("LeftHand"))].p - want[size_t(s.find("LeftHand"))].p) < 0.02f);
}

TEST_CASE("motion: heading and placement")
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    const float h = headingOf(s.reference);
    const Pose p = placed(s.reference, 1.0f, {3.0f, -2.0f});
    CHECK(p[0].p.x == doctest::Approx(3.0f));
    CHECK(p[0].p.z == doctest::Approx(-2.0f));
    float d = headingOf(p) - h - 1.0f;
    while (d > 3.14159f)
        d -= 6.28318f;
    while (d < -3.14159f)
        d += 6.28318f;
    CHECK(std::abs(d) < 1e-3f);
}

TEST_CASE("motion: the player chains generations from what it showed")
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    Player player(s.reference);
    CHECK(player.framesToTransition() == 0);
    Result r;
    for (int f = 0; f < 20; ++f)
    {
        Pose p = s.reference;
        p[0].p.z += 0.05f * float(f);
        r.frames.push_back(p);
        r.contacts.push_back({1, 1, 0, 0});
    }
    player.play(r, 5);
    CHECK(player.framesToTransition() == 14);
    for (int i = 0; i < 14; ++i)
        player.step();
    CHECK(player.framesToTransition() == 0);
    CHECK(player.current()[0].p.z == doctest::Approx(0.05f * 14));
    const auto ctx = player.context();
    CHECK(ctx[3][0].p.z == doctest::Approx(0.05f * 14));
    CHECK(ctx[0][0].p.z == doctest::Approx(0.05f * 11));
    CHECK(player.recorded().size() == 14);
}

TEST_CASE("motion: spots store and place their targets")
{
    const SomaSkeleton& s = SomaSkeleton::shipped();
    Spot spot;
    spot.name = "Test";
    SpotTarget t;
    t.name = "Reach";
    t.poses = {placed(s.reference, 0.0f, {0.0f, -0.5f})};
    t.pins = kPinLeftHand | kPinRightFoot;
    t.tokens = tokenMask(7, 9);
    t.hold = 5;
    spot.targets.push_back(t);
    const Spot back = Spot::fromJson(spot.toJson());
    REQUIRE(back.targets.size() == 1);
    CHECK(back.targets[0].pins == t.pins);
    CHECK(back.targets[0].tokens == t.tokens);
    CHECK(back.targets[0].hold == 5);
    const auto placedT = placeSpot(back, {2.0f, 3.0f}, 3.14159265f / 2.0f);
    // Half a meter behind the spot, turned a quarter: half a meter along -X.
    CHECK(placedT[0].poses[0][0].p.x == doctest::Approx(1.5f).epsilon(0.01));
    CHECK(placedT[0].poses[0][0].p.z == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("motion: the shipped gaits and spots load")
{
    const GaitLibrary& lib = GaitLibrary::shipped();
    for (const char* style : {"walk", "run", "crouch", "strafeLeft", "strafeRight", "backward"})
    {
        INFO(style);
        const Gait* g = lib.find(style);
        REQUIRE(g);
        CHECK(g->windows.size() >= 4);
        CHECK(g->speed > 0.2);
    }
    CHECK(lib.find("run")->speed > lib.find("walk")->speed);
    CHECK(lib.provenance.find("ACCAD") != std::string::npos);
    const auto spots = shippedSpots();
    CHECK(spots.size() >= 4);
    for (const auto& [name, file] : spots)
    {
        INFO(name);
        const Spot sp = Spot::load(file);
        CHECK(!sp.targets.empty());
        CHECK(sp.provenance.find("ACCAD") != std::string::npos);
    }
}

namespace
{
/** Largest joint turn (degrees) and root move (m) between two poses. */
std::pair<double, double> step(const Pose& a, const Pose& b)
{
    double turn = 0.0;
    for (size_t j = 0; j < a.size(); ++j)
        turn = std::max(turn, double(glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a[j].q, b[j].q)))))));
    return {turn, double(glm::length(a[0].p - b[0].p))};
}
} // namespace

TEST_CASE("motion: a cycle's length comes from one foot's strikes, past a missed one")
{
    CHECK(cycleFrames({.left = {10.0, 42.5, 74.5, 141.0}, .right = {26.0, 58.5}}) == doctest::Approx(97.0 / 3.0));
    // One strike per foot measures no cycle.
    CHECK(cycleFrames({.left = {10.0}, .right = {26.0}}) == 0.0);
}

TEST_CASE("motion: the shipped gaits carry seamless cycles measured from their foot strikes")
{
    const GaitLibrary& lib = GaitLibrary::shipped();
    int whole = 0;
    for (const Gait& g : lib.gaits)
    {
        INFO(g.name << " " << g.period << " s");
        REQUIRE(!g.cycle.empty());
        CHECK(int(g.cycle.size()) == int(std::lround(g.period * kSampleRate)));
        CHECK(g.period > 0.5);
        CHECK(g.period < 1.6);
        whole += std::abs(g.period * kSampleRate - std::round(g.period * kSampleRate)) < 1e-6 ? 1 : 0;

        // The step from the last pose back to the first is no bigger than the cycle's own steps.
        double turn = 0.0, move = 0.0;
        for (size_t f = 1; f < g.cycle.size(); ++f)
        {
            const auto [t, m] = step(g.cycle[f - 1], g.cycle[f]);
            turn = std::max(turn, t);
            move = std::max(move, m);
        }
        const auto [wrapTurn, wrapMove] = step(g.cycle.back(), g.cycle.front());
        CHECK(wrapTurn <= turn * 1.25 + 0.5);
        CHECK(wrapMove <= move * 1.25 + 0.005);
        // In place: the root ends near where it starts.
        CHECK(glm::length(glm::vec2(g.cycle.back()[0].p.x, g.cycle.back()[0].p.z)) < 0.1f);

        // Played three times over, each foot comes down once a cycle: the loop steps, it doesn't slide.
        std::vector<Pose> tiled;
        for (int k = 0; k < 3; ++k)
            tiled.insert(tiled.end(), g.cycle.begin(), g.cycle.end());
        const FootStrikes s = footStrikes(tiled);
        const double n = double(g.cycle.size());
        CHECK(cycleFrames(s) == doctest::Approx(n).epsilon(0.03));
        for (const std::vector<double>* foot : {&s.left, &s.right})
        {
            CHECK(foot->size() >= 2);
            CHECK(foot->size() <= 3);
        }
    }
    // Measured to the sub-frame, not rounded to whole frames.
    CHECK(whole <= 1);
}

TEST_CASE("motion: a journey walks a path, performs a spot and stops")
{
    if (!std::filesystem::exists(modelPath()))
        return;
    Engine* engine = Engine::shared();
    REQUIRE_MESSAGE(engine, Engine::status().message);
    const GaitLibrary& lib = GaitLibrary::shipped();
    const Pose start = placed(lib.idle, 0.0f, {0.0f, 0.0f});

    Journey walk;
    walk.start = start;
    walk.legs = {Leg{.path = {{0.0f, 0.0f}, {0.0f, 4.0f}, {3.0f, 6.0f}}}};
    Journey run = walk;
    run.options.style = "run";
    Journey spot = walk;
    spot.legs = {Leg{.path = {{0.0f, 0.0f}, {0.0f, 3.0f}}, .targets = placeSpot(Spot::load(shippedSpots().front().second), {0.0f, 4.0f}, 0.0f)}};

    Journey strafe = walk;
    strafe.options.style = "strafeLeft";
    strafe.legs = {Leg{.path = {{0.0f, 0.0f}, {1.5f, 0.0f}}}};

    const Journey batch[] = {walk, run, spot, strafe};
    const std::vector<Clip> clips = followPaths(*engine, batch);
    REQUIRE(clips.size() == 4);
    {
        // Strafing left along +X faces +Z and ends where the path does.
        const Pose& last = clips[3].frames.back();
        CHECK(glm::length(glm::vec2(last[0].p.x, last[0].p.z) - glm::vec2(1.5f, 0.0f)) < 0.3f);
        CHECK(std::abs(headingOf(last)) < 0.5f);
    }
    for (size_t i = 0; i < clips.size(); ++i)
    {
        INFO("journey " << i << ": " << clips[i].error);
        CHECK(clips[i].error.empty());
        CHECK(clips[i].generations >= 2);
        CHECK(clips[i].frames.size() == clips[i].contacts.size());
    }
    // Walk and run end at the path's end, standing; the run gets there sooner.
    for (size_t i : {size_t(0), size_t(1)})
    {
        const glm::vec3 end = clips[i].frames.back()[0].p;
        CHECK(glm::length(glm::vec2(end.x, end.z) - glm::vec2(3.0f, 6.0f)) < 0.6f);
    }
    CHECK(clips[1].frames.size() < clips[0].frames.size());
    // The spot's targets are reached in order.
    REQUIRE(clips[2].reached.size() == Spot::load(shippedSpots().front().second).targets.size());
    for (size_t k = 1; k < clips[2].reached.size(); ++k)
        CHECK(clips[2].reached[k].first > clips[2].reached[k - 1].first);
    // Feet touch down while walking.
    int planted = 0;
    for (const Contacts& c : clips[0].contacts)
        planted += (c[0] > 0.5f || c[1] > 0.5f || c[2] > 0.5f || c[3] > 0.5f) ? 1 : 0;
    CHECK(planted > int(clips[0].contacts.size()) * 2 / 5);
}

TEST_CASE("motion: a live driver steers and records")
{
    if (!std::filesystem::exists(modelPath()))
        return;
    Engine* engine = Engine::shared();
    REQUIRE(engine);
    const GaitLibrary& lib = GaitLibrary::shipped();
    LiveDriver drive(*engine, lib.idle, {});
    drive.setSynchronous(true);
    drive.setInput({.move = {0.0f, 1.0f}});
    for (int f = 0; f < 60; ++f)
        drive.tick();
    drive.setInput({.move = {1.0f, 0.0f}, .style = "run"});
    for (int f = 0; f < 60; ++f)
        drive.tick();
    drive.setInput({});
    for (int f = 0; f < 60; ++f)
        drive.tick();
    CHECK(drive.error().empty());
    CHECK(drive.recorded().size() == 180);
    CHECK(drive.generations() >= 4);
    const glm::vec3 a = drive.recorded()[60][0].p, b = drive.recorded().back()[0].p;
    CHECK(a.z > 0.5f);
    CHECK(b.x > a.x + 1.0f);
    // It stops: the last frames barely move.
    CHECK(glm::length(drive.recorded().back()[0].p - drive.recorded()[drive.recorded().size() - 10][0].p) < 0.1f);
}
