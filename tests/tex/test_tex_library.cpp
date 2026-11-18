#include "TexTest.h"

#include "tex/Natives.h"
#include "tex/NodeDef.h"
#include "tex/Ports.h"
#include "tex/SlangRt.h"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <set>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** A graph holding just the node, its output requested (material outputs through their base colour). */
Plan singlePlan(const NodeDef& d, const PortDecl& o, int res)
{
    Graph g;
    add(g, "N", d.type);
    PlanRequest r;
    r.width = r.height = res;
    OutputRequest q{"out", "N", o.id, o.type == PortType::Material ? PortType::Color : o.type, {}};
    if (o.type == PortType::Material)
        q.field = "baseColor";
    r.outputs = {q};
    return buildPlan(g, r);
}

std::vector<const NodeDef*> builtins()
{
    std::vector<const NodeDef*> out;
    for (const NodeDef* d : allNodes())
        if (d->tier == "builtin")
            out.push_back(d);
    return out;
}

using Make = std::function<Graph()>;

Make single(std::string type, json values, std::string socket)
{
    return [=] {
        Graph g;
        add(g, "N", type, values);
        return into(g, "N." + socket);
    };
}

/** type fed from source.socket into input. */
Make fed(std::string srcType, json srcValues, std::string srcSocket, std::string type, json values, std::string input, std::string socket = "out")
{
    return [=] {
        Graph g;
        add(g, "Src", srcType, srcValues);
        add(g, "N", type, values);
        link(g, "Src." + srcSocket, "N." + input);
        return into(g, "N." + socket);
    };
}

Make noisy(std::string type, json values, std::string input, std::string socket = "out")
{
    return fed("perlin", {{"scale", 4}, {"seed", 7}}, "out", type, values, input, socket);
}

Make bricky(std::string type, json values, std::string input, std::string socket = "out")
{
    return fed("bricks", {{"rows", 6}, {"columns", 3}, {"bevel", 0.02}, {"mortar", 0.12}}, "mask", type, values, input, socket);
}

/** Bricks mask -> flood fill -> fill node. */
Make filled(std::string type, json values, std::string socket)
{
    return [=] {
        Graph g;
        add(g, "B", "bricks", {{"rows", 6}, {"columns", 3}, {"bevel", 0.02}, {"mortar", 0.12}, {"offset", 0.37}});
        add(g, "F", "floodFill");
        add(g, "N", type, values);
        link(g, "B.mask", "F.input");
        link(g, "F.out", "N.input");
        return into(g, "N." + socket);
    };
}

image::Image evalAt(const Graph& g, int res, const EvalOptions& o = {})
{
    Plan p = plan(g, "Output", "baseColor", res);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    return evaluateCpu(p, o).outputs.at("out");
}
} // namespace

TEST_CASE("tex library: every built-in node loads")
{
    for (const std::string& e : libraryErrors())
        FAIL_CHECK(e);
    CHECK(builtins().size() >= 85);
}

TEST_CASE("tex library: every node compiles to SPIR-V and runs on the CPU")
{
    for (const NodeDef* d : builtins())
    {
        if (d->kind != NodeKind::Slang && d->kind != NodeKind::Native)
            continue;
        for (const PortDecl& o : d->outputs)
        {
            CAPTURE(d->type);
            CAPTURE(o.id);
            Plan p = singlePlan(*d, o, 16);
            REQUIRE_MESSAGE(p.error.empty(), p.error);
            for (const Pass& pass : p.passes)
            {
                if (pass.kind != PassKind::Fused)
                    continue;
                std::string err;
                CHECK_MESSAGE(!slangrt::spirv(pass.source, &err).empty(), err);
            }
            CHECK_NOTHROW(evaluateCpu(p));
        }
    }
}

TEST_CASE("tex library: native kernels compile to SPIR-V")
{
    for (const NodeDef* d : builtins())
    {
        if (d->kind != NodeKind::Native)
            continue;
        CAPTURE(d->type);
        REQUIRE(hasNativeKernels(d->type));
        NativeSchedule s = nativeSchedule(d->type, 64, 32, [&](const std::string& id, int) {
            const ParamDecl* p = d->param(id);
            return p && p->defaultValue.is_number() ? p->defaultValue.get<float>() : 0.0f;
        });
        REQUIRE(!s.steps.empty());
        std::set<std::string> entries;
        for (const NativeStep& st : s.steps)
            entries.insert(st.entry);
        for (const std::string& e : entries)
        {
            CAPTURE(e);
            std::string err;
            std::string src = nativeKernelSource(s.file, e, &err);
            REQUIRE_MESSAGE(!src.empty(), err);
            CHECK_MESSAGE(!slangrt::spirv(src, &err).empty(), err);
        }
    }
}

TEST_CASE("tex library: native passes are deterministic and their GPU kernels match")
{
    struct Case
    {
        std::string type;
        Make make;
        double tolerance;
    };
    std::vector<Case> cases = {
        {"blur", noisy("blur", {{"radius", 0.02}}, "input"), 1e-4},
        {"floodFill", bricky("floodFill", json::object(), "input"), 1e-6},
        {"distance", bricky("distance", {{"mode", "signed"}}, "input"), 0.02},
        {"distance", bricky("distance", {{"maxDistance", 0.05}}, "input"), 0.05},
        {"dilate", noisy("dilate", {{"radius", 0.03}}, "input"), 1e-6},
        {"erode", noisy("erode", {{"radius", 0.03}}, "input"), 1e-6},
        {"ambientOcclusion", noisy("ambientOcclusion", {{"radius", 0.08}, {"depth", 0.3}}, "height"), 1e-3},
        {"normalToHeight", [] {
             Graph g;
             add(g, "P", "perlin", {{"scale", 3}, {"octaves", 3}});
             add(g, "H", "heightToNormal");
             add(g, "N", "normalToHeight");
             link(g, "P.out", "H.height");
             link(g, "H.normal", "N.input");
             return into(g, "N.out");
         }, 1e-3},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.type);
        Plan p = plan(c.make(), "Output", "baseColor", 64);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        image::Image a = evaluateCpu(p).outputs.at("out");
        image::Image b = evaluateCpu(p).outputs.at("out");
        CHECK(maxDiff(a, b) == 0.0);

        // Swap the C++ implementation for the Slang kernels run on the CPU, the way the GPU runs them.
        NativeCpuFn cpu = nativeCpu(c.type);
        REQUIRE(cpu);
        registerNativeCpu(c.type, [](const NativeCall& call) { runNativeKernelsCpu(call); });
        image::Image k = evaluateCpu(p).outputs.at("out");
        registerNativeCpu(c.type, cpu);
        CHECK(meanDiff(a, k) < c.tolerance);
        image::Image again = evaluateCpu(p).outputs.at("out");
        CHECK(maxDiff(a, again) == 0.0);
    }
}

TEST_CASE("tex library: normal to height inverts height to normal")
{
    Graph g;
    add(g, "P", "perlin", {{"scale", 3}, {"octaves", 3}, {"seed", 2}});
    add(g, "H", "heightToNormal", {{"intensity", 2.0}});
    add(g, "N", "normalToHeight", {{"intensity", 2.0}});
    link(g, "P.out", "H.height");
    link(g, "H.normal", "N.input");
    image::Image height = evalAt(into(g, "P.out"), 64);
    image::Image back = evalAt(into(g, "N.out"), 64);
    double mean = 0.0;
    for (size_t i = 0; i < height.rgba.size(); i += 4)
        mean += height.rgba[i];
    mean /= double(height.rgba.size() / 4);
    for (size_t i = 0; i < height.rgba.size(); ++i)
        if (i % 4 != 3)
            height.rgba[i] += float(0.5 - mean);
    CHECK(meanDiff(height, back) < 0.02);
}

TEST_CASE("tex library: flood fill finds each brick")
{
    Graph g;
    add(g, "B", "bricks", {{"rows", 4}, {"columns", 2}, {"offset", 0.0}, {"bevel", 0.01}, {"mortar", 0.15}});
    add(g, "F", "floodFill");
    add(g, "S", "fillToSize", {{"mode", "minimum"}});
    link(g, "B.mask", "F.input");
    link(g, "F.out", "S.input");
    image::Image size = evalAt(into(g, "S.out"), 64);
    // A brick is 32 x 16 texels less the mortar: its smaller side is a little under 0.25.
    CHECK(texel(size, 16, 8)[0] > 0.2f);
    CHECK(texel(size, 16, 8)[0] < 0.25f);
}

TEST_CASE("tex library: pixel processor code recompiles, its values don't")
{
    Graph g;
    add(g, "Px", "pixelProcessor", {{"expression", "return float4(uv.x * p.x, 0.0, 0.0, 1.0);"}, {"x", 2.0}});
    Graph full = into(g, "Px.out");
    image::Image a = evalAt(full, 8);
    CHECK(texel(a, 3, 0)[0] == doctest::Approx(2.0 * 3.5 / 8.0));
    full.node("Px")->values["x"] = 3.0;
    Plan p1 = plan(full, "Output", "baseColor", 8);
    full.node("Px")->values["x"] = 2.0;
    Plan p0 = plan(full, "Output", "baseColor", 8);
    CHECK(p0.structureHash == p1.structureHash);
    full.node("Px")->values["expression"] = "return float4(uv.y, 0.0, 0.0, 1.0);";
    Plan p2 = plan(full, "Output", "baseColor", 8);
    CHECK(p2.structureHash != p0.structureHash);
    CHECK(texel(evaluateCpu(p2).outputs.at("out"), 0, 0)[0] == doctest::Approx(15.0 / 16.0));
}

TEST_CASE("tex library: generators tile")
{
    std::vector<std::pair<std::string, json>> gens = {
        {"simplex", json::object()},   {"cells", {{"mode", "crackle"}}}, {"worley", json::object()},
        {"gabor", {{"angleRandom", 0.5}}}, {"anisotropic", json::object()}, {"directional", {{"angle", 33.0}}},
        {"crystal", json::object()},   {"clouds", {{"mode", "billow"}}}, {"dirt", json::object()},
        {"tiles", {{"rotationRandom", 0.5}}}, {"weave", {{"count", 7}}}, {"hex", json::object()},
        {"scales", json::object()},    {"planks", json::object()},       {"scratches", json::object()},
        {"shape", {{"shape", "star"}, {"tiles", 3}}},
    };
    for (const auto& [type, values] : gens)
    {
        CAPTURE(type);
        const NodeDef* d = findNode(type);
        REQUIRE(d);
        Graph g = single(type, values, d->outputs[0].id)();
        Plan p = plan(g, "Output", "baseColor", 32);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        image::Image a = evaluateCpu(p).outputs.at("out");
        EvalOptions shifted;
        shifted.uvOffset[0] = 1.0f;
        shifted.uvOffset[1] = -2.0f;
        CHECK(maxDiff(a, evaluateCpu(p, shifted).outputs.at("out")) < 1e-4);
    }
    // Scatter tiles too, fed a shape.
    for (const char* type : {"tiler", "splatter"})
    {
        CAPTURE(type);
        Graph g = fed("shape", json::object(), "mask", type, {{"rotationRandom", 1.0}, {"offsetRandom", 0.5}}, "pattern")();
        Plan p = plan(g, "Output", "baseColor", 32);
        image::Image a = evaluateCpu(p).outputs.at("out");
        EvalOptions shifted;
        shifted.uvOffset[0] = -1.0f;
        shifted.uvOffset[1] = 1.0f;
        CHECK(maxDiff(a, evaluateCpu(p, shifted).outputs.at("out")) < 1e-4);
    }
}

TEST_CASE("tex library: golden images at 64 px")
{
    const json mats = json::object();
    std::vector<std::pair<std::string, Make>> cases = {
        {"simplex", single("simplex", {{"scale", 4}, {"seed", 1}}, "out")},
        {"cells_cells", single("cells", json::object(), "out")},
        {"cells_crackle", single("cells", {{"mode", "crackle"}}, "out")},
        {"cells_f2MinusF1", single("cells", {{"mode", "f2MinusF1"}, {"metric", "manhattan"}}, "out")},
        {"cells_color", single("cells", json::object(), "color")},
        {"worley", single("worley", json::object(), "out")},
        {"gabor", single("gabor", {{"angle", 30.0}}, "out")},
        {"anisotropic", single("anisotropic", json::object(), "out")},
        {"directional", single("directional", json::object(), "out")},
        {"crystal", single("crystal", json::object(), "out")},
        {"clouds", single("clouds", json::object(), "out")},
        {"clouds_ridged", single("clouds", {{"mode", "ridged"}}, "out")},
        {"dirt", single("dirt", json::object(), "out")},
        {"tiles_mask", single("tiles", {{"sizeRandom", 0.5}, {"rotationRandom", 0.5}}, "mask")},
        {"tiles_color", single("tiles", json::object(), "color")},
        {"weave_height", single("weave", json::object(), "height")},
        {"weave_direction", single("weave", json::object(), "direction")},
        {"hex_mask", single("hex", json::object(), "mask")},
        {"hex_cellUv", single("hex", json::object(), "cellUv")},
        {"scales", single("scales", json::object(), "height")},
        {"planks_mask", single("planks", json::object(), "mask")},
        {"planks_random", single("planks", json::object(), "random")},
        {"shape_circle", single("shape", json::object(), "mask")},
        {"shape_polygon", single("shape", {{"shape", "polygon"}, {"sides", 5}, {"tiles", 2}}, "mask")},
        {"shape_star", single("shape", {{"shape", "star"}, {"rotation", 15.0}}, "mask")},
        {"shape_ring", single("shape", {{"shape", "ring"}}, "mask")},
        {"scratches", single("scratches", json::object(), "out")},
        {"sdf_circle", fed("sdfCircle", json::object(), "sdf", "sdfToMask", json::object(), "input")},
        {"sdf_box", fed("sdfBox", {{"rotation", 20.0}, {"corner", 0.05}}, "sdf", "sdfToMask", json::object(), "input")},
        {"sdf_segment", fed("sdfSegment", json::object(), "sdf", "sdfToMask", json::object(), "input")},
        {"sdf_polygon", fed("sdfPolygon", {{"sides", 5}}, "sdf", "sdfToMask", {{"mode", "outline"}}, "input")},
        {"sdf_star", fed("sdfStar", json::object(), "sdf", "sdfToMask", {{"mode", "glow"}, {"width", 0.1}}, "input")},
        {"sdf_boolean", [] {
             Graph g;
             add(g, "A", "sdfCircle", {{"centerX", 0.4}});
             add(g, "B", "sdfBox", {{"centerX", 0.6}, {"width", 0.3}});
             add(g, "U", "sdfBoolean", {{"operation", "subtract"}, {"smoothness", 0.05}});
             add(g, "R", "sdfRound", {{"radius", 0.01}});
             add(g, "An", "sdfAnnular", {{"thickness", 0.03}});
             add(g, "M", "sdfToMask");
             link(g, "A.sdf", "U.a");
             link(g, "B.sdf", "U.b");
             link(g, "U.sdf", "R.input");
             link(g, "R.sdf", "An.input");
             link(g, "An.sdf", "M.input");
             return into(g, "M.out");
         }},
        {"mirror", bricky("mirror", {{"axis", "both"}, {"position", 0.4}}, "input")},
        {"kaleidoscope", noisy("kaleidoscope", json::object(), "input")},
        {"polar", bricky("polar", json::object(), "input")},
        {"polar_cartesian", bricky("polar", {{"mode", "toCartesian"}}, "input")},
        {"directionalWarp", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "P", "perlin");
             add(g, "N", "directionalWarp", {{"angle", 45.0}});
             link(g, "B.mask", "N.input");
             link(g, "P.out", "N.intensity");
             return into(g, "N.out");
         }},
        {"multiWarp", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "P", "perlin", {{"scale", 3}});
             add(g, "N", "multiWarp", {{"intensity", 2.0}});
             link(g, "B.mask", "N.input");
             link(g, "P.out", "N.warp");
             return into(g, "N.out");
         }},
        {"swirl", bricky("swirl", json::object(), "input")},
        {"tiler", fed("shape", {{"shape", "star"}}, "mask", "tiler", {{"rotationRandom", 1.0}, {"sizeRandom", 0.5}, {"offsetRandom", 0.3}}, "pattern")},
        {"tiler_color", fed("shape", json::object(), "mask", "tiler", {{"sizeRandom", 0.5}}, "pattern", "color")},
        {"splatter", fed("shape", {{"shape", "polygon"}, {"sides", 3}}, "mask", "splatter", json::object(), "pattern")},
        {"splatter_masked", [] {
             Graph g;
             add(g, "S", "shape");
             add(g, "G", "gradient");
             add(g, "N", "splatter", {{"maskSize", 1.0}});
             link(g, "S.mask", "N.pattern");
             link(g, "G.out", "N.mask");
             return into(g, "N.out");
         }},
        {"splatter_instanceUv", fed("shape", json::object(), "mask", "splatter", json::object(), "pattern", "instanceUv")},
        {"curves", noisy("curves", {{"curve", {{"points", json::array({json::array({0.0, 0.0}), json::array({0.4, 0.8}), json::array({1.0, 1.0})})}}}}, "input")},
        {"hsv", fed("cells", json::object(), "color", "hsv", {{"hue", 0.3}, {"saturation", 0.5}}, "input")},
        {"invert", noisy("invert", json::object(), "input")},
        {"clamp", noisy("clamp", {{"low", 0.4}, {"high", 0.6}}, "input")},
        {"threshold", noisy("threshold", {{"softness", 0.1}}, "input")},
        {"histogramScan", noisy("histogramScan", {{"position", 0.4}, {"contrast", 0.8}}, "input")},
        {"histogramRange", noisy("histogramRange", {{"range", 0.3}}, "input")},
        {"makeTileable", fed("gradient", {{"mode", "radial"}}, "out", "makeTileable", json::object(), "input")},
        {"sharpen", noisy("sharpen", {{"amount", 2.0}}, "input")},
        {"emboss", bricky("emboss", json::object(), "input")},
        {"edgeDetect", bricky("edgeDetect", json::object(), "input")},
        {"directionalBlur", bricky("directionalBlur", {{"angle", 30.0}}, "input")},
        {"slopeBlur", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "P", "perlin", {{"scale", 6}});
             add(g, "N", "slopeBlur", {{"intensity", 0.05}, {"mode", "min"}});
             link(g, "B.mask", "N.input");
             link(g, "P.out", "N.slope");
             return into(g, "N.out");
         }},
        {"fillToRandom", filled("fillToRandom", json::object(), "out")},
        {"fillToRandom_color", filled("fillToRandom", json::object(), "color")},
        {"fillToGradient", filled("fillToGradient", {{"angleRandom", 1.0}}, "out")},
        {"fillToUv", filled("fillToUv", json::object(), "uv")},
        {"fillToSize", filled("fillToSize", {{"scale", 2.0}}, "out")},
        {"distance", bricky("distance", {{"maxDistance", 0.05}}, "input")},
        {"distance_signed", fed("shape", {{"tiles", 2}}, "mask", "distance", {{"mode", "signed"}}, "input")},
        {"dilate", noisy("dilate", {{"radius", 0.03}}, "input")},
        {"erode", noisy("erode", {{"radius", 0.03}}, "input")},
        {"ambientOcclusion", noisy("ambientOcclusion", {{"radius", 0.08}, {"depth", 0.3}}, "height")},
        {"normalToHeight", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "H", "heightToNormal");
             add(g, "N", "normalToHeight");
             link(g, "B.mask", "H.height");
             link(g, "H.normal", "N.input");
             return into(g, "N.out");
         }},
        {"normalCombine_udn", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "Hb", "heightToNormal");
             add(g, "P", "perlin", {{"scale", 8}});
             add(g, "Hp", "heightToNormal");
             add(g, "N", "normalCombine", {{"mode", "udn"}});
             link(g, "B.mask", "Hb.height");
             link(g, "P.out", "Hp.height");
             link(g, "Hb.normal", "N.base");
             link(g, "Hp.normal", "N.detail");
             return into(g, "N.out");
         }},
        {"normalInvert", fed("heightToNormal", {{"height", 0.5}}, "normal", "normalInvert", {{"invertX", true}}, "input")},
        {"normalStrength", [] {
             Graph g;
             add(g, "P", "perlin");
             add(g, "H", "heightToNormal");
             add(g, "N", "normalStrength", {{"strength", 3.0}});
             link(g, "P.out", "H.height");
             link(g, "H.normal", "N.input");
             return into(g, "N.out");
         }},
        {"curvatureFromNormal", [] {
             Graph g;
             add(g, "B", "bricks");
             add(g, "H", "heightToNormal", {{"intensity", 4.0}});
             add(g, "N", "curvatureFromNormal");
             link(g, "B.mask", "H.height");
             link(g, "H.normal", "N.input");
             return into(g, "N.out");
         }},
        {"materialHeightBlend", [] {
             Graph g;
             add(g, "Ma", "material", {{"baseColor", json::array({0.8, 0.2, 0.1, 1.0})}});
             add(g, "Mb", "material", {{"baseColor", json::array({0.1, 0.3, 0.8, 1.0})}});
             add(g, "Ha", "perlin", {{"scale", 6}});
             add(g, "G", "gradient");
             add(g, "N", "materialHeightBlend");
             link(g, "Ha.out", "Ma.height");
             link(g, "Ma.material", "N.a");
             link(g, "Mb.material", "N.b");
             link(g, "G.out", "N.mask");
             g.nodes.push_back({"Output", kMaterialOutputType, {0.0, 0.0}, json::object()});
             link(g, "N.material", "Output.material");
             return g;
         }},
        {"materialBlend", [] {
             Graph g;
             add(g, "Ma", "material", {{"baseColor", json::array({0.8, 0.2, 0.1, 1.0})}});
             add(g, "Mb", "material", {{"baseColor", json::array({0.1, 0.3, 0.8, 1.0})}});
             add(g, "G", "gradient", {{"mode", "radial"}});
             add(g, "N", "materialBlend");
             link(g, "Ma.material", "N.a");
             link(g, "Mb.material", "N.b");
             link(g, "G.out", "N.mask");
             g.nodes.push_back({"Output", kMaterialOutputType, {0.0, 0.0}, json::object()});
             link(g, "N.material", "Output.material");
             return g;
         }},
        {"triplanar", bricky("triplanar", {{"scale", 2.0}}, "input")},
        {"pixelProcessor", noisy("pixelProcessor", {{"expression", "float v = a.sample(uv).r;\nreturn float4(v, uv.x * p.x, sin(v * 20.0) * 0.5 + 0.5, 1.0);"}, {"x", 0.8}}, "a")},
        {"switch", fed("perlin", json::object(), "out", "switch", {{"selected", "b"}}, "b")},
        {"random", single("random", {{"seed", 3}}, "color")},
    };
    // A dome for the mesh-map inputs: positions over the texture, normals tilting out from the centre.
    EvalOptions dome;
    image::Image pos{64, 64, std::vector<float>(64 * 64 * 4)}, nrm = pos;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const float u = (float(x) + 0.5f) / 64.0f - 0.5f, v = 0.5f - (float(y) + 0.5f) / 64.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(u, v, 0.35f));
            float* pp = &pos.rgba[(size_t(y) * 64 + x) * 4];
            float* np = &nrm.rgba[(size_t(y) * 64 + x) * 4];
            pp[0] = u, pp[1] = v, pp[2] = n.z * 0.5f, pp[3] = 1.0f;
            // As the bake stores them: n * 0.5 + 0.5.
            np[0] = n.x * 0.5f + 0.5f, np[1] = n.y * 0.5f + 0.5f, np[2] = n.z * 0.5f + 0.5f, np[3] = 1.0f;
        }
    dome.externals["position"] = pos;
    dome.externals["worldNormal"] = nrm;
    for (const auto& [name, make] : cases)
    {
        CAPTURE(name);
        golden(name, evalAt(make(), 64, name == "triplanar" ? dome : EvalOptions{}));
    }
}

TEST_CASE("tex library: triplanar picks its axis from the bake's encoded normal and keeps the bounds' proportions")
{
    // Left half faces -X, right half +Z, each at one position in the bake's 0-1 bounds.
    EvalOptions maps;
    image::Image pos{8, 8, std::vector<float>(8 * 8 * 4)}, nrm = pos;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            const bool left = x < 4;
            const glm::vec3 p = left ? glm::vec3(0.2f, 0.6f, 0.9f) : glm::vec3(0.3f, 0.7f, 0.1f);
            const glm::vec3 n = left ? glm::vec3(-1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
            float* pp = &pos.rgba[(size_t(y) * 8 + x) * 4];
            float* np = &nrm.rgba[(size_t(y) * 8 + x) * 4];
            pp[0] = p.x, pp[1] = p.y, pp[2] = p.z, pp[3] = 1.0f;
            np[0] = n.x * 0.5f + 0.5f, np[1] = n.y * 0.5f + 0.5f, np[2] = n.z * 0.5f + 0.5f, np[3] = 1.0f;
        }
    maps.externals["position"] = pos;
    maps.externals["worldNormal"] = nrm;

    // A linear gradient reads back the u it is sampled at: y for the X projection (yz), x for the Z projection (xy).
    for (const char* type : {"triplanar", "triplanarTiled"})
    {
        CAPTURE(type);
        Graph g;
        add(g, "G", "gradient");
        add(g, "T", type, {{"sharpness", 32.0}, {"rotation", 0.0}});
        link(g, "G.out", "T.input");
        if (std::string_view(type) == "triplanar")
        {
            image::Image cube = evalAt(into(g, "T.out"), 8, maps);
            CHECK(texel(cube, 1, 4)[0] == doctest::Approx(0.6).epsilon(0.01));
            CHECK(texel(cube, 6, 4)[0] == doctest::Approx(0.3).epsilon(0.01));
            // A 100 x 400 x 100 box: x and z shrink to a quarter of y's span, so a square pattern stays square.
            g.node("T")->values["boundsMax"] = json::array({100.0, 400.0, 100.0});
            image::Image tall = evalAt(into(g, "T.out"), 8, maps);
            CHECK(texel(tall, 1, 4)[0] == doctest::Approx(0.6).epsilon(0.01));
            CHECK(texel(tall, 6, 4)[0] == doctest::Approx(0.075).epsilon(0.01));
        }
        else
        {
            // The tiled form turns and offsets each axis, so only the choice of axis shows: the halves differ.
            g.node("G")->values["repeat"] = 3.0;
            image::Image tiled = evalAt(into(g, "T.out"), 8, maps);
            CHECK(std::abs(texel(tiled, 1, 4)[0] - texel(tiled, 6, 4)[0]) > 0.01f);
        }
    }
}
