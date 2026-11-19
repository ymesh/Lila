#include "TexTest.h"

#include "core/ExtensionContext.h"
#include "tex/NodeDef.h"
#include "geo/Attribute.h"
#include "geo/Primitives.h"
#include "tex/SlangRt.h"

#include <cstdlib>
#include <filesystem>
#include <functional>

using namespace textest;

namespace
{
namespace fs = std::filesystem;
} // namespace

TEST_CASE("tex: the CPU target is available")
{
    std::string why;
    CHECK_MESSAGE(slangrt::cpuAvailable(&why), why);
}

TEST_CASE("tex: simple nodes evaluate to their exact values")
{
    SUBCASE("constant and colour")
    {
        Graph g;
        add(g, "C", "color", {{"color", json::array({0.25, 0.5, 0.75, 1.0})}});
        image::Image img = eval(into(g, "C.out"), "Output", "baseColor", 16);
        CHECK(texel(img, 3, 7)[0] == doctest::Approx(0.25));
        CHECK(texel(img, 3, 7)[2] == doctest::Approx(0.75));
    }
    SUBCASE("unlinked channels read the output node's value")
    {
        Graph g = Graph::makeMaterial("S");
        g.node("Output")->values["roughness"] = 0.2;
        image::Image img = eval(g, "Output", "roughness", 8, PortType::Gray);
        CHECK(texel(img, 0, 0)[0] == doctest::Approx(0.2));
        CHECK(texel(img, 0, 0)[3] == doctest::Approx(1.0));
    }
    SUBCASE("uv: row 0 is the top, v = 1")
    {
        Graph g;
        add(g, "UV", "uv");
        image::Image img = eval(into(g, "UV.uv"), "Output", "baseColor", 8);
        CHECK(texel(img, 0, 0)[0] == doctest::Approx(1.0 / 16.0));
        CHECK(texel(img, 0, 0)[1] == doctest::Approx(15.0 / 16.0));
        CHECK(texel(img, 7, 7)[1] == doctest::Approx(1.0 / 16.0));
    }
    SUBCASE("linear gradient is u, levels and math apply")
    {
        Graph g;
        add(g, "G", "gradient");
        add(g, "M", "math", {{"operation", "multiply"}, {"b", 2.0}});
        add(g, "L", "levels", {{"outLow", 0.25}, {"outHigh", 0.75}});
        link(g, "G.out", "M.a");
        link(g, "M.out", "L.input");
        image::Image img = eval(into(g, "L.out"), "Output", "baseColor", 32);
        for (int x : {0, 5, 13, 31})
        {
            const double u = (x + 0.5) / 32.0;
            CHECK(texel(img, x, 9)[0] == doctest::Approx(0.25 + 0.5 * std::min(1.0, 2.0 * u)).epsilon(1e-4));
        }
    }
    SUBCASE("colorize follows the gradient stops")
    {
        Graph g;
        add(g, "G", "gradient");
        add(g, "Cz", "colorize",
            {{"gradient", {{"stops", json::array({json::array({0.0, "#000000"}), json::array({0.5, json::array({1.0, 0.0, 0.0, 1.0})}),
                                                   json::array({1.0, json::array({1.0, 1.0, 1.0, 1.0})})})}}}});
        link(g, "G.out", "Cz.input");
        image::Image img = eval(into(g, "Cz.out"), "Output", "baseColor", 64);
        const float* q = texel(img, 15, 0); // u = 0.2421875
        CHECK(q[0] == doctest::Approx(0.2421875 / 0.5).epsilon(1e-4));
        CHECK(q[1] == doctest::Approx(0.0));
        const float* h = texel(img, 47, 0); // u = 0.7421875
        CHECK(h[0] == doctest::Approx(1.0));
        CHECK(h[1] == doctest::Approx((0.7421875 - 0.5) / 0.5).epsilon(1e-4));
    }
    SUBCASE("blend multiply through a mask")
    {
        Graph g;
        add(g, "Bl", "blend", {{"mode", "multiply"}, {"foreground", json::array({0.5, 0.5, 0.5, 1.0})}, {"background", json::array({0.8, 0.4, 0.2, 1.0})},
                               {"mask", 0.5}});
        image::Image img = eval(into(g, "Bl.out"), "Output", "baseColor", 4);
        CHECK(texel(img, 1, 1)[0] == doctest::Approx(0.8 * 0.5 + 0.4 * 0.5));
        CHECK(texel(img, 1, 1)[2] == doctest::Approx(0.2 * 0.5 + 0.1 * 0.5));
    }
    SUBCASE("a flat height gives a flat normal, a slope tilts it")
    {
        Graph g;
        add(g, "N", "heightToNormal", {{"height", 0.3}});
        image::Image flat = eval(into(g, "N.normal"), "Output", "baseColor", 8);
        CHECK(texel(flat, 2, 2)[0] == doctest::Approx(0.5));
        CHECK(texel(flat, 2, 2)[2] == doctest::Approx(1.0));
        Graph s;
        add(s, "G", "gradient", {{"angle", 90.0}});
        add(s, "N", "heightToNormal");
        link(s, "G.out", "N.height");
        image::Image slope = eval(into(s, "N.normal"), "Output", "baseColor", 32);
        // Height rises with v: OpenGL normals lean towards -v, so green drops below 0.5.
        CHECK(texel(slope, 16, 16)[1] < 0.49f);
        CHECK(texel(slope, 16, 16)[0] == doctest::Approx(0.5).epsilon(1e-3));
    }
    SUBCASE("RNM of a flat base keeps the detail")
    {
        Graph g;
        add(g, "R", "normalBlend", {{"detail", json::array({0.7, 0.4, 0.9, 1.0})}});
        image::Image img = eval(into(g, "R.out"), "Output", "baseColor", 4);
        glm::vec3 d = glm::normalize(glm::vec3(0.4f, -0.2f, 0.8f)) * 0.5f + 0.5f;
        CHECK(texel(img, 0, 0)[0] == doctest::Approx(d.x).epsilon(1e-4));
        CHECK(texel(img, 0, 0)[1] == doctest::Approx(d.y).epsilon(1e-4));
    }
    SUBCASE("noises tile")
    {
        Graph g;
        add(g, "P", "perlin", {{"scale", 3}});
        Plan p = plan(into(g, "P.out"), "Output", "baseColor", 32);
        image::Image a = evaluateCpu(p).outputs.at("out");
        EvalOptions shifted;
        shifted.uvOffset[0] = 1.0f;
        shifted.uvOffset[1] = -2.0f;
        image::Image b = evaluateCpu(p, shifted).outputs.at("out");
        CHECK(maxDiff(a, b) < 1e-5);
    }
}

TEST_CASE("tex: material values reach the output's channels")
{
    Graph g = Graph::makeMaterial("S");
    add(g, "M", "material", {{"roughness", 0.15}, {"baseColor", json::array({0.1, 0.2, 0.3, 1.0})}});
    link(g, "M.material", "Output.material");
    g.node("Output")->values["metallic"] = 0.9;
    PlanRequest r;
    r.width = r.height = 8;
    r.outputs = materialRequests(g, {{"baseColor", PortType::Color}, {"roughness", PortType::Gray}, {"metallic", PortType::Gray}});
    Plan p = buildPlan(g, r);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    EvalResult e = evaluateCpu(p);
    CHECK(texel(e.outputs.at("baseColor"), 1, 1)[1] == doctest::Approx(0.2));
    CHECK(texel(e.outputs.at("roughness"), 1, 1)[0] == doctest::Approx(0.15));
    // With a material linked, unlinked channels come from it rather than from the output's own values.
    CHECK(texel(e.outputs.at("metallic"), 1, 1)[0] == doctest::Approx(0.0));
}

TEST_CASE("tex: golden images per node at 64 px")
{
    struct Case
    {
        std::string name;
        std::function<Graph()> make;
    };
    auto single = [](std::string type, json values, std::string out) {
        return [=] {
            Graph g;
            add(g, "N", type, values);
            return into(g, "N." + out);
        };
    };
    auto noiseInto = [](std::string type, json values, std::string in) {
        return [=] {
            Graph g;
            add(g, "Src", "perlin", {{"scale", 4}, {"seed", 7}});
            add(g, "N", type, values);
            link(g, "Src.out", "N." + in);
            return into(g, "N.out");
        };
    };
    std::vector<Case> cases = {
        {"perlin", single("perlin", {{"scale", 4}, {"seed", 1}}, "out")},
        {"valueNoise", single("valueNoise", {{"scale", 5}}, "out")},
        {"voronoi_f1", single("voronoi", {{"scale", 5}}, "f1")},
        {"voronoi_f2", single("voronoi", {{"scale", 5}}, "f2")},
        {"voronoi_edge", single("voronoi", {{"scale", 5}}, "edge")},
        {"voronoi_random", single("voronoi", {{"scale", 5}}, "random")},
        {"gradient_radial", single("gradient", {{"mode", "radial"}}, "out")},
        {"gradient_angular", single("gradient", {{"mode", "angular"}, {"repeat", 3.0}}, "out")},
        {"bricks_mask", single("bricks", json::object(), "mask")},
        {"bricks_random", single("bricks", json::object(), "random")},
        {"bricks_uv", single("bricks", json::object(), "brickUv")},
        {"tile", noiseInto("tile", {{"countX", 3}}, "input")},
        {"transform", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "N", "transform", {{"rotation", 30.0}, {"scale", 0.7}, {"offsetX", 0.1}});
             link(g, "B.mask", "N.input");
             return into(g, "N.out");
         }},
        {"warp", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "W", "perlin", {{"scale", 3}});
             add(g, "N", "warp", {{"intensity", 2.0}});
             link(g, "B.mask", "N.input");
             link(g, "W.out", "N.warp");
             return into(g, "N.out");
         }},
        {"levels", noiseInto("levels", {{"inLow", 0.3}, {"inHigh", 0.7}, {"gamma", 2.0}}, "input")},
        {"blend_overlay", [] {
             Graph g;
             add(g, "V", "voronoi");
             add(g, "P", "perlin");
             add(g, "N", "blend", {{"mode", "overlay"}, {"opacity", 0.8}});
             link(g, "V.edge", "N.foreground");
             link(g, "P.out", "N.background");
             return into(g, "N.out");
         }},
        {"math_power", noiseInto("math", {{"operation", "power"}, {"b", 3.0}}, "a")},
        {"colorize", noiseInto("colorize", {{"gradient", {{"stops", json::array({json::array({0.2, "#203040"}), json::array({0.8, "#e0c080"})})}}}}, "input")},
        {"heightToNormal", [] {
             Graph g;
             add(g, "Src", "perlin", {{"scale", 4}, {"seed", 7}});
             add(g, "N", "heightToNormal", {{"intensity", 2.0}});
             link(g, "Src.out", "N.height");
             return into(g, "N.normal");
         }},
        {"normalBlend", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "Hb", "heightToNormal");
             add(g, "P", "perlin", {{"scale", 8}});
             add(g, "Hp", "heightToNormal");
             add(g, "N", "normalBlend");
             link(g, "B.mask", "Hb.height");
             link(g, "P.out", "Hp.height");
             link(g, "Hb.normal", "N.base");
             link(g, "Hp.normal", "N.detail");
             return into(g, "N.out");
         }},
        {"blur", noiseInto("blur", {{"radius", 0.03}}, "input")},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.name);
        golden(c.name, eval(c.make(), "Output", "baseColor", 64));
    }
}

TEST_CASE("tex: generators are resolution independent")
{
    for (const char* type : {"perlin", "voronoi", "bricks"})
    {
        CAPTURE(type);
        Graph g;
        add(g, "N", type, {{"scale", 3}, {"rows", 4}, {"columns", 2}});
        std::string out = std::string(type) == "voronoi" ? "f1" : std::string(type) == "bricks" ? "mask" : "out";
        Graph full = into(g, "N." + out);
        image::Image big = eval(full, "Output", "baseColor", 1024);
        image::Image small = eval(full, "Output", "baseColor", 256);
        CHECK(meanDiff(downsample(big, 4), small) < 0.01);
    }
    // Height to normal measures slope in uv, so its result doesn't depend on the texel size.
    Graph h;
    add(h, "P", "perlin", {{"scale", 3}, {"octaves", 2}});
    add(h, "N", "heightToNormal");
    link(h, "P.out", "N.height");
    Graph hn = into(h, "N.normal");
    CHECK(meanDiff(downsample(eval(hn, "Output", "baseColor", 1024), 4), eval(hn, "Output", "baseColor", 256)) < 0.01);
}

TEST_CASE("tex: param edits reuse the compiled kernels")
{
    Graph g;
    add(g, "P", "perlin", {{"scale", 4}});
    add(g, "L", "levels");
    link(g, "P.out", "L.input");
    Graph full = into(g, "L.out");
    EvalResult first = evaluateCpu(plan(full, "Output", "baseColor", 32));
    full.node("P")->values["scale"] = 11;
    full.node("L")->values["gamma"] = 3.0;
    EvalResult second = evaluateCpu(plan(full, "Output", "baseColor", 32));
    CHECK(second.compiled == 0);
    CHECK(second.cached == 1);
    CHECK(maxDiff(first.outputs.at("out"), second.outputs.at("out")) > 0.01);
}

TEST_CASE("tex: graph-defined nodes and subgroups evaluate like their inline graphs")
{
    // A library node made of a graph: perlin through levels, the noise scale exposed, a gray port multiplied in.
    Graph inner = Graph::makeGroup("Grain");
    inner.outputs = {{"out", PortType::Color, {}}};
    inner.inputs.push_back({"mask", false, PortType::Gray, ParamType::Float, 1.0});
    GraphInput sc;
    sc.name = "noiseScale";
    sc.param = true;
    sc.paramType = ParamType::Int;
    sc.defaultValue = 2;
    sc.targets = {"P.scale"};
    inner.inputs.push_back(sc);
    add(inner, "P", "perlin", {{"seed", 4}});
    add(inner, "M", "math", {{"operation", "multiply"}});
    link(inner, "P.out", "M.a");
    link(inner, "GroupInput.mask", "M.b");
    link(inner, "M.out", "GroupOutput.out");

    Graph manual;
    add(manual, "P", "perlin", {{"seed", 4}, {"scale", 6}});
    add(manual, "G", "gradient");
    add(manual, "M", "math", {{"operation", "multiply"}});
    link(manual, "P.out", "M.a");
    link(manual, "G.out", "M.b");
    image::Image expected = eval(into(manual, "M.out"), "Output", "baseColor", 32);

    {
        ext::Context ctx("grainPack");
        NodeDef d;
        d.type = "grain";
        d.label = "Grain";
        d.kind = NodeKind::Graph;
        d.graph = inner.toJson();
        registerExtensionNode(ctx, d);
        Graph g;
        add(g, "G", "gradient");
        add(g, "X", "grain", {{"noiseScale", 6}});
        link(g, "G.out", "X.mask");
        image::Image viaNode = eval(into(g, "X.out"), "Output", "baseColor", 32);
        CHECK(maxDiff(viaNode, expected) < 1e-5);
    }

    Graph withGroup;
    withGroup.subgroups["Grain"] = inner.toJson();
    add(withGroup, "G", "gradient");
    add(withGroup, "X", kGroupType, {{"group", "Grain"}, {"noiseScale", 6}});
    link(withGroup, "G.out", "X.mask");
    image::Image viaGroup = eval(into(withGroup, "X.out"), "Output", "baseColor", 32);
    CHECK(maxDiff(viaGroup, expected) < 1e-5);
}

TEST_CASE("tex: externals feed position, with uv as the fallback")
{
    Graph g;
    add(g, "Pos", "position");
    Plan p = plan(into(g, "Pos.position"), "Output", "baseColor", 8);
    REQUIRE(p.error.empty());
    image::Image fallback = evaluateCpu(p).outputs.at("out");
    CHECK(texel(fallback, 0, 0)[0] == doctest::Approx(1.0 / 16.0));
    EvalOptions o;
    o.externals["position"] = {1, 1, {3.0f, 4.0f, 5.0f, 1.0f}};
    image::Image given = evaluateCpu(p, o).outputs.at("out");
    CHECK(texel(given, 4, 4)[2] == doctest::Approx(5.0));

    // With the bake's bounds, its 0-1 positions come out in cm.
    g.node("Pos")->values["boundsMin"] = json::array({250.0, 0.0, -50.0});
    g.node("Pos")->values["boundsMax"] = json::array({350.0, 400.0, 50.0});
    Plan sized = plan(into(g, "Pos.position"), "Output", "baseColor", 8);
    REQUIRE(sized.error.empty());
    o.externals["position"] = {1, 1, {0.5f, 0.25f, 1.0f, 1.0f}};
    image::Image cm = evaluateCpu(sized, o).outputs.at("out");
    CHECK(texel(cm, 4, 4)[0] == doctest::Approx(300.0));
    CHECK(texel(cm, 4, 4)[1] == doctest::Approx(100.0));
    CHECK(texel(cm, 4, 4)[2] == doctest::Approx(50.0));
}

TEST_CASE("tex: an unlinked UV input reads the coordinate, Mapping moves it")
{
    Graph uvOnly;
    add(uvOnly, "U", "uv");
    image::Image expected = eval(into(uvOnly, "U.uv"), "Output", "baseColor", 16);

    Graph g;
    add(g, "M", "mapping");
    image::Image same = eval(into(g, "M.out"), "Output", "baseColor", 16);
    CHECK(maxDiff(same, expected) < 1e-5);

    g.node("M")->values["scaleX"] = 2.0;
    g.node("M")->values["offsetY"] = 0.25;
    image::Image moved = eval(into(g, "M.out"), "Output", "baseColor", 16);
    for (int y : {1, 7, 12})
        for (int x : {2, 9, 14})
        {
            // Rows run top down: row 0 is v 1.
            const float u = (float(x) + 0.5f) / 16.0f, v = 1.0f - (float(y) + 0.5f) / 16.0f;
            CHECK(texel(moved, x, y)[0] == doctest::Approx((u - 0.5f) * 2.0f + 0.5f).epsilon(1e-4));
            CHECK(texel(moved, x, y)[1] == doctest::Approx(v + 0.25f).epsilon(1e-4));
        }
}

/** Projection's spherical coordinate for an object-space point, as the node computes it with axis y. */
static glm::vec2 sphericalUv(glm::vec3 q)
{
    const float u = std::atan2(q.z, -q.x) * 0.15915494f + 0.5f;
    return {u, std::asin(std::clamp(q.y / glm::length(q), -1.0f, 1.0f)) * 0.31830989f + 0.5f};
}

TEST_CASE("tex: spherical Projection lines up with a sphere's own uvs")
{
    geo::SphereOptions so;
    so.radius = 50.0;
    so.subdivAxis = 24;
    so.subdivHeight = 12;
    const geo::Mesh m = geo::makeSphere(so);
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    int checked = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::vec3 p(m.position(m.cornerPoint(c)));
            const glm::vec2 s = sphericalUv(p);
            // Away from the poles, where u is undefined, and the seam, where it wraps.
            if (std::abs(s.y - 0.5f) > 0.4f || uv[c].x < 0.02f || uv[c].x > 0.98f)
                continue;
            CAPTURE(f);
            CHECK(s.x == doctest::Approx(uv[c].x).epsilon(0.002));
            CHECK(s.y == doctest::Approx(uv[c].y).epsilon(0.002));
            ++checked;
        }
    }
    CHECK(checked > 100);

    // The node itself, fed the bake's 0-1 positions and its bounds, gives the same coordinate about the bounds' middle.
    Graph g;
    add(g, "P", "projection", {{"shape", "spherical"}, {"boundsMin", {-50.0, -50.0, -50.0}}, {"boundsMax", {50.0, 50.0, 50.0}}});
    Plan plan_ = plan(into(g, "P.out"), "Output", "baseColor", 4);
    REQUIRE(plan_.error.empty());
    EvalOptions o;
    const glm::vec3 point(30.0f, 20.0f, -10.0f);
    o.externals["position"] = {1, 1, {point.x / 100.0f + 0.5f, point.y / 100.0f + 0.5f, point.z / 100.0f + 0.5f, 1.0f}};
    image::Image out = evaluateCpu(plan_, o).outputs.at("out");
    CHECK(texel(out, 1, 2)[0] == doctest::Approx(sphericalUv(point).x).epsilon(1e-4));
    CHECK(texel(out, 1, 2)[1] == doctest::Approx(sphericalUv(point).y).epsilon(1e-4));
}

TEST_CASE("tex: Image reads through its UV input")
{
    Graph plain;
    add(plain, "I", "image", {{"file", "strokes/strokes_soft.png"}});
    image::Image expected = eval(into(plain, "I.out"), "Output", "baseColor", 32);

    Graph g;
    add(g, "I", "image", {{"file", "strokes/strokes_soft.png"}});
    add(g, "M", "mapping");
    link(g, "M.out", "I.coord");
    CHECK(maxDiff(eval(into(g, "I.out"), "Output", "baseColor", 32), expected) < 1e-5);

    g.node("M")->values["offsetX"] = 0.37;
    CHECK(maxDiff(eval(into(g, "I.out"), "Output", "baseColor", 32), expected) > 0.05);
}

TEST_CASE("tex: Image gives the file's alpha and reads colour as linear or sRGB")
{
    const fs::path file = tempDir("image_alpha") / "half.png";
    const float v = 128.0f / 255.0f, a = 64.0f / 255.0f;
    image::writePng(file, 2, 2, std::vector<float>{v, v, v, a, v, v, v, a, v, v, v, a, v, v, v, a}, 8);

    Graph g;
    add(g, "I", "image", {{"file", file.string()}});
    PlanRequest r;
    r.width = r.height = 4;
    r.outputs = {{"color", "I", "out", PortType::Color, {}}, {"alpha", "I", "alpha", PortType::Gray, {}}};
    EvalResult linear = evaluateCpu(buildPlan(g, r));
    CHECK(texel(linear.outputs.at("color"), 1, 1)[0] == doctest::Approx(v).epsilon(1e-3));
    CHECK(texel(linear.outputs.at("color"), 1, 1)[3] == doctest::Approx(a).epsilon(1e-3));
    CHECK(texel(linear.outputs.at("alpha"), 1, 1)[0] == doctest::Approx(a).epsilon(1e-3));

    g.node("I")->values["colorSpace"] = "srgb";
    EvalResult srgb = evaluateCpu(buildPlan(g, r));
    CHECK(texel(srgb.outputs.at("color"), 1, 1)[0] == doctest::Approx(std::pow((v + 0.055) / 1.055, 2.4)).epsilon(1e-3));
    CHECK(texel(srgb.outputs.at("color"), 1, 1)[3] == doctest::Approx(a).epsilon(1e-3));
    CHECK(texel(srgb.outputs.at("alpha"), 1, 1)[0] == doctest::Approx(a).epsilon(1e-3));

    SUBCASE("no file reads alpha 1")
    {
        Graph none;
        add(none, "I", "image");
        PlanRequest one;
        one.width = one.height = 4;
        one.outputs = {{"alpha", "I", "alpha", PortType::Gray, {}}};
        CHECK(texel(evaluateCpu(buildPlan(none, one)).outputs.at("alpha"), 0, 0)[0] == doctest::Approx(1.0));
    }
}

TEST_CASE("tex eval: an Image file larger than the plan is read at the plan's size")
{
    image::Image big{64, 32, std::vector<float>(64 * 32 * 4, 0.25f)};
    std::string warning;
    image::Image fit = tex::fitSourceImage(big, "big.png", 16, 16, 0, &warning);
    CHECK(fit.width == 16);
    CHECK(fit.height == 16);
    CHECK(warning.empty());
    CHECK(fit.rgba[5] == doctest::Approx(0.25f));

    fit = tex::fitSourceImage(big, "big.png", 64, 64, 128, &warning);
    CHECK(size_t(fit.width) * size_t(fit.height) <= 128);
    CHECK(fit.width == 2 * fit.height);
    CHECK(warning.find("UDIM") != std::string::npos);
}
