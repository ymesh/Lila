#include <doctest/doctest.h>

#include "TestUtil.h"
#include "render/Atmosphere.h"
#include "scene/Graph.h"
#include "scene/SkyAtmosphere.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace lila;
using namespace lila::render;

namespace
{
const AtmosphereSettings kEarth;
const glm::dvec3 kSeaLevel(0.0, kEarth.bottomRadius + 0.001, 0.0);

glm::dvec3 sunAtElevation(double degrees)
{
    const double e = glm::radians(degrees);
    return {std::cos(e), std::sin(e), 0.0};
}

/** Sky light on a level patch of ground, as a share of the sun's illuminance. */
glm::vec3 skyIrradiance(const AtmosphereLuts& luts, const glm::dvec3& sun)
{
    constexpr int n = 24;
    glm::dvec3 sum(0.0);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < 2 * n; ++i)
        {
            const double cosTheta = (double(j) + 0.5) / n; // cosine-weighted rings: the weight is the ring's height
            const double phi = 2.0 * glm::pi<double>() * (double(i) + 0.5) / (2 * n);
            const double sinTheta = std::sqrt(1.0 - cosTheta * cosTheta);
            const glm::dvec3 d(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            sum += glm::dvec3(skyRadiance(kEarth, luts, kSeaLevel, d, sun, 32)) * cosTheta;
        }
    return glm::vec3(sum * (2.0 * glm::pi<double>() / (n * 2 * n)));
}
} // namespace

TEST_CASE("atmosphere: zenith transmittance matches the analytic optical depth")
{
    const AtmosphereLuts luts = buildAtmosphereLuts(kEarth);
    const glm::vec3 t = atmosphereTransmittance(kEarth, luts, kEarth.bottomRadius, 1.0);
    // Exponential layers integrate to coefficient times scale height; the ozone tent to coefficient times width.
    const glm::dvec3 depth = glm::dvec3(kEarth.rayleighScattering) * double(kEarth.rayleighScaleHeight) +
                             glm::dvec3(kEarth.mieScattering + kEarth.mieAbsorption) * double(kEarth.mieScaleHeight) +
                             glm::dvec3(kEarth.otherAbsorption) * double(kEarth.otherWidth * kEarth.otherTipValue);
    const glm::dvec3 expected = glm::exp(-depth);
    for (int c = 0; c < 3; ++c)
        CHECK(double(t[c]) == doctest::Approx(expected[c]).epsilon(0.015));
}

TEST_CASE("atmosphere: a low sun reddens and the planet shadows it")
{
    const AtmosphereLuts luts = buildAtmosphereLuts(kEarth);
    const glm::vec3 noon = sunTransmittance(kEarth, luts, kSeaLevel, sunAtElevation(90.0));
    const glm::vec3 low = sunTransmittance(kEarth, luts, kSeaLevel, sunAtElevation(1.0));
    CHECK(noon.b > 0.7f);
    CHECK(low.r > low.g);
    CHECK(low.g > low.b);
    CHECK(low.b < 0.05f);
    const glm::vec3 night = sunTransmittance(kEarth, luts, kSeaLevel, sunAtElevation(-10.0));
    CHECK(night == glm::vec3(0.0f));
}

TEST_CASE("atmosphere: the day sky is blue and lights the ground at a plausible share of the sun")
{
    const AtmosphereLuts luts = buildAtmosphereLuts(kEarth);
    const glm::dvec3 sun = sunAtElevation(60.0);
    const glm::vec3 zenith = skyRadiance(kEarth, luts, kSeaLevel, {0.0, 1.0, 0.0}, sun);
    CHECK(zenith.b > zenith.g);
    CHECK(zenith.g > zenith.r);
    // Clear skies give about a tenth to a fifth of the sun's light on level ground as diffuse light.
    const glm::vec3 e = skyIrradiance(luts, sunAtElevation(90.0));
    const float luma = 0.2126f * e.r + 0.7152f * e.g + 0.0722f * e.b;
    CHECK(luma > 0.06f);
    CHECK(luma < 0.3f);
}

TEST_CASE("atmosphere: sunset glows warm toward the sun")
{
    const AtmosphereLuts luts = buildAtmosphereLuts(kEarth);
    const glm::dvec3 sun = sunAtElevation(2.0);
    const glm::vec3 towardSun = skyRadiance(kEarth, luts, kSeaLevel, glm::normalize(glm::dvec3(1.0, 0.05, 0.0)), sun);
    const glm::vec3 zenith = skyRadiance(kEarth, luts, kSeaLevel, {0.0, 1.0, 0.0}, sun);
    CHECK(towardSun.r / towardSun.b > zenith.r / zenith.b);
    CHECK(towardSun.r > towardSun.b);
}

TEST_CASE("atmosphere: from orbit space is black and the day side of the planet is lit")
{
    const AtmosphereLuts luts = buildAtmosphereLuts(kEarth);
    const glm::dvec3 orbit(0.0, kEarth.bottomRadius + 400.0, 0.0);
    const glm::dvec3 sun = sunAtElevation(70.0);
    CHECK(skyRadiance(kEarth, luts, orbit, {0.0, 1.0, 0.0}, sun) == glm::vec3(0.0f));
    const glm::vec3 down = skyRadiance(kEarth, luts, orbit, {0.0, -1.0, 0.0}, sun);
    CHECK(down.r > 0.01f);
    // What the air adds over the ground seen through it is blue.
    const double mu = std::sin(glm::radians(70.0));
    const glm::vec3 viewT = atmosphereTransmittance(kEarth, luts, kEarth.bottomRadius, 1.0);
    const glm::vec3 sunT = atmosphereTransmittance(kEarth, luts, kEarth.bottomRadius, mu);
    const glm::vec3 groundOnly = kEarth.groundAlbedo / glm::pi<float>() * float(mu) * sunT * viewT;
    const glm::vec3 air = down - groundOnly;
    CHECK(air.b > air.r);
    CHECK(air.r > 0.0f);
    // The limb: a ray grazing the atmosphere's edge glows.
    const double limb = std::asin((kEarth.bottomRadius + 10.0) / glm::length(orbit));
    const glm::vec3 edge = skyRadiance(kEarth, luts, orbit, {std::sin(limb), -std::cos(limb), 0.0}, glm::dvec3(1.0, 0.0, 0.0));
    CHECK(edge.b > 0.0f);
}

TEST_CASE("atmosphere: the environment key round-trips and bakes a sky without the sun")
{
    AtmosphereFrame f;
    f.settings.mieAnisotropy = 0.7f;
    f.settings.groundAlbedo = glm::vec3(0.1f, 0.2f, 0.3f);
    f.sunDir = glm::normalize(glm::dvec3(0.3, 0.8, -0.2));
    f.sunRadiance = glm::vec3(3.0f, 2.9f, 2.8f);
    f.zUp = false;
    const glm::dvec3 eye(120.0, 180.0, -40.0);
    const std::string key = atmosphereEnvironmentKey(f, eye);
    CHECK(isAtmosphereKey(key));
    CHECK_FALSE(isAtmosphereKey("C:/hdri/sky.exr"));
    glm::dvec3 bakedEye(0.0);
    const std::optional<AtmosphereFrame> back = parseAtmosphereKey(key, &bakedEye);
    REQUIRE(back);
    CHECK(back->settings.mieAnisotropy == doctest::Approx(0.7f));
    CHECK(back->settings.groundAlbedo.b == doctest::Approx(0.3f));
    CHECK(back->sunRadiance.r == doctest::Approx(3.0f));
    CHECK(glm::length(back->sunDir - f.sunDir) < 1e-3);
    // Small camera moves near the ground bake the same sky.
    CHECK(atmosphereEnvironmentKey(f, eye + glm::dvec3(500.0, 300.0, 0.0)) == key);
    CHECK(atmosphereEnvironmentKey(f, eye + glm::dvec3(0.0, 5.0e5, 0.0)) != key);

    const EnvironmentData env = buildAtmosphereEnvironment(*back, bakedEye);
    REQUIRE(env.size == 128);
    REQUIRE(!env.mips.empty());
    // Ambient light from above is blue-ish and well under the sun's own.
    const glm::vec4 dc = env.sh[0];
    CHECK(dc.b > dc.r);
    CHECK(dc.g < 3.0f);
    CHECK(dc.g > 0.0f);
}

TEST_CASE("atmosphere: the sky node sits in the DAG, its position places the planet, and hiding it turns it off")
{
    using namespace lila::scene;
    testutil::run("newScene");
    const std::string sky = testutil::run("skyAtmosphere", {{"sun", false}}).get<std::string>();
    Graph& g = Graph::current();
    const Node* n = g.find(sky);
    REQUIRE(n);
    CHECK(n->type().dag);
    CHECK(skyAtmosphere::active(g) == n->id());

    testutil::run("setAttr", {{"args", {sky + ".transformMode", 2}}});
    testutil::run("setAttr", {{"args", {sky + ".translate", nlohmann::json::array({100.0, 200.0, 300.0})}}});
    CHECK(skyAtmosphere::planetCenter(*n, false) == glm::dvec3(100.0, 200.0, 300.0));

    testutil::run("setAttr", {{"args", {sky + ".visibility", false}}});
    CHECK(skyAtmosphere::active(g) == kNoNode);
}

TEST_CASE("atmosphere: the sky node's scale sizes the planet and its air together")
{
    using namespace lila::scene;
    namespace sa = skyAtmosphere;
    testutil::run("newScene");
    const std::string sky = testutil::run("skyAtmosphere", {{"sun", false}}).get<std::string>();
    const Node* n = Graph::current().find(sky);
    REQUIRE(n);
    testutil::run("setAttr", {{"args", {sky + ".transformMode", 1}}});
    testutil::run("setAttr", {{"args", {sky + ".scale", nlohmann::json::array({0.001, 0.001, 0.001})}}});
    CHECK(sa::planetScale(*n) == doctest::Approx(0.001));
    CHECK(sa::scaled(*n, sa::kBottomRadius) == doctest::Approx(6.36));
    CHECK(sa::scaled(*n, sa::kAtmosphereHeight) == doctest::Approx(0.06));
    CHECK(sa::scaled(*n, sa::kRayleighScatteringScale) == doctest::Approx(33.1));
    CHECK(sa::scaled(*n, sa::kMieAnisotropy) == doctest::Approx(0.8));
    CHECK(sa::planetCenter(*n, false).y == doctest::Approx(-6.36e5));
}
