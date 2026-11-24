#include "TexTest.h"

#include "tex/NodeDef.h"

#include <array>
#include <fstream>

using namespace textest;

namespace
{
/** The node fed by perlin noise on its first input. */
Graph fedBy(const std::string& type, json values, const std::string& input = "input", const std::string& out = "out")
{
    Graph g;
    add(g, "Src", "perlin", {{"scale", 4}, {"seed", 7}});
    add(g, "N", type, std::move(values));
    link(g, "Src.out", "N." + input);
    return into(g, "N." + out);
}
} // namespace

TEST_CASE("tex more nodes: golden images")
{
    struct Case
    {
        const char* name;
        Graph graph;
    };
    Graph bricks;
    add(bricks, "B", "bricks");
    add(bricks, "H", "hexTile", {{"scale", 3}, {"seed", 2}, {"hueJitter", 0.3}});
    link(bricks, "B.mask", "H.input");
    bricks = into(bricks, "H.out");

    Graph wipe;
    add(wipe, "A", "perlin", {{"scale", 4}});
    add(wipe, "B", "voronoi", {{"scale", 5}});
    add(wipe, "C", "compare", {{"position", 0.4}, {"angle", 20.0}});
    link(wipe, "A.out", "C.a");
    link(wipe, "B.f1", "C.b");
    wipe = into(wipe, "C.out");

    Graph cells;
    add(cells, "R", "randomPerCell", {{"cellsX", 6}, {"cellsY", 3}, {"seed", 4}});
    cells = into(cells, "R.color");

    const std::vector<Case> cases = {
        {"hexTile", bricks},
        {"compare", wipe},
        {"colorMask", fedBy("colorMask", {{"color", {0.5, 0.5, 0.5, 1.0}}, {"tolerance", 0.1}, {"softness", 0.1}})},
        {"multiColorMask", fedBy("multiColorMask", {{"count", 2}, {"color1", {0.3, 0.3, 0.3, 1.0}}, {"color2", {0.7, 0.7, 0.7, 1.0}}, {"tolerance", 0.05}})},
        {"trig", fedBy("trig", {{"operation", "arcsine"}}, "a")},
        {"condition", fedBy("condition", {{"comparison", "greater"}}, "a")},
        {"remap", fedBy("remap", {{"fromLow", 0.3}, {"fromHigh", 0.7}, {"toLow", 1.0}, {"toHigh", 0.0}})},
        {"randomPerCell", cells},
    };
    for (const Case& c : cases)
    {
        INFO(c.name);
        Plan p = plan(c.graph, "Output", "baseColor", 64);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        golden(std::string("more_") + c.name, evaluateCpu(p).outputs.at("out"));
    }
}

TEST_CASE("tex more nodes: remap, condition and color mask values")
{
    Graph g;
    add(g, "C", "constant", {{"value", 0.6}});
    add(g, "R", "remap", {{"fromLow", 0.5}, {"fromHigh", 1.0}, {"toLow", 0.0}, {"toHigh", 10.0}});
    link(g, "C.out", "R.input");
    CHECK(texel(eval(into(g, "R.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(2.0).epsilon(1e-3));

    Graph h;
    add(h, "C", "constant", {{"value", 0.6}});
    add(h, "K", "condition", {{"comparison", "less"}});
    add(h, "T", "color", {{"color", {1.0, 0.0, 0.0, 1.0}}});
    add(h, "F", "color", {{"color", {0.0, 0.0, 1.0, 1.0}}});
    link(h, "C.out", "K.a");
    link(h, "T.out", "K.ifTrue");
    link(h, "F.out", "K.ifFalse");
    // 0.6 is not less than b's default 0.5: ifFalse.
    CHECK(texel(eval(into(h, "K.out"), "Output", "baseColor", 4), 1, 1)[2] == doctest::Approx(1.0));

    Graph m;
    add(m, "C", "color", {{"color", {0.9, 0.1, 0.1, 1.0}}});
    add(m, "M", "colorMask", {{"color", {1.0, 0.0, 0.0, 1.0}}, {"tolerance", 0.2}, {"softness", 0.01}});
    link(m, "C.out", "M.input");
    CHECK(texel(eval(into(m, "M.out"), "Output", "baseColor", 4, PortType::Gray), 1, 1)[0] == doctest::Approx(1.0));
}

TEST_CASE("tex more nodes: Separate and Combine route channels, Remap bounds take links")
{
    Graph rgb;
    add(rgb, "C", "color", {{"color", {0.2, 0.4, 0.6, 1.0}}});
    add(rgb, "S", "separateRgb");
    add(rgb, "J", "combineRgb");
    link(rgb, "C.out", "S.input");
    link(rgb, "S.b", "J.r");
    link(rgb, "S.r", "J.b");
    link(rgb, "S.g", "J.g");
    const float* swapped = texel(eval(into(rgb, "J.out"), "Output", "baseColor", 4), 1, 1);
    CHECK(swapped[0] == doctest::Approx(0.6).epsilon(1e-4));
    CHECK(swapped[1] == doctest::Approx(0.4).epsilon(1e-4));
    CHECK(swapped[2] == doctest::Approx(0.2).epsilon(1e-4));

    // Separate XYZ reads the uvs unlinked; Math doubles u and Combine XYZ rebuilds the vector.
    Graph xyz;
    add(xyz, "S", "separateXyz");
    add(xyz, "M", "math", {{"operation", "multiply"}, {"b", 2.0}});
    add(xyz, "J", "combineXyz");
    link(xyz, "S.x", "M.a");
    link(xyz, "M.out", "J.x");
    link(xyz, "S.y", "J.y");
    image::Image img = eval(into(xyz, "J.out"), "Output", "baseColor", 16);
    const float u = 9.5f / 16.0f, v = 1.0f - 4.5f / 16.0f;
    CHECK(texel(img, 9, 4)[0] == doctest::Approx(u * 2.0f).epsilon(1e-4));
    CHECK(texel(img, 9, 4)[1] == doctest::Approx(v).epsilon(1e-4));

    Graph r;
    add(r, "C", "constant", {{"value", 0.6}});
    add(r, "H", "constant", {{"value", 4.0}});
    add(r, "R", "remap", {{"fromLow", 0.5}, {"fromHigh", 1.0}});
    link(r, "C.out", "R.input");
    link(r, "H.out", "R.toHigh");
    CHECK(texel(eval(into(r, "R.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.8).epsilon(1e-3));
}

TEST_CASE("tex more nodes: Vector Math works on UV coordinates")
{
    Graph g;
    add(g, "U", "uv");
    add(g, "M", "vectorMath", {{"operation", "scale"}, {"scale", 3.0}});
    link(g, "U.uv", "M.a");
    image::Image scaled = eval(into(g, "M.out"), "Output", "baseColor", 16);
    const float u = 5.5f / 16.0f, v = 1.0f - 10.5f / 16.0f;
    CHECK(texel(scaled, 5, 10)[0] == doctest::Approx(u * 3.0f).epsilon(1e-4));
    CHECK(texel(scaled, 5, 10)[1] == doctest::Approx(v * 3.0f).epsilon(1e-4));

    g.node("M")->values["operation"] = "length";
    image::Image len = eval(into(g, "M.value"), "Output", "baseColor", 16, PortType::Gray);
    CHECK(texel(len, 5, 10)[0] == doctest::Approx(std::sqrt(u * u + v * v)).epsilon(1e-4));
}

namespace
{
/** One gray value of the node's output at a texel of a 4 x 4 evaluation. */
float grayOf(const Graph& g, const std::string& plug)
{
    return texel(eval(into(g, plug), "Output", "baseColor", 4, PortType::Gray), 1, 1)[0];
}

image::Image flatImage(float r, float g, float b)
{
    image::Image img{4, 4, {}, {}};
    for (int k = 0; k < 16; ++k)
        img.rgba.insert(img.rgba.end(), {r, g, b, 1.0f});
    return img;
}

/** Fresnel at one texel, the mesh facing +Z (world normal map) at the middle of the position map. */
float fresnelAt(json values)
{
    Graph g;
    add(g, "F", "fresnel", std::move(values));
    Plan p = plan(into(g, "F.out"), "Output", "baseColor", 4, PortType::Gray);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    EvalOptions o;
    o.externals["worldNormal"] = flatImage(0.5f, 0.5f, 1.0f);
    o.externals["position"] = flatImage(0.5f, 0.5f, 0.5f);
    return texel(evaluateCpu(p, o).outputs.at("out"), 1, 1)[0];
}
} // namespace

TEST_CASE("tex more nodes: Image wraps past its edge by repeat, extend, mirror or clip, and filters by closest or cubic")
{
    // A 4 x 4 ramp holding its column index, read at uv * 2 so the right half of the result lies past the image's edge.
    image::Image ramp{4, 4, {}, {}};
    for (int k = 0; k < 16; ++k)
        ramp.rgba.insert(ramp.rgba.end(), {float(k % 4), 0.0f, 0.0f, 1.0f});
    auto at = [&](const char* wrap, const char* filter, const image::Image& file, int x) {
        Graph g;
        add(g, "U", "uv");
        add(g, "S", "vectorMath", {{"operation", "scale"}, {"scale", 2.0}});
        add(g, "I", "image", {{"file", "external:ramp"}, {"wrap", wrap}, {"filter", filter}});
        link(g, "U.uv", "S.a");
        link(g, "S.out", "I.coord");
        Plan p = plan(into(g, "I.out"), "Output", "baseColor", 8);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        EvalOptions o;
        o.externals["ramp"] = file;
        return texel(evaluateCpu(p, o).outputs.at("out"), x, 7)[0];
    };
    // Column 5 reads image texel 5 and column 7 texel 7, one and three past the last.
    CHECK(at("repeat", "closest", ramp, 5) == doctest::Approx(1.0));
    CHECK(at("repeat", "closest", ramp, 7) == doctest::Approx(3.0));
    CHECK(at("extend", "closest", ramp, 5) == doctest::Approx(3.0));
    CHECK(at("extend", "closest", ramp, 7) == doctest::Approx(3.0));
    CHECK(at("mirror", "closest", ramp, 5) == doctest::Approx(2.0));
    CHECK(at("mirror", "closest", ramp, 7) == doctest::Approx(0.0));
    CHECK(at("clip", "closest", ramp, 5) == doctest::Approx(0.0));
    CHECK(at("clip", "closest", ramp, 2) == doctest::Approx(2.0));
    // Cubic weights sum to one, so a flat image stays flat.
    CHECK(at("repeat", "cubic", flatImage(0.5f, 0.5f, 0.5f), 3) == doctest::Approx(0.5).epsilon(1e-4));
}

TEST_CASE("tex more nodes: Math's extended operations")
{
    const std::vector<std::tuple<const char*, double, double, double, double>> cases = {
        {"multiplyAdd", 2.0, 3.0, 1.0, 7.0},    {"squareRoot", 9.0, 0.0, 0.0, 3.0},     {"inverseSquareRoot", 4.0, 0.0, 0.0, 0.5},
        {"logarithm", 8.0, 2.0, 0.0, 3.0},      {"sign", -2.0, 0.0, 0.0, -1.0},          {"round", 2.5, 0.0, 0.0, 3.0},
        {"ceiling", 2.1, 0.0, 0.0, 3.0},        {"truncate", -2.7, 0.0, 0.0, -2.0},      {"snap", 0.74, 0.25, 0.0, 0.75},
        {"wrap", 1.25, 0.0, 1.0, 0.25},         {"pingPong", 1.5, 1.0, 0.0, 0.5},        {"smoothMinimum", 0.3, 0.3, 0.0, 0.3},
        {"compare", 0.30, 0.32, 0.05, 1.0},     {"lessThan", 0.2, 0.3, 0.0, 1.0},        {"greaterThan", 0.2, 0.3, 0.0, 0.0},
        {"mix", 0.0, 10.0, 0.25, 2.5},          {"toDegrees", 3.14159265, 0.0, 0.0, 180.0},
    };
    for (const auto& [op, a, b, c, want] : cases)
    {
        INFO(op);
        Graph g;
        add(g, "M", "math", {{"operation", op}, {"a", a}, {"b", b}, {"c", c}});
        CHECK(texel(eval(into(g, "M.out"), "Output", "baseColor", 4, PortType::Gray), 1, 1)[0] == doctest::Approx(want).epsilon(1e-3));
    }
}

TEST_CASE("tex more nodes: White Noise, Checker, Wave, Grayscale, Posterize, Mix, Height Blend and the normal tools")
{
    Graph chk;
    add(chk, "C", "checker", {{"countX", 2}, {"countY", 2}});
    image::Image board = eval(into(chk, "C.out"), "Output", "baseColor", 4, PortType::Gray);
    CHECK(texel(board, 0, 0)[0] == doctest::Approx(texel(board, 2, 2)[0]));
    CHECK(texel(board, 0, 0)[0] != doctest::Approx(texel(board, 2, 0)[0]));

    Graph noise;
    add(noise, "N", "whiteNoise", {{"scale", 4}, {"seed", 3}});
    image::Image n = eval(into(noise, "N.out"), "Output", "baseColor", 4, PortType::Gray);
    CHECK(texel(n, 0, 0)[0] != doctest::Approx(texel(n, 1, 0)[0]));

    Graph wave;
    add(wave, "W", "wave", {{"profile", "saw"}, {"count", 1}});
    // A saw along x is u itself.
    CHECK(texel(eval(into(wave, "W.out"), "Output", "baseColor", 4, PortType::Gray), 1, 1)[0] == doctest::Approx(1.5 / 4.0).epsilon(1e-4));

    Graph gray;
    add(gray, "C", "color", {{"color", {0.2, 0.4, 0.9, 1.0}}});
    add(gray, "G", "grayscale", {{"mode", "maximum"}});
    link(gray, "C.out", "G.input");
    CHECK(grayOf(gray, "G.out") == doctest::Approx(0.9));

    Graph post;
    add(post, "C", "color", {{"color", {0.6, 0.6, 0.6, 1.0}}});
    add(post, "P", "posterize", {{"steps", 2}});
    link(post, "C.out", "P.input");
    CHECK(texel(eval(into(post, "P.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(1.0));

    Graph mix;
    add(mix, "M", "mix", {{"a", {0.0, 0.0, 0.0, 1.0}}, {"b", {1.0, 0.5, 0.0, 1.0}}, {"factor", 0.5}});
    CHECK(texel(eval(into(mix, "M.out"), "Output", "baseColor", 4), 1, 1)[1] == doctest::Approx(0.25));

    Graph hb;
    add(hb, "H", "heightBlend", {{"heightTop", 0.8}, {"heightBottom", 0.2}, {"contrast", 0.9}});
    CHECK(grayOf(hb, "H.mask") == doctest::Approx(1.0));
    CHECK(grayOf(hb, "H.height") == doctest::Approx(0.8));

    // A normal leaning +x turned a quarter turn leans +y.
    Graph nr;
    add(nr, "C", "color", {{"color", {1.0, 0.5, 0.5, 1.0}}});
    add(nr, "R", "normalRotate", {{"angle", 90.0}});
    link(nr, "C.out", "R.input");
    const float* turned = texel(eval(into(nr, "R.out"), "Output", "baseColor", 4), 1, 1);
    CHECK(turned[0] == doctest::Approx(0.5).epsilon(1e-3));
    CHECK(turned[1] == doctest::Approx(1.0).epsilon(1e-3));

    Graph nn;
    add(nn, "C", "color", {{"color", {0.5, 0.5, 0.75, 1.0}}});
    add(nn, "N", "normalNormalize");
    link(nn, "C.out", "N.input");
    CHECK(texel(eval(into(nn, "N.out"), "Output", "baseColor", 4), 1, 1)[2] == doctest::Approx(1.0).epsilon(1e-4));

    Graph bc;
    add(bc, "C", "color", {{"color", {0.5, 0.5, 0.5, 1.0}}});
    add(bc, "B", "brightnessContrast", {{"brightness", 0.1}, {"contrast", 0.5}});
    link(bc, "C.out", "B.input");
    // 1.5 * 0.5 + (0.1 - 0.25)
    CHECK(texel(eval(into(bc, "B.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.6).epsilon(1e-4));
}

TEST_CASE("tex more nodes: Blend's layer modes, Remap's interpolations and Transform's pivot and edges")
{
    Graph bl;
    add(bl, "F", "color", {{"color", {0.5, 0.5, 0.5, 1.0}}});
    add(bl, "K", "color", {{"color", {0.5, 0.5, 0.5, 1.0}}});
    add(bl, "B", "blend", {{"mode", "exclusion"}});
    link(bl, "F.out", "B.foreground");
    link(bl, "K.out", "B.background");
    CHECK(texel(eval(into(bl, "B.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.5));

    Graph rm;
    add(rm, "C", "constant", {{"value", 0.6}});
    add(rm, "R", "remap", {{"interpolation", "stepped"}, {"steps", 4}});
    link(rm, "C.out", "R.input");
    CHECK(grayOf(rm, "R.out") == doctest::Approx(0.5));

    // A quarter turn around the left edge's middle reads the input's lower left from the result's upper left, and past
    // the input's edge for the right half, where clip leaves nothing.
    Graph tr;
    add(tr, "C", "color", {{"color", {1.0, 1.0, 1.0, 1.0}}});
    add(tr, "T", "transform", {{"rotation", 90.0}, {"pivotX", -0.5}, {"edges", "clip"}});
    link(tr, "C.out", "T.input");
    image::Image turned = eval(into(tr, "T.out"), "Output", "baseColor", 8);
    CHECK(texel(turned, 1, 1)[3] == doctest::Approx(1.0));
    CHECK(texel(turned, 6, 6)[3] == doctest::Approx(0.0));
}

TEST_CASE("tex more nodes: an SVG reads as an image, drawn at the plan's size")
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_tex_square.svg";
    {
        std::ofstream out(file);
        out << R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50"><rect x="0" y="0" width="50" height="50" fill="#ff0000"/></svg>)";
    }
    std::string err;
    image::Image raw = image::readSvg(file, 200, &err);
    REQUIRE_MESSAGE(raw.width == 200, err);
    CHECK(raw.height == 100);
    // The left half is the red square; the right half is outside every shape.
    CHECK(raw.rgba[(size_t(50) * 200 + 50) * 4 + 0] == doctest::Approx(1.0));
    CHECK(raw.rgba[(size_t(50) * 200 + 150) * 4 + 3] == doctest::Approx(0.0));

    Graph g;
    add(g, "I", "image", {{"file", file.generic_string()}});
    image::Image img = eval(into(g, "I.out"), "Output", "baseColor", 16);
    CHECK(texel(img, 3, 8)[0] == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(texel(img, 12, 8)[0] == doctest::Approx(0.0).epsilon(1e-3));
    std::filesystem::remove(file);
}

TEST_CASE("tex more nodes: Non-Uniform Blur, Bevel, Radial Splatter, Warp's amount and Tiler's extra patterns")
{
    // A hard black and white split, blurred only where the map is white.
    Graph nb;
    add(nb, "G", "gradient");
    add(nb, "B", "nonUniformBlur", {{"radius", 0.1}, {"map", 0.0}});
    link(nb, "G.out", "B.input");
    Plan p = plan(into(nb, "B.out"), "Output", "baseColor", 32);
    REQUIRE_MESSAGE(p.error.empty(), p.error);

    // A filled square bevels to 0 at its edge and 1 deep inside.
    Graph bv;
    add(bv, "S", "sdfBox", {{"width", 0.8}, {"height", 0.8}});
    add(bv, "M", "sdfToMask");
    add(bv, "B", "bevel", {{"width", 0.1}});
    link(bv, "S.sdf", "M.input");
    link(bv, "M.out", "B.input");
    image::Image bevel = eval(into(bv, "B.out"), "Output", "baseColor", 64, PortType::Gray);
    CHECK(texel(bevel, 32, 32)[0] == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(texel(bevel, 7, 32)[0] < 0.5f);
    CHECK(texel(bevel, 1, 1)[0] == doctest::Approx(0.0));

    Graph rs;
    add(rs, "R", "radialSplatter", {{"count", 4}, {"radius", 0.3}, {"size", 0.1}});
    image::Image radial = eval(into(rs, "R.out"), "Output", "baseColor", 64, PortType::Gray);
    // Copies sit at 0.3 right of the centre and nowhere in the middle.
    CHECK(texel(radial, 51, 32)[0] == doctest::Approx(1.0));
    CHECK(texel(radial, 32, 32)[0] == doctest::Approx(0.0));

    Graph wp;
    add(wp, "N", "perlin", {{"scale", 4}});
    add(wp, "W", "warp", {{"intensity", 4.0}, {"amount", 0.0}});
    link(wp, "N.out", "W.input");
    link(wp, "N.out", "W.warp");
    image::Image still = eval(into(wp, "W.out"), "Output", "baseColor", 16);
    image::Image source = eval(into(wp, "N.out"), "Output", "baseColor", 16);
    CHECK(texel(still, 5, 5)[0] == doctest::Approx(texel(source, 5, 5)[0]).epsilon(1e-4));

    // Two patterns, a constant 1 and a constant 0.5: instances draw both.
    Graph tl;
    add(tl, "A", "constant", {{"value", 1.0}});
    add(tl, "H", "constant", {{"value", 0.5}});
    add(tl, "T", "tiler", {{"countX", 8}, {"countY", 8}, {"size", 1.0}, {"patterns", 2}});
    link(tl, "A.out", "T.pattern");
    link(tl, "H.out", "T.pattern2");
    image::Image tiles = eval(into(tl, "T.out"), "Output", "baseColor", 64, PortType::Gray);
    bool full = false, half = false;
    for (int y = 4; y < 64; y += 8)
        for (int x = 4; x < 64; x += 8)
        {
            full |= std::abs(texel(tiles, x, y)[0] - 1.0f) < 1e-3f;
            half |= std::abs(texel(tiles, x, y)[0] - 0.5f) < 1e-3f;
        }
    CHECK(full);
    CHECK(half);
}

TEST_CASE("tex more nodes: the third batch compiles and evaluates, with its values")
{
    for (const char* type : {"highPass", "pixelate", "gamma", "dropShadow", "glow", "heightToShadow", "normalReconstruct", "safeTransform", "vectorRotate",
                             "cornerPin", "vector", "sphereMask", "curlNoise", "grid", "starburst", "alphaTools"})
    {
        INFO(type);
        const tex::NodeDef* def = tex::findNode(type);
        REQUIRE(def);
        Graph g;
        add(g, "N", type);
        Plan p = plan(into(g, "N." + def->outputs.front().id), "Output", "baseColor", 8);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        CHECK(evaluateCpu(p).outputs.at("out").width == 8);
    }

    Graph gm;
    add(gm, "C", "color", {{"color", {0.25, 0.25, 0.25, 1.0}}});
    add(gm, "G", "gamma", {{"gamma", 2.0}});
    link(gm, "C.out", "G.input");
    CHECK(texel(eval(into(gm, "G.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.5).epsilon(1e-4));

    Graph sm;
    add(sm, "S", "sphereMask", {{"radius", 0.25}, {"hardness", 1.0}});
    image::Image mask = eval(into(sm, "S.out"), "Output", "baseColor", 16, PortType::Gray);
    CHECK(texel(mask, 8, 8)[0] == doctest::Approx(1.0));
    CHECK(texel(mask, 0, 0)[0] == doctest::Approx(0.0));

    // A pinned square shrunk to the left half: the right half is clipped away.
    Graph cp;
    add(cp, "C", "color", {{"color", {1.0, 1.0, 1.0, 1.0}}});
    add(cp, "P", "cornerPin", {{"bottomRightX", 0.5}, {"topRightX", 0.5}});
    link(cp, "C.out", "P.input");
    image::Image pinned = eval(into(cp, "P.out"), "Output", "baseColor", 16);
    CHECK(texel(pinned, 3, 8)[3] == doctest::Approx(1.0));
    CHECK(texel(pinned, 12, 8)[3] == doctest::Approx(0.0));

    Graph nr;
    add(nr, "C", "color", {{"color", {0.5, 0.5, 0.0, 1.0}}});
    add(nr, "N", "normalReconstruct");
    link(nr, "C.out", "N.input");
    CHECK(texel(eval(into(nr, "N.out"), "Output", "baseColor", 4), 1, 1)[2] == doctest::Approx(1.0).epsilon(1e-4));

    Graph gr;
    add(gr, "G", "grid", {{"countX", 4}, {"countY", 4}, {"thickness", 0.2}});
    image::Image grid = eval(into(gr, "G.out"), "Output", "baseColor", 32, PortType::Gray);
    CHECK(texel(grid, 0, 4)[0] == doctest::Approx(1.0));
    CHECK(texel(grid, 4, 4)[0] == doctest::Approx(0.0));

    Graph vm;
    add(vm, "A", "vector", {{"x", 1.0}, {"y", -1.0}, {"z", 0.0}});
    add(vm, "B", "vector", {{"x", 0.0}, {"y", 1.0}, {"z", 0.0}});
    add(vm, "M", "vectorMath", {{"operation", "reflect"}});
    link(vm, "A.out", "M.a");
    link(vm, "B.out", "M.b");
    const float* reflected = texel(eval(into(vm, "M.out"), "Output", "baseColor", 4), 1, 1);
    CHECK(reflected[0] == doctest::Approx(1.0));
    CHECK(reflected[1] == doctest::Approx(1.0));
}

TEST_CASE("tex more nodes: the fourth batch compiles and evaluates, with its values")
{
    for (const char* type : {"fractalSum", "streaks", "fibers", "cobblestones", "tileRandom", "integer", "time", "panner", "rotator"})
    {
        INFO(type);
        const tex::NodeDef* def = tex::findNode(type);
        REQUIRE(def);
        Graph g;
        add(g, "N", type);
        Plan p = plan(into(g, "N." + def->outputs.front().id), "Output", "baseColor", 8);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        CHECK(evaluateCpu(p).outputs.at("out").width == 8);
    }
    for (const char* base : {"perlin", "value", "simplex", "cells"})
    {
        INFO(base);
        Graph g;
        add(g, "F", "fractalSum", {{"base", base}});
        image::Image img = eval(into(g, "F.out"), "Output", "baseColor", 16, PortType::Gray);
        CHECK(texel(img, 3, 3)[0] >= 0.0f);
        CHECK(texel(img, 3, 3)[0] <= 1.0f);
    }

    // Lacunarity 2 and no distortion are what Perlin always made; distortion changes it and it still tiles.
    Graph plain;
    add(plain, "P", "perlin", {{"scale", 4}, {"seed", 2}});
    Graph bent;
    add(bent, "P", "perlin", {{"scale", 4}, {"seed", 2}, {"distortion", 1.0}});
    image::Image a = eval(into(plain, "P.out"), "Output", "baseColor", 16, PortType::Gray);
    image::Image b = eval(into(bent, "P.out"), "Output", "baseColor", 16, PortType::Gray);
    CHECK(maxDiff(a, b) > 1e-3);

    Graph cells;
    add(cells, "C", "cells", {{"metric", "minkowski"}, {"exponent", 1.0}, {"mode", "f1"}});
    Graph manhattan;
    add(manhattan, "C", "cells", {{"metric", "manhattan"}, {"mode", "f1"}});
    CHECK(maxDiff(eval(into(cells, "C.out"), "Output", "baseColor", 16), eval(into(manhattan, "C.out"), "Output", "baseColor", 16)) < 1e-4);

    Graph vor;
    add(vor, "V", "voronoi", {{"smoothness", 0.5}});
    Plan vp = plan(into(vor, "V.smoothF1"), "Output", "baseColor", 8, PortType::Gray);
    REQUIRE_MESSAGE(vp.error.empty(), vp.error);

    // Hue Map shifts a pure red half a turn to cyan.
    Graph hsv;
    add(hsv, "C", "color", {{"color", {1.0, 0.0, 0.0, 1.0}}});
    add(hsv, "H", "hsv", {{"hueMap", 0.5}});
    link(hsv, "C.out", "H.input");
    const float* cyan = texel(eval(into(hsv, "H.out"), "Output", "baseColor", 4), 1, 1);
    CHECK(cyan[0] == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(cyan[2] == doctest::Approx(1.0).epsilon(1e-3));

    // A panner at 0.25 textures a second, two seconds in, reads the input half a texture along.
    Graph pan;
    add(pan, "U", "uv");
    add(pan, "S", "separateXyz");
    add(pan, "P", "panner", {{"speedX", 0.25}, {"speedY", 0.0}, {"time", 2.0}});
    link(pan, "U.uv", "S.vec");
    link(pan, "S.x", "P.input");
    image::Image panned = eval(into(pan, "P.out"), "Output", "baseColor", 16);
    CHECK(texel(panned, 12, 8)[0] == doctest::Approx(12.5 / 16.0 - 0.5).epsilon(1e-3));

    Graph cob;
    add(cob, "C", "cobblestones", {{"gap", 0.1}});
    image::Image stones = eval(into(cob, "C.mask"), "Output", "baseColor", 32, PortType::Gray);
    bool stone = false, gap = false;
    for (float v : stones.rgba)
    {
        stone |= v > 0.5f;
        gap |= v < 0.5f;
    }
    CHECK(stone);
    CHECK(gap);
}

TEST_CASE("tex more nodes: Comparison, Logic, Select, Step and Smooth Step")
{
    const std::vector<std::pair<std::string, std::array<float, 3>>> ops = {
        // results for a = 0.3 against b = 0.5, 0.3 and 0.2
        {"greaterThan", {0, 0, 1}}, {"lessThan", {1, 0, 0}}, {"greaterOrEqual", {0, 1, 1}},
        {"lessOrEqual", {1, 1, 0}}, {"equal", {0, 1, 0}},    {"notEqual", {1, 0, 1}},
    };
    for (const auto& [op, want] : ops)
    {
        const float bs[3] = {0.5f, 0.3f, 0.2f};
        for (int k = 0; k < 3; ++k)
        {
            INFO(op << " b=" << bs[k]);
            Graph g;
            add(g, "C", "comparison", {{"operation", op}, {"a", 0.3}, {"b", bs[k]}});
            CHECK(grayOf(g, "C.out") == doctest::Approx(want[size_t(k)]));
        }
    }
    // Tolerance takes a link: 0.3 and 0.35 are equal within 0.1.
    Graph tol;
    add(tol, "T", "constant", {{"value", 0.1}});
    add(tol, "C", "comparison", {{"operation", "equal"}, {"a", 0.3}, {"b", 0.35}});
    link(tol, "T.out", "C.tolerance");
    CHECK(grayOf(tol, "C.out") == doctest::Approx(1.0));

    const std::vector<std::pair<std::string, std::array<float, 4>>> logic = {
        // results for (a, b) = (0, 0), (0, 1), (1, 0), (1, 1)
        {"and", {0, 0, 0, 1}}, {"or", {0, 1, 1, 1}},  {"not", {1, 1, 0, 0}},  {"xor", {0, 1, 1, 0}},
        {"nand", {1, 1, 1, 0}}, {"nor", {1, 0, 0, 0}}, {"xnor", {1, 0, 0, 1}},
    };
    for (const auto& [op, want] : logic)
        for (int k = 0; k < 4; ++k)
        {
            INFO(op << " case " << k);
            Graph g;
            add(g, "L", "logic", {{"operation", op}, {"a", double(k / 2)}, {"b", double(k % 2)}});
            CHECK(grayOf(g, "L.out") == doctest::Approx(want[size_t(k)]));
        }

    // Select by a comparison: a greater-than test picks ifTrue.
    Graph sel;
    add(sel, "C", "comparison", {{"operation", "greaterThan"}, {"a", 0.7}, {"b", 0.5}});
    add(sel, "S", "select", {{"ifTrue", {0.9, 0.1, 0.2, 1.0}}, {"ifFalse", {0.0, 0.0, 1.0, 1.0}}});
    link(sel, "C.out", "S.condition");
    const float* picked = texel(eval(into(sel, "S.out"), "Output", "baseColor", 4), 1, 1);
    CHECK(picked[0] == doctest::Approx(0.9));
    CHECK(picked[2] == doctest::Approx(0.2));
    Graph soft;
    add(soft, "S", "select", {{"condition", 0.25}, {"soft", true}, {"ifTrue", {1.0, 1.0, 1.0, 1.0}}, {"ifFalse", {0.0, 0.0, 0.0, 1.0}}});
    CHECK(texel(eval(into(soft, "S.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.25));

    Graph step;
    add(step, "S", "step", {{"input", 0.4}, {"edge", 0.5}});
    CHECK(grayOf(step, "S.out") == doctest::Approx(0.0));
    step.node("S")->values["edge"] = 0.4;
    CHECK(grayOf(step, "S.out") == doctest::Approx(1.0));

    Graph ss;
    add(ss, "S", "smoothStep", {{"input", 0.5}, {"low", 0.0}, {"high", 1.0}});
    CHECK(grayOf(ss, "S.out") == doctest::Approx(0.5));
    ss.node("S")->values["input"] = 0.25;
    CHECK(grayOf(ss, "S.out") == doctest::Approx(0.15625));
    ss.node("S")->values["curve"] = "smoother";
    CHECK(grayOf(ss, "S.out") == doctest::Approx(0.103515625));
    ss.node("S")->values["curve"] = "linear";
    CHECK(grayOf(ss, "S.out") == doctest::Approx(0.25));
}

TEST_CASE("tex more nodes: Colorize range and Clamp bounds take links")
{
    // Low 0.5 to High 1 maps 0.75 to the gradient's middle.
    Graph c;
    add(c, "Hi", "constant", {{"value", 1.0}});
    add(c, "Z", "colorize", {{"input", 0.75}, {"low", 0.5}});
    link(c, "Hi.out", "Z.high");
    CHECK(texel(eval(into(c, "Z.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.5).epsilon(1e-4));
    c.node("Z")->values["gradient"] = {{"stops", {{0.0, {0, 0, 0, 1}}, {1.0, {1, 1, 1, 1}}}}, {"interpolation", "constant"}};
    CHECK(texel(eval(into(c, "Z.out"), "Output", "baseColor", 4), 1, 1)[0] == doctest::Approx(0.0));

    Graph k;
    add(k, "Lo", "constant", {{"value", 0.6}});
    add(k, "K", "clamp", {{"input", {0.2, 0.9, 0.5, 1.0}}, {"high", 0.8}});
    link(k, "Lo.out", "K.low");
    const float* clamped = texel(eval(into(k, "K.out"), "Output", "baseColor", 4), 1, 1);
    CHECK(clamped[0] == doctest::Approx(0.6));
    CHECK(clamped[1] == doctest::Approx(0.8));
    CHECK(clamped[2] == doctest::Approx(0.6));
}

TEST_CASE("tex more nodes: Fresnel from a view direction or a camera position")
{
    // Facing the viewer, Schlick at IOR 1.5 gives 0.04; edge-on gives 1.
    CHECK(fresnelAt({{"viewDirection", {0, 0, -1}}}) == doctest::Approx(0.04).epsilon(1e-3));
    CHECK(fresnelAt({{"viewDirection", {1, 0, 0}}}) == doctest::Approx(1.0));
    CHECK(fresnelAt({{"viewDirection", {-1, 0, -1}}, {"model", "power"}, {"power", 2.0}}) == doctest::Approx(std::pow(1.0 - std::sqrt(0.5), 2.0)).epsilon(1e-3));
    CHECK(fresnelAt({{"viewDirection", {0, 0, -1}}, {"bias", 0.5}}) == doctest::Approx(0.52).epsilon(1e-3));

    // The position map's middle is the origin of a -10..10 box: a camera above it sees the surface face on, one beside it edge-on.
    json persp = {{"view", "cameraPosition"}, {"boundsMin", {-10, -10, -10}}, {"boundsMax", {10, 10, 10}}, {"cameraPosition", {0, 0, 50}}};
    CHECK(fresnelAt(persp) == doctest::Approx(0.04).epsilon(1e-3));
    persp["cameraPosition"] = {50, 0, 0};
    CHECK(fresnelAt(persp) == doctest::Approx(1.0));
}

TEST_CASE("tex more nodes: operations are searchable variants, keywords listed")
{
    json types = run("texNodeTypes");
    auto find = [&](const std::string& type) {
        for (const json& t : types)
            if (t.value("type", "") == type)
                return t;
        return json();
    };
    json cmp = find("comparison");
    REQUIRE(cmp.is_object());
    bool greater = false;
    for (const json& v : cmp.value("variants", json::array()))
        greater |= v.value("label", "") == "Greater Than" && v["values"].value("operation", "") == "greaterThan";
    CHECK(greater);
    CHECK(find("math").contains("variants"));
    json fresnel = find("fresnel");
    REQUIRE(fresnel.is_object());
    CHECK(std::ranges::count(fresnel.value("keywords", std::vector<std::string>()), std::string("facing ratio")) == 1);
    CHECK_FALSE(find("fresnel").contains("variants"));

    const NodeDef* d = findNode("fresnel");
    REQUIRE(d);
    REQUIRE(d->param("camera"));
    CHECK(d->param("camera")->type == ParamType::Node);
    CHECK(d->param("camera")->options == std::vector<std::string>{"camera"});
}
