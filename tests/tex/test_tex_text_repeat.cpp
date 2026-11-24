#include "TexTest.h"

#include "tex/Expressions.h"
#include "tex/NodeDef.h"
#include "tex/Sequence.h"
#include "tex/TextRaster.h"

using namespace textest;

namespace
{
struct Box
{
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    double sum = 0.0;
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
};

/** Where a mask covers more than half a texel, and its total. */
Box coverage(const image::Image& img)
{
    Box b;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
        {
            const float v = texel(img, x, y)[0];
            b.sum += v;
            if (v > 0.5f)
            {
                b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x), b.y1 = std::max(b.y1, y);
            }
        }
    return b;
}

image::Image textMask(json values, int res = 128)
{
    Graph g;
    add(g, "T", "text", std::move(values));
    return eval(g, "T", "mask", res, PortType::Gray);
}
} // namespace

TEST_CASE("text: draws its words into a mask, sized and placed in uv")
{
    REQUIRE(findNode("text"));
    const Box b = coverage(textMask({{"text", "HHHH"}, {"size", 0.25}}));
    REQUIRE(b.x1 >= 0);
    // Centred on (0.5, 0.5), wider than tall.
    CHECK(std::abs((b.x0 + b.x1) * 0.5 - 63.5) < 3.0);
    CHECK(std::abs((b.y0 + b.y1) * 0.5 - 63.5) < 3.0);
    CHECK(b.width() > 2 * b.height());
    // Cap height of Inter is about 0.73 em: 0.25 * 128 * 0.73.
    CHECK(b.height() > 20);
    CHECK(b.height() < 27);

    const Box big = coverage(textMask({{"text", "HHHH"}, {"size", 0.4}}));
    CHECK(big.sum > b.sum * 2.0);

    const Box empty = coverage(textMask({{"text", ""}}));
    CHECK(empty.sum == 0.0);
}

TEST_CASE("text: alignment, rotation, line spacing and position")
{
    const Box left = coverage(textMask({{"text", "HHH"}, {"alignment", "left"}, {"positionX", 0.25}}));
    CHECK(std::abs(left.x0 - 32) <= 3);
    const Box right = coverage(textMask({{"text", "HHH"}, {"alignment", "right"}, {"positionX", 0.75}}));
    CHECK(std::abs(right.x1 - 95) <= 3);

    const Box flat = coverage(textMask({{"text", "HHHH"}, {"size", 0.2}}));
    const Box turned = coverage(textMask({{"text", "HHHH"}, {"size", 0.2}, {"rotation", 90.0}}));
    CHECK(std::abs(turned.width() - flat.height()) <= 2);
    CHECK(std::abs(turned.height() - flat.width()) <= 2);

    const Box one = coverage(textMask({{"text", "H\nH"}, {"size", 0.15}, {"lineSpacing", 1.0}}));
    const Box two = coverage(textMask({{"text", "H\nH"}, {"size", 0.15}, {"lineSpacing", 2.0}}));
    CHECK(two.height() > one.height() + 10);

    const Box low = coverage(textMask({{"text", "H"}, {"positionY", 0.25}}));
    CHECK((low.y0 + low.y1) * 0.5 > 90.0);
}

TEST_CASE("text: tile wraps letters across the edges; off, they are cut")
{
    auto leftColumns = [](const image::Image& m) {
        double s = 0.0;
        for (int y = 0; y < m.height; ++y)
            for (int x = 0; x < 8; ++x)
                s += texel(m, x, y)[0];
        return s;
    };
    CHECK(leftColumns(textMask({{"text", "HHHH"}, {"positionX", 1.0}, {"tile", true}})) > 10.0);
    CHECK(leftColumns(textMask({{"text", "HHHH"}, {"positionX", 1.0}, {"tile", false}})) == 0.0);
}

TEST_CASE("text: colour output mixes background into colour by the mask; fonts differ")
{
    Graph g;
    add(g, "T", "text", {{"text", "Lila"}, {"color", json::array({1.0, 0.5, 0.0, 1.0})}, {"background", json::array({0.0, 0.0, 0.25, 1.0})}});
    const image::Image mask = eval(g, "T", "mask", 64, PortType::Gray);
    const image::Image col = eval(g, "T", "out", 64);
    for (int i = 0; i < 64 * 64; i += 7)
    {
        const float m = mask.rgba[size_t(i) * 4];
        CHECK(col.rgba[size_t(i) * 4 + 0] == doctest::Approx(m).epsilon(1e-4));
        CHECK(col.rgba[size_t(i) * 4 + 1] == doctest::Approx(0.5f * m).epsilon(1e-4));
        CHECK(col.rgba[size_t(i) * 4 + 2] == doctest::Approx(0.25f * (1.0f - m)).epsilon(1e-4));
    }
    const image::Image mono = textMask({{"text", "Lila"}, {"font", "JetBrains Mono"}}, 64);
    CHECK(meanDiff(mono, mask) > 0.01);
}

TEST_CASE("text: a word edit redraws the image but keeps the plan's structure")
{
    Graph a, b;
    add(a, "T", "text", {{"text", "one"}});
    add(b, "T", "text", {{"text", "two"}, {"size", 0.3}, {"rotation", 15.0}});
    const Plan pa = plan(a, "T", "mask", 64, PortType::Gray), pb = plan(b, "T", "mask", 64, PortType::Gray);
    REQUIRE(pa.error.empty());
    CHECK(pa.structureHash == pb.structureHash);
    const auto it = std::find_if(pa.buffers.begin(), pa.buffers.end(), [](const BufferDesc& d) { return d.kind == BufferKind::Image; });
    REQUIRE(it != pa.buffers.end());
    CHECK(generatedSource(it->name));
    CHECK(textSourceImage(it->name)->width == 64);
}

namespace
{
/** A repeat zone In -> Out with the gray lane starting at start, running passes times. */
Graph zone(int passes, double start = 0.0)
{
    Graph g;
    add(g, "In", kRepeatInputType, {{"output", "Out"}, {"iterations", passes}, {"gray", start}});
    add(g, "Out", kRepeatOutputType);
    return g;
}

float centreTexel(const image::Image& img) { return texel(img, img.width / 2, img.height / 2)[0]; }
} // namespace

TEST_CASE("repeat: the body runs Iterations times, each pass reading the last")
{
    Graph g = zone(5);
    add(g, "Add", "math", {{"operation", "add"}, {"b", 0.1}});
    textest::link(g, "In.gray", "Add.a");
    textest::link(g, "Add.out", "Out.gray");
    CHECK(centreTexel(eval(g, "Out", "gray", 8, PortType::Gray)) == doctest::Approx(0.5f).epsilon(1e-4));

    g.node("In")->values["iterations"] = 0;
    CHECK(centreTexel(eval(g, "Out", "gray", 8, PortType::Gray)) == doctest::Approx(0.0f));

    // The iteration index: 0 + 1 + 2 + 3.
    Graph h = zone(4);
    add(h, "Add", "math", {{"operation", "add"}});
    textest::link(h, "In.gray", "Add.a");
    textest::link(h, "In.iteration", "Add.b");
    textest::link(h, "Add.out", "Out.gray");
    CHECK(centreTexel(eval(h, "Out", "gray", 8, PortType::Gray)) == doctest::Approx(6.0f));
}

TEST_CASE("repeat: an unlinked lane carries through; nodes outside the body run once")
{
    Graph g = zone(3);
    g.node("In")->values["color"] = json::array({0.25, 0.5, 0.75, 1.0});
    add(g, "P", "perlin", {{"scale", 3}});
    add(g, "Add", "math", {{"operation", "add"}});
    textest::link(g, "In.gray", "Add.a");
    textest::link(g, "P.out", "Add.b");
    textest::link(g, "Add.out", "Out.gray");
    const image::Image col = eval(g, "Out", "color", 8);
    CHECK(texel(col, 3, 3)[0] == doctest::Approx(0.25f));
    CHECK(texel(col, 3, 3)[2] == doctest::Approx(0.75f));

    const Plan p = plan(g, "Out", "gray", 16, PortType::Gray);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    int noise = 0, adds = 0;
    for (const UniformEntry& u : p.params)
    {
        noise += u.node == "P" && u.id == "scale";
        adds += u.node.starts_with("Add#") && u.id == "operation";
    }
    CHECK(noise == 1);
    CHECK(adds == 3);

    Graph once;
    add(once, "P", "perlin", {{"scale", 3}});
    const image::Image n = eval(once, "P", "out", 16, PortType::Gray);
    const image::Image r = evaluateCpu(p).outputs.at("out");
    CHECK(texel(r, 5, 7)[0] == doctest::Approx(3.0f * texel(n, 5, 7)[0]).epsilon(1e-3));
}

TEST_CASE("repeat: an iterated transform equals one transform of the summed offset")
{
    Graph g = zone(3);
    add(g, "B", "bricks");
    add(g, "T", "transform", {{"offsetX", 0.1}});
    textest::link(g, "B.mask", "In.color");
    textest::link(g, "In.color", "T.input");
    textest::link(g, "T.out", "Out.color");
    Graph one;
    add(one, "B", "bricks");
    add(one, "T", "transform", {{"offsetX", 0.3}});
    textest::link(one, "B.mask", "T.input");
    CHECK(maxDiff(eval(g, "Out", "color", 64), eval(one, "T", "out", 64)) < 1e-3);
}

TEST_CASE("repeat: zones nest, and passes stop at the limit")
{
    Graph g = zone(3);
    add(g, "In2", kRepeatInputType, {{"output", "Out2"}, {"iterations", 2}});
    add(g, "Out2", kRepeatOutputType);
    add(g, "Add", "math", {{"operation", "add"}, {"b", 1.0}});
    textest::link(g, "In.gray", "In2.gray");
    textest::link(g, "In2.gray", "Add.a");
    textest::link(g, "Add.out", "Out2.gray");
    textest::link(g, "Out2.gray", "Out.gray");
    CHECK(centreTexel(eval(g, "Out", "gray", 8, PortType::Gray)) == doctest::Approx(6.0f));

    Graph big = zone(500);
    add(big, "Add", "math", {{"operation", "add"}, {"b", 1.0}});
    textest::link(big, "In.gray", "Add.a");
    textest::link(big, "Add.out", "Out.gray");
    const Plan p = plan(big, "Out", "gray", 8, PortType::Gray);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    CHECK_FALSE(p.warnings.empty());
    CHECK(centreTexel(evaluateCpu(p).outputs.at("out")) == doctest::Approx(float(kMaxRepeatIterations)));

    Graph orphan;
    add(orphan, "Out", kRepeatOutputType);
    CHECK_FALSE(plan(orphan, "Out", "gray", 8, PortType::Gray).error.empty());
}

TEST_CASE("repeat: texAddNode adds the pair, and the graph info lists the zone")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Loop"}}, {"resolution", 16}});
    const std::string in = run("texAddNode", {{"args", {"Loop", kRepeatInputType}}}).get<std::string>();
    run("texAddNode", {{"args", {"Loop", "math"}}, {"name", "Add"}});
    run("texConnect", {{"args", {"Loop", in + ".gray", "Add.a"}}});
    const json info0 = run("texGraphInfo", {{"args", {"Loop"}}});
    REQUIRE(info0["zones"].size() == 1);
    const std::string out = info0["zones"][0]["output"].get<std::string>();
    run("texConnect", {{"args", {"Loop", "Add.out", out + ".gray"}}});
    CHECK(run("texGraphInfo", {{"args", {"Loop"}}})["zones"][0]["nodes"] == json::array({"Add"}));
    run("texDuplicateNodes", {{"args", {"Loop", in, "Add", out}}});
    CHECK(run("texGraphInfo", {{"args", {"Loop"}}})["zones"].size() == 2);
}

namespace
{
/** N = a + b, with b's expression. */
Graph sum(const std::string& expression, double a = 0.25)
{
    Graph g;
    add(g, "Src", "math", {{"b", 0.3}});
    add(g, "N", "math", {{"operation", "add"}, {"a", a}, {"b", 0.0}, {kExpressionsKey, {{"b", expression}}}});
    return g;
}
} // namespace

TEST_CASE("expressions: read other nodes, the graph's inputs and time, as values only")
{
    CHECK(centreTexel(eval(sum("Src.b * 2"), "N", "out", 8, PortType::Gray)) == doctest::Approx(0.85f));
    CHECK(centreTexel(eval(sum("= clamp(Src.b * 10, 0, 1) + N.a"), "N", "out", 8, PortType::Gray)) == doctest::Approx(1.5f));
    CHECK(centreTexel(eval(sum("0.5 if Src.b > 0.2 else 0.1"), "N", "out", 8, PortType::Gray)) == doctest::Approx(0.75f));

    Graph in = sum("input.wear * 2");
    GraphInput wear;
    wear.name = "wear";
    wear.param = true;
    wear.defaultValue = 0.125;
    in.inputs.push_back(wear);
    CHECK(centreTexel(eval(in, "N", "out", 8, PortType::Gray)) == doctest::Approx(0.5f));

    Graph t = sum("time * 0.25");
    CHECK(sequence::animated(t));
    CHECK_FALSE(sequence::animated(sum("Src.b")));
    t.time = 2.0;
    CHECK(centreTexel(eval(t, "N", "out", 8, PortType::Gray)) == doctest::Approx(0.75f));
    Graph later = t;
    later.time = 3.0;
    CHECK(plan(t, "N", "out", 8, PortType::Gray).structureHash == plan(later, "N", "out", 8, PortType::Gray).structureHash);
    CHECK(centreTexel(eval(later, "N", "out", 8, PortType::Gray)) == doctest::Approx(1.0f));
}

TEST_CASE("expressions: a broken one warns and keeps the stored value; int params round")
{
    for (const char* bad : {"Missing.b + 1", "nosuch(1)", "N.b + 1", "1 +"})
    {
        CAPTURE(bad);
        const Plan p = plan(sum(bad), "N", "out", 8, PortType::Gray);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        CHECK_FALSE(p.warnings.empty());
        CHECK(centreTexel(evaluateCpu(p).outputs.at("out")) == doctest::Approx(0.25f));
    }

    Graph g = zone(1);
    g.node("In")->values[kExpressionsKey] = {{"iterations", "1.6 + 1"}};
    add(g, "Add", "math", {{"operation", "add"}, {"b", 1.0}});
    textest::link(g, "In.gray", "Add.a");
    textest::link(g, "Add.out", "Out.gray");
    CHECK(centreTexel(eval(g, "Out", "gray", 8, PortType::Gray)) == doctest::Approx(3.0f));
}

TEST_CASE("expressions: texSetExpression sets, shows and removes one")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Expr"}}, {"resolution", 16}});
    run("texAddNode", {{"args", {"Expr", "transform"}}, {"name", "A"}});
    run("texAddNode", {{"args", {"Expr", "transform"}}, {"name", "B"}});
    run("texSetValue", {{"args", {"Expr", "A.offsetX", 0.2}}});
    CHECK(run("texSetExpression", {{"args", {"Expr", "B.offsetX"}}, {"string", "A.offsetX + 0.05"}}).get<double>() == doctest::Approx(0.25));
    auto param = [](const std::string& node, const std::string& id) {
        const json info = run("texGraphInfo", {{"args", {"Expr"}}});
        for (const json& n : info["nodes"])
            if (n["name"] == node)
                for (const json& p : n["params"])
                    if (p["id"] == id)
                        return p;
        return json();
    };
    CHECK(param("B", "offsetX")["expression"] == "A.offsetX + 0.05");
    run("texSetValue", {{"args", {"Expr", "A.offsetX", 0.5}}});
    CHECK(param("B", "offsetX")["value"].get<double>() == doctest::Approx(0.55));

    CHECK_THROWS(run("texSetExpression", {{"args", {"Expr", "B.offsetY"}}, {"string", "Nope.x"}}));
    CHECK_THROWS(run("texSetExpression", {{"args", {"Expr", "B.input"}}, {"string", "1"}}));
    CHECK_FALSE(param("B", "offsetY").contains("expression"));

    run("texSetExpression", {{"args", {"Expr", "B.offsetX"}}, {"remove", true}});
    CHECK_FALSE(param("B", "offsetX").contains("expression"));
    CHECK(param("B", "offsetX")["value"].get<double>() == doctest::Approx(0.55));
}
