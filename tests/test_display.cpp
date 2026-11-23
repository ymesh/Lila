#include <doctest/doctest.h>

#include "render/DisplayOptions.h"
#include "ui/ViewportDisplay.h"

#include <string>

using namespace lila::render;

TEST_CASE("viewport MSAA sample count")
{
    constexpr uint32_t upTo8 = 1 | 2 | 4 | 8;
    CHECK(msaaSamples(false, 8, upTo8) == 1);
    CHECK(msaaSamples(true, 8, upTo8) == 8);
    CHECK(msaaSamples(true, 4, upTo8) == 4);
    CHECK(msaaSamples(true, 3, upTo8) == 2);
    CHECK(msaaSamples(true, 0, upTo8) == 1);
    CHECK(msaaSamples(true, 64, upTo8) == 8);
    SUBCASE("rounds down to what the device supports")
    {
        CHECK(msaaSamples(true, 8, 1 | 4) == 4);
        CHECK(msaaSamples(true, 2, 1 | 4) == 1);
        CHECK(msaaSamples(true, 8, 1) == 1);
    }
}

TEST_CASE("viewport lighting and exposure prefs")
{
    CHECK(lightingFromName("studio") == FrameState::Lighting::Studio);
    CHECK(lightingFromName("flat") == FrameState::Lighting::Flat);
    CHECK(lightingFromName("environment") == FrameState::Lighting::Environment);
    CHECK(lightingFromName("bogus") == FrameState::Lighting::Environment);
    CHECK(exposureScale(0.0) == doctest::Approx(1.0));
    CHECK(exposureScale(1.0) == doctest::Approx(2.0));
    CHECK(exposureScale(-2.0) == doctest::Approx(0.25));
}

TEST_CASE("environment intensity per HDRI")
{
    nlohmann::json byHdri = nlohmann::json::object();
    CHECK(environmentIntensity(byHdri, "a.hdr", 1.5f) == doctest::Approx(1.5f));
    CHECK(environmentIntensity(nullptr, "a.hdr", 0.8f) == doctest::Approx(0.8f));

    byHdri = withEnvironmentIntensity(byHdri, "a.hdr", 2.0f);
    byHdri = withEnvironmentIntensity(byHdri, "C:/hdri/b.hdr", 0.5f);
    CHECK(environmentIntensity(byHdri, "a.hdr", 1.0f) == doctest::Approx(2.0f));
    CHECK(environmentIntensity(byHdri, "C:/hdri/b.hdr", 1.0f) == doctest::Approx(0.5f));
    CHECK(environmentIntensity(byHdri, "c.hdr", 1.25f) == doctest::Approx(1.25f));

    SUBCASE("clearing an entry falls back to the global value")
    {
        byHdri = withEnvironmentIntensity(byHdri, "a.hdr", std::nullopt);
        CHECK(environmentIntensity(byHdri, "a.hdr", 1.25f) == doctest::Approx(1.25f));
        CHECK(byHdri.contains("C:/hdri/b.hdr"));
    }
    SUBCASE("bad values")
    {
        byHdri["bad"] = "bright";
        CHECK(environmentIntensity(byHdri, "bad", 1.0f) == doctest::Approx(1.0f));
        CHECK(environmentIntensity(byHdri, "a.hdr", -3.0f) == doctest::Approx(2.0f));
        CHECK(environmentIntensity(withEnvironmentIntensity(byHdri, "a.hdr", -1.0f), "a.hdr", 1.0f) == doctest::Approx(0.0f));
        CHECK(environmentIntensity(withEnvironmentIntensity(nlohmann::json(3), "a.hdr", 2.0f), "a.hdr", 1.0f) == doctest::Approx(2.0f));
    }
}

TEST_CASE("component hairline width under display scaling")
{
    using lila::ui::viewportDisplay::hairlineWidth;
    CHECK(hairlineWidth(1.0, 1.0) == doctest::Approx(1.0f));
    CHECK(hairlineWidth(1.0, 2.0) == doctest::Approx(1.0f));
    CHECK(hairlineWidth(3.0, 1.0) == doctest::Approx(3.0f));
    CHECK(hairlineWidth(3.0, 1.5) == doctest::Approx(4.0f));
    CHECK(hairlineWidth(0.5, 2.0) == doctest::Approx(1.0f));
}

TEST_CASE("monotone overrides the selective accent colours")
{
    using lila::ui::viewportDisplay::accentColors;
    CHECK(accentColors(false, true));
    CHECK_FALSE(accentColors(false, false));
    CHECK_FALSE(accentColors(true, true));
    CHECK_FALSE(accentColors(true, false));
}

TEST_CASE("random colours are stable per name and tell names apart")
{
    using lila::ui::viewportDisplay::randomColor;
    CHECK(randomColor("pCube1") == randomColor("pCube1"));
    const glm::vec3 a = randomColor("pCube1"), b = randomColor("pCube2");
    CHECK(glm::length(a - b) > 0.02f);
    for (const char* name : {"", "a", "pSphere1", "wall_segment_042"})
    {
        const glm::vec3 c = randomColor(name);
        CHECK(glm::all(glm::greaterThanEqual(c, glm::vec3(0.0f))));
        CHECK(glm::all(glm::lessThanEqual(c, glm::vec3(1.0f))));
        CHECK(std::max({c.r, c.g, c.b}) > 0.3f); // never near black
    }
}

TEST_CASE("pastel random colours are light and soft")
{
    using namespace lila::ui::viewportDisplay;
    for (int i = 0; i < 64; ++i)
    {
        const std::string name = "pCube" + std::to_string(i);
        const glm::vec3 c = glm::pow(randomColor(name, RandomPalette::Pastel), glm::vec3(1.0f / 2.2f));
        const float hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
        CHECK(lo > 0.55f);
        CHECK((hi - lo) / hi < 0.31f);
    }
    CHECK(randomColor("pCube1", RandomPalette::Pastel) != randomColor("pCube2", RandomPalette::Pastel));
}

TEST_CASE("the random colour seed reshuffles colours")
{
    using namespace lila::ui::viewportDisplay;
    CHECK(randomColor("pCube1", RandomPalette::Contrast, 0) == randomColor("pCube1"));
    CHECK(randomColor("pCube1", RandomPalette::Contrast, 7) == randomColor("pCube1", RandomPalette::Contrast, 7));
    CHECK(glm::length(randomColor("pCube1", RandomPalette::Contrast, 1) - randomColor("pCube1")) > 0.02f);
}
