#include "TexPaintTest.h"

#include "core/PrefStore.h"
#include "ui/QuickMaskOverlay.h"

using namespace textest;
namespace qmo = lila::ui::quickMaskOverlay;

namespace
{
const json kOverlayView = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 800}, {"height", 600}};

/** A quick mask stroke along screen y 300 from x0 to x1 (x 300..390 is the left face, 410..500 the right). */
void maskStroke(double x0, double x1)
{
    json brush = brushJson();
    brush["radius"] = 25.0;
    json points = json::array();
    for (int i = 0; i <= 6; ++i)
        points.push_back({x0 + (x1 - x0) * i / 6.0, 300.0, 1.0});
    paintStroke("Panel", qmo::kBuffer, {{"points", points}, {"view", kOverlayView}, {"brush", brush}, {"payload", {{"mask", true}}}});
    lila::mainq::drain();
    // As a frame would: the first read makes the mask image, which fills on the main queue.
    qmo::tint("Panel");
    lila::mainq::drain();
}

/** The tint's alpha (0..1) at a uv, v up. */
float tintAlpha(const lila::render::TexturePtr& t, glm::vec2 uv)
{
    const int x = std::clamp(int(uv.x * t->width), 0, t->width - 1), y = std::clamp(int((1.0f - uv.y) * t->height), 0, t->height - 1);
    return float(t->rgba[(size_t(y) * t->width + x) * 4 + 3]) / 255.0f;
}
} // namespace

TEST_CASE("quick mask overlay: the tint follows the quick mask buffer and the style prefs")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    makeSet("Panel", 256);
    lila::PrefStore& prefs = lila::PrefStore::get();
    prefs.set("general", qmo::kOpacityPref, 0.5);
    prefs.set("general", qmo::kColorPref, std::string("#ff0000"));

    // No quick mask yet: nothing to tint.
    CHECK_FALSE(qmo::tint("Panel"));

    maskStroke(310.0, 380.0);
    uint64_t v1 = 0;
    lila::render::TexturePtr t = qmo::tint("Panel", &v1);
    REQUIRE(t);
    const glm::vec2 left = seamUv(0, -20.0, 0.0), right = seamUv(1, 20.0, 0.0);
    CHECK(tintAlpha(t, left) < 0.05f);
    CHECK(tintAlpha(t, right) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(t->rgba[0] == 255);
    CHECK(t->rgba[1] == 0);

    // Unchanged paint keeps the same image.
    uint64_t same = 0;
    CHECK(qmo::tint("Panel", &same) == t);
    CHECK(same == v1);

    // Painting the right face too clears its tint.
    maskStroke(420.0, 490.0);
    uint64_t v2 = 0;
    t = qmo::tint("Panel", &v2);
    REQUIRE(t);
    CHECK(v2 > v1);
    CHECK(tintAlpha(t, right) < 0.05f);

    // The style follows the prefs.
    prefs.set("general", qmo::kOpacityPref, 1.0);
    run("texClearLayer", {{"args", {"Panel", qmo::kBuffer}}});
    lila::mainq::drain();
    maskStroke(310.0, 380.0);
    t = qmo::tint("Panel");
    REQUIRE(t);
    CHECK(tintAlpha(t, right) > 0.98f);
    CHECK(tintAlpha(t, left) < 0.05f);
    prefs.set("general", qmo::kOpacityPref, qmo::kDefaultOpacity);
    prefs.set("general", qmo::kColorPref, std::string(qmo::kDefaultColor));
}
