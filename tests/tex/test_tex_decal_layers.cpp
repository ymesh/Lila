#include "TexTest.h"

#include "core/ImageIO.h"
#include "tex/Layers.h"
#include "tex/TextImage.h"

#include <filesystem>

using namespace textest;

namespace
{
std::string writePicture(const std::string& name, int w, int h, const std::function<std::array<float, 4>(int, int)>& fn)
{
    std::vector<float> rgba;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (float c : fn(x, y))
                rgba.push_back(c);
    const std::filesystem::path file = tempDir("decal_layers") / (name + ".exr");
    image::writeExr(file, w, h, rgba, 16);
    return file.generic_string();
}

image::Image evalChannel(const Graph& g, const std::string& channel, PortType type)
{
    PlanRequest r;
    r.width = r.height = 64;
    r.outputs = materialRequests(g, {{channel, type}});
    Plan p = buildPlan(g, r);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    return evaluateCpu(p).outputs.at(channel);
}

Graph compiled(const tex::Layer& l)
{
    tex::LayerStack st;
    st.layers.push_back(l);
    const tex::StackCompile c = tex::compileStack(st, tex::SetSettings::makeDefault(), Graph::makeMaterial("S"));
    return c.graph;
}
} // namespace

TEST_CASE("tex decal layers: decal, text and scatter layers round-trip through JSON and the compiled graph")
{
    const std::string red = writePicture("red", 8, 8, [](int, int) { return std::array{1.0f, 0.0f, 0.0f, 1.0f}; });
    for (const char* kind : {"decal", "text", "scatter"})
    {
        INFO(kind);
        tex::Layer l;
        l.id = "L1";
        REQUIRE(tex::parseLayerKind(kind, &l.kind));
        l.material = {{"roughness", 0.2}};
        if (l.kind == tex::LayerKind::Decal)
            l.decal = {{"maps", {{"baseColor", red}}}, {"columns", 2}, {"rows", 2}, {"cell", 3}};
        if (l.kind == tex::LayerKind::Text)
            l.text = {{"text", "Lila"}, {"outline", 0.05}};
        if (l.kind == tex::LayerKind::Scatter)
            l.scatter = {{"pattern", red}, {"maps", {{"baseColor", red}}}, {"seed", 7}, {"countX", 4}};
        const tex::Layer back = tex::Layer::fromJson(l.toJson());
        CHECK(back.toJson() == l.toJson());

        // The stack read back from its own compiled graph is the same stack.
        tex::LayerStack st;
        st.layers.push_back(l);
        const tex::SetSettings s = tex::SetSettings::makeDefault();
        const tex::StackCompile c = tex::compileStack(st, s, Graph::makeMaterial("S"));
        for (const std::string& w : c.warnings)
            MESSAGE(w);
        const tex::StackState read = tex::readStack(c.graph, s);
        CHECK_FALSE(read.graphMode);
        REQUIRE(read.stack.layers.size() == 1);
        CHECK(read.stack.layers[0].toJson() == l.toJson());
    }
}

TEST_CASE("tex decal layers: changing a text layer's words changes what it paints")
{
    tex::Layer l;
    l.id = "L1";
    l.kind = tex::LayerKind::Text;
    l.text = {{"text", "AB"}};
    l.projection = {{"mode", "planar"}, {"sizeX", 1.0}, {"sizeY", 1.0}, {"positionX", 0.5}, {"positionY", 0.5}};
    const image::Image a = evalChannel(compiled(l), "baseColor", PortType::Color);
    l.text["text"] = "XY";
    const image::Image b = evalChannel(compiled(l), "baseColor", PortType::Color);
    CHECK(maxDiff(a, b) > 0.1);

    const tex::TextImage t = tex::renderText({.text = "Lila", .outline = 0.05});
    REQUIRE(t.error.empty());
    CHECK(std::filesystem::exists(t.file));
    CHECK(t.aspect > 1.0);
}

TEST_CASE("tex decal layers: a scatter with the same seed paints the same pixels, another seed others")
{
    const std::string dot = writePicture("dot", 16, 16, [](int x, int y) {
        const float d = std::hypot(x - 7.5f, y - 7.5f) < 6.0f ? 1.0f : 0.0f;
        return std::array{d, d, d, 1.0f};
    });
    tex::Layer l;
    l.id = "L1";
    l.kind = tex::LayerKind::Scatter;
    l.material = {{"baseColor", {0.1, 0.8, 0.2}}};
    l.scatter = {{"pattern", dot}, {"seed", 3}, {"countX", 5}, {"countY", 5}, {"size", 0.5}};
    const image::Image a = evalChannel(compiled(l), "baseColor", PortType::Color);
    const image::Image b = evalChannel(compiled(l), "baseColor", PortType::Color);
    CHECK(maxDiff(a, b) == 0.0);
    l.scatter["seed"] = 4;
    const image::Image c = evalChannel(compiled(l), "baseColor", PortType::Color);
    CHECK(maxDiff(a, c) > 0.1);
}
