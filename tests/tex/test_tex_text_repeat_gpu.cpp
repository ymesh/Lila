#include "TexTest.h"

#include "render/Renderer.h"
#include "render/TexEval.h"

using namespace textest;

namespace
{
bool textGpu()
{
    lila::render::Renderer& r = lila::render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for text and repeat GPU tests");
    return false;
}

void matchesCpu(const Graph& g, const std::string& node, const std::string& socket, PortType type)
{
    const Plan p = plan(g, node, socket, 96, type);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    const EvalResult cpu = evaluateCpu(p);
    const EvalResult gpu = lila::render::texgpu::evaluate(p);
    CHECK(maxDiff(cpu.outputs.at("out"), gpu.outputs.at("out")) < 4e-3);
    CHECK(meanDiff(cpu.outputs.at("out"), gpu.outputs.at("out")) < 1e-4);
}
} // namespace

TEST_CASE("text gpu: the Text node matches the CPU evaluator")
{
    if (!textGpu())
        return;
    Graph g;
    add(g, "T", "text", {{"text", "Lila\ntext"}, {"size", 0.3}, {"rotation", 20.0}, {"positionX", 0.9}, {"alignment", "left"}});
    matchesCpu(g, "T", "mask", PortType::Gray);
    matchesCpu(g, "T", "out", PortType::Color);
}

TEST_CASE("repeat gpu: an iterated warp and blur match the CPU evaluator")
{
    if (!textGpu())
        return;
    Graph g;
    add(g, "B", "bricks");
    add(g, "W", "perlin", {{"scale", 3}});
    add(g, "In", kRepeatInputType, {{"output", "Out"}, {"iterations", 6}});
    add(g, "Out", kRepeatOutputType);
    add(g, "Warp", "warp", {{"intensity", 0.5}});
    add(g, "Blur", "blur", {{"radius", 0.01}});
    textest::link(g, "B.mask", "In.color");
    textest::link(g, "In.color", "Warp.input");
    textest::link(g, "W.out", "Warp.warp");
    textest::link(g, "Warp.out", "Blur.input");
    textest::link(g, "Blur.out", "Out.color");
    matchesCpu(g, "Out", "color", PortType::Color);
}
