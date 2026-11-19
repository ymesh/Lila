#include "TexTest.h"

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "render/Compute.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "render/TexEval.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "tex/NodeDef.h"
#include "tex/TextureSet.h"
#include "ui/MaterialDraw.h"
#include "ui/TexLive.h"

#include <array>
#include <functional>

using namespace textest;
using namespace lila::render;

namespace
{
bool graphGpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for texture graph GPU tests");
    return false;
}

/** Runs an evaluator to completion on the render thread, slices of budgetMs. */
void runAll(texgpu::Evaluator& ev, double budgetMs = 40.0)
{
    Renderer::get().runTask([&ev, budgetMs](VulkanContext& vk) { return ev.step(vk, budgetMs); }).get();
}

/** GPU and CPU results of the same plan. */
void compare(const Plan& p, double maxTol, double meanTol)
{
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    EvalResult cpu = evaluateCpu(p);
    EvalResult gpu = texgpu::evaluate(p);
    for (const auto& [name, img] : cpu.outputs)
    {
        CAPTURE(name);
        REQUIRE(gpu.outputs.contains(name));
        const image::Image& g = gpu.outputs.at(name);
        CHECK(maxDiff(img, g) < maxTol);
        CHECK(meanDiff(img, g) < meanTol);
    }
}

/** A flat-lit quad over the whole target, uv 0 to 1, with one material. */
FrameState quad(const Material& mat)
{
    auto mesh = std::make_shared<RenderMesh>();
    const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const glm::vec2& c : corners)
    {
        MeshVertex v{};
        v.pos = glm::vec3(c, 0.5f);
        v.normal = {0, 0, -1};
        v.tangent = {1, 0, 0, 1};
        v.uv = {c.x * 0.5f + 0.5f, c.y * 0.5f + 0.5f};
        mesh->vertices.push_back(v);
    }
    mesh->indices = {0, 1, 2, 0, 2, 3};
    mesh->boundsMin = {-1, -1, 0.5f};
    mesh->boundsMax = {1, 1, 0.5f};
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.lighting = FrameState::Lighting::Flat;
    MeshDraw d;
    d.mesh = mesh;
    d.materials = {mat};
    s.meshes.push_back(d);
    return s;
}

std::array<int, 4> centrePixel(Material m)
{
    // The quad is back-facing to the offscreen camera.
    m.flags |= MatTwoSided;
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(quad(m), 32, 32, bgra, &err), err);
    const uint8_t* p = &bgra[(16 * 32 + 16) * 4];
    return {p[0], p[1], p[2], p[3]};
}
} // namespace

TEST_CASE("tex gpu: golden cases match the CPU evaluator")
{
    if (!graphGpu())
        return;
    auto single = [](std::string type, json values, std::string out) {
        Graph g;
        add(g, "N", type, values);
        return into(g, "N." + out);
    };
    auto noiseInto = [](std::string type, json values, std::string in) {
        Graph g;
        add(g, "Src", "perlin", {{"scale", 4}, {"seed", 7}});
        add(g, "N", type, values);
        link(g, "Src.out", "N." + in);
        return into(g, "N.out");
    };
    std::vector<std::pair<std::string, Graph>> cases = {
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
        {"levels", noiseInto("levels", {{"inLow", 0.3}, {"inHigh", 0.7}, {"gamma", 2.0}}, "input")},
        {"math_power", noiseInto("math", {{"operation", "power"}, {"b", 3.0}}, "a")},
        {"colorize", noiseInto("colorize", {{"gradient", {{"stops", json::array({json::array({0.2, "#203040"}), json::array({0.8, "#e0c080"})})}}}}, "input")},
        {"blur", noiseInto("blur", {{"radius", 0.03}}, "input")},
    };
    {
        Graph g;
        add(g, "B", "bricks");
        add(g, "N", "transform", {{"rotation", 30.0}, {"scale", 0.7}, {"offsetX", 0.1}});
        link(g, "B.mask", "N.input");
        cases.emplace_back("transform", into(g, "N.out"));
    }
    {
        Graph g;
        add(g, "B", "bricks");
        add(g, "W", "perlin", {{"scale", 3}});
        add(g, "N", "warp", {{"intensity", 2.0}});
        link(g, "B.mask", "N.input");
        link(g, "W.out", "N.warp");
        cases.emplace_back("warp", into(g, "N.out"));
    }
    {
        Graph g;
        add(g, "V", "voronoi");
        add(g, "P", "perlin");
        add(g, "N", "blend", {{"mode", "overlay"}, {"opacity", 0.8}});
        link(g, "V.edge", "N.foreground");
        link(g, "P.out", "N.background");
        cases.emplace_back("blend_overlay", into(g, "N.out"));
    }
    {
        Graph g;
        add(g, "Src", "perlin", {{"scale", 4}, {"seed", 7}});
        add(g, "N", "heightToNormal", {{"intensity", 2.0}});
        link(g, "Src.out", "N.height");
        cases.emplace_back("heightToNormal", into(g, "N.normal"));
    }
    {
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
        cases.emplace_back("normalBlend", into(g, "N.out"));
    }
    for (const auto& [name, g] : cases)
    {
        CAPTURE(name);
        compare(plan(g, "Output", "baseColor", 64), 2e-3, 1e-4);
    }
}

TEST_CASE("tex gpu: every library node matches the CPU evaluator")
{
    if (!graphGpu())
        return;
    size_t checked = 0;
    for (const NodeDef* d : allNodes())
    {
        if (d->kind != NodeKind::Slang && d->kind != NodeKind::Native && d->kind != NodeKind::Graph)
            continue;
        for (const PortDecl& o : d->outputs)
        {
            CAPTURE(d->type);
            CAPTURE(o.id);
            Graph g;
            // Flood fill's GPU sweeps are exact only on regions they converge on (Natives.h): give it bricks.
            if (d->type == "floodFill")
                add(g, "Src", "bricks");
            else
                add(g, "Src", "perlin", {{"scale", 3}, {"seed", 5}});
            add(g, "N", d->type);
            for (const PortDecl& in : d->inputs)
                if (in.type != PortType::Material)
                {
                    link(g, d->type == "floodFill" ? "Src.mask" : "Src.out", "N." + in.id);
                    break;
                }
            PlanRequest r;
            r.width = r.height = 48;
            r.outputs = {{"out", "N", o.id, o.type == PortType::Material ? PortType::Color : o.type, o.type == PortType::Material ? "baseColor" : ""}};
            Plan p = buildPlan(g, r);
            if (!p.error.empty())
            {
                MESSAGE("skipped (plan): " << p.error);
                continue;
            }
            EvalResult cpu;
            try
            {
                cpu = evaluateCpu(p);
            }
            catch (const std::exception& e)
            {
                MESSAGE("skipped (CPU): " << e.what());
                continue;
            }
            EvalResult gpuRes = texgpu::evaluate(p);
            CHECK(maxDiff(cpu.outputs.at("out"), gpuRes.outputs.at("out")) < 5e-3);
            ++checked;
        }
    }
    CHECK(checked > 10);
}

TEST_CASE("tex gpu: value edits upload uniforms, create no pipelines and rerun only what they feed")
{
    if (!graphGpu())
        return;
    // Base colour: perlin -> blur (native, buffered) -> levels. Roughness: voronoi, independent of both.
    Graph g = Graph::makeMaterial("S");
    add(g, "P", "perlin", {{"scale", 4}});
    add(g, "B", "blur", {{"radius", 0.02}});
    add(g, "L", "levels");
    add(g, "V", "voronoi", {{"scale", 6}});
    link(g, "P.out", "B.input");
    link(g, "B.out", "L.input");
    link(g, "L.out", "Output.baseColor");
    link(g, "V.f1", "Output.roughness");
    PlanRequest r;
    r.width = r.height = 128;
    r.outputs = materialRequests(g, {{"baseColor", PortType::Color}, {"roughness", PortType::Gray}});
    auto build = [&] {
        Plan p = buildPlan(g, r);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        return p;
    };
    Plan first = build();
    const size_t total = first.passes.size();
    REQUIRE(total >= 4);

    texgpu::Evaluator ev;
    REQUIRE(ev.setPlan(first).empty());
    runAll(ev);
    CHECK(ev.idle());
    CHECK(ev.stats().passesRun == total);

    // Downstream of the blur: only the levels pass runs again, from the cached blur.
    g.node("L")->values["gamma"] = 2.5;
    Plan edit = build();
    CHECK(edit.structureHash == first.structureHash);
    REQUIRE(ev.setPlan(edit).empty());
    runAll(ev);
    texgpu::Stats s = ev.stats();
    CHECK(s.compiled == 0);
    CHECK(s.pipelinesCreated == 0);
    CHECK(s.passesDirty == 1);
    CHECK(s.passesRun == 1);
    std::map<std::string, image::Image> out = ev.readOutputs();
    EvalResult cpu = evaluateCpu(edit);
    CHECK(maxDiff(out.at("baseColor"), cpu.outputs.at("baseColor")) < 2e-3);
    CHECK(maxDiff(out.at("roughness"), cpu.outputs.at("roughness")) < 2e-3);

    // Upstream of the blur: perlin, blur and levels rerun; the voronoi pass doesn't.
    g.node("P")->values["scale"] = 7;
    edit = build();
    REQUIRE(ev.setPlan(edit).empty());
    runAll(ev);
    s = ev.stats();
    CHECK(s.pipelinesCreated == 0);
    CHECK(s.passesRun == total - 1);
    out = ev.readOutputs();
    cpu = evaluateCpu(edit);
    CHECK(maxDiff(out.at("baseColor"), cpu.outputs.at("baseColor")) < 2e-3);

    // Nothing changed: nothing runs.
    REQUIRE(ev.setPlan(edit).empty());
    runAll(ev);
    CHECK(ev.stats().passesRun == 0);
}

TEST_CASE("tex gpu: a changed structure keeps the buffers it computes the same way")
{
    if (!graphGpu())
        return;
    Graph g = Graph::makeMaterial("S");
    add(g, "P", "perlin", {{"scale", 4}});
    add(g, "B", "blur", {{"radius", 0.02}});
    add(g, "V", "voronoi", {{"scale", 6}});
    link(g, "P.out", "B.input");
    link(g, "B.out", "Output.baseColor");
    link(g, "V.f1", "Output.roughness");
    PlanRequest r;
    r.width = r.height = 128;
    r.outputs = materialRequests(g, {{"baseColor", PortType::Color}, {"roughness", PortType::Gray}});
    Plan first = buildPlan(g, r);
    REQUIRE(first.error.empty());
    texgpu::Evaluator ev;
    REQUIRE(ev.setPlan(first).empty());
    runAll(ev);

    // A new node on top of the blur, as a layer added over the stack: the blur below it is kept, not rerun.
    add(g, "L", "levels", {{"gamma", 2.0}});
    link(g, "B.out", "L.input");
    link(g, "L.out", "Output.baseColor");
    Plan next = buildPlan(g, r);
    REQUIRE(next.error.empty());
    CHECK(next.structureHash != first.structureHash);
    REQUIRE(ev.setPlan(next).empty());
    runAll(ev);
    texgpu::Stats s = ev.stats();
    CHECK(s.passesCarried >= 1);
    CHECK(s.passesRun < next.passes.size());
    std::map<std::string, image::Image> out = ev.readOutputs();
    EvalResult cpu = evaluateCpu(next);
    CHECK(maxDiff(out.at("baseColor"), cpu.outputs.at("baseColor")) < 2e-3);
    CHECK(maxDiff(out.at("roughness"), cpu.outputs.at("roughness")) < 2e-3);
}

TEST_CASE("tex gpu: a trimmed evaluator stays idle, then reruns in full on the next edit")
{
    if (!graphGpu())
        return;
    Graph g = Graph::makeMaterial("S");
    add(g, "P", "perlin", {{"scale", 4}});
    add(g, "B", "blur", {{"radius", 0.02}});
    add(g, "L", "levels");
    link(g, "P.out", "B.input");
    link(g, "B.out", "L.input");
    link(g, "L.out", "Output.baseColor");
    PlanRequest r;
    r.width = r.height = 128;
    r.outputs = materialRequests(g, {{"baseColor", PortType::Color}});
    Plan first = buildPlan(g, r);
    REQUIRE_MESSAGE(first.error.empty(), first.error);
    const size_t total = first.passes.size();

    texgpu::Evaluator ev;
    bool trimmed = false;
    auto trim = [&] { Renderer::get().runTask([&](VulkanContext& vk) { return (trimmed = ev.trim(vk), true); }).get(); };
    trim();
    CHECK_FALSE(trimmed);

    REQUIRE(ev.setPlan(first).empty());
    runAll(ev);
    trim();
    CHECK(trimmed);
    CHECK(ev.idle());
    trim();
    CHECK_FALSE(trimmed);

    // An edit that would rerun one pass reruns them all, since nothing was kept.
    g.node("L")->values["gamma"] = 2.5;
    Plan edit = buildPlan(g, r);
    REQUIRE(ev.setPlan(edit).empty());
    runAll(ev);
    CHECK(ev.stats().passesRun == total);
    EvalResult cpu = evaluateCpu(edit);
    CHECK(maxDiff(ev.readOutputs().at("baseColor"), cpu.outputs.at("baseColor")) < 2e-3);

    // A refresh with no changed externals has nothing to run, trimmed or not.
    trim();
    REQUIRE(trimmed);
    ev.refreshExternals();
    runAll(ev);
    CHECK(ev.idle());
}

TEST_CASE("tex gpu: small slices split passes into row bands with the same result")
{
    if (!graphGpu())
        return;
    Graph g;
    add(g, "V", "voronoi", {{"scale", 9}});
    add(g, "W", "perlin", {{"scale", 3}});
    add(g, "N", "warp", {{"intensity", 1.5}});
    link(g, "V.edge", "N.input");
    link(g, "W.out", "N.warp");
    Plan p = plan(into(g, "N.out"), "Output", "baseColor", 512);
    REQUIRE(p.error.empty());
    texgpu::Evaluator ev;
    REQUIRE(ev.setPlan(p).empty());
    runAll(ev, 0.05);
    const texgpu::Stats s = ev.stats();
    CHECK(s.slices > 1);
    CHECK(s.dispatches > p.passes.size());
    EvalResult cpu = evaluateCpu(p);
    CHECK(maxDiff(ev.readOutputs().at("out"), cpu.outputs.at("out")) < 5e-3);
}

TEST_CASE("tex gpu: outputs land in viewport images in their formats")
{
    if (!graphGpu())
        return;
    Graph g;
    add(g, "P", "perlin", {{"scale", 4}});
    add(g, "Cz", "colorize", {{"gradient", {{"stops", json::array({json::array({0.0, "#102040"}), json::array({1.0, "#f0d080"})})}}}});
    link(g, "P.out", "Cz.input");
    PlanRequest r;
    r.width = r.height = 32;
    r.outputs = {{"color", "Cz", "out", PortType::Color, {}}, {"gray", "P", "out", PortType::Gray, {}}};
    Plan p = buildPlan(g, r);
    REQUIRE(p.error.empty());
    EvalResult cpu = evaluateCpu(p);

    GpuImageRef srgb = GpuImage::create({32, 32, TextureFormat::RGBA8, true, true, "color"});
    GpuImageRef half = GpuImage::create({32, 32, TextureFormat::RGBA16F, false, false, "half"});
    GpuImageRef r8 = GpuImage::create({32, 32, TextureFormat::R8, false, false, "r8"});
    GpuImageRef big = GpuImage::create({64, 64, TextureFormat::R16F, false, false, "upsampled"});
    using S = texgpu::StoreTarget::Source;
    std::vector<texgpu::StoreTarget> stores = {
        {srgb, {S{"color", 0}, S{"color", 1}, S{"color", 2}, S{"color", 3}}},
        {half, {S{"color", 0}, S{"color", 1}, S{"color", 2}, S{{}, 0, 0.25f}}},
        {r8, {S{"gray", 0}, S{}, S{}, S{}}},
        {big, {S{"gray", 0}, S{}, S{}, S{}}},
    };
    texgpu::Evaluator ev;
    REQUIRE(ev.setPlan(p, {}, stores).empty());
    runAll(ev);
    CHECK(ev.stats().stores == 4);

    auto expect = [&](TextureFormat f, bool isSrgb, const image::Image& src, float alpha = -1.0f) {
        std::vector<float> rgba = src.rgba;
        if (alpha >= 0.0f)
            for (size_t i = 3; i < rgba.size(); i += 4)
                rgba[i] = alpha;
        std::vector<uint8_t> bytes;
        encodeTexels(f, isSrgb, rgba, bytes);
        return bytes;
    };
    auto closeBytes = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        REQUIRE(a.size() == b.size());
        int worst = 0;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
        return worst;
    };
    CHECK(closeBytes(readGpuImage(srgb), expect(TextureFormat::RGBA8, true, cpu.outputs.at("color"))) <= 1);
    CHECK(closeBytes(readGpuImage(r8), expect(TextureFormat::R8, false, cpu.outputs.at("gray"))) <= 1);
    std::vector<float> back;
    decodeTexels(TextureFormat::RGBA16F, false, readGpuImage(half), back);
    REQUIRE(back.size() == cpu.outputs.at("color").rgba.size());
    double worst = 0.0;
    for (size_t i = 0; i < back.size(); ++i)
        worst = std::max(worst, std::abs(double(back[i]) - double(i % 4 == 3 ? 0.25f : cpu.outputs.at("color").rgba[i])));
    CHECK(worst < 2e-3);
    // The 32 px result fills a 64 px image: its mean matches.
    std::vector<float> up;
    decodeTexels(TextureFormat::R16F, false, readGpuImage(big), up);
    double a = 0.0, b = 0.0;
    for (size_t i = 0; i < up.size(); i += 4)
        a += up[i];
    for (size_t i = 0; i < cpu.outputs.at("gray").rgba.size(); i += 4)
        b += cpu.outputs.at("gray").rgba[i];
    CHECK(a / double(64 * 64) == doctest::Approx(b / double(32 * 32)).epsilon(0.02));
}

TEST_CASE("tex gpu: externals read GPU images, with uv as the fallback")
{
    if (!graphGpu())
        return;
    Graph g;
    add(g, "Pos", "position");
    Plan p = plan(into(g, "Pos.position"), "Output", "baseColor", 8);
    REQUIRE(p.error.empty());
    compare(p, 1e-5, 1e-6);

    GpuImageRef map = GpuImage::create({4, 4, TextureFormat::RGBA16F, false, false, "position"});
    std::vector<float> texels;
    for (int i = 0; i < 16; ++i)
        texels.insert(texels.end(), {3.0f, 4.0f, 5.0f, 1.0f});
    map->updateRgba(0, 0, 4, 4, texels);
    texgpu::Evaluator ev;
    texgpu::Inputs in;
    in.gpuExternals["position"] = map;
    REQUIRE(ev.setPlan(p, in).empty());
    runAll(ev);
    CHECK(texel(ev.readOutputs().at("out"), 4, 4)[2] == doctest::Approx(5.0));

    // A rebaked map reruns its readers.
    for (size_t i = 2; i < texels.size(); i += 4)
        texels[i] = 7.0f;
    map->updateRgba(0, 0, 4, 4, texels);
    REQUIRE(ev.setPlan(p, in).empty());
    runAll(ev);
    CHECK(ev.stats().passesRun == p.passes.size());
    CHECK(texel(ev.readOutputs().at("out"), 4, 4)[2] == doctest::Approx(7.0));
}

TEST_CASE("tex gpu: Image's sRGB colour and alpha match the CPU")
{
    if (!graphGpu())
        return;
    const auto file = tempDir("gpu_image_srgb") / "ramp.png";
    std::vector<float> rgba;
    for (int i = 0; i < 16; ++i)
        rgba.insert(rgba.end(), {float(i) / 15.0f, 0.5f, 1.0f - float(i) / 15.0f, float(15 - i) / 15.0f});
    image::writePng(file, 4, 4, rgba, 8);
    Graph g;
    add(g, "I", "image", {{"file", file.string()}, {"colorSpace", "srgb"}});
    PlanRequest r;
    r.width = r.height = 16;
    r.outputs = {{"color", "I", "out", PortType::Color, {}}, {"alpha", "I", "alpha", PortType::Gray, {}}};
    compare(buildPlan(g, r), 5e-3, 1e-3);
}

TEST_CASE("tex gpu: texEvaluate runs on the GPU by default and matches the CPU")
{
    if (!graphGpu())
        return;
    run("newScene");
    run("texCreateSet", {{"args", {"Gpu"}}, {"resolution", 32}});
    run("texAddNode", {{"args", {"Gpu", "bricks"}}, {"name", "Bricks"}});
    run("texConnect", {{"args", {"Gpu", "Bricks.mask", "Output.baseColor"}}});
    auto dir = std::filesystem::temp_directory_path() / "lila_tex_gpu_tests" / "eval";
    json g = run("texEvaluate", {{"args", {"Gpu"}}, {"channels", {"baseColor"}}, {"folder", (dir / "gpu").string()}, {"format", "exr"}});
    json c = run("texEvaluate", {{"args", {"Gpu"}}, {"channels", {"baseColor"}}, {"folder", (dir / "cpu").string()}, {"format", "exr"}, {"backend", "cpu"}});
    CHECK(g["backend"] == "gpu");
    CHECK(c["backend"] == "cpu");
    image::Image a = image::read(g["files"][0].get<std::string>());
    image::Image b = image::read(c["files"][0].get<std::string>());
    CHECK(maxDiff(a, b) < 2e-3);
}

TEST_CASE("tex gpu: a texture set drives its material in the viewport, live")
{
    if (!graphGpu())
        return;
    ui::texLive::install();
    run("newScene");
    run("texCreateSet", {{"args", {"Paint"}}, {"settings", {{"width", 512}, {"height", 512}}}, {"materials", {"PaintMat"}}});
    run("createMaterial", {{"args", {"PaintMat"}}});
    run("texAddNode", {{"args", {"Paint", "color"}}, {"name", "C"}});
    run("texSetValue", {{"args", {"Paint", "C.color", json::array({0.8, 0.05, 0.05, 1.0})}}});
    run("texConnect", {{"args", {"Paint", "C.out", "Output.baseColor"}}});

    CHECK_FALSE(ui::texLive::mapsFor("NoSuchMaterial"));
    std::optional<ui::texLive::Maps> maps = ui::texLive::mapsFor("PaintMat");
    REQUIRE(maps);
    REQUIRE(maps->baseColor);
    CHECK(maps->normal);
    CHECK(maps->orm);
    REQUIRE(ui::texLive::waitIdle());
    json status = ui::texLive::status();
    REQUIRE(status.size() == 1);
    CHECK(status[0]["error"] == "");
    // 256 preview, then the 512 tile.
    CHECK(status[0]["tiles"][0]["levels"].size() == 2);

    // drawMaterials picks the set's maps up through the material's slot.
    geo::Mesh mesh;
    scene::materials::slotIndex(mesh, "PaintMat");
    std::vector<Material> mats = ui::drawMaterials(mesh);
    REQUIRE(mats.size() == 1);
    CHECK(mats[0].baseColorMap == maps->baseColor);
    CHECK((mats[0].flags & MatNormalGreenUp) != 0);
    CHECK(ui::needsTangents(mesh));

    std::array<int, 4> before = centrePixel(mats[0]);
    CHECK(before[2] > 150);
    CHECK(before[1] < 80);

    run("texSetValue", {{"args", {"Paint", "C.color", json::array({0.05, 0.05, 0.8, 1.0})}}});
    REQUIRE(ui::texLive::waitIdle());
    status = ui::texLive::status();
    for (const json& level : status[0]["tiles"][0]["levels"])
    {
        CHECK(level["pipelinesCreated"] == 0);
        CHECK(level["compiled"] == 0);
    }
    std::array<int, 4> after = centrePixel(ui::drawMaterials(mesh)[0]);
    CHECK(after[0] > 150);
    CHECK(after[2] < 80);

    GpuImageRef rough = ui::texLive::channelImage("Paint", "roughness");
    REQUIRE(rough);
    CHECK(rough->format() == TextureFormat::R8);
    ui::texLive::shutdown();
}

namespace
{
/** Mesh maps that vary across the tile in a different way each, so texels read from the wrong place show. */
std::map<std::string, image::Image> wavyMaps(const Plan& p, int res)
{
    std::map<std::string, image::Image> out;
    int seed = 1;
    for (const BufferDesc& b : p.buffers)
    {
        if (b.kind != BufferKind::External || out.contains(b.name))
            continue;
        image::Image img{res, res, std::vector<float>(size_t(res) * size_t(res) * 4)};
        const float k = float(seed++);
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x)
            {
                const float u = (float(x) + 0.5f) / float(res), v = (float(y) + 0.5f) / float(res);
                float* t = &img.rgba[(size_t(y) * size_t(res) + size_t(x)) * 4];
                t[0] = 0.5f + 0.4f * std::sin(6.2831853f * (2.0f * u + 0.1f * k));
                t[1] = 0.5f + 0.4f * std::cos(6.2831853f * (3.0f * v + 0.2f * k));
                t[2] = 0.5f + 0.4f * std::sin(6.2831853f * (u + v) * k);
                t[3] = 1.0f;
            }
        out.emplace(b.name, std::move(img));
    }
    return out;
}

Plan setPlan(const std::string& set, int res)
{
    scene::Node* n = findSet(scene::Graph::current(), set);
    REQUIRE(n);
    const Graph g = readGraph(*n);
    PlanRequest r;
    r.width = r.height = res;
    r.outputs = channelRequests(g, readSettings(*n));
    Plan p = buildPlan(g, r);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    return p;
}

void layeredSet(const std::string& name, int res)
{
    run("newScene");
    run("texCreateSet", {{"args", {name}}, {"resolution", res}});
    for (const char* smart : {"wornPaintedMetal", "rustyIron", "dustyWood"})
        run("texApplySmartMaterial", {{"args", {name, smart}}});
}
} // namespace

TEST_CASE("tex gpu: mesh maps at another resolution than the set read as on the CPU")
{
    if (!graphGpu())
        return;
    run("newScene");
    run("texCreateSet", {{"args", {"Mixed"}}, {"resolution", 64}});
    run("texApplySmartMaterial", {{"args", {"Mixed", "wornPaintedMetal"}}});
    const Plan p = setPlan("Mixed", 64);
    for (int mapRes : {128, 32})
    {
        CAPTURE(mapRes);
        EvalOptions o;
        o.externals = wavyMaps(p, mapRes);
        REQUIRE_FALSE(o.externals.empty());
        EvalResult cpu = evaluateCpu(p, o);
        EvalResult gpuRes = texgpu::evaluate(p, o);
        for (const auto& [name, img] : cpu.outputs)
        {
            CAPTURE(name);
            REQUIRE(gpuRes.outputs.contains(name));
            CHECK(maxDiff(img, gpuRes.outputs.at(name)) < 5e-3);
        }
    }
}

TEST_CASE("tex gpu: compact buffers share memory and match the CPU")
{
    if (!graphGpu())
        return;
    layeredSet("Packed", 64);
    const Plan p = setPlan("Packed", 64);
    EvalOptions o;
    o.externals = wavyMaps(p, 64);
    EvalResult cpu = evaluateCpu(p, o);

    auto evalWith = [&](bool compact, texgpu::Stats& stats) {
        texgpu::Evaluator ev;
        texgpu::Inputs in;
        in.cpuExternals = o.externals;
        in.compact = compact;
        REQUIRE(ev.setPlan(p, in).empty());
        runAll(ev, 0.5);
        stats = ev.stats();
        return ev.readOutputs();
    };
    texgpu::Stats side, packed;
    auto a = evalWith(false, side);
    auto b = evalWith(true, packed);
    CHECK(packed.arenaBytes * 2 <= side.arenaBytes);

    // Capped below even the packed arena, evaluate runs a share of the outputs at a time.
    EvalResult split = texgpu::evaluate(p, o, packed.arenaBytes * 3 / 4);
    CHECK(split.outputs.size() == cpu.outputs.size());
    for (const auto& [name, img] : cpu.outputs)
    {
        CAPTURE(name);
        CHECK(maxDiff(img, a.at(name)) < 5e-3);
        CHECK(maxDiff(img, b.at(name)) < 5e-3);
        CHECK(maxDiff(img, split.outputs.at(name)) < 5e-3);
    }
    // Too small for even one output, each runs on the CPU instead, with a warning saying so.
    EvalResult tiny = texgpu::evaluate(p, o, 4096);
    for (const auto& [name, img] : cpu.outputs)
    {
        CAPTURE(name);
        REQUIRE(tiny.outputs.contains(name));
        CHECK(maxDiff(img, tiny.outputs.at(name)) < 5e-3);
    }
    CHECK(std::any_of(tiny.warnings.begin(), tiny.warnings.end(), [](const std::string& w) { return w.find("CPU") != std::string::npos; }));
}

TEST_CASE("tex gpu: a 4K set with many generator layers and 4K mesh maps evaluates")
{
    if (!graphGpu())
        return;
    layeredSet("Big", 4096);
    const Plan p = setPlan("Big", 4096);
    EvalOptions o;
    o.externals = wavyMaps(p, 4096);
    EvalResult gpuRes = texgpu::evaluate(p, o);
    o.externals.clear();
    REQUIRE(gpuRes.outputs.size() >= 7);

    // The same plan at 64 on the CPU matches the 4K result averaged down to 64.
    const Plan small = setPlan("Big", 64);
    EvalOptions so;
    so.externals = wavyMaps(small, 64);
    EvalResult cpu = evaluateCpu(small, so);
    for (const auto& [name, img] : gpuRes.outputs)
    {
        CAPTURE(name);
        REQUIRE(img.width == 4096);
        REQUIRE(cpu.outputs.contains(name));
        const double m = meanDiff(downsample(img, 64), cpu.outputs.at(name));
        // Normals come from height slopes, which sharpen with resolution.
        CHECK(m < (name == "normal" ? 0.15 : 0.05));
    }
}

TEST_CASE("tex gpu: outputs too large for one GPU buffer evaluate in pages, or on the CPU, and match")
{
    if (!graphGpu())
        return;
    Graph g;
    add(g, "B", "bricks", {{"rows", 37}, {"columns", 23}});
    add(g, "I", "image", {{"file", "external:map"}});
    // A blur is a whole-size intermediate that the page passes read around each texel.
    add(g, "S", "blur", {{"radius", 0.02}});
    link(g, "B.mask", "S.input");
    PlanRequest r;
    r.width = r.height = 256;
    r.outputs = {{"color", "I", "out", PortType::Color, {}}, {"mask", "B", "mask", PortType::Gray, {}}, {"soft", "S", "out", PortType::Gray, {}}};
    const Plan p = buildPlan(g, r);
    REQUIRE_MESSAGE(p.error.empty(), p.error);

    EvalOptions o;
    image::Image map{64, 32, {}, {}};
    for (int i = 0; i < 64 * 32; ++i)
        map.rgba.insert(map.rgba.end(), {float(i % 64) / 63.0f, float(i / 64) / 31.0f, 0.5f, 1.0f});
    o.externals["map"] = map;

    const EvalResult whole = texgpu::evaluate(p, o);
    // One float output at 256 x 256 is 1 MB: a byte less makes every output go page by page.
    const EvalResult paged = texgpu::evaluate(p, o, 256 * 256 * 16 - 1);
    // Fused outputs go page by page; the blur's own output is a whole buffer, so it falls back to the CPU and says so.
    size_t onCpu = 0;
    for (const std::string& w : paged.warnings)
        if (w.find("CPU") != std::string::npos)
        {
            ++onCpu;
            CHECK_MESSAGE(w.find("soft") != std::string::npos, w);
        }
    CHECK(onCpu == 1);
    for (const auto& [name, img] : whole.outputs)
    {
        CAPTURE(name);
        REQUIRE(paged.outputs.contains(name));
        CHECK(maxDiff(img, paged.outputs.at(name)) < 1e-3);
    }
}
