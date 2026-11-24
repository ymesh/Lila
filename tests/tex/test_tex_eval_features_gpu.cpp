#include "TexTest.h"

#include "core/Paths.h"
#include "render/Renderer.h"
#include "render/TexEval.h"

#include <filesystem>

using namespace textest;
using namespace lila::render;

namespace
{
namespace fs = std::filesystem;

bool haveGpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for texture graph GPU tests");
    return false;
}

/** Noise texels, alpha a ramp. */
image::Image noiseImage(int w, int h)
{
    image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
    for (size_t i = 0; i < img.rgba.size(); ++i)
        img.rgba[i] = i % 4 == 3 ? 0.5f + 0.5f * float(i / 4 % size_t(w)) / float(w) : float((i * 2654435761u) % 1000u) / 999.0f;
    return img;
}

void matches(const Plan& p, double tol = 5e-3, const EvalOptions& options = {})
{
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    EvalResult cpu = evaluateCpu(p, options);
    EvalResult gpu = texgpu::evaluate(p, options);
    for (const auto& [name, img] : cpu.outputs)
    {
        CAPTURE(name);
        REQUIRE(gpu.outputs.contains(name));
        CHECK(maxDiff(img, gpu.outputs.at(name)) < tol);
    }
}
} // namespace

TEST_CASE("tex eval features gpu: nodes at their own size and bit depth match the CPU")
{
    if (!haveGpu())
        return;
    for (const char* size : {"quarter", "half", "set", "double"})
        for (const char* bits : {"set", "8", "32"})
        {
            CAPTURE(size);
            CAPTURE(bits);
            Graph g;
            add(g, "P", "perlin", {{"scale", 3}, {"seed", 4}, {"outputSize", size}, {"bitDepth", bits}});
            add(g, "B", "blur", {{"radius", 0.03}, {"outputSize", size}});
            add(g, "L", "levels");
            link(g, "P.out", "B.input");
            link(g, "B.out", "L.input");
            PlanRequest r;
            r.width = r.height = 48;
            r.outputs = {{"out", "L", "out", PortType::Color, {}}, {"p", "P", "out", PortType::Color, {}}};
            matches(buildPlan(g, r));
        }
}

TEST_CASE("tex eval features gpu: UDIM image reads match the CPU per tile")
{
    if (!haveGpu())
        return;
    const fs::path dir = tempDir("eval_features_gpu_udim");
    image::write(dir / "n_1001.exr", noiseImage(16, 16), 32);
    image::write(dir / "n_1002.exr", noiseImage(24, 24), 32);
    Graph g;
    add(g, "I", "image", {{"file", paths::toUtf8(dir / "n_<UDIM>.exr")}, {"tiles", 2}});
    PlanRequest r;
    r.width = r.height = 16;
    r.outputs = {{"out", "I", "out", PortType::Color, {}}};
    const Plan p = buildPlan(g, r);
    for (float u : {0.0f, 1.0f})
    {
        CAPTURE(u);
        EvalOptions o;
        o.uvOffset[0] = u;
        matches(p, 5e-3, o);
    }
    EvalOptions second;
    second.uvOffset[0] = 1.0f;
    CHECK(maxDiff(evaluateCpu(p).outputs.at("out"), evaluateCpu(p, second).outputs.at("out")) > 0.05);
}

TEST_CASE("tex eval features gpu: tiled and premultiplied image reads match the CPU")
{
    if (!haveGpu())
        return;
    const fs::path dir = tempDir("eval_features_gpu_mips");
    const std::string file = paths::toUtf8(dir / "noise.exr");
    image::write(file, noiseImage(40, 24), 32);
    for (int tiles : {1, 2, 3, 5})
        for (const char* node : {"image", "imageSequence"})
        {
            CAPTURE(tiles);
            CAPTURE(node);
            Graph g;
            add(g, "I", node, {{"file", file}, {"tiles", tiles}, {"alphaMode", tiles == 3 ? "premultiplied" : "straight"}});
            PlanRequest r;
            r.width = 40;
            r.height = 24;
            r.outputs = {{"out", "I", "out", PortType::Color, {}}, {"alpha", "I", "alpha", PortType::Gray, {}}};
            Plan p = buildPlan(g, r);
            matches(p);
        }
}
