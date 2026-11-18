#include "TexPaintTest.h"

#include "core/MainQueue.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/GpuImage.h"
#include "render/PaintGpu.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "tex/BrushPresets.h"
#include "tex/PaintBuffer.h"
#include "tex/PaintParticles.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSurface.h"
#include "tex/TextureSet.h"
#include "tex/Library.h"
#include "ui/TexLive.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace textest;
using namespace lila::render;
namespace fs = std::filesystem;
namespace gp = lila::render::paint;
namespace tp = lila::tex::paint;

namespace
{
json surfaceStroke(std::vector<glm::dvec3> points, double radius, json payload)
{
    json dabs = json::array();
    for (const glm::dvec3& p : points)
        dabs.push_back({{"position", {p.x, p.y, p.z}}, {"radius", radius}});
    return {{"space", "surface"}, {"brush", brushJson()}, {"payload", std::move(payload)}, {"dabs", dabs}, {"mesh", "PanelMesh"}};
}

/** Looking straight down at the panel: screen x runs along world x, screen y along world z. */
json topView(int w = 800, int h = 600)
{
    return {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", w}, {"height", h}};
}

std::shared_ptr<const tp::PaintMesh> panelMesh(const scene::Node& set) { return gp::paintMesh(sg(), "PanelMesh", gp::setMaterial(set)); }
} // namespace

TEST_CASE("texture paint tools: a projection commit matches the same dab painted directly")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 512);
    json brush = brushJson();
    brush["radius"] = 60.0;
    run("texPaintStroke", {{"args", {"Panel", "Direct"}},
                           {"points", {{400.0, 300.0, 1.0}}},
                           {"view", topView()},
                           {"brush", brush},
                           {"payload", {{"channels", {{"baseColor", {0.0, 1.0, 0.0}}}}}}});

    // The same disc painted in a screen-sized buffer and projected.
    tp::ProjectionBuffer buf;
    buf.resize(800, 600);
    buf.paint({{400.0f, 300.0f}, 60.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("smooth")), {0.0f, 1.0f, 0.0f});
    const image::Image pic = buf.resolved();
    const auto img = tp::PaintImage::fromRgba(pic.width, pic.height, pic.rgba);
    run("texPaintStroke", {{"args", {"Panel", "Projected"}},
                           {"view", topView()},
                           {"brush", brushJson()},
                           {"payload", {{"kind", "project"}, {"image", img->toJson()}, {"mask", true}}}});
    const image::Image a = gp::readChannel(set.id(), "Direct", "baseColor");
    const image::Image b = gp::readChannel(set.id(), "Projected", "baseColor");
    REQUIRE(a.width == 512);
    REQUIRE(b.width == 512);
    double sum = 0.0, worst = 0.0;
    int painted = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
    {
        const double d = std::abs(double(a.rgba[i + 3]) - double(b.rgba[i + 3]));
        sum += d;
        worst = std::max(worst, d);
        painted += a.rgba[i + 3] > 0.5f;
    }
    CAPTURE(worst);
    CHECK(painted > 500);
    CHECK(sum / double(a.rgba.size() / 4) < 0.01);
    CHECK(worst < 0.15);
    CHECK(at(b, seamUv(1, 0.5, 0.0))[1] > 0.9f);
    // The record keeps the picture once, by key, and replays to the same pages.
    const tp::PaintPtr data = tp::readPaint(set);
    REQUIRE(data->images.size() == 1);
    CHECK(data->buffer("Projected")->strokes.front()->payload.image == data->images.begin()->first);
    const auto before = gp::pageBytes(set.id(), "Projected");
    run("texRasterize", {{"args", {"Panel", "Projected"}}});
    CHECK(gp::pageBytes(set.id(), "Projected") == before);
    // Clearing the layer drops the picture with the stroke.
    run("texClearLayer", {{"args", {"Panel", "Projected"}}});
    CHECK(tp::readPaint(set)->images.empty());
}

TEST_CASE("texture paint tools: smudge, blur and clone are deterministic, live or replayed")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "P", surfaceStroke({{-30, 0, -10}, {-25, 0, -10}, {-20, 0, -10}}, 8.0, {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}}));
    paintStroke("Panel", "P", surfaceStroke({{20, 0, 10}}, 10.0, {{"channels", {{"baseColor", {0.0, 0.0, 1.0}}}}}));
    const image::Image painted = gp::readChannel(set.id(), "P", "baseColor");

    // Smudge from inside the red streak outward: red is dragged onto bare surface.
    paintStroke("Panel", "P",
                surfaceStroke({{-25, 0, -10}, {-25, 0, -7}, {-25, 0, -4}, {-25, 0, -1}, {-25, 0, 2}}, 6.0,
                              {{"kind", "smudge"}, {"amount", 1.0}, {"channels", {{"baseColor", 0.0}}}}));
    const image::Image smudged = gp::readChannel(set.id(), "P", "baseColor");
    const glm::vec2 dragged = seamUv(0, -25.0, 0.5);
    CHECK(at(painted, dragged)[3] < 0.05f);
    CHECK(at(smudged, dragged)[3] > 0.1f);
    CHECK(at(smudged, dragged)[0] > 0.5f);

    // Blur softens the blue disc's rim.
    paintStroke("Panel", "P", surfaceStroke({{20, 0, 10}}, 14.0, {{"kind", "blur"}, {"filterRadius", 6.0}, {"channels", {{"baseColor", 0.0}}}}));
    // Clone the blue disc 20 units over (in uv: the right island runs u down as x goes up).
    const glm::vec2 src = seamUv(1, 20.0, 10.0), dst = seamUv(1, 40.0, -20.0);
    json clone = surfaceStroke({{40, 0, -20}}, 9.0, {{"kind", "clone"}, {"channels", {{"baseColor", 0.0}}}});
    clone["payload"]["sourceOffset"] = {src.x - dst.x, src.y - dst.y};
    paintStroke("Panel", "P", clone);
    const image::Image cloned = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(cloned, dst)[2] > 0.5f);
    CHECK(at(cloned, dst)[3] > 0.5f);

    // Rebuilt from the records: the same bytes.
    const auto all = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == all);

    // A smudge painted live in batches commits to the same pixels as the record replayed.
    scene::Node& other = makeSet("Other", 256);
    const tp::PaintPtr data = tp::readPaint(set);
    for (size_t i = 0; i < 2; ++i)
        paintStroke("Other", "P", data->buffer("P")->strokes[i]->toJson(false));
    tp::Stroke smudge = *data->buffer("P")->strokes[2];
    smudge.id = 3;
    gp::beginLive(sg(), other, "P", std::make_shared<const tp::Stroke>(smudge));
    gp::liveDabs(other.id(), "P", std::span(smudge.dabs).subspan(0, 2));
    gp::liveDabs(other.id(), "P", std::span(smudge.dabs).subspan(2));
    paintStroke("Other", "P", smudge.toJson(false));
    CHECK(gp::readChannel(other.id(), "P", "baseColor").rgba == smudged.rgba);
}

TEST_CASE("texture paint tools: fill by island paints exactly the island's texels")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const json r = paintStroke("Panel", "P",
                               {{"mesh", "PanelMesh"},
                                {"brush", brushJson()},
                                {"payload", {{"kind", "fill"}, {"fillBy", "island"}, {"at", {-25.0, 0.0, 0.0}}, {"channels", {{"roughness", 1.0}}}}}});
    CHECK(r["dabs"].get<int>() == 0);
    const image::Image img = gp::readChannel(set.id(), "P", "roughness");
    REQUIRE(img.width == 256);

    const auto mesh = panelMesh(set);
    geo::BvhClosest hit;
    REQUIRE(mesh->bvh->closestPoint({-25.0, 0.0, 0.0}, hit, 1.0));
    const std::vector<uint32_t> island = mesh->islandOf(uint32_t(hit.triangle));
    CHECK(island.size() == 2);
    tp::SurfaceAtlas atlas(mesh, 1001, 256, 256);
    size_t inside = 0, wrong = 0;
    for (uint32_t page = 0; page < uint32_t(atlas.pagesX() * atlas.pagesY()); ++page)
    {
        const std::vector<tp::GeoTexel>& texels = atlas.page(page);
        const int x0 = int(page % uint32_t(atlas.pagesX())) * tp::kPageSize, y0 = int(page / uint32_t(atlas.pagesX())) * tp::kPageSize;
        for (int y = 0; y < tp::kPageSize; ++y)
            for (int x = 0; x < tp::kPageSize; ++x)
            {
                const tp::GeoTexel& g = texels[size_t(y) * tp::kPageSize + size_t(x)];
                const bool want = g.triangle != tp::kNoTriangle && std::find(island.begin(), island.end(), g.triangle) != island.end();
                const float* t = texel(img, x0 + x, y0 + y);
                inside += want;
                if (want ? (t[0] != 1.0f || t[3] != 1.0f) : (t[3] != 0.0f))
                    ++wrong;
            }
    }
    CHECK(inside > 5000);
    CHECK(wrong == 0);

    // By material and over the whole mesh too.
    paintStroke("Panel", "Q", {{"mesh", "PanelMesh"}, {"payload", {{"kind", "fill"}, {"fillBy", "mesh"}, {"channels", {{"metallic", 1.0}}}}}});
    const image::Image all = gp::readChannel(set.id(), "Q", "metallic");
    CHECK(at(all, seamUv(0, -25, 0))[3] == 1.0f);
    CHECK(at(all, seamUv(1, 25, 0))[3] == 1.0f);

    // By polygon: the quad under the click, the same texels as its island here; by triangle: half of them.
    auto covered = [&](const char* layer) {
        const image::Image im = gp::readChannel(set.id(), layer, "roughness");
        size_t n = 0;
        for (size_t i = 0; i < size_t(im.width) * size_t(im.height); ++i)
            n += im.rgba[i * 4 + 3] == 1.0f;
        return n;
    };
    for (const char* by : {"polygon", "triangle"})
        paintStroke("Panel", by, {{"mesh", "PanelMesh"}, {"payload", {{"kind", "fill"}, {"fillBy", by}, {"at", {-25.0, 0.0, 0.0}}, {"channels", {{"roughness", 1.0}}}}}});
    const size_t islandTexels = covered("P"), polygonTexels = covered("polygon"), triangleTexels = covered("triangle");
    CHECK(polygonTexels == islandTexels);
    CHECK(triangleTexels > islandTexels / 3);
    CHECK(triangleTexels < islandTexels * 2 / 3);
}

TEST_CASE("texture paint tools: graph tips and material sources evaluate")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    // A noise tip: coverage varies inside one big dab.
    json s = surfaceStroke({{-25, 0, 0}}, 20.0, {{"channels", {{"roughness", 1.0}}}, {"tip", {{"node", "perlin"}, {"resolution", 128}}}});
    s["brush"]["falloff"] = "constant";
    paintStroke("Panel", "Tip", s);
    const image::Image tip = gp::readChannel(set.id(), "Tip", "roughness");
    float lo = 1.0f, hi = 0.0f;
    for (double x = -35.0; x <= -15.0; x += 1.0)
        for (double z = -10.0; z <= 10.0; z += 1.0)
        {
            const float a = at(tip, seamUv(0, x, z))[3];
            lo = std::min(lo, a);
            hi = std::max(hi, a);
        }
    CHECK(hi - lo > 0.2f);

    // A material source: the bricks paint varying colour and roughness through one stroke.
    json m = surfaceStroke({{25, 0, 0}}, 25.0, {{"channels", {{"baseColor", {1.0, 1.0, 1.0}}, {"roughness", 0.0}}},
                                               {"source", {{"library", "bricks"}, {"resolution", 256}, {"scale", 2.0}}}});
    m["brush"]["falloff"] = "constant";
    paintStroke("Panel", "Mat", m);
    const image::Image base = gp::readChannel(set.id(), "Mat", "baseColor");
    float cmin = 10.0f, cmax = -10.0f;
    for (double x = 15.0; x <= 35.0; x += 1.0)
        for (double z = -10.0; z <= 10.0; z += 1.0)
        {
            const float* t = at(base, seamUv(1, x, z));
            if (t[3] < 0.99f)
                continue;
            cmin = std::min(cmin, t[0]);
            cmax = std::max(cmax, t[0]);
        }
    CHECK(cmax - cmin > 0.05f);
    // Neither is the constant the payload names.
    CHECK(cmax < 0.999f);

    const auto bytes = gp::pageBytes(set.id(), "Mat");
    run("texRasterize", {{"args", {"Panel", "Mat"}}});
    CHECK(gp::pageBytes(set.id(), "Mat") == bytes);
}

TEST_CASE("texture paint tools: varied tiling breaks the repeat; a scene set paints as a material, edits included")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const json channels = {{"baseColor", {1.0, 1.0, 1.0}}, {"roughness", 0.0}, {"normal", {0.5, 0.5, 1.0}}};
    auto stroke = [&](const char* layer, json source) {
        json s = surfaceStroke({{25, 0, 0}}, 25.0, {{"channels", channels}, {"source", std::move(source)}});
        s["brush"]["falloff"] = "constant";
        paintStroke("Panel", layer, s);
        return gp::readChannel(set.id(), layer, "baseColor");
    };
    // Eight tiles per uv unit: one tile is 32 texels, so a plain repeat matches itself 32 texels along.
    const json bricks = {{"library", "bricks"}, {"resolution", 256}, {"scale", 8.0}};
    json varied = bricks;
    varied["tiling"] = {{"rotate", 1.0}, {"contrast", 0.5}, {"jitter", 0.3}};
    const image::Image plain = stroke("Plain", bricks);
    const image::Image hex = stroke("Hex", varied);
    double plainShift = 0.0, hexShift = 0.0, apart = 0.0;
    int n = 0;
    for (int y = 0; y < plain.height; ++y)
        for (int x = 0; x + 32 < plain.width; ++x)
        {
            const float* a = &plain.rgba[(size_t(y) * plain.width + x) * 4];
            const float* b = &plain.rgba[(size_t(y) * plain.width + x + 32) * 4];
            const float* c = &hex.rgba[(size_t(y) * hex.width + x) * 4];
            const float* d = &hex.rgba[(size_t(y) * hex.width + x + 32) * 4];
            if (a[3] < 0.99f || b[3] < 0.99f || c[3] < 0.99f || d[3] < 0.99f)
                continue;
            plainShift += std::abs(a[0] - b[0]);
            hexShift += std::abs(c[0] - d[0]);
            apart += std::abs(a[0] - c[0]);
            ++n;
        }
    REQUIRE(n > 100);
    CHECK(plainShift / n < 0.02);
    CHECK(hexShift / n > 3.0 * (plainShift / n) + 0.02);
    CHECK(apart / n > 0.02);
    // Normals blended across cells stay unit length.
    const image::Image nrm = gp::readChannel(set.id(), "Hex", "normal");
    for (size_t i = 0; i < nrm.rgba.size(); i += 4 * 37)
        if (nrm.rgba[i + 3] > 0.99f)
            CHECK(std::abs(glm::length(glm::vec3(nrm.rgba[i], nrm.rgba[i + 1], nrm.rgba[i + 2]) * 2.0f - 1.0f) - 1.0f) < 0.02f);
    // Replaying the strokes (a rasterize rebuilds from them) gives the same texels.
    run("texRasterize", {{"args", {"Panel", "Hex"}}});
    CHECK(gp::readChannel(set.id(), "Hex", "baseColor").rgba == hex.rgba);

    // A texture set built in the scene paints as a material, and its graph edited since paints the edit.
    scene::Node& brushMat = makeSet("BrushMat", 256);
    lila::tex::writeGraph(sg(), brushMat, lila::tex::library::graphOf(lila::tex::library::document(*lila::tex::library::find("bricks"))));
    const json scene = {{"library", "scene/BrushMat"}, {"resolution", 256}, {"scale", 2.0}};
    const image::Image first = stroke("Scene1", scene);
    lila::tex::writeGraph(sg(), brushMat, lila::tex::library::graphOf(lila::tex::library::document(*lila::tex::library::find("copperPatina"))));
    const image::Image second = stroke("Scene2", scene);
    double changed = 0.0;
    int m = 0;
    for (size_t i = 0; i < first.rgba.size(); i += 4)
        if (first.rgba[i + 3] > 0.99f && second.rgba[i + 3] > 0.99f)
        {
            changed += std::abs(first.rgba[i] - second.rgba[i]);
            ++m;
        }
    REQUIRE(m > 100);
    CHECK(changed / m > 0.02);
}

TEST_CASE("texture paint tools: one stroke paints every shading channel of a set")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 128);
    run("texAddShadingChannels", {{"args", {"Panel"}}});
    // Past the first eight: every channel of the set but the two directions.
    json channels = {{"baseColor", {0.9, 0.2, 0.1}}, {"roughness", 0.3}, {"metallic", 0.0}, {"height", 0.7}, {"emissive", {0.0, 0.0, 0.0}},
                     {"sheenColor", {0.2, 0.4, 0.6}}, {"sheenRoughness", 0.4}, {"coat", 0.8}, {"coatRoughness", 0.1},
                     {"subsurfaceColor", {0.8, 0.3, 0.2}}, {"subsurfaceRadius", 0.6}, {"anisotropy", 0.5}, {"transmission", 0.25},
                     {"ior", 0.25}, {"specularWeight", 0.9}, {"specularColor", {0.7, 0.8, 0.9}}};
    json s = surfaceStroke({{-25, 0, 0}}, 12.0, {{"channels", channels}});
    s["brush"]["falloff"] = "constant";
    paintStroke("Panel", "All", s);
    const glm::vec2 uv = seamUv(0, -25.0, 0.0);
    for (const auto& [name, v] : channels.items())
    {
        const image::Image img = gp::readChannel(set.id(), "All", name);
        INFO(name);
        REQUIRE(img.width > 0);
        const float* t = at(img, uv);
        CHECK(t[3] > 0.99f);
        const float want = v.is_array() ? v[0].get<float>() : v.get<float>();
        CHECK(std::abs(t[0] - want) < 0.01f);
    }
}

TEST_CASE("texture paint tools: normal bumps, flat normals and direction flow maps")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "N", surfaceStroke({{-25, 0, 0}}, 12.0, {{"kind", "normal"}, {"normalMode", "bump"}, {"intensity", 1.0}}));
    const image::Image n = gp::readChannel(set.id(), "N", "normal");
    // On the rim the normal leans; at the centre and outside it stays flat; everywhere it is unit length.
    const float* rim = at(n, seamUv(0, -25.0 + 6.0, 0.0));
    const glm::vec3 r(rim[0] * 2 - 1, rim[1] * 2 - 1, rim[2] * 2 - 1);
    CHECK(rim[3] > 0.1f);
    CHECK(glm::length(glm::vec2(r)) > 0.1f);
    CHECK(std::abs(glm::length(r) - 1.0f) < 0.01f);

    // A stamp from a normal map file.
    const fs::path stamp = fs::temp_directory_path() / "lila_paint_stamp.png";
    image::Image st{32, 32, std::vector<float>(32 * 32 * 4)};
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
        {
            const glm::vec3 v = glm::normalize(glm::vec3(0.6f, 0.0f, 1.0f)) * 0.5f + 0.5f;
            float* t = &st.rgba[(size_t(y) * 32 + x) * 4];
            t[0] = v.x;
            t[1] = v.y;
            t[2] = v.z;
            t[3] = 1.0f;
        }
    image::write(stamp, st);
    json sj = surfaceStroke({{25, 0, 0}}, 10.0, {{"kind", "normal"}, {"normalMode", "stamp"}, {"stampImage", stamp.generic_string()}});
    sj["brush"]["falloff"] = "constant";
    paintStroke("Panel", "S", sj);
    const image::Image stamped = gp::readChannel(set.id(), "S", "normal");
    const float* sc = at(stamped, seamUv(1, 25.0, 0.0));
    CHECK(sc[3] > 0.5f);
    CHECK(std::abs(sc[0] - 0.5f) + std::abs(sc[1] - 0.5f) > 0.1f);
    fs::remove(stamp);

    // Flatten: painting the flat normal over the bump, renormalised.
    json flatten = surfaceStroke({{-25, 0, 0}}, 14.0, {{"channels", {{"normal", {0.5, 0.5, 1.0}}}}});
    flatten["brush"]["falloff"] = "constant";
    paintStroke("Panel", "N", flatten);
    const image::Image flattened = gp::readChannel(set.id(), "N", "normal");
    const float* flat = at(flattened, seamUv(0, -25.0 + 6.0, 0.0));
    CHECK(std::abs(flat[2] - 1.0f) < 0.05f);

    // Direction into a user channel: a flow map, (x, y) * 0.5 + 0.5.
    json d = surfaceStroke({{-35, 0, 20}, {-30, 0, 20}, {-25, 0, 20}, {-20, 0, 20}}, 5.0, {{"kind", "direction"}, {"channel", "strokeDir"}});
    paintStroke("Panel", "D", d);
    const image::Image flow = gp::readChannel(set.id(), "D", "strokeDir");
    const float* f = at(flow, seamUv(0, -27.5, 20.0));
    CHECK(f[3] > 0.5f);
    CHECK(std::abs(f[0] - 0.5f) > 0.3f);
    CHECK(std::abs(f[2] - 0.5f) < 0.01f);
}

TEST_CASE("texture paint tools: particle brushes and line strokes")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const auto mesh = panelMesh(set);
    tp::ParticleSpec spec;
    spec.kind = "drips";
    spec.rate = 1.0f;
    spec.down = {0.0f, 0.0f, 1.0f};
    spec.seed = 7;
    tp::Dab base;
    REQUIRE(tp::recordDab(*mesh, [] {
        brush::Dab d;
        d.center = {-25.0, 0.0, -20.0};
        d.radius = 4.0;
        return d;
    }(), base));
    const std::vector<tp::Dab> drips = tp::emitParticles(*mesh, base, 0, spec);
    CHECK(drips.size() > 5);
    CHECK(tp::emitParticles(*mesh, base, 0, spec) == drips);
    // Drips run down (+z here).
    CHECK(mesh->point(drips.back()).z > mesh->point(base).z + 4.0f);

    json st = surfaceStroke({{-25, 0, -20}, {20, 0, -20}}, 4.0, {{"channels", {{"baseColor", {0.2, 0.1, 0.0}}}}});
    st["particles"] = spec.toJson();
    const json r = paintStroke("Panel", "P", st);
    CHECK(r["dabs"].get<int>() > 10);
    CHECK(tp::readPaint(set)->buffer("P")->strokes.front()->particles.value("expanded", false));

    // A line stroke from press to release: dabs all along it.
    json brush = brushJson();
    brush["radius"] = 10.0;
    brush["strokeMode"] = "line";
    const json line = run("texPaintStroke", {{"args", {"Panel", "L"}},
                                             {"points", {{200.0, 300.0}, {300.0, 250.0}, {600.0, 300.0}}},
                                             {"view", topView()},
                                             {"brush", brush},
                                             {"payload", {{"channels", {{"roughness", 1.0}}}}}});
    CHECK(line["dabs"].get<int>() > 20);
    const image::Image li = gp::readChannel(set.id(), "L", "roughness");
    CHECK(at(li, seamUv(0, -20.0, 0.0))[3] > 0.5f);
}

TEST_CASE("texture paint tools: screen uvs and the UV editor's channel view")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "P", surfaceStroke({{25, 0, 0}}, 20.0, {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}}));
    // Straight down at the panel: the pixel over (25, 0, 0) sees its uv.
    tp::StrokeView view;
    {
        const json r = run("texPaintStroke", {{"args", {"Panel", "Probe"}},
                                              {"points", {{200.0, 150.0}}},
                                              {"view", topView(400, 300)},
                                              {"brush", brushJson()},
                                              {"payload", {{"channels", {{"roughness", 1.0}}}}}});
        view = *tp::readPaint(set).get()->buffer("Probe")->strokes.front()->view;
    }
    const image::Image uvs = gp::screenUvs(sg(), "PanelMesh", view, 400, 300);
    // x = 25 is at screen x = 200 + 25 / (200 * tan 20deg * 4 / 3) * 200.
    const double half = 200.0 * std::tan(glm::radians(20.0)) * 400.0 / 300.0;
    const int px = int(200.0 + 25.0 / half * 200.0);
    const float* t = texel(uvs, px, 150);
    REQUIRE(t[3] == 1.0f);
    const glm::vec2 want = seamUv(1, 25.0, 0.0);
    CHECK(std::abs(t[0] - want.x) < 0.01f);
    CHECK(std::abs(t[1] - want.y) < 0.01f);
    CHECK(texel(uvs, 0, 0)[3] == 0.0f);

    // The channel image resampled to a view: the painted red shows, sRGB encoded.
    GpuImageRef img = gp::channelImage(set.id(), "P", "baseColor");
    REQUIRE(img);
    mainq::drain();
    const std::vector<uint8_t> px8 = gp::viewImage(img, {0.0f, 0.0f, 1.0f, 1.0f}, 64, 64, true);
    const int vx = int(want.x * 64), vy = int((1.0f - want.y) * 64);
    const uint8_t* v = &px8[(size_t(vy) * 64 + vx) * 4];
    CHECK(v[0] > 200);
    CHECK(v[1] < 30);
    // Outside the 0-1 tile the view is transparent.
    const std::vector<uint8_t> out = gp::viewImage(img, {1.0f, 1.0f, 2.0f, 2.0f}, 8, 8, true);
    CHECK(out[3] == 0);
}

TEST_CASE("texture paint tools: stroke frame time at 2K and 4K with 8 channels")
{
    if (!gpu())
        return;
    for (int res : {2048, 4096})
    {
        newScene();
        const std::string meshPath = addMesh("PanelMesh", seamPanel());
        scene::Node& set = makeSet("Panel", res);
        // A graph that composites the paint layer's channels, so the live viewport re-evaluates on every dab.
        tex::Graph g = tex::Graph::makeMaterial("Panel");
        for (const char* c : {"baseColor", "roughness", "metallic", "normal", "height", "ao", "emissive"})
        {
            const std::string node = std::string("Paint_") + c;
            add(g, node, "image", {{"file", tp::imageRef("P", c)}});
            link(g, node + ".out", std::string("Output.") + c);
        }
        tex::writeGraph(sg(), set, g);
        ui::texLive::install();
        REQUIRE(ui::texLive::mapsFor("M_Panel"));
        REQUIRE(ui::texLive::waitIdle(120000));

        tp::Stroke header;
        header.id = 1;
        header.mesh = meshPath;
        header.brush = brushJson();
        header.payload.channels = {{"baseColor", {1, 0, 0}}, {"roughness", glm::vec3(0.3f)}, {"metallic", glm::vec3(1.0f)},
                                   {"normal", {0.5f, 0.5f, 1.0f}}, {"height", glm::vec3(0.7f)}, {"ao", glm::vec3(0.5f)},
                                   {"emissive", {0.1f, 0.1f, 0.1f}}};
        const auto mesh = panelMesh(set);
        std::vector<tp::Dab> dabs;
        for (int i = 0; i < 120; ++i)
        {
            brush::Dab d;
            d.center = {-40.0 + i * 0.7, 0.0, 15.0 * std::sin(i * 0.1)};
            d.radius = 5.0;
            tp::Dab r;
            if (tp::recordDab(*mesh, d, r))
                dabs.push_back(r);
        }
        gp::beginLive(sg(), set, "P", std::make_shared<const tp::Stroke>(header));
        // Mouse moves of four dabs each: paint, then the viewport's re-evaluation, timed together.
        double paintMs = 0.0, frameMs = 0.0, worst = 0.0;
        int moves = 0;
        for (size_t i = 0; i < dabs.size(); i += 4)
        {
            const auto t0 = std::chrono::steady_clock::now();
            gp::liveDabs(set.id(), "P", std::span(dabs).subspan(i, std::min<size_t>(4, dabs.size() - i)));
            const auto t1 = std::chrono::steady_clock::now();
            REQUIRE(ui::texLive::waitIdle(60000));
            const auto t2 = std::chrono::steady_clock::now();
            const double p = std::chrono::duration<double, std::milli>(t1 - t0).count(), f = std::chrono::duration<double, std::milli>(t2 - t0).count();
            paintMs += p;
            frameMs += f;
            worst = std::max(worst, f);
            ++moves;
        }
        gp::cancelLive(set.id(), "P");
        const json status = ui::texLive::status();
        std::printf("[perf] %dx%d, 8 channels, %zu dabs over %d moves: paint %.2f ms/move, paint + viewport %.2f ms/move (worst %.2f)\n", res, res,
                    dabs.size(), moves, paintMs / moves, frameMs / moves, worst);
        std::printf("[perf] live levels: %s\n", status.dump().c_str());
        CHECK(moves > 10);
        ui::texLive::shutdown();
    }
}

TEST_CASE("texture paint tools: every starter brush preset paints")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "P", surfaceStroke({{-25, 0, 0}}, 15.0, {{"channels", {{"baseColor", {1.0, 0.5, 0.0}}, {"normal", {0.5, 0.5, 1.0}}}}}));
    const json presets = run("texBrushPresets", {{"reload", true}});
    CHECK(presets["errors"].empty());
    CHECK(presets["presets"].size() >= 20);
    int painted = 0;
    for (const json& entry : presets["presets"])
    {
        const auto p = tex::brushes::find(entry["id"].get<std::string>());
        REQUIRE(p);
        if (p->payload.value("fillBy", std::string()) == "selection")
            continue;
        CAPTURE(p->id);
        json payload = p->payload;
        // Raise and Lower are the paint tool's: it paints them as height.
        if (const std::string mode = payload.value("normalMode", std::string()); mode == "raise" || mode == "lower")
        {
            const double h = payload.value("intensity", 0.2);
            payload = {{"kind", "paint"}, {"channels", {{"height", mode == "lower" ? -h : h}}}};
        }
        const std::string kind = payload.value("kind", std::string("paint"));
        if ((kind == "paint" || kind == "fill") && !payload.contains("channels") && !payload.contains("source"))
            payload["channels"] = {{"baseColor", {0.2, 0.4, 0.8}}};
        if (kind == "paint" && payload.contains("source"))
            payload["channels"] = {{"baseColor", {1.0, 1.0, 1.0}}, {"roughness", 0.5}};
        if (kind == "fill")
            payload["at"] = {25.0, 0.0, 0.0};
        if (kind == "clone" || kind == "heal")
            payload["sourceOffset"] = {0.05, 0.0};
        json stroke = surfaceStroke({{-28, 0, 0}, {-25, 0, 2}, {-22, 0, 4}}, 6.0, payload);
        stroke["brush"] = p->brush;
        if (!p->particles.is_null())
        {
            stroke["particles"] = p->particles;
            stroke["particles"]["down"] = {0.0, 0.0, 1.0};
        }
        if (kind == "fill")
            stroke.erase("dabs");
        json r;
        CHECK_NOTHROW(r = paintStroke("Panel", "Presets", stroke));
        painted += r.is_object() && r.value("pages", 0) > 0;
    }
    CHECK(painted >= 20);
    // Every one of them replays from its record.
    const auto bytes = gp::pageBytes(set.id(), "Presets");
    run("texRasterize", {{"args", {"Panel", "Presets"}}});
    CHECK(gp::pageBytes(set.id(), "Presets") == bytes);
}
