#include "TexTest.h"

#include "core/ImageIO.h"
#include "tex/PatchFill.h"
#include "tex/TextureSet.h"

using namespace textest;

namespace
{
/** A periodic pattern: a checker of 8-texel cells with a soft diagonal ramp inside each. */
image::Image pattern(int size)
{
    image::Image img;
    img.width = img.height = size;
    img.rgba.resize(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float* p = &img.rgba[(size_t(y) * size + x) * 4];
            const bool odd = ((x / 8) + (y / 8)) % 2 == 1;
            const float ramp = float((x + y) % 8) / 8.0f;
            p[0] = odd ? 0.9f : 0.1f;
            p[1] = ramp;
            p[2] = odd ? 0.2f : 0.7f;
            p[3] = 1.0f;
        }
    return img;
}

double holeError(const image::Image& a, const image::Image& b, int x0, int y0, int x1, int y1)
{
    double sum = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c, ++n)
                sum += std::abs(texel(a, x, y)[c] - texel(b, x, y)[c]);
    return sum / n;
}
} // namespace

TEST_CASE("tex patch fill: a hole in a periodic pattern is rebuilt to match the pattern")
{
    const image::Image truth = pattern(96);
    image::Image damaged = truth;
    std::vector<uint8_t> hole(size_t(96) * 96, 0);
    for (int y = 36; y < 56; ++y)
        for (int x = 40; x < 60; ++x)
        {
            hole[size_t(y) * 96 + x] = 1;
            for (int c = 0; c < 3; ++c)
                damaged.rgba[(size_t(y) * 96 + x) * 4 + c] = 1.0f;
        }
    const std::vector<image::Image> out = tex::patchFill({damaged}, hole);
    REQUIRE(out.size() == 1);
    CHECK(holeError(out[0], truth, 40, 36, 60, 56) < 0.05);
    // Outside the hole nothing changes.
    CHECK(texel(out[0], 5, 5)[0] == texel(truth, 5, 5)[0]);

    std::vector<uint8_t> none(hole.size(), 0);
    CHECK_THROWS(tex::patchFill({damaged}, hole, none));
}

TEST_CASE("tex patch fill: texPatchFill fills a uv hole of a set as a masked layer")
{
    run("newScene");
    const std::filesystem::path dir = tempDir("patchfill");
    image::Image src = pattern(128);
    for (int y = 50; y < 78; ++y)
        for (int x = 50; x < 78; ++x)
            for (int c = 0; c < 3; ++c)
                src.rgba[(size_t(y) * 128 + x) * 4 + c] = 1.0f;
    image::writeExr(dir / "damaged.exr", 128, 128, src.rgba, 32);
    run("texCreateSet", {{"args", {"Wall"}}, {"resolution", 128}});
    run("texAddLayer", {{"args", {"Wall", "fill"}}, {"material", {{"baseColor", (dir / "damaged.exr").string()}}}});

    // The hole in uv space, v up: texels 50..78 from the top.
    const float u0 = 48.0f / 128.0f, u1 = 80.0f / 128.0f, v0 = 1.0f - 80.0f / 128.0f, v1 = 1.0f - 48.0f / 128.0f;
    const json r = run("texPatchFill", {{"args", {"Wall"}}, {"holeUv", {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}}, {"folder", dir.string()}});
    CHECK(r["pieces"] == 1);
    CHECK(r["channels"].contains("baseColor"));
    CHECK(run("texLayerStack", {{"args", {"Wall"}}})["layers"].size() == 2);

    scene::Node& set = *tex::findSet(scene::Graph::current(), "Wall");
    const tex::SetSettings s = tex::readSettings(set);
    const image::Image after = tex::evaluateOutputs(set, tex::channelRequests(tex::readGraph(set), s), 1001, 128, 128, "cpu").at("baseColor");
    CHECK(holeError(after, pattern(128), 50, 50, 78, 78) < 0.06);

    CHECK_THROWS(run("texPatchFill", {{"args", {"Wall"}}}));
}
