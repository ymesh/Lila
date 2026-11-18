#include "TexTest.h"

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
