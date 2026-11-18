#include "TexPaintTest.h"

#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/FrameState.h"
#include "render/GpuImage.h"
#include "render/PaintGpu.h"
#include "render/Renderer.h"
#include "render/TexEval.h"
#include "render/VirtualTexture.h"
#include "render/VulkanContext.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "tex/PageStore.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"
#include "ui/TexLive.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <filesystem>
#include <set>

using namespace textest;
using namespace lila::render;
namespace tp = lila::tex::paint;

namespace
{
bool gpuVt()
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for virtual texture tests");
        return false;
    }
    if (!vt::available())
    {
        MESSAGE("the device can't do virtual texturing");
        return false;
    }
    return true;
}

void newSceneWithoutLive()
{
    ui::texLive::shutdown();
    run("newScene");
    UndoStack::get().clear();
    mainq::drain();
}

void setMode(const char* mode) { PrefStore::get().set("general", "/texture/virtualTexturing", mode); }

size_t deviceBytes() { return vt::deviceBytesInUse(); }

/** Quads in clip space, each mapping a uv rectangle {u0, v0, u1, v1} (V up) over its screen rectangle {x0, y0, x1, y1} (NDC, y down). */
struct Quad
{
    glm::vec4 screen;
    glm::vec4 uv;
};

FrameState frame(const std::vector<Quad>& quads, const Material& mat)
{
    auto mesh = std::make_shared<RenderMesh>();
    for (const Quad& q : quads)
    {
        const uint32_t base = uint32_t(mesh->vertices.size());
        const glm::vec2 corners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (const glm::vec2& c : corners)
        {
            MeshVertex v{};
            v.pos = glm::vec3(glm::mix(q.screen.x, q.screen.z, c.x), glm::mix(q.screen.y, q.screen.w, c.y), 0.5f);
            v.normal = {0, 0, -1};
            v.tangent = {1, 0, 0, 1};
            // Screen rows run down and v up.
            v.uv = {glm::mix(q.uv.x, q.uv.z, c.x), glm::mix(q.uv.w, q.uv.y, c.y)};
            mesh->vertices.push_back(v);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
            mesh->indices.push_back(base + i);
    }
    mesh->boundsMin = {-1, -1, 0.5f};
    mesh->boundsMax = {1, 1, 0.5f};
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.lighting = FrameState::Lighting::Flat;
    s.background = {0.0f, 0.0f, 0.0f};
    MeshDraw d;
    d.mesh = mesh;
    d.materials = {mat};
    d.materials[0].flags |= MatTwoSided;
    s.meshes.push_back(d);
    return s;
}

Material baseColorOf(const ui::texLive::Maps& maps)
{
    Material m;
    m.baseColorMap = maps.baseColor;
    m.flags = MatBaseColorMap | MatTwoSided;
    return m;
}

std::vector<uint8_t> draw(const FrameState& s, int w, int h)
{
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, w, h, bgra, &err), err);
    return bgra;
}

/** Frames until the virtual textures stop asking for pages (each frame's feedback asks, the streamer answers). */
std::vector<uint8_t> settle(const FrameState& s, int w, int h, int maxFrames = 64)
{
    std::vector<uint8_t> px;
    size_t lastProduced = ~size_t(0);
    int quiet = 0;
    for (int i = 0; i < maxFrames; ++i)
    {
        px = draw(s, w, h);
        REQUIRE(vt::waitIdle(60000));
        const size_t produced = vt::stats().produced;
        // Sixteen frames cover every feedback pixel; stop once a full cycle made nothing new.
        quiet = produced == lastProduced ? quiet + 1 : 0;
        lastProduced = produced;
        if (quiet >= 17)
            break;
    }
    return draw(s, w, h);
}

/** cols x rows quads of size units in the XZ plane facing +Y, quad (c, r) covering UDIM tile 1001 + r * 10 + c. */
geo::Mesh tileGrid(int cols, int rows, double size)
{
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    std::vector<glm::vec2> uvs;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
        {
            const double x0 = c * size, z0 = r * size, x1 = x0 + size, z1 = z0 + size;
            const geo::Index b = geo::Index(pts.size());
            pts.insert(pts.end(), {{x0, 0, z0}, {x0, 0, z1}, {x1, 0, z1}, {x1, 0, z0}});
            uvs.insert(uvs.end(), {{float(c), float(r)}, {float(c), float(r + 1)}, {float(c + 1), float(r + 1)}, {float(c + 1), float(r)}});
            sizes.push_back(4);
            idx.insert(idx.end(), {b, b + 1, b + 2, b + 3});
        }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            for (geo::Index c : m.faceCorners(f))
                uv[c] = uvs[size_t(m.cornerPoint(c))];
    return m;
}

const uint8_t* texelAt(const std::vector<uint8_t>& bgra, int w, int x, int y) { return &bgra[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
} // namespace

TEST_CASE("virtual texture: pages sample like the whole image")
{
    if (!gpuVt())
        return;
    newSceneWithoutLive();
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 1024}, {"materials", {"M_Rock"}}});
    scene::Node& set = *tex::findSet(sg(), "Rock");
    tex::Graph g = tex::Graph::makeMaterial("Rock");
    add(g, "Noise", "perlin", {{"scale", 6}, {"octaves", 4}});
    link(g, "Noise.out", "Output.baseColor");
    tex::writeGraph(sg(), set, g);
    const int size = 256;
    // Not whole pixels per texel, so filtering decides what shows.
    const std::vector<Quad> view = {{{-1, -1, 1, 1}, {0.1f, 0.15f, 0.83f, 0.88f}}};

    setMode("off");
    ui::texLive::install();
    std::optional<ui::texLive::Maps> whole = ui::texLive::mapsFor("M_Rock");
    REQUIRE(whole);
    REQUIRE(ui::texLive::waitIdle(120000));
    CHECK_FALSE(whole->baseColor->vt);
    const std::vector<uint8_t> a = draw(frame(view, baseColorOf(*whole)), size, size);
    ui::texLive::shutdown();

    setMode("on");
    ui::texLive::install();
    std::optional<ui::texLive::Maps> paged = ui::texLive::mapsFor("M_Rock");
    REQUIRE(paged);
    REQUIRE(paged->baseColor);
    REQUIRE(paged->baseColor->vt);
    REQUIRE(ui::texLive::waitIdle(120000));
    CHECK_FALSE(ui::texLive::channelImage("Rock", "baseColor"));
    const std::vector<uint8_t> b = settle(frame(view, baseColorOf(*paged)), size, size);

    double sum = 0.0;
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (i % 4 != 3)
        {
            const int d = std::abs(int(a[i]) - int(b[i]));
            sum += d;
            worst = std::max(worst, d);
        }
    const double mean = sum / double(a.size() / 4 * 3);
    std::printf("[vt] virtual vs whole image at 1K: mean %.3f, worst %d (of 255), pages %zu\n", mean, worst, vt::stats().residentPages);
    CHECK(mean < 1.5);
    CHECK(worst <= 24);
    ui::texLive::shutdown();
    setMode("auto");
}

TEST_CASE("virtual texture: feedback asks only for visible pages, and memory follows them")
{
    if (!gpuVt())
        return;
    newSceneWithoutLive();
    run("texCreateSet",
        {{"args", {"Wall"}}, {"settings", {{"width", 4096}, {"height", 4096}, {"tiles", {1001, 1002, 1011, 1012}}}}, {"materials", {"M_Wall"}}});
    scene::Node& set = *tex::findSet(sg(), "Wall");
    tex::Graph g = tex::Graph::makeMaterial("Wall");
    add(g, "Noise", "perlin", {{"scale", 8}});
    link(g, "Noise.out", "Output.baseColor");
    tex::writeGraph(sg(), set, g);
    setMode("on");
    ui::texLive::install();
    std::optional<ui::texLive::Maps> maps = ui::texLive::mapsFor("M_Wall");
    REQUIRE(maps);
    REQUIRE(maps->baseColor->vt);
    REQUIRE(ui::texLive::waitIdle(120000));
    vt::Texture& tex = *maps->baseColor->vt;

    // Tile 1001's uv [0.25, 0.6036]^2 over 256 pixels: 4 sqrt 2 texels a pixel, level 2.5, so requests at level 2.
    const float span = 0.25f * std::sqrt(2.0f);
    const FrameState part = frame({{{-1, -1, 1, 1}, {0.25f, 0.25f, 0.25f + span, 0.25f + span}}}, baseColorOf(*maps));
    std::set<std::array<int, 4>> asked;
    for (int i = 0; i < 16; ++i)
    {
        draw(part, 256, 256);
        mainq::drain();
        for (const auto& r : tex.stats().lastRequests)
            asked.insert(r);
    }
    REQUIRE_FALSE(asked.empty());
    // Level 2 is 1024 texels: u 0.25-0.60 is pages 2-4, rows (1 - v) 0.40-0.75 pages 3-5.
    bool inside = true;
    for (const auto& r : asked)
        inside = inside && r[0] == 0 && r[1] == 2 && r[2] >= 2 && r[2] <= 4 && r[3] >= 3 && r[3] <= 5;
    CHECK(inside);
    CHECK(asked.size() == 9);

    settle(part, 256, 256);
    const vt::TextureStats st = tex.stats();
    const vt::SystemStats sys = vt::stats();
    // The nine pages, their ancestors on the way to each tile's coarsest page, and those four coarsest pages.
    std::printf("[vt] 4 tiles at 4K: %zu pages asked, %zu resident (%.1f MB), %.1f MB allocated of a %.0f MB budget; whole images would be %.0f MB\n",
                asked.size(), st.resident, st.bytes / 1048576.0, sys.allocatedBytes / 1048576.0, sys.budgetBytes / 1048576.0,
                4.0 * 4096.0 * 4096.0 * 4.0 * 4.0 * 4.0 / 3.0 / 1048576.0);
    CHECK(st.resident >= 9 + 4);
    CHECK(st.resident <= 9 + 9 + 4 + 4 + 8);
    CHECK(st.bytes < size_t(64) << 20);

    // Looking at tile 1012 instead asks for nothing on tile 1001.
    asked.clear();
    const FrameState other = frame({{{-1, -1, 1, 1}, {1.2f, 1.2f, 1.6f, 1.6f}}}, baseColorOf(*maps));
    for (int i = 0; i < 16; ++i)
    {
        draw(other, 256, 256);
        mainq::drain();
        for (const auto& r : tex.stats().lastRequests)
            asked.insert(r);
    }
    bool onlyOther = !asked.empty();
    for (const auto& r : asked)
        onlyOther = onlyOther && r[0] == tex.tileOf(1012);
    CHECK(onlyOther);
    ui::texLive::shutdown();
    setMode("auto");
}

TEST_CASE("virtual texture: a 60-tile 8K set opens and paints within its budget")
{
    if (!gpuVt())
        return;
    newSceneWithoutLive();
    const size_t budget = size_t(256) << 20;
    vt::setBudget(budget);
    const std::string meshPath = addMesh("HeroMesh", tileGrid(10, 6, 100.0));
    json tiles = json::array();
    for (int t = 0; t < 60; ++t)
        tiles.push_back(1001 + t);
    run("texCreateSet", {{"args", {"Hero"}}, {"settings", {{"width", 8192}, {"height", 8192}, {"tiles", tiles}}}, {"materials", {"M_Hero"}}});
    scene::Node& set = *tex::findSet(sg(), "Hero");
    tex::Graph g = tex::Graph::makeMaterial("Hero");
    add(g, "Paint", "image", {{"file", tp::imageRef("P", "baseColor")}});
    link(g, "Paint.out", "Output.baseColor");
    add(g, "Grain", "perlin", {{"scale", 32}});
    link(g, "Grain.out", "Output.roughness");
    tex::writeGraph(sg(), set, g);

    const size_t before = deviceBytes();
    setMode("auto");
    const auto t0 = std::chrono::steady_clock::now();
    ui::texLive::install();
    std::optional<ui::texLive::Maps> maps = ui::texLive::mapsFor("M_Hero");
    REQUIRE(maps);
    REQUIRE(maps->baseColor);
    REQUIRE(maps->baseColor->vt);
    REQUIRE(ui::texLive::waitIdle(120000));
    vt::Texture& tex = *maps->baseColor->vt;

    // Every tile on screen at once: 64 pixels a tile, so only the coarsest levels.
    std::vector<Quad> all;
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 10; ++c)
        {
            const float x0 = -1.0f + c * 0.2f, y0 = 1.0f - (r + 1) * (2.0f / 6.0f);
            all.push_back({{x0, y0, x0 + 0.2f, y0 + 2.0f / 6.0f}, {float(c), float(r), float(c + 1), float(r + 1)}});
        }
    Material mat = baseColorOf(*maps);
    settle(frame(all, mat), 640, 384);
    const double openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const vt::TextureStats opened = tex.stats();
    CHECK(opened.resident >= 60);

    // A close-up of tile 1001's centre, then a stroke there.
    const FrameState close = frame({{{-1, -1, 1, 1}, {0.45f, 0.45f, 0.55f, 0.55f}}}, mat);
    settle(close, 256, 256);
    const auto t1 = std::chrono::steady_clock::now();
    json dabs = json::array({{{"position", {50.0, 0.0, 50.0}}, {"radius", 4.0}}});
    run("texPaintStroke", {{"args", {"Hero", "P"}},
                           {"stroke", {{"space", "surface"}, {"brush", {{"falloff", "smooth"}, {"strength", 1.0}, {"spacing", 0.15}}},
                                       {"payload", {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}}}, {"dabs", dabs}, {"mesh", meshPath}}}});
    mainq::drain();
    const std::vector<uint8_t> px = settle(close, 256, 256);
    const double strokeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    const uint8_t* c = texelAt(px, 256, 128, 128);
    // BGRA: red where the stroke landed.
    CHECK(c[2] > 150);
    CHECK(c[1] < 60);

    const vt::SystemStats sys = vt::stats();
    const size_t after = deviceBytes();
    const json paint = render::paint::bufferInfo(set.id(), "P");
    std::printf("[vt] 60 tiles at 8K (%.0f GB as whole RGBA8 images): open %.0f ms, %zu pages resident (%.0f MB) under a %.0f MB budget "
                "(%.0f MB allocated), stroke + pages %.0f ms, paint pages %d (%.1f MB), device memory +%.0f MB\n",
                60.0 * 8192.0 * 8192.0 * 4.0 * 4.0 / 1073741824.0, openMs, sys.residentPages, sys.residentBytes / 1048576.0,
                budget / 1048576.0, sys.allocatedBytes / 1048576.0, strokeMs, paint.value("pages", 0), paint.value("bytes", 0) / 1048576.0,
                (double(after) - double(before)) / 1048576.0);
    CHECK(sys.allocatedBytes <= budget);
    CHECK(after < before + budget + (size_t(512) << 20));
    ui::texLive::shutdown();
    vt::setBudget(0);
}

TEST_CASE("texture evaluation: painting re-evaluates only the region the stroke changed")
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        return;
    newSceneWithoutLive();
    setMode("off");
    const std::string meshPath = addMesh("PanelMesh", tileGrid(1, 1, 100.0));
    run("texCreateSet", {{"args", {"Panel"}}, {"resolution", 1024}, {"materials", {"M_Panel"}}});
    scene::Node& set = *tex::findSet(sg(), "Panel");
    tex::Graph g = tex::Graph::makeMaterial("Panel");
    add(g, "Paint", "image", {{"file", tp::imageRef("P", "baseColor")}});
    add(g, "Noise", "perlin", {{"scale", 4}});
    add(g, "Mix", "blend", {});
    link(g, "Noise.out", "Mix.background");
    link(g, "Paint.out", "Mix.foreground");
    link(g, "Mix.out", "Output.baseColor");
    tex::writeGraph(sg(), set, g);
    auto stroke = [&](double x, glm::vec3 color) {
        json dabs = json::array({{{"position", {x, 0.0, 50.0}}, {"radius", 5.0}}});
        run("texPaintStroke", {{"args", {"Panel", "P"}},
                               {"stroke", {{"space", "surface"}, {"brush", {{"falloff", "smooth"}, {"strength", 1.0}}},
                                           {"payload", {{"channels", {{"baseColor", {color.x, color.y, color.z}}}}}}, {"dabs", dabs}, {"mesh", meshPath}}}});
    };
    stroke(30.0, {1, 0, 0});
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Panel"));
    REQUIRE(ui::texLive::waitIdle(120000));

    // A live stroke elsewhere: only its region is loaded and computed again.
    auto header = std::make_shared<tp::Stroke>();
    header->id = 77;
    header->mesh = meshPath;
    header->brush = {{"falloff", "smooth"}, {"strength", 1.0}};
    header->payload.channels = {{"baseColor", {0.0f, 0.0f, 1.0f}}};
    auto mesh = render::paint::paintMesh(sg(), meshPath, render::paint::setMaterial(set));
    REQUIRE(mesh);
    std::vector<tp::Dab> dabs;
    for (int i = 0; i < 8; ++i)
    {
        brush::Dab d;
        d.center = {60.0 + i, 0.0, 60.0};
        d.radius = 4.0;
        tp::Dab rd;
        if (tp::recordDab(*mesh, d, rd))
            dabs.push_back(rd);
    }
    REQUIRE_FALSE(dabs.empty());
    const json before = ui::texLive::status();
    render::paint::beginLive(sg(), set, "P", header);
    render::paint::liveDabs(set.id(), "P", dabs);
    REQUIRE(ui::texLive::waitIdle(60000));
    const json after = ui::texLive::status();
    const json& lb = before[0]["tiles"][0]["levels"].back();
    const json& la = after[0]["tiles"][0]["levels"].back();
    const double computed = la["texelsComputed"].get<double>() - lb["texelsComputed"].get<double>();
    std::printf("[region] live stroke at 1K: %.0f texels computed of %d (%.1f%%)\n", computed, 1024 * 1024, 100.0 * computed / (1024.0 * 1024.0));
    CHECK(computed > 0.0);
    CHECK(computed < 0.25 * 1024.0 * 1024.0);

    // What the region updates left is what a whole evaluation makes.
    GpuImageRef live = ui::texLive::channelImage("Panel", "baseColor");
    REQUIRE(live);
    const std::vector<uint8_t> partial = readGpuImage(live);
    ui::texLive::shutdown();
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Panel"));
    REQUIRE(ui::texLive::waitIdle(120000));
    GpuImageRef fresh = ui::texLive::channelImage("Panel", "baseColor");
    REQUIRE(fresh);
    const std::vector<uint8_t> whole = readGpuImage(fresh);
    REQUIRE(partial.size() == whole.size());
    int worst = 0;
    for (size_t i = 0; i < whole.size(); ++i)
        worst = std::max(worst, std::abs(int(partial[i]) - int(whole[i])));
    CHECK(worst <= 1);
    render::paint::cancelLive(set.id(), "P");
    ui::texLive::shutdown();
    setMode("auto");
}

TEST_CASE("texture evaluation: strokes far apart re-evaluate their own regions, and neighbour taps grow them")
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        return;
    newSceneWithoutLive();
    setMode("off");
    const std::string meshPath = addMesh("PanelMesh", tileGrid(1, 1, 100.0));
    run("texCreateSet", {{"args", {"Panel"}}, {"resolution", 1024}, {"materials", {"M_Panel"}}});
    scene::Node& set = *tex::findSet(sg(), "Panel");
    // Height painted into a normal through heightToNormal, which reads its neighbours.
    tex::Graph g = tex::Graph::makeMaterial("Panel");
    add(g, "Paint", "image", {{"file", tp::imageRef("P", "height")}});
    add(g, "Slope", "heightToNormal", {{"intensity", 4.0}});
    link(g, "Paint.out", "Slope.height");
    link(g, "Slope.normal", "Output.normal");
    tex::writeGraph(sg(), set, g);
    auto mesh = render::paint::paintMesh(sg(), meshPath, render::paint::setMaterial(set));
    REQUIRE(mesh);
    auto dabsAt = [&](double x, double z) {
        std::vector<tp::Dab> out;
        for (int i = 0; i < 6; ++i)
        {
            brush::Dab d;
            d.center = {x + i, 0.0, z};
            d.radius = 4.0;
            tp::Dab rd;
            if (tp::recordDab(*mesh, d, rd))
                out.push_back(rd);
        }
        return out;
    };
    auto header = std::make_shared<tp::Stroke>();
    header->id = 1;
    header->mesh = meshPath;
    header->brush = {{"falloff", "smooth"}, {"strength", 1.0}};
    header->payload.channels = {{"height", {1.0f, 1.0f, 1.0f}}};
    render::paint::beginLive(sg(), set, "P", header);
    render::paint::liveDabs(set.id(), "P", dabsAt(50.0, 50.0));
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Panel"));
    REQUIRE(ui::texLive::waitIdle(120000));

    // Dabs at opposite corners in one batch: two small regions, not the box over the texture between them.
    std::vector<tp::Dab> both = dabsAt(8.0, 8.0);
    const std::vector<tp::Dab> far = dabsAt(86.0, 92.0);
    both.insert(both.end(), far.begin(), far.end());
    REQUIRE(both.size() > 6);
    const json before = ui::texLive::status();
    render::paint::liveDabs(set.id(), "P", both);
    REQUIRE(ui::texLive::waitIdle(60000));
    const json after = ui::texLive::status();
    const json& lb = before[0]["tiles"][0]["levels"].back();
    const json& la = after[0]["tiles"][0]["levels"].back();
    const double computed = la["texelsComputed"].get<double>() - lb["texelsComputed"].get<double>();
    std::printf("[regions] two far strokes at 1K: %.0f texels computed of %d (%.1f%%)\n", computed, 1024 * 1024,
                100.0 * computed / (1024.0 * 1024.0));
    CHECK(computed > 0.0);
    CHECK(computed < 0.2 * 1024.0 * 1024.0);

    // The regions, grown by the slope's taps, leave what a whole evaluation makes.
    GpuImageRef live = ui::texLive::channelImage("Panel", "normal");
    REQUIRE(live);
    const std::vector<uint8_t> partial = readGpuImage(live);
    ui::texLive::shutdown();
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Panel"));
    REQUIRE(ui::texLive::waitIdle(120000));
    GpuImageRef fresh = ui::texLive::channelImage("Panel", "normal");
    REQUIRE(fresh);
    const std::vector<uint8_t> whole = readGpuImage(fresh);
    REQUIRE(partial.size() == whole.size());
    int worst = 0;
    for (size_t i = 0; i < whole.size(); ++i)
        worst = std::max(worst, std::abs(int(partial[i]) - int(whole[i])));
    CHECK(worst <= 1);
    render::paint::cancelLive(set.id(), "P");
    ui::texLive::shutdown();
    setMode("auto");
}

TEST_CASE("page store: flattened paint lives beside the scene, and a save rewrites only the chunks that changed")
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        return;
    newSceneWithoutLive();
    const std::string meshPath = addMesh("PanelMesh", tileGrid(1, 1, 100.0));
    run("texCreateSet", {{"args", {"Panel"}}, {"resolution", 4096}, {"materials", {"M_Panel"}}});
    scene::Node* set = tex::findSet(sg(), "Panel");
    auto stroke = [&](double x, double z) {
        json dabs = json::array({{{"position", {x, 0.0, z}}, {"radius", 3.0}}});
        run("texPaintStroke", {{"args", {"Panel", "P"}},
                               {"stroke", {{"space", "surface"}, {"brush", {{"falloff", "smooth"}, {"strength", 1.0}}},
                                           {"payload", {{"channels", {{"baseColor", {0.0, 1.0, 0.0}}}}}}, {"dabs", dabs}, {"mesh", meshPath}}}});
    };
    // Two strokes far apart, flattened into pixels.
    stroke(10.0, 10.0);
    stroke(90.0, 90.0);
    run("texFlattenLayer", {{"args", {"Panel", "P"}}});
    const auto painted = render::paint::pageBytes(set->id(), "P");
    REQUIRE_FALSE(painted.empty());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_tex_store";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "panel.lila";
    run("saveScene", {{"args", {file.generic_string()}}});
    const tex::store::SaveStats first = tex::store::lastSave();
    const std::filesystem::path storeDir = tex::store::storeFor(file);
    REQUIRE(std::filesystem::is_directory(storeDir));
    std::map<std::string, std::filesystem::file_time_type> times;
    for (const auto& e : std::filesystem::recursive_directory_iterator(storeDir))
        if (e.path().extension() == ".lpg")
            times[e.path().generic_string()] = e.last_write_time();
    CHECK(first.written == times.size());
    CHECK(first.written >= 2);
    // The scene file holds stroke records and chunk references, not pixels.
    const auto sceneBytes = std::filesystem::file_size(file);
    CHECK(sceneBytes < 64 * 1024);

    // Saving again unchanged writes nothing; a third stroke near the first rewrites only its chunk.
    run("saveScene", {{"args", {file.generic_string()}}});
    CHECK(tex::store::lastSave().written == 0);
    CHECK(tex::store::lastSave().removed == 0);
    stroke(12.0, 12.0);
    run("texFlattenLayer", {{"args", {"Panel", "P"}}});
    run("saveScene", {{"args", {file.generic_string()}}});
    const tex::store::SaveStats third = tex::store::lastSave();
    std::printf("[store] 4K paint flattened: first save %zu chunks (%.0f KB, scene file %.0f KB); after a stroke: %zu written, %zu kept, %zu removed\n",
                first.written, first.bytesWritten / 1024.0, sceneBytes / 1024.0, third.written, third.kept, third.removed);
    CHECK(third.written >= 1);
    CHECK(third.written < first.written + 1);
    CHECK(third.kept >= 1);
    CHECK(third.removed == third.written);
    size_t untouched = 0;
    for (const auto& [path, time] : times)
        if (std::filesystem::exists(path) && std::filesystem::last_write_time(path) == time)
            ++untouched;
    CHECK(untouched >= 1);
    const auto repainted = render::paint::pageBytes(set->id(), "P");

    // Reopened, the pixels come back from the store.
    newSceneWithoutLive();
    run("openScene", {{"args", {file.generic_string()}}, {"force", true}});
    mainq::drain();
    set = tex::findSet(sg(), "Panel");
    REQUIRE(set);
    render::paint::sync(sg(), *set);
    CHECK(render::paint::pageBytes(set->id(), "P") == repainted);

    // Old scenes kept pixels inline: a document written outside a save still loads.
    const json doc = io::serialize(dir, false);
    newSceneWithoutLive();
    io::deserialize(doc, dir);
    mainq::drain();
    set = tex::findSet(sg(), "Panel");
    REQUIRE(set);
    render::paint::sync(sg(), *set);
    CHECK(render::paint::pageBytes(set->id(), "P") == repainted);
    std::filesystem::remove_all(dir);
}
