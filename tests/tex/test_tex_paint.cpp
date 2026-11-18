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
#include "tex/PaintStrokes.h"
#include "tex/TextureSet.h"
#include "ui/TexLive.h"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace textest;
using namespace lila::render;
namespace fs = std::filesystem;
namespace gp = lila::render::paint;

namespace
{
json redStroke(std::vector<glm::dvec3> points, double radius, json channels = {{"baseColor", {1.0, 0.0, 0.0}}, {"roughness", 0.2}})
{
    json dabs = json::array();
    for (const glm::dvec3& p : points)
        dabs.push_back({{"position", {p.x, p.y, p.z}}, {"radius", radius}});
    return {{"space", "surface"}, {"brush", brushJson()}, {"payload", {{"channels", channels}}}, {"dabs", dabs}, {"mesh", "PanelMesh"}};
}
} // namespace

TEST_CASE("texture paint: strokes replay bit for bit, live or recorded")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& a = makeSet("PanelA", 256);
    scene::Node& b = makeSet("PanelB", 256);
    const json stroke = redStroke({{-30, 0, 0}, {-20, 0, 5}, {-10, 0, 8}, {0, 0, 10}, {10, 0, 8}, {25, 0, -5}}, 12.0);
    const json r = paintStroke("PanelA", "P", stroke);
    CHECK(r["dabs"].get<int>() == 6);
    CHECK(r["pages"].get<int>() > 0);
    const auto first = gp::pageBytes(a.id(), "P");
    REQUIRE(!first.empty());

    // The same record painted elsewhere, and the layer rebuilt from its records, give the same bytes.
    paintStroke("PanelB", "P", stroke);
    CHECK(gp::pageBytes(b.id(), "P") == first);
    run("texRasterize", {{"args", {"PanelA", "P"}}});
    CHECK(gp::pageBytes(a.id(), "P") == first);

    // Painted live in three batches and then committed: the same pixels as the record replayed.
    const tex::paint::PaintPtr data = tex::paint::readPaint(a);
    tex::paint::Stroke rec = *data->buffer("P")->strokes.front();
    scene::Node& c = makeSet("PanelC", 256);
    rec.id = 7;
    gp::beginLive(sg(), c, "P", std::make_shared<const tex::paint::Stroke>(rec));
    gp::liveDabs(c.id(), "P", std::span(rec.dabs).subspan(0, 2));
    gp::liveDabs(c.id(), "P", std::span(rec.dabs).subspan(2, 1));
    gp::liveDabs(c.id(), "P", std::span(rec.dabs).subspan(3));
    CHECK(gp::liveActive(c.id(), "P"));
    json recJson = rec.toJson(false);
    paintStroke("PanelC", "P", recJson);
    CHECK_FALSE(gp::liveActive(c.id(), "P"));
    CHECK(gp::pageBytes(c.id(), "P") == first);
}

TEST_CASE("texture paint: queued live batches keep their order through commit, cancel, undo and redo")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& a = makeSet("PanelA", 256);
    const json stroke = redStroke({{-30, 0, 0}, {-20, 0, 5}, {-10, 0, 8}, {0, 0, 10}, {10, 0, 8}, {25, 0, -5}}, 12.0);
    paintStroke("PanelA", "P", stroke);
    const auto whole = gp::pageBytes(a.id(), "P");
    REQUIRE(!whole.empty());
    tex::paint::Stroke rec = *tex::paint::readPaint(a)->buffer("P")->strokes.front();

    // A dab per call, nothing waited on, committed at once: the same bytes as the stroke in one go.
    scene::Node& b = makeSet("PanelB", 256);
    rec.id = 11;
    gp::beginLive(sg(), b, "P", std::make_shared<const tex::paint::Stroke>(rec));
    for (size_t i = 0; i < rec.dabs.size(); ++i)
        gp::liveDabs(b.id(), "P", std::span(rec.dabs).subspan(i, 1));
    paintStroke("PanelB", "P", rec.toJson(false));
    CHECK_FALSE(gp::liveActive(b.id(), "P"));
    CHECK(gp::pageBytes(b.id(), "P") == whole);

    // Undo and redo of the live stroke restore its pages bit for bit.
    UndoStack::get().undo();
    mainq::drain();
    CHECK(gp::pageBytes(b.id(), "P").empty());
    UndoStack::get().redo();
    mainq::drain();
    CHECK(gp::pageBytes(b.id(), "P") == whole);

    // A second stroke cancelled while its dabs are still queued leaves the first stroke's pages as they were.
    tex::paint::Stroke second = rec;
    second.id = 12;
    for (tex::paint::Dab& d : second.dabs)
        d.radius *= 1.5f;
    gp::beginLive(sg(), b, "P", std::make_shared<const tex::paint::Stroke>(second));
    for (size_t i = 0; i < second.dabs.size(); ++i)
        gp::liveDabs(b.id(), "P", std::span(second.dabs).subspan(i, 1));
    gp::cancelLive(b.id(), "P");
    CHECK_FALSE(gp::liveActive(b.id(), "P"));
    CHECK(gp::pageBytes(b.id(), "P") == whole);

    // Queued dabs reach the change listeners once painted.
    int heard = 0;
    const int listener = gp::addChangeListener([&](scene::NodeId id) { heard += id == b.id() ? 1 : 0; });
    gp::beginLive(sg(), b, "P", std::make_shared<const tex::paint::Stroke>(second));
    gp::liveDabs(b.id(), "P", second.dabs);
    gp::settleLive();
    CHECK(heard >= 1);
    CHECK(gp::pageBytes(b.id(), "P") != whole);
    gp::cancelLive(b.id(), "P");
    gp::removeChangeListener(listener);
    CHECK(gp::pageBytes(b.id(), "P") == whole);
}

TEST_CASE("texture paint: seams stay continuous across UV islands")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 512);
    paintStroke("Panel", "P", redStroke({{0, 0, 0}}, 15.0, {{"roughness", 1.0}}));
    const image::Image img = gp::readChannel(set.id(), "P", "roughness");
    REQUIRE(img.width == 512);
    int compared = 0;
    double worst = 0.0;
    for (double z = -12.0; z <= 12.0; z += 2.0)
    {
        const float left = at(img, seamUv(0, -0.01, z))[0], right = at(img, seamUv(1, 0.01, z))[0];
        CAPTURE(z);
        CHECK(left > 0.05f);
        worst = std::max(worst, double(std::abs(left - right)));
        ++compared;
    }
    CHECK(compared > 5);
    CHECK(worst < 0.03);
    // Away from the dab nothing is painted, on either island.
    CHECK(at(img, seamUv(0, -45, -40))[0] == 0.0f);
    CHECK(at(img, seamUv(1, 45, 40))[0] == 0.0f);
    // The padding just outside an island's seam edge is painted as well (dilation by the edge's surface point).
    const glm::vec2 outside = seamUv(0, 0.0, 0.0) + glm::vec2(3.0f / 512.0f, 0.0f);
    CHECK(at(img, outside)[0] > 0.05f);
}

TEST_CASE("texture paint: pages are allocated in proportion to what is painted")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 1024);
    const size_t before = gp::pagesInUse();
    paintStroke("Panel", "P", redStroke({{-25, 0, 0}}, 2.0, {{"roughness", 1.0}}));
    const json small = run("texPaintInfo", {{"args", {"Panel", "P"}}});
    const size_t smallPages = small["pages"].get<size_t>();
    // A dab of radius 2 (about 8 texels at this density) touches a page or four per channel (roughness and the mask).
    CHECK(smallPages >= 2);
    CHECK(smallPages <= 8);
    CHECK(gp::pagesInUse() - before == smallPages);

    paintStroke("Panel", "P", redStroke({{-25, 0, 0}}, 30.0, {{"roughness", 1.0}}));
    const size_t bigPages = run("texPaintInfo", {{"args", {"Panel", "P"}}})["pages"].get<size_t>();
    CHECK(bigPages > smallPages * 3);
    // Never more than the islands' pages: a 1024 tile has 64 pages per channel.
    CHECK(bigPages < 2 * 64);
    const json info = gp::bufferInfo(set.id(), "P");
    CHECK(info["channels"].contains("roughness"));
    CHECK(info["channels"].contains("mask"));
}

TEST_CASE("texture paint: undo restores pages bit for bit")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "P", redStroke({{-20, 0, 0}}, 10.0));
    const auto one = gp::pageBytes(set.id(), "P");
    paintStroke("Panel", "P", redStroke({{-15, 0, 4}, {5, 0, 4}}, 14.0, {{"baseColor", {0.0, 0.0, 1.0}}}));
    const auto two = gp::pageBytes(set.id(), "P");
    CHECK(two != one);
    CHECK(tex::paint::readPaint(set)->buffer("P")->strokes.size() == 2);

    UndoStack::get().undo();
    mainq::drain();
    CHECK(gp::pageBytes(set.id(), "P") == one);
    CHECK(tex::paint::readPaint(set)->buffer("P")->strokes.size() == 1);
    UndoStack::get().redo();
    mainq::drain();
    CHECK(gp::pageBytes(set.id(), "P") == two);

    run("texClearLayer", {{"args", {"Panel", "P"}}});
    mainq::drain();
    CHECK(gp::pageBytes(set.id(), "P").empty());
    UndoStack::get().undo();
    mainq::drain();
    CHECK(gp::pageBytes(set.id(), "P") == two);
}

TEST_CASE("texture paint: re-rasterising at twice the resolution matches a downsample")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    paintStroke("Panel", "P", redStroke({{-30, 0, -10}, {-20, 0, 0}, {-10, 0, 10}, {10, 0, 10}}, 9.0, {{"roughness", 1.0}}));
    const image::Image low = gp::readChannel(set.id(), "P", "roughness");
    run("texRasterize", {{"args", {"Panel", "P"}}, {"resolution", 512}});
    const image::Image high = gp::readChannel(set.id(), "P", "roughness");
    REQUIRE(high.width == 512);
    // Values are straight (unpremultiplied): what composites is value times coverage, so compare that.
    image::Image premul = high;
    for (size_t i = 0; i < premul.rgba.size(); i += 4)
        premul.rgba[i] *= premul.rgba[i + 3];
    const image::Image down = downsample(premul, 2);
    double sum = 0.0, worst = 0.0;
    int n = 0;
    for (int y = 0; y < low.height; ++y)
        for (int x = 0; x < low.width; ++x)
        {
            // Inside the islands only: the padding is narrower in uv at the higher resolution.
            const float u = (x + 0.5f) / low.width, v = 1.0f - (y + 0.5f) / low.height;
            const bool inside = v > 0.12f && v < 0.88f && ((u > 0.07f && u < 0.43f) || (u > 0.57f && u < 0.93f));
            if (!inside)
                continue;
            const float* l = texel(low, x, y);
            const double d = std::abs(double(l[0]) * l[3] - double(texel(down, x, y)[0]));
            sum += d;
            worst = std::max(worst, d);
            ++n;
        }
    CHECK(n > 1000);
    CHECK(sum / n < 0.01);
    CHECK(worst < 0.08);
    CHECK(run("texPaintInfo", {{"args", {"Panel", "P"}}})["width"].get<int>() == 512);
}

TEST_CASE("texture paint: texPaintStroke from JSON, screen space, scene files and flatten")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    // Looking straight down at the panel: screen x runs along world x.
    const json view = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 800}, {"height", 600}};
    json brush = brushJson();
    brush["radius"] = 25.0;
    const json r = run("texPaintStroke", {{"args", {"Panel"}},
                                          {"layer", "P"},
                                          {"points", {{300.0, 300.0, 1.0}, {400.0, 300.0, 0.8}, {500.0, 300.0, 0.5}}},
                                          {"view", view},
                                          {"brush", brush},
                                          {"payload", {{"channels", {{"baseColor", {0.0, 1.0, 0.0}}}}}}});
    CHECK(r["dabs"].get<int>() > 5);
    const tex::paint::PaintPtr data = tex::paint::readPaint(set);
    REQUIRE(data->buffer("P"));
    const tex::paint::Stroke& s = *data->buffer("P")->strokes.front();
    CHECK(s.view.has_value());
    CHECK(s.dabs.size() == size_t(r["dabs"].get<int>()));
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, seamUv(1, 0.5, 0.0))[1] > 0.5f);
    CHECK(at(img, seamUv(0, -45.0, 45.0))[3] == 0.0f);
    const auto painted = gp::pageBytes(set.id(), "P");

    // The records survive an ASCII scene file and rasterise back to the same pages.
    const fs::path file = fs::temp_directory_path() / "lila_tex_paint.lilaa";
    run("saveScene", {{"args", {file.generic_string()}}});
    newScene();
    run("openScene", {{"args", {file.generic_string()}}, {"force", true}});
    mainq::drain();
    scene::Node* loaded = tex::findSet(sg(), "Panel");
    REQUIRE(loaded);
    gp::sync(sg(), *loaded);
    CHECK(gp::pageBytes(loaded->id(), "P") == painted);

    // Flatten keeps the pixels and drops the records; rasterising again restores them from the kept pixels.
    run("texFlattenLayer", {{"args", {"Panel", "P"}}});
    CHECK(tex::paint::readPaint(*loaded)->buffer("P")->strokes.empty());
    CHECK(gp::pageBytes(loaded->id(), "P") == painted);
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(loaded->id(), "P") == painted);
    fs::remove(file);
}

TEST_CASE("texture paint: height, direction strokes and occlusion")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    json d = redStroke({{-35, 0, 0}, {-30, 0, 0}, {-25, 0, 0}, {-20, 0, 0}}, 6.0, json::object());
    d["payload"] = {{"kind", "direction"}, {"tilt", 0.8}};
    paintStroke("Panel", "P", d);
    const image::Image n = gp::readChannel(set.id(), "P", "normal");
    const float* mid = at(n, seamUv(0, -27.5, 0.0));
    CHECK(mid[3] > 0.5f);
    // Tilted along the stroke (+x in object space, +u on the left island): x away from 0.5, y close to it.
    CHECK(std::abs(mid[0] - 0.5f) > 0.1f);
    CHECK(std::abs(mid[1] - 0.5f) < 0.05f);

    paintStroke("Panel", "P", redStroke({{20, 0, 0}}, 8.0, {{"height", 1.0}}));
    const image::Image h = gp::readChannel(set.id(), "P", "height");
    CHECK(at(h, seamUv(1, 20.0, 0.0))[0] > 0.9f);

}

TEST_CASE("texture paint: the stroke's view hides what it can't see")
{
    if (!gpu())
        return;
    newScene();
    // The panel with a small quad floating 20 above its middle, in its own corner of uv.
    geo::Mesh m = seamPanel();
    {
        const std::vector<glm::dvec3> pts = {{-10, 20, -10}, {-10, 20, 10}, {10, 20, 10}, {10, 20, -10}};
        const std::vector<geo::Index> sizes = {4}, idx = {0, 1, 2, 3};
        geo::Mesh lid = geo::Mesh::fromPolygons(pts, sizes, idx);
        auto uv = lid.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (geo::Index c : lid.faceCorners(0))
        {
            const glm::dvec3 p = lid.position(lid.cornerPoint(c));
            uv[c] = {float(0.47 + (p.x + 10.0) / 20.0 * 0.06), float(0.93 + (p.z + 10.0) / 20.0 * 0.05)};
        }
        m.append(lid);
    }
    addMesh("PanelMesh", std::move(m));
    scene::Node& set = makeSet("Panel", 512);
    const json view = {{"eye", {0.0, 300.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 400}, {"height", 400}};
    json brush = brushJson();
    brush["radius"] = 1.0;
    brush["radiusUnit"] = "world";
    brush["worldRadius"] = 30.0;
    brush["falloff"] = "constant";
    run("texPaintStroke", {{"args", {"Panel", "P"}}, {"points", {{200.0, 200.0}}}, {"view", view}, {"brush", brush},
                           {"payload", {{"channels", {{"roughness", 1.0}}}}}});
    const image::Image img = gp::readChannel(set.id(), "P", "roughness");
    REQUIRE(img.width == 512);
    // Under the lid: hidden. Beside it, within reach of the dab on the lid: painted.
    CHECK(at(img, seamUv(0, -5.0, 0.0))[3] == 0.0f);
    CHECK(at(img, seamUv(1, 5.0, 3.0))[3] == 0.0f);
    CHECK(at(img, seamUv(0, -15.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 15.0, 0.0))[3] > 0.5f);

    // Without occlusion the same stroke paints under the lid too.
    run("texPaintStroke", {{"args", {"Panel", "Q"}}, {"points", {{200.0, 200.0}}}, {"view", view}, {"brush", brush},
                           {"stroke", {{"occlusion", false}}}, {"payload", {{"channels", {{"roughness", 1.0}}}}}});
    CHECK(at(gp::readChannel(set.id(), "Q", "roughness"), seamUv(0, -5.0, 0.0))[3] > 0.5f);
}

TEST_CASE("texture paint: a stroke never touches pages only surfaces facing away from its view reach")
{
    if (!gpu())
        return;
    newScene();
    // The panel, and the panel with a quad just below it facing down, its uv in a corner of its own.
    geo::Mesh withFloor = seamPanel();
    {
        const std::vector<glm::dvec3> pts = {{10, -5, -10}, {10, -5, 10}, {-10, -5, 10}, {-10, -5, -10}};
        const std::vector<geo::Index> sizes = {4}, idx = {0, 1, 2, 3};
        geo::Mesh floor = geo::Mesh::fromPolygons(pts, sizes, idx);
        auto uv = floor.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (geo::Index c : floor.faceCorners(0))
        {
            const glm::dvec3 p = floor.position(floor.cornerPoint(c));
            uv[c] = {float(0.47 + (p.x + 10.0) / 20.0 * 0.06), float(0.93 + (p.z + 10.0) / 20.0 * 0.05)};
        }
        withFloor.append(floor);
    }
    addMesh("PanelMesh", seamPanel());
    scene::Node& plain = makeSet("Panel", 512);
    const json view = {{"eye", {0.0, 300.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 400}, {"height", 400}};
    json brush = brushJson();
    brush["radius"] = 1.0;
    brush["radiusUnit"] = "world";
    brush["worldRadius"] = 30.0;
    brush["falloff"] = "constant";
    const json paint = {{"args", {"Panel", "P"}}, {"points", {{200.0, 200.0}}}, {"view", view}, {"brush", brush}, {"stroke", {{"occlusion", false}}},
                        {"payload", {{"channels", {{"roughness", 1.0}}}}}};
    run("texPaintStroke", paint);
    const size_t plainPages = gp::pageBytes(plain.id(), "P").size();
    REQUIRE(plainPages > 0);

    newScene();
    addMesh("PanelMesh", std::move(withFloor));
    scene::Node& floored = makeSet("Panel", 512);
    run("texPaintStroke", paint);
    // The floor is within the dab's reach but faces away: its texels stay unpainted and its pages untouched.
    CHECK(gp::pageBytes(floored.id(), "P").size() == plainPages);
    CHECK(at(gp::readChannel(floored.id(), "P", "roughness"), {0.5f, 0.955f})[3] == 0.0f);
}

TEST_CASE("texture paint: the live viewport composites a painted buffer")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 128);
    tex::Graph g = tex::Graph::makeMaterial("Panel");
    add(g, "Paint", "image", {{"file", tex::paint::imageRef("P", "baseColor")}});
    link(g, "Paint.out", "Output.baseColor");
    tex::writeGraph(sg(), set, g);
    paintStroke("Panel", "P", redStroke({{25, 0, 0}}, 20.0));
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Panel"));
    REQUIRE(ui::texLive::waitIdle());
    GpuImageRef img = ui::texLive::channelImage("Panel", "baseColor");
    REQUIRE(img);
    std::vector<uint8_t> bytes = readGpuImage(img);
    const glm::vec2 uv = seamUv(1, 25.0, 0.0);
    const int x = int(uv.x * img->width()), y = int((1.0f - uv.y) * img->height());
    // RGBA8 sRGB: red painted.
    const uint8_t* t = &bytes[(size_t(y) * img->width() + x) * 4];
    CHECK(t[0] > 200);
    CHECK(t[1] < 40);

    // A second stroke reaches the viewport through the change listener.
    paintStroke("Panel", "P", redStroke({{-25, 0, 0}}, 20.0, {{"baseColor", {0.0, 0.0, 1.0}}}));
    REQUIRE(ui::texLive::waitIdle());
    bytes = readGpuImage(img);
    const glm::vec2 uv2 = seamUv(0, -25.0, 0.0);
    const uint8_t* t2 = &bytes[(size_t(int((1.0f - uv2.y) * img->height())) * img->width() + int(uv2.x * img->width())) * 4];
    CHECK(t2[2] > 200);

    // texEvaluate (CPU or GPU) reads the same paint.
    const json out = run("texEvaluate", {{"args", {"Panel"}}, {"channels", {"baseColor"}}, {"folder", (fs::temp_directory_path() / "lila_tex_paint_eval").generic_string()}});
    REQUIRE(out["files"].size() == 1);
    image::Image file = image::read(fs::path(out["files"][0].get<std::string>()));
    CHECK(at(file, uv)[0] > 0.8f);
}

TEST_CASE("texture paint: a committed or undone stroke redraws the viewport at full size only, never through a preview")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Sharp", 512);
    tex::Graph g = tex::Graph::makeMaterial("Sharp");
    add(g, "Paint", "image", {{"file", tex::paint::imageRef("P", "baseColor")}});
    link(g, "Paint.out", "Output.baseColor");
    tex::writeGraph(sg(), set, g);
    paintStroke("Sharp", "P", redStroke({{25, 0, 0}}, 20.0));
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Sharp"));
    REQUIRE(ui::texLive::waitIdle());
    auto counts = [](const char* key) {
        std::vector<uint64_t> out;
        for (const json& set : ui::texLive::status())
            if (set["set"] == "Sharp")
                for (const json& level : set["tiles"][0]["levels"])
                    out.push_back(level[key].get<uint64_t>());
        return out;
    };
    auto plans = [&] { return counts("plans"); };
    auto swaps = [&] { return counts("swaps"); };
    const std::vector<uint64_t> start = plans();
    // The 256 preview, then the 512 tile.
    REQUIRE(start.size() == 2);

    // Painted live and committed: the preview would write its upsampled images over the sharp ones.
    makeSet("Scratch", 512);
    paintStroke("Scratch", "P", redStroke({{-25, 0, 0}, {-15, 0, 5}}, 15.0, {{"baseColor", {0.0, 0.0, 1.0}}}));
    tex::paint::Stroke rec = *tex::paint::readPaint(*tex::findSet(sg(), "Scratch"))->buffer("P")->strokes.front();
    rec.id = 100;
    REQUIRE(ui::texLive::waitIdle());
    const std::vector<uint64_t> before = plans();
    const std::vector<uint64_t> swapsBefore = swaps();
    gp::beginLive(sg(), set, "P", std::make_shared<const tex::paint::Stroke>(rec));
    gp::liveDabs(set.id(), "P", rec.dabs);
    REQUIRE(ui::texLive::waitIdle());
    paintStroke("Sharp", "P", rec.toJson(false));
    REQUIRE(ui::texLive::waitIdle());
    // Same graph: the full size takes the new paint in place, with no new plan (which would reload and redo the whole
    // stack in one slice), and the preview stays put.
    std::vector<uint64_t> now = plans();
    CHECK(now[0] == before[0]);
    CHECK(now[1] == before[1]);
    CHECK(swaps()[1] > swapsBefore[1]);

    REQUIRE(UndoStack::get().undo());
    mainq::drain();
    REQUIRE(ui::texLive::waitIdle());
    CHECK(plans()[0] == before[0]);

    // An edit to the graph itself still shows a preview first.
    tex::Graph edited = tex::readGraph(set);
    add(edited, "Extra", "color", {});
    tex::writeGraph(sg(), set, edited);
    mainq::drain();
    REQUIRE(ui::texLive::waitIdle());
    CHECK(plans()[0] > before[0]);
    ui::texLive::shutdown();
}

TEST_CASE("texture paint: prewarming builds the stroke pipelines without waiting, and strokes paint after it")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    makeSet("Warm", 128);
    gp::prewarm();
    const json r = paintStroke("Warm", "P", redStroke({{25, 0, 0}}, 20.0));
    CHECK(r["pages"].get<int>() > 0);
}

TEST_CASE("texture paint: the graph a first stroke makes is compiled ahead, so adding and wiring its paint layer compiles nothing")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Ahead", 256);
    ui::texLive::install();
    REQUIRE(ui::texLive::mapsFor("M_Ahead"));
    REQUIRE(ui::texLive::waitIdle());
    std::vector<std::string> channels;
    for (const tex::Channel& c : tex::readSettings(set).channels)
        channels.push_back(c.name);
    channels.push_back(tex::paint::kMaskChannel);

    // What Texture Paint's first stroke does: a new paint layer on top, every channel wired.
    std::optional<tex::Graph> next = gp::wiredGraph(set, "", channels);
    REQUIRE(next);
    ui::texLive::warm("Ahead", *next);
    auto warmed = [] {
        for (const json& s : ui::texLive::status())
            if (s["set"] == "Ahead")
                return s.value("warmed", uint64_t(0));
        return uint64_t(0);
    };
    for (int i = 0; i < 6000 && warmed() == 0; ++i)
    {
        mainq::drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(warmed() == 1);

    const std::string layer = run("texAddLayer", {{"args", {"Ahead", "paint"}}})["id"].get<std::string>();
    gp::wireLayer(set, layer, channels);
    mainq::drain();
    REQUIRE(ui::texLive::waitIdle());
    // The prediction is exactly the graph the edits make, so the plans find every pass compiled.
    CHECK(tex::readGraph(set).toJson() == next->toJson());
    for (const json& s : ui::texLive::status())
        if (s["set"] == "Ahead")
            for (const json& level : s["tiles"][0]["levels"])
            {
                CHECK(level["compiled"].get<size_t>() == 0);
                CHECK(level["pipelinesCreated"].get<size_t>() == 0);
            }
    // Wired already: nothing left to compile ahead.
    CHECK_FALSE(gp::wiredGraph(set, layer, channels));
    ui::texLive::shutdown();
}

TEST_CASE("texture paint: a new layer never takes the id of paint left behind")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    makeSet("Panel", 128);
    // Paint under L1 with no layer of that id, as an undone layer leaves it.
    paintStroke("Panel", "L1", redStroke({{0, 0, 0}}, 15.0, json::object()));
    REQUIRE(run("texStrokes", {{"args", {"Panel", "L1"}}}).size() == 1);
    const std::string id = run("texAddLayer", {{"args", {"Panel", "paint"}}})["id"];
    CHECK(id != "L1");
    // The new layer starts with no paint of its own.
    CHECK_FALSE(run("texPaintInfo", {{"args", {"Panel"}}})["layers"].contains(id));
}

namespace
{
/** Two 100 cm quads side by side; the second's uv island is a fifth the size of the first's. */
geo::Mesh unevenPanel()
{
    const std::vector<glm::dvec3> pts = {{-100, 0, -50}, {0, 0, -50}, {100, 0, -50}, {-100, 0, 50}, {0, 0, 50}, {100, 0, 50}};
    const std::vector<geo::Index> sizes = {4, 4}, idx = {0, 3, 4, 1, 1, 4, 5, 2};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < 2; ++f)
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::dvec3 p = m.position(m.cornerPoint(c));
            const double u = (p.x - (f == 0 ? -100.0 : 0.0)) / 100.0, v = (p.z + 50.0) / 100.0;
            uv[c] = f == 0 ? glm::vec2(float(0.05 + 0.4 * u), float(0.05 + 0.4 * v)) : glm::vec2(float(0.6 + 0.08 * u), float(0.6 + 0.08 * v));
        }
    return m;
}

/** How often the base colour crosses its mean along a row of a uv box, per uv unit. */
double crossingsPerUv(const image::Image& img, float u0, float u1, float v)
{
    std::vector<float> row;
    for (float u = u0; u < u1; u += 1.0f / float(img.width))
        row.push_back(at(img, {u, v})[0]);
    float mean = 0.0f;
    for (float x : row)
        mean += x / float(row.size());
    int n = 0;
    for (size_t i = 1; i < row.size(); ++i)
        n += (row[i - 1] - mean) * (row[i] - mean) < 0.0f ? 1 : 0;
    return n / double(u1 - u0);
}

double islandRatio(const std::string& space)
{
    newScene();
    addMesh("PanelMesh", unevenPanel());
    scene::Node& set = makeSet("Panel", 1024);
    json stroke = redStroke({{-50, 0, 0}, {50, 0, 0}}, 80.0, {{"baseColor", {1.0, 1.0, 1.0}}});
    stroke["payload"]["source"] = {{"node", "perlin"}, {"values", {{"scale", 8}}}, {"resolution", 256}, {"space", space}, {"size", 50.0}};
    paintStroke("Panel", "P", stroke);
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    return crossingsPerUv(img, 0.6f, 0.68f, 0.64f) / crossingsPerUv(img, 0.05f, 0.45f, 0.25f);
}
} // namespace

TEST_CASE("texture paint: a material source keeps its size on the surface, however dense the uvs")
{
    if (!gpu())
        return;
    // The second island is 5x denser in uv: the same pattern on the surface packs 5x more changes into each uv unit.
    const double object = islandRatio("object");
    const double uv = islandRatio("uv");
    INFO("object " << object << ", uv " << uv);
    CHECK(object > 3.0);
    CHECK(uv < 2.0);
}
