#include "TexTest.h"

#include "tex/Natives.h"

#include <set>

using namespace textest;

namespace
{
image::Image at64(const Graph& g, const std::string& from)
{
    return eval(into(g, from), "Output", "baseColor", 64);
}

/** A constant value through type, read back. */
float constantThrough(const std::string& type, double input, json values)
{
    Graph g;
    add(g, "C", "constant", {{"value", input}});
    add(g, "N", type, std::move(values));
    link(g, "C.out", "N.input");
    return texel(eval(into(g, "N.out"), "Output", "baseColor", 4), 1, 1)[0];
}

/** A pixel processor writing expression's colour. */
Graph ramp(const std::string& expression)
{
    Graph g;
    add(g, "Src", "pixelProcessor", {{"expression", expression}});
    return g;
}

/** Bricks (2 columns, 4 rows, no offset) through a flood fill into type. */
Graph bricksInto(const std::string& type, json values = json::object())
{
    Graph g;
    add(g, "B", "bricks", {{"rows", 4}, {"columns", 2}, {"offset", 0.0}, {"bevel", 0.01}, {"mortar", 0.15}});
    add(g, "F", "floodFill");
    add(g, "N", type, std::move(values));
    link(g, "B.mask", "F.input");
    link(g, "F.out", "N.input");
    return g;
}

/** The node's CPU result against its Slang kernels run through the CPU target, the way the GPU runs them. */
double kernelGap(const std::string& type, const Graph& g, const std::string& from)
{
    Plan p = plan(into(g, from), "Output", "baseColor", 48);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    image::Image a = evaluateCpu(p).outputs.at("out");
    NativeCpuFn cpu = nativeCpu(type);
    REQUIRE(cpu);
    registerNativeCpu(type, [](const NativeCall& call) { runNativeKernelsCpu(call); });
    image::Image k = evaluateCpu(p).outputs.at("out");
    registerNativeCpu(type, cpu);
    return maxDiff(a, k);
}
} // namespace

TEST_CASE("tex natives more: auto levels stretches to the full range")
{
    Graph g = ramp("return float4(0.25 + 0.5 * uv.x, 0.1 + 0.2 * uv.y, 0.5, 0.7);");
    add(g, "N", "autoLevels");
    link(g, "Src.out", "N.input");
    image::Image img = at64(g, "N.out");
    CHECK(texel(img, 0, 5)[0] == doctest::Approx(0.0).epsilon(1e-4));
    CHECK(texel(img, 63, 5)[0] == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(texel(img, 5, 63)[1] == doctest::Approx(0.0).epsilon(1e-4));
    CHECK(texel(img, 5, 0)[1] == doctest::Approx(1.0).epsilon(1e-4));
    // A flat channel has no range to stretch and alpha is never touched.
    CHECK(texel(img, 20, 20)[2] == doctest::Approx(0.5));
    CHECK(texel(img, 20, 20)[3] == doctest::Approx(0.7));

    Graph l = ramp("float v = 0.3 + 0.4 * uv.x; return float4(v, v, v, 1.0);");
    add(l, "N", "autoLevels", {{"mode", "luminance"}});
    link(l, "Src.out", "N.input");
    image::Image lum = at64(l, "N.out");
    CHECK(texel(lum, 0, 9)[0] == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(texel(lum, 63, 9)[1] == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(texel(lum, 32, 9)[2] == doctest::Approx(32.0 / 63.0).epsilon(1e-3));

    CHECK(kernelGap("autoLevels", g, "N.out") < 1e-5);
    CHECK(kernelGap("autoLevels", l, "N.out") < 1e-5);
}

TEST_CASE("tex natives more: equalize spreads a skewed ramp evenly")
{
    // Squared: most texels are dark. Equalized, each column's value is about its share of the columns before it.
    Graph g = ramp("float v = 0.1 + 0.8 * uv.x * uv.x; return float4(v, v, v, 1.0);");
    add(g, "N", "equalize");
    link(g, "Src.out", "N.input");
    image::Image img = at64(g, "N.out");
    for (int x : {24, 32, 48, 60})
    {
        CAPTURE(x);
        CHECK(texel(img, x, 7)[0] == doctest::Approx((float(x) + 0.5f) / 64.0f).epsilon(0.04));
    }
    for (int x = 1; x < 64; ++x)
        CHECK(texel(img, x, 7)[0] >= texel(img, x - 1, 7)[0]);

    add(g, "C", "equalize", {{"mode", "perChannel"}});
    link(g, "Src.out", "C.input");
    CHECK(maxDiff(img, at64(g, "C.out")) < 1e-3);

    Graph c = ramp("return float4(uv.x * uv.x, uv.y, 0.5 * uv.x, 1.0);");
    add(c, "N", "equalize", {{"mode", "perChannel"}});
    link(c, "Src.out", "N.input");
    CHECK(kernelGap("equalize", g, "N.out") < 1e-5);
    CHECK(kernelGap("equalize", c, "N.out") < 1e-5);
}

TEST_CASE("tex natives more: histogram shift wraps")
{
    CHECK(constantThrough("histogramShift", 0.3, {{"position", 0.5}}) == doctest::Approx(0.8));
    CHECK(constantThrough("histogramShift", 0.3, {{"position", 0.8}}) == doctest::Approx(0.1));
    CHECK(constantThrough("histogramShift", 1.0, {{"position", 0.0}}) == doctest::Approx(1.0));
}

TEST_CASE("tex natives more: histogram select keeps a band")
{
    auto value = constantThrough;
    CHECK(value("histogramSelect", 0.5, json::object()) == doctest::Approx(1.0));
    CHECK(value("histogramSelect", 0.9, json::object()) == doctest::Approx(0.0));
    CHECK(value("histogramSelect", 0.55, {{"contrast", 0.0}}) == doctest::Approx(0.6));
    CHECK(value("histogramSelect", 0.6, {{"contrast", 1.0}}) == doctest::Approx(1.0));
}

TEST_CASE("tex natives more: fill to index numbers each brick")
{
    Graph g = bricksInto("fillToIndex");
    image::Image img = at64(g, "N.out");
    std::set<int> values;
    for (size_t i = 0; i < img.rgba.size(); i += 4)
        if (img.rgba[i] > 0.0f)
            values.insert(int(std::lround(img.rgba[i] * 8.0f)));
    CHECK(values == std::set<int>{1, 2, 3, 4, 5, 6, 7, 8});
    // Mortar is outside every region.
    CHECK(texel(img, 0, 0)[0] == 0.0f);
    CHECK(kernelGap("fillToIndex", g, "N.out") < 1e-6);
}

TEST_CASE("tex natives more: fill to position and bounding box size")
{
    image::Image pos = at64(bricksInto("fillToPosition"), "N.out");
    // The brick covering texel (16, 8) spans the left half of the top row.
    CHECK(texel(pos, 16, 8)[0] == doctest::Approx(0.25).epsilon(0.03));
    CHECK(texel(pos, 16, 8)[1] == doctest::Approx(1.0 - 8.0 / 64.0).epsilon(0.03));
    CHECK(texel(pos, 5, 3)[0] == texel(pos, 16, 8)[0]);
    CHECK(texel(pos, 5, 3)[1] == texel(pos, 16, 8)[1]);
    CHECK(texel(pos, 48, 40)[0] == doctest::Approx(0.75).epsilon(0.03));
    CHECK(texel(pos, 48, 40)[1] == doctest::Approx(1.0 - 40.0 / 64.0).epsilon(0.03));

    Graph g = bricksInto("fillToBoundingBoxSize");
    image::Image width = at64(g, "N.width");
    image::Image height = at64(g, "N.height");
    CHECK(texel(width, 16, 8)[0] > 0.4f);
    CHECK(texel(width, 16, 8)[0] < 0.5f);
    CHECK(texel(height, 16, 8)[0] > 0.18f);
    CHECK(texel(height, 16, 8)[0] < 0.25f);
    CHECK(texel(width, 0, 0)[0] == 0.0f);
}

TEST_CASE("tex natives more: flood fill mapper stamps the pattern into each region")
{
    image::Image full = at64(bricksInto("floodFillMapper"), "N.out");
    image::Image index = at64(bricksInto("fillToIndex"), "N.out");
    for (size_t i = 0; i < full.rgba.size(); i += 4)
        CHECK(full.rgba[i] == (index.rgba[i] > 0.0f ? 1.0f : 0.0f));

    // Half size: the centre of each brick keeps the pattern, its corners lose it.
    image::Image half = at64(bricksInto("floodFillMapper", {{"size", 0.5}}), "N.out");
    CHECK(texel(half, 16, 8)[0] == 1.0f);
    CHECK(texel(half, 4, 3)[0] == 0.0f);

    // A gradient pattern runs across each brick from its left edge.
    Graph g = bricksInto("floodFillMapper");
    add(g, "G", "gradient");
    link(g, "G.out", "N.pattern");
    image::Image ramped = at64(g, "N.out");
    CHECK(texel(ramped, 30, 8)[0] > texel(ramped, 4, 8)[0]);

    image::Image varied = at64(bricksInto("floodFillMapper", {{"valueRandom", 1.0}, {"seed", 3}}), "N.out");
    std::set<float> values;
    for (size_t i = 0; i < varied.rgba.size(); i += 4)
        if (index.rgba[i] > 0.0f)
            values.insert(varied.rgba[i]);
    CHECK(values.size() == 8);
}
