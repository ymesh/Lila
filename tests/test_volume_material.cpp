#include <doctest/doctest.h>

#include "TestUtil.h"

#include "gn/Volume.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <algorithm>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;
namespace material = scene::material;

namespace
{
struct Stats
{
    size_t filled = 0, full = 0;
};

Stats stats(const gn::Volume& v)
{
    Stats s;
    for (float x : v.values)
    {
        s.filled += x > 0.0f;
        s.full += x > 0.999f;
    }
    return s;
}
} // namespace

TEST_CASE("volume material noise and edge fade shape a filled mesh")
{
    run("newScene");
    const std::string cube = run("polyCube", {{"width", 100}, {"height", 100}, {"depth", 100}})[0];
    run("createMaterial", {{"args", {"Cloud"}}});
    run("assignMaterial", {{"args", {"Cloud", cube}}});
    scene::Graph& g = scene::Graph::current();
    const scene::NodeId mat = g.find("Cloud")->id();
    auto set = [&](int attr, scene::PlugValue v) {
        g.setValue({mat, attr, -1}, std::move(v));
        g.evaluate();
    };
    set(material::kShadingModel, int64_t(material::ShadingModel::Volume));
    const scene::NodeId mesh = g.find(cube)->id();

    const auto plain = gn::meshVolume(g, mesh);
    REQUIRE(plain);
    const Stats even = stats(*plain);
    REQUIRE(even.filled > 0);
    CHECK(even.full == even.filled);

    set(material::kVolumeEdgeFade, 20.0);
    const auto faded = gn::meshVolume(g, mesh);
    REQUIRE(faded);
    CHECK(faded != plain);
    const Stats fade = stats(*faded);
    CHECK(fade.full < fade.filled);
    CHECK(fade.full > 0);

    set(material::kVolumeEdgeFade, 0.0);
    set(material::kVolumeNoise, 1.0);
    set(material::kVolumeNoiseScale, 30.0);
    const auto cloud = gn::meshVolume(g, mesh);
    REQUIRE(cloud);
    const Stats noisy = stats(*cloud);
    CHECK(noisy.filled < even.filled);
    CHECK(noisy.filled > even.filled / 10);
    // Same settings, same grid: the cache keys on the shape.
    CHECK(gn::meshVolume(g, mesh) == cloud);

    set(material::kVolumeNoiseCoverage, 1.0);
    CHECK(stats(*gn::meshVolume(g, mesh)).filled > noisy.filled);
    set(material::kVolumeNoiseCoverage, 0.0);
    CHECK(stats(*gn::meshVolume(g, mesh)).filled == 0);
}
