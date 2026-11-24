#include "TexTest.h"

#include "core/Paths.h"

#include <filesystem>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** A size x size checkerboard of single texels, 0 and 1, alpha 1. */
image::Image checker(int size)
{
    image::Image img{size, size, std::vector<float>(size_t(size) * size_t(size) * 4)};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float* t = &img.rgba[(size_t(y) * size_t(size) + size_t(x)) * 4];
            t[0] = t[1] = t[2] = float((x + y) & 1);
            t[3] = 1.0f;
        }
    return img;
}

std::string writeImage(const fs::path& dir, const std::string& name, const image::Image& img)
{
    const fs::path path = dir / name;
    image::write(path, img, 32);
    return paths::toUtf8(path);
}

Graph imageGraph(const std::string& file, json values = json::object())
{
    Graph g;
    values["file"] = file;
    add(g, "I", "image", values);
    return g;
}
} // namespace

TEST_CASE("tex eval features: image mip levels pack after the image, area averaged")
{
    image::Image img{8, 4, std::vector<float>(8 * 4 * 4, 0.0f)};
    for (size_t i = 0; i < 32; ++i)
        img.rgba[i * 4] = float(i);
    uint32_t flags = 0;
    image::Image packed = withMipLevels(img, flags);
    // 8x4, 4x2, 2x1, 1x1: 43 texels in rows of 8.
    CHECK(((flags >> 2) & 31u) == 4u);
    CHECK((flags >> 8) == 4u);
    CHECK((flags & 3u) == 0u);
    CHECK(packed.width == 8);
    CHECK(packed.height == 6);
    const float* level1 = &packed.rgba[32 * 4];
    CHECK(level1[0] == doctest::Approx((0.0 + 1.0 + 8.0 + 9.0) / 4.0));
    const float* last = &packed.rgba[42 * 4];
    CHECK(last[0] == doctest::Approx(15.5));

    uint32_t none = 7;
    CHECK(withMipLevels(image::Image{1, 1, {1, 1, 1, 1}}, none).width == 1);
    CHECK(none == 0u);
}

TEST_CASE("tex eval features: an image read at its own uv has no mips, a tiled one does")
{
    const fs::path dir = tempDir("eval_features_mips_plan");
    const std::string file = writeImage(dir, "checker.exr", checker(16));
    auto mips = [&](json values) {
        Plan p = plan(imageGraph(file, values), "I", "out", 16);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        for (const BufferDesc& b : p.buffers)
            if (b.kind == BufferKind::Image)
                return b.mips;
        FAIL("no image buffer");
        return false;
    };
    CHECK_FALSE(mips({{"tiles", 1}}));
    CHECK(mips({{"tiles", 4}}));
}

TEST_CASE("tex eval features: a tiled image reads the prefiltered level instead of aliasing")
{
    const fs::path dir = tempDir("eval_features_mips_read");
    const std::string file = writeImage(dir, "checker.exr", checker(64));

    // At its own size the checker reads back exactly.
    const image::Image same = eval(imageGraph(file, {{"tiles", 1}}), "I", "out", 64);
    CHECK(texel(same, 0, 0)[0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(texel(same, 1, 0)[0] == doctest::Approx(1.0f).epsilon(1e-4));

    // Three repeats: each output texel covers 3 x 3 checks, so the read lands between levels 1 and 2, both grey.
    const image::Image three = eval(imageGraph(file, {{"tiles", 3}}), "I", "out", 64);
    double worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            worst = std::max(worst, std::abs(double(texel(three, x, y)[0]) - 0.5));
    CHECK(worst < 0.01);

    // Noise read four times over lands on level 2 texel for texel: the 4 x 4 averages.
    image::Image noise = checker(64);
    for (size_t i = 0; i < noise.rgba.size(); ++i)
        if (i % 4 != 3)
            noise.rgba[i] = float((i / 4 * 2654435761u) % 1000u) / 999.0f;
    const std::string noisy = writeImage(dir, "noise.exr", noise);
    const image::Image tiled = eval(imageGraph(noisy, {{"tiles", 4}}), "I", "out", 64);
    const image::Image level2 = downsample(noise, 4);
    worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            worst = std::max(worst, std::abs(double(texel(tiled, x, y)[0]) - double(texel(level2, x % 16, y % 16)[0])));
    CHECK(worst < 1e-4);
}

TEST_CASE("tex eval features: a node at half size runs at half size and reads back bilinear")
{
    Graph g;
    add(g, "P", "perlin", {{"scale", 3}, {"seed", 2}});
    const image::Image small = eval(g, "P", "out", 32);
    g.node("P")->values["outputSize"] = "half";
    Plan p = plan(g, "P", "out", 64);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    bool halfBuffer = false;
    for (const BufferDesc& b : p.buffers)
        halfBuffer |= b.kind == BufferKind::Intermediate && b.width == 32 && b.height == 32;
    CHECK(halfBuffer);
    const image::Image big = evaluateCpu(p).outputs.at("out");
    REQUIRE(big.width == 64);
    auto at = [&](int x, int y) { return texel(small, (x + 32) % 32, (y + 32) % 32)[0]; };
    double worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const float px = float(x) / 2.0f - 0.25f, py = float(y) / 2.0f - 0.25f;
            const int ix = int(std::floor(px)), iy = int(std::floor(py));
            const float tx = px - float(ix), ty = py - float(iy);
            const float top = at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx;
            const float bottom = at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx;
            worst = std::max(worst, std::abs(double(top * (1 - ty) + bottom * ty) - double(texel(big, x, y)[0])));
        }
    CHECK(worst < 1e-4);

    g.node("P")->values["outputSize"] = "double";
    Plan twice = plan(g, "P", "out", 64);
    bool doubled = false;
    for (const BufferDesc& b : twice.buffers)
        doubled |= b.kind == BufferKind::Intermediate && b.width == 128;
    CHECK(doubled);

    g.node("P")->values["outputSize"] = "huge";
    CHECK_FALSE(plan(g, "P", "out", 64).warnings.empty());
}

TEST_CASE("tex eval features: bit depth stores the node's output at 8 or 32 bits")
{
    Graph g;
    add(g, "P", "perlin", {{"scale", 3}, {"seed", 2}});
    add(g, "L", "levels");
    link(g, "P.out", "L.input");
    g.node("P")->values["bitDepth"] = "8";
    Plan p = plan(g, "L", "out", 32);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    const BufferDesc* own = nullptr;
    for (const BufferDesc& b : p.buffers)
        if (b.name == "P.out")
            own = &b;
    REQUIRE(own);
    CHECK(own->format == "rgba16f");
    const image::Image stepped = eval(g, "P", "out", 32);
    for (size_t i = 0; i < stepped.rgba.size(); ++i)
        CHECK(std::abs(stepped.rgba[i] * 255.0f - std::round(stepped.rgba[i] * 255.0f)) < 1e-3f);

    g.node("P")->values["bitDepth"] = "32";
    for (const BufferDesc& b : plan(g, "L", "out", 32).buffers)
        if (b.name == "P.out")
            CHECK(b.format == "rgba32f");
}

TEST_CASE("tex eval features: node settings set through texSetValue and listed with the node")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Sz"}}, {"resolution", 16}});
    run("texAddNode", {{"args", {"Sz", "perlin"}}, {"name", "P"}});
    run("texSetValue", {{"args", {"Sz", "P.outputSize", "quarter"}}});
    run("texSetValue", {{"args", {"Sz", "P.bitDepth", "16"}}});
    CHECK_THROWS(run("texSetValue", {{"args", {"Sz", "P.bitDepth", "12"}}}));
    const json info = run("texGraphInfo", args({"Sz"}));
    json node;
    for (const json& n : info["nodes"])
        if (n["name"] == "P")
            node = n;
    REQUIRE(node.contains("settings"));
    std::map<std::string, json> values;
    for (const json& s : node["settings"])
        values[s["id"]] = s["value"];
    CHECK(values["outputSize"] == "quarter");
    CHECK(values["bitDepth"] == "16");
    // Back to the default leaves no value behind.
    run("texSetValue", {{"args", {"Sz", "P.outputSize", "set"}}});
    for (const json& n : run("texGraphInfo", args({"Sz"}))["nodes"])
        if (n["name"] == "P")
            for (const json& s : n["settings"])
                if (s["id"] == "outputSize")
                    CHECK(s["value"] == "set");
}

TEST_CASE("tex eval features: an image with a UDIM token reads the tile being evaluated")
{
    const float first[2] = {0.0f, 0.0f}, second[2] = {1.0f, 0.0f}, above[2] = {0.0f, 1.0f};
    CHECK(imageForTile("rock_<UDIM>.png", second) == "rock_1002.png");
    CHECK(imageForTile("rock_<UDIM>.png", above) == "rock_1011.png");
    CHECK(imageForTile("rock_<UVTILE>.png", second) == "rock_u2_v1.png");
    CHECK(imageForTile("rock_1001.png", second) == "rock_1001.png");

    const fs::path dir = tempDir("eval_features_udim");
    writeImage(dir, "rock_1001.exr", image::Image{1, 1, {0.2f, 0.2f, 0.2f, 1.0f}});
    writeImage(dir, "rock_1002.exr", image::Image{1, 1, {0.7f, 0.7f, 0.7f, 1.0f}});
    Plan p = plan(imageGraph(paths::toUtf8(dir / "rock_<UDIM>.exr")), "I", "out", 4);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    auto shown = [&](const float origin[2], std::vector<std::string>* warnings = nullptr) {
        EvalOptions o;
        o.uvOffset[0] = origin[0];
        o.uvOffset[1] = origin[1];
        EvalResult r = evaluateCpu(p, o);
        if (warnings)
            *warnings = r.warnings;
        return texel(r.outputs.at("out"), 1, 2)[0];
    };
    CHECK(shown(first) == doctest::Approx(0.2f));
    CHECK(shown(second) == doctest::Approx(0.7f));
    std::vector<std::string> warnings;
    CHECK(shown(above, &warnings) == doctest::Approx(0.0f));
    CHECK_FALSE(warnings.empty());
}

TEST_CASE("tex eval features: a set's own named outputs export as files of their own")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Pk"}}, {"resolution", 8}});
    const json channels = run("texAddChannel", {{"args", {"Pk", "packed"}}, {"suffix", "PK"}});
    CHECK(std::ranges::any_of(channels, [](const json& c) { return c["name"] == "packed" && c["suffix"] == "PK"; }));
    run("texAddChannel", {{"args", {"Pk", "tint"}}, {"colorSpace", "srgb"}});
    CHECK_THROWS(run("texAddChannel", {{"args", {"Pk", "packed"}}}));
    CHECK_THROWS(run("texAddChannel", {{"args", {"Pk", "bad"}}, {"colorSpace", "aces"}}));

    run("texAddNode", {{"args", {"Pk", "color"}}, {"name", "C"}});
    run("texSetValue", {{"args", {"Pk", "C.color", {0.25, 0.5, 0.75, 1.0}}}});
    run("texConnect", {{"args", {"Pk", "C.out", "Output.packed"}}});
    run("texConnect", {{"args", {"Pk", "C.out", "Output.tint"}}});

    const fs::path dir = tempDir("eval_features_outputs");
    const json r = run("texExport", {{"args", {"Pk"}}, {"preset", "unreal"}, {"folder", dir.string()}, {"backend", "cpu"}});
    std::string packed, tint;
    for (const json& f : r["files"])
    {
        const std::string name = fs::path(f.get<std::string>()).filename().string();
        if (name.find("_PK.") != std::string::npos)
            packed = f.get<std::string>();
        if (name.find("_tint.") != std::string::npos)
            tint = f.get<std::string>();
    }
    REQUIRE(!packed.empty());
    REQUIRE(!tint.empty());
    const image::Image p = image::read(packed);
    REQUIRE(p.width == 8);
    CHECK(texel(p, 3, 3)[0] == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(texel(p, 3, 3)[2] == doctest::Approx(0.75f).epsilon(0.01));
    // sRGB-encoded on disk: 0.5 linear is about 0.735.
    CHECK(texel(image::read(tint), 3, 3)[1] == doctest::Approx(0.735f).epsilon(0.01));
    for (const json& o : r["outputs"])
        if (o["file"] == tint)
            CHECK(o["colorSpace"] == "srgb");
}

TEST_CASE("tex eval features: premultiplied alpha divides back out on read")
{
    const fs::path dir = tempDir("eval_features_alpha");
    // Left half premultiplied colour at half alpha, right half fully transparent.
    image::Image img{2, 1, {0.25f, 0.1f, 0.05f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f}};
    const std::string file = writeImage(dir, "premul.exr", img);
    for (const char* node : {"image", "imageSequence"})
    {
        CAPTURE(node);
        Graph g;
        add(g, "I", node, {{"file", file}});
        const image::Image straight = eval(g, "I", "out", 2);
        CHECK(texel(straight, 0, 0)[0] == doctest::Approx(0.25f));
        g.node("I")->values["alphaMode"] = "premultiplied";
        const image::Image fixed = eval(g, "I", "out", 2);
        CHECK(texel(fixed, 0, 0)[0] == doctest::Approx(0.5f));
        CHECK(texel(fixed, 0, 0)[1] == doctest::Approx(0.2f));
        CHECK(texel(fixed, 0, 0)[2] == doctest::Approx(0.1f));
        CHECK(texel(fixed, 0, 0)[3] == doctest::Approx(0.5f));
        CHECK(texel(fixed, 1, 0)[0] == doctest::Approx(0.0f));
        CHECK(texel(fixed, 1, 0)[3] == doctest::Approx(0.0f));
        // Alpha itself is never changed.
        CHECK(texel(eval(g, "I", "alpha", 2, PortType::Gray), 0, 0)[0] == doctest::Approx(0.5f));
    }
}
