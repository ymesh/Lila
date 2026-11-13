// Hydra viewport renderers: Lila's scene and live USD stages drawn by Storm (on Vulkan, offscreen) through the viewport
// renderer override, read back as pixels and depth, and composited under Lila's overlays. The cases pass without
// checking when no Vulkan device can be created.

#include "UsdTestUtil.h"

#include "core/ImageIO.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "render/FrameState.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "scene/Isolation.h"
#include "scene/Light.h"
#include "ui/Camera.h"
#include "ui/ViewportRenderers.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <chrono>
#include <fstream>
#include <cmath>
#include <optional>
#include <thread>

using namespace lila;
using namespace lila::usdtest;
namespace vr = lila::ui::viewportRenderers;

namespace
{
constexpr int kSize = 128;
constexpr int kPanel = 11;
constexpr const char* kStorm = "hydraStorm";
const glm::dvec3 kEye(0.0, 0.0, 600.0);

/** Sessions deliver from their own threads: frames land on the main queue, like the viewport's redraws. */
template <typename Fn>
vr::Deliver onMain(Fn fn)
{
    return [fn](vr::Frame f) { mainq::post([fn, f = std::move(f)]() mutable { fn(std::move(f)); }); };
}

/** Drains the main queue until done() or ten seconds pass. */
template <typename Fn>
bool pumpUntil(Fn done)
{
    const auto start = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
    {
        mainq::drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

/** A frame as Lila's composite shows it: straight-alpha float RGBA top row first, exposed, depth as view distance in cm. */
vr::Frame resolved(const vr::Frame& in)
{
    const int w = in.width, h = in.height;
    const size_t n = size_t(w) * size_t(h);
    std::vector<float> rgba(n * 4);
    auto decode = [&](std::span<const uint8_t> bytes, render::TextureFormat format) {
        for (size_t i = 0; i < n * 4; ++i)
            rgba[i] = format == render::TextureFormat::RGBA8 ? bytes[i] / 255.0f
                                                              : glm::unpackHalf1x16(reinterpret_cast<const uint16_t*>(bytes.data())[i]);
    };
    if (in.image)
        decode(render::readGpuImage(in.image), in.image->format());
    else if (!in.texels.empty())
        decode(in.texels, in.texelFormat);
    else
        rgba = in.rgba32f;
    std::vector<float> depth = in.depth;
    if (in.depthImage)
    {
        const std::vector<uint8_t> bytes = render::readGpuImage(in.depthImage);
        depth.assign(reinterpret_cast<const float*>(bytes.data()), reinterpret_cast<const float*>(bytes.data()) + n);
    }
    vr::Frame out;
    out.serial = in.serial;
    out.width = w;
    out.height = h;
    out.rgba32f.resize(n * 4);
    if (!depth.empty())
        out.depth.resize(n);
    const glm::dmat4 unproject = in.glDepthProj ? glm::inverse(*in.glDepthProj) : glm::dmat4(1.0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const size_t from = size_t(in.bottomUp ? h - 1 - y : y) * w + x, to = size_t(y) * w + x;
            const float a = rgba[from * 4 + 3];
            const float scale = (in.premultiplied && a > 0.0f && a < 1.0f ? 1.0f / a : 1.0f) * in.exposure;
            for (int c = 0; c < 3; ++c)
                out.rgba32f[to * 4 + c] = rgba[from * 4 + c] * scale;
            out.rgba32f[to * 4 + 3] = a;
            if (depth.empty())
                continue;
            const float d = depth[from];
            if (!in.glDepthProj)
                out.depth[to] = d;
            else if (d < 1.0f && a > 0.0f)
            {
                const glm::dvec4 v = unproject * glm::dvec4(0.0, 0.0, 2.0 * d - 1.0, 1.0);
                out.depth[to] = float(-v.z / v.w);
            }
        }
    return out;
}

/** A Storm session drawing what requests ask for, keeping the newest frame. */
struct Storm
{
    std::unique_ptr<vr::Session> session;
    std::optional<vr::Frame> frame;
    uint64_t serial = 0;
    /** What requests carry as Request::sceneRevision; raised by every request unless held. */
    uint64_t revision = 0;
    bool holdRevision = false;

    Storm()
    {
        const vr::Renderer* r = vr::registry().find(kStorm);
        REQUIRE(r);
        session = r->open(kPanel);
        if (!session)
            MESSAGE("Storm can't run here (no Vulkan device): Hydra cases skipped");
    }

    vr::Request request(const glm::dvec3& eye = kEye)
    {
        vr::Request r;
        r.panel = kPanel;
        r.serial = ++serial;
        r.width = r.height = kSize;
        r.view.view = glm::lookAt(eye, glm::dvec3(eye.x, eye.y, 0.0), glm::dvec3(0.0, 1.0, 0.0));
        r.view.proj = ui::Camera().proj(1.0);
        r.view.eye = eye;
        r.view.width = r.view.height = kSize;
        r.graph = &graph();
        r.sceneRevision = holdRevision ? revision : ++revision;
        // Only the newest request's frames, as the viewport keeps: a refinement of an older one may still arrive.
        r.deliver = onMain([this](vr::Frame f) {
            if (f.serial == serial)
                frame = std::move(f);
        });
        return r;
    }

    /** Renders and returns the frame, written to <temp>/lila_usd_tests/hydra_<name>.png to look at. */
    const vr::Frame& draw(const char* name, const glm::dvec3& eye = kEye, const scene::isolation::Set* isolation = nullptr)
    {
        frame.reset();
        vr::Request r = request(eye);
        r.view.isolation = isolation;
        session->render(r);
        REQUIRE(pumpUntil([this] { return frame.has_value(); }));
        frame = resolved(*frame);
        REQUIRE(frame->width == kSize);
        REQUIRE(frame->rgba32f.size() == size_t(kSize) * kSize * 4);
        image::Image img;
        img.width = img.height = kSize;
        img.rgba.resize(frame->rgba32f.size());
        for (size_t i = 0; i < img.rgba.size(); ++i)
        {
            const float v = std::clamp(frame->rgba32f[i], 0.0f, 1.0f);
            img.rgba[i] = i % 4 == 3 ? 1.0f : (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
        }
        image::write(fs::temp_directory_path() / "lila_usd_tests" / (std::string("hydra_") + name + ".png"), img, 8);
        return *frame;
    }
};

glm::vec4 px(const vr::Frame& f, int x, int y)
{
    const float* p = &f.rgba32f[(size_t(y) * f.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

float depthAt(const vr::Frame& f, int x, int y) { return f.depth.empty() ? -1.0f : f.depth[size_t(y) * f.width + x]; }

/** Covered pixels in the column band [x0, x1). */
int coveredIn(const vr::Frame& f, int x0, int x1)
{
    int n = 0;
    for (int y = 0; y < f.height; ++y)
        for (int x = x0; x < x1; ++x)
            n += px(f, x, y).a > 0.5f;
    return n;
}

/** Pixels in the row band [y0, y1) anything covers, even partly: curves are a pixel or two wide. */
int coveredRows(const vr::Frame& f, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < f.width; ++x)
            n += px(f, x, y).a > 0.2f;
    return n;
}

void cube(const char* name, double size)
{
    run("polyCube", {{"name", name}, {"width", size}, {"height", size}, {"depth", size}});
}

void moveTo(const std::string& name, const glm::dvec3& t) { graph().setValue({node(name).id(), scene::Transform::kTranslate, -1}, t); }
} // namespace

TEST_CASE("hydra: each render delegate on the plugin path is a viewport renderer")
{
    fresh();
    const vr::Renderer* storm = vr::registry().find(kStorm);
    REQUIRE(storm);
    CHECK(storm->name == "Hydra: Storm");
    bool listed = false;
    for (const auto& [id, name] : vr::choices())
        listed |= id == kStorm;
    CHECK(listed);
}

TEST_CASE("hydra: an extension's usdPlugins folders add their render delegates while it is loaded")
{
    fresh();
    const fs::path pkg = fs::temp_directory_path() / "lila_usd_tests" / "TestEngine.lilaext";
    fs::remove_all(pkg);
    fs::create_directories(pkg / "usd" / "hdTestEngine" / "resources");
    std::ofstream(pkg / "extension.json") << R"({"id": "TestEngine", "version": "1.0", "sdk": "1.0", "usdPlugins": ["usd"]})";
    std::ofstream(pkg / "usd" / "hdTestEngine" / "resources" / "plugInfo.json") << R"({"Plugins": [{
        "Info": {"Types": {"HdTestEngineRendererPlugin": {"bases": ["HdRendererPlugin"], "displayName": "Test Engine"}}},
        "LibraryPath": "../../hdTestEngine.dll", "Name": "hdTestEngine", "ResourcePath": "resources", "Root": "..", "Type": "library"}]})";

    auto& em = extensions::ExtensionManager::get();
    CHECK_FALSE(vr::registry().find("hydraTestEngine"));
    REQUIRE(em.load(pkg.string()) == "TestEngine");
    const vr::Renderer* r = vr::registry().find("hydraTestEngine");
    REQUIRE(r);
    CHECK(r->name == "Hydra: Test Engine");
    CHECK(vr::registry().find(kStorm));
    em.unload("TestEngine");
    CHECK_FALSE(vr::registry().find("hydraTestEngine"));
    CHECK(vr::registry().find(kStorm));
}

TEST_CASE("hydra: Storm draws Lila's meshes with their materials, depth in cm along the view axis")
{
    fresh();
    cube("Box", 100.0);
    run("createMaterial", {{"args", {"Red"}}, {"baseColor", {0.8, 0.05, 0.05}}, {"roughness", 0.6}});
    run("assignMaterial", {{"args", {"Red", "Box"}}});
    Storm storm;
    if (!storm.session)
        return;
    const vr::Frame& f = storm.draw("box");
    const glm::vec4 centre = px(f, 64, 64);
    CHECK(centre.a == doctest::Approx(1.0f));
    CHECK(centre.r > 0.05f);
    CHECK(centre.r > 3.0f * centre.g);
    CHECK(centre.r > 3.0f * centre.b);
    CHECK(px(f, 2, 2).a == 0.0f);
    // The front face is 50 cm in front of the origin, 600 cm from the eye.
    CHECK(depthAt(f, 64, 64) == doctest::Approx(550.0f).epsilon(0.01));
    CHECK(depthAt(f, 2, 2) <= 0.0f);

    // Editing the material redraws it.
    graph().setValue({node("Red").id(), scene::material::kBaseColor, -1}, glm::dvec3(0.05, 0.05, 0.8));
    storm.session->sceneChanged(scene::Change::Value, node("Red").id());
    const glm::vec4 blue = px(storm.draw("box_blue"), 64, 64);
    CHECK(blue.b > 3.0f * blue.r);
}

TEST_CASE("hydra: a request with the scene unchanged only moves the camera, and the next scene change syncs")
{
    fresh();
    cube("Still", 100.0);
    Storm storm;
    if (!storm.session)
        return;
    CHECK(px(storm.draw("skip_before"), 64, 64).a == 1.0f);
    // The graph changes but the requests say it didn't: the session draws the scene it has from the new camera.
    storm.holdRevision = true;
    moveTo("Still", {-150.0, 0.0, 0.0});
    const vr::Frame& held = storm.draw("skip_held", {10.0, 0.0, 600.0});
    CHECK(px(held, 64, 64).a == 1.0f);
    storm.holdRevision = false;
    CHECK(px(storm.draw("skip_synced", {10.0, 0.0, 600.0}), 64, 64).a == 0.0f);
}

TEST_CASE("hydra: with Lila's renderer running on the same GPU, Storm's frames reach it without leaving the GPU")
{
    fresh();
    render::Renderer& gpu = render::Renderer::get();
    if (!gpu.running() && !gpu.start())
    {
        MESSAGE("no GPU for Lila's renderer");
        return;
    }
    if (!render::GpuImage::sharingDevice())
    {
        MESSAGE("Lila's device can't share memory: frames go through the CPU");
        return;
    }
    cube("Box", 100.0);
    run("createMaterial", {{"args", {"Red"}}, {"baseColor", {0.8, 0.05, 0.05}}, {"roughness", 0.6}});
    run("assignMaterial", {{"args", {"Red", "Box"}}});
    Storm storm;
    if (!storm.session)
        return;
    // The first frames go through the CPU while the shared images are made.
    std::optional<vr::Frame> raw;
    for (int i = 0; i < 200 && !(raw && raw->image); ++i)
    {
        storm.frame.reset();
        storm.session->render(storm.request());
        REQUIRE(pumpUntil([&] { return storm.frame.has_value(); }));
        raw = storm.frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(raw);
    REQUIRE(raw->image);
    CHECK(raw->depthImage);
    CHECK(raw->rgba32f.empty());
    CHECK(raw->texels.empty());
    CHECK(raw->bottomUp);
    CHECK(raw->premultiplied);
    const vr::Frame f = resolved(*raw);
    const glm::vec4 centre = px(f, 64, 64);
    CHECK(centre.a == doctest::Approx(1.0f));
    CHECK(centre.r > 3.0f * centre.g);
    CHECK(px(f, 2, 2).a == 0.0f);
    CHECK(depthAt(f, 64, 64) == doctest::Approx(550.0f).epsilon(0.01));
    CHECK(depthAt(f, 2, 2) <= 0.0f);

    // Later frames cycle through the shared images and keep matching what is drawn.
    moveTo("Box", {-150.0, 0.0, 0.0});
    for (int i = 0; i < 5; ++i)
        CHECK(px(storm.draw("shared_moved"), 64, 64).a == 0.0f);
}

TEST_CASE("hydra: Lila's lights light the scene in place of the eye light")
{
    fresh();
    cube("Lit", 100.0);
    Storm storm;
    if (!storm.session)
        return;
    const glm::vec4 eyeLit = px(storm.draw("light_none"), 64, 64);
    CHECK(std::abs(eyeLit.r - eyeLit.b) < 0.02f);

    scene::lights::Settings sun;
    sun.kind = scene::lights::Kind::Directional;
    sun.color = {1.0, 0.1, 0.1};
    sun.intensity = 2.0;
    scene::lights::create(graph(), "Sun", scene::kNoNode, sun);
    const glm::vec4 lit = px(storm.draw("light_sun"), 64, 64);
    CHECK(lit.r > 0.1f);
    CHECK(lit.r > 3.0f * lit.b);
}

TEST_CASE("hydra: a dome light's built-in HDRI resolves for Storm, and an unreadable image is left out rather than faulting the GPU")
{
    fresh();
    cube("Domed", 100.0);
    Storm storm;
    if (!storm.session)
        return;
    const std::string hdri = "forest_slope_2k.hdr";
    if (!fs::exists(paths::resourcesDir() / "hdri" / hdri))
    {
        MESSAGE("no built-in " << hdri << ": dome case skipped");
        return;
    }
    scene::lights::Settings dome;
    dome.kind = scene::lights::Kind::Dome;
    dome.texture = hdri;
    scene::lights::create(graph(), "Dome", scene::kNoNode, dome);
    // The dome shows behind everything, so coverage is read from depth.
    CHECK(depthAt(storm.draw("dome_hdri"), 64, 64) > 0.0f);
    // A lost device repeats its last frame, so a frame showing the move proves it is still drawing.
    moveTo("Domed", {-150.0, 0.0, 0.0});
    CHECK(depthAt(storm.draw("dome_hdri_moved"), 64, 64) == 0.0f);

    const fs::path bogus = fs::temp_directory_path() / "lila_usd_tests" / "not_an_image.hdr";
    fs::create_directories(bogus.parent_path());
    std::ofstream(bogus) << "not an image";
    graph().setValue({node("Dome").id(), scene::Light::kTexture, -1}, paths::toUtf8(bogus));
    CHECK(depthAt(storm.draw("dome_bogus"), 64, 64) == 0.0f);
    moveTo("Domed", {0.0, 0.0, 0.0});
    CHECK(depthAt(storm.draw("dome_bogus_moved"), 64, 64) > 0.0f);
}

TEST_CASE("hydra: moving a Lila node moves its Hydra prim, and a new node appears")
{
    fresh();
    cube("Mover", 100.0);
    Storm storm;
    if (!storm.session)
        return;
    const vr::Frame& before = storm.draw("move_before");
    CHECK(px(before, 64, 64).a == 1.0f);
    const int left = coveredIn(before, 0, 40);

    moveTo("Mover", {-150.0, 0.0, 0.0});
    const vr::Frame& after = storm.draw("move_after");
    CHECK(px(after, 64, 64).a == 0.0f);
    CHECK(coveredIn(after, 0, 40) > left + 100);

    cube("Second", 60.0);
    moveTo("Second", {150.0, 0.0, 0.0});
    const vr::Frame& added = storm.draw("move_added");
    CHECK(coveredIn(added, 88, 128) > 100);

    run("delete", {{"args", {"Second"}}});
    CHECK(coveredIn(storm.draw("move_deleted"), 88, 128) == 0);
}

TEST_CASE("hydra: Lila's curves draw as basis curves a few pixels wide")
{
    fresh();
    run("circle", {{"radius", 100.0}, {"normal", {0, 0, 1}}, {"name", "Ring"}});
    Storm storm;
    if (!storm.session)
        return;
    const vr::Frame& f = storm.draw("curve_ring");
    CHECK(px(f, 64, 64).a == 0.0f);
    const int ring = coveredIn(f, 0, kSize);
    CHECK(ring > 60);
    // A line about two pixels wide around the circle, not a filled disc.
    CHECK(ring < 1000);
    CHECK(depthAt(f, 64, 64) <= 0.0f);

    moveTo("Ring", {-300.0, 0.0, 0.0});
    CHECK(coveredIn(storm.draw("curve_moved"), 64, kSize) < ring / 4);
}

TEST_CASE("hydra: with curves in the scene, a camera move alone still skips the scene sync")
{
    fresh();
    run("circle", {{"radius", 100.0}, {"normal", {0, 0, 1}}, {"name", "Ring"}});
    cube("Still", 50.0);
    Storm storm;
    if (!storm.session)
        return;
    CHECK(px(storm.draw("curve_skip_before"), 64, 64).a == 1.0f);
    // Curve widths follow the camera, but only they are refreshed: the moved cube stays where the last sync left it.
    storm.holdRevision = true;
    moveTo("Still", {-150.0, 0.0, 0.0});
    CHECK(px(storm.draw("curve_skip_held", {10.0, 0.0, 300.0}), 64, 64).a == 1.0f);
    storm.holdRevision = false;
    CHECK(px(storm.draw("curve_skip_synced", {10.0, 0.0, 300.0}), 64, 64).a == 0.0f);
}

TEST_CASE("hydra: smooth preview subdivides the cage, and isolate select hides objects and faces per view")
{
    fresh();
    cube("Smooth", 100.0);
    Storm storm;
    if (!storm.session)
        return;
    const int cage = coveredIn(storm.draw("smooth_cage"), 0, kSize);
    graph().setValue({node("Smooth").id(), scene::MeshNode::kSmoothPreview, -1}, int64_t(2));
    const int smooth = coveredIn(storm.draw("smooth_level2"), 0, kSize);
    // The subdivided cube is rounder and smaller than its cage.
    CHECK(smooth < cage * 0.9);
    CHECK(smooth > cage * 0.4);
    graph().setValue({node("Smooth").id(), scene::MeshNode::kSmoothPreview, -1}, int64_t(0));

    cube("Other", 50.0);
    moveTo("Other", {150.0, 0.0, 0.0});
    scene::isolation::Set iso;
    iso.nodes = {node("Other").id()};
    iso.version = 1;
    const vr::Frame& isolated = storm.draw("isolate_other", kEye, &iso);
    CHECK(px(isolated, 64, 64).a == 0.0f);
    CHECK(coveredIn(isolated, 88, kSize) > 50);
    // Another view without the set still shows both.
    CHECK(px(storm.draw("isolate_none"), 64, 64).a == 1.0f);

    // Only the box's faces but the one facing the camera: its inside shows through, 100 cm further away.
    graph().evaluate();
    const scene::MeshPtr box = node("Smooth").as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    scene::isolation::Set faces;
    faces.nodes = {node("Smooth").id()};
    faces.version = 2;
    faces.partial[node("Smooth").id()].capacity = box->faceCapacity();
    for (geo::Index f = 0; f < box->faceCapacity(); ++f)
        if (box->faceValid(f) && box->faceNormal(f).z < 0.9)
            faces.partial[node("Smooth").id()].ids.push_back(f);
    const vr::Frame& holed = storm.draw("isolate_faces", kEye, &faces);
    CHECK(px(holed, 64, 64).a == 1.0f);
    CHECK(depthAt(holed, 64, 64) == doctest::Approx(650.0f).epsilon(0.01));
}

TEST_CASE("hydra: a stage's curves are Lila's, with their unsaved edits, and an emptied mesh leaves its prim to the stage")
{
    fresh();
    const fs::path dir = fs::temp_directory_path() / "lila_usd_tests" / "hydra";
    fs::create_directories(dir);
    const fs::path file = dir / "lines.usda";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "World"
{
    def BasisCurves "Line"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [2]
        point3f[] points = [(-100, 0, 0), (100, 0, 0)]
        float[] widths = [4] (interpolation = "constant")
    }
    def Mesh "Card"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-30, 40, 0), (30, 40, 0), (30, 100, 0), (-30, 100, 0)]
    }
}
)";
    }
    run("usdStageOpen", {{"args", {file.generic_string()}}});
    Storm storm;
    if (!storm.session)
        return;
    const vr::Frame& f = storm.draw("stage_line");
    CHECK(coveredRows(f, 60, 68) > 40);
    const int withCard = coveredRows(f, 40, 57);
    CHECK(withCard > 20);

    // The line is a Lila curve: moving it moves what Hydra draws, without saving.
    scene::Node* lineNode = graph().find("Line");
    REQUIRE(lineNode);
    REQUIRE(lineNode->type().isA("curve"));
    graph().setValue({lineNode->id(), scene::Transform::kTranslate, -1}, glm::dvec3(0.0, -200.0, 0.0));
    const vr::Frame& moved = storm.draw("stage_line_moved");
    CHECK(coveredRows(moved, 60, 68) == 0);
    CHECK(coveredRows(moved, 90, kSize) > 40);

    // Emptying the card's mesh leaves the stage to draw the card.
    scene::Node* card = graph().find("Card");
    REQUIRE(card);
    REQUIRE(card->type().isA("mesh"));
    graph().setValue({card->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>()));
    CHECK(coveredRows(storm.draw("stage_card_emptied"), 40, 57) >= withCard - 5);
}

TEST_CASE("hydra: a live stage draws prims Lila has no objects for, placed by the objects it has")
{
    fresh();
    const fs::path dir = fs::temp_directory_path() / "lila_usd_tests" / "hydra";
    fs::create_directories(dir);
    const fs::path file = dir / "dots.usda";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Z"
)

def Xform "World"
{
    def Xform "Group"
    {
        def Points "Dots"
        {
            point3f[] points = [(-40, -40, 0), (40, -40, 0), (40, 40, 0), (-40, 40, 0), (0, 0, 0)]
            float[] widths = [30, 30, 30, 30, 30]
            color3f[] primvars:displayColor = [(0.1, 0.9, 0.1)]
        }
    }
}
)";
    }
    run("usdStageOpen", {{"args", {file.generic_string()}}});
    Storm storm;
    if (!storm.session)
        return;
    const vr::Frame& f = storm.draw("stage_points");
    // The points are drawn by Storm from the stage: Lila builds no geometry for them.
    const glm::vec4 centre = px(f, 64, 64);
    CHECK(centre.a == doctest::Approx(1.0f));
    CHECK(centre.g > centre.r);
    CHECK(depthAt(f, 64, 64) == doctest::Approx(600.0f).epsilon(0.05));

    // Moving the Lila object of their parent prim moves them, before anything is written to the stage.
    scene::Node* group = graph().find("Group");
    REQUIRE(group);
    graph().setValue({group->id(), scene::Transform::kTranslate, -1}, glm::dvec3(-150.0, 0.0, 0.0));
    const vr::Frame& moved = storm.draw("stage_points_moved");
    CHECK(px(moved, 64, 64).a == 0.0f);
    CHECK(coveredIn(moved, 0, 45) > 20);

    // Hiding the stage hides its prims.
    graph().setValue({node("dots").id(), scene::Transform::kVisibility, -1}, false);
    CHECK(coveredIn(storm.draw("stage_hidden"), 0, kSize) == 0);
}

TEST_CASE("hydra: a progressive delegate (Embree) delivers frames as it converges, then stops")
{
    fresh();
    cube("Traced", 100.0);
    const vr::Renderer* embree = vr::registry().find("hydraEmbree");
    REQUIRE_MESSAGE(embree, "hdEmbree is not on the plugin path");
    CHECK(embree->name == "Hydra: Embree");
    std::unique_ptr<vr::Session> session = embree->open(kPanel);
    REQUIRE(session);

    Storm requests; // only for its request helper; its own session stays idle
    std::vector<vr::Frame> frames;
    vr::Request r = requests.request();
    r.deliver = onMain([&frames](vr::Frame f) { frames.push_back(std::move(f)); });
    session->render(r);
    // Refinements come back through the main queue: pump it until a second passes without one.
    auto quietSince = std::chrono::steady_clock::now();
    size_t seen = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - quietSince < std::chrono::seconds(1) && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
    {
        mainq::drain();
        if (frames.size() != seen)
        {
            seen = frames.size();
            quietSince = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MESSAGE("Embree delivered " << frames.size() << " frames");
    REQUIRE(frames.size() >= 3);
    for (vr::Frame& f : frames)
    {
        CHECK(f.serial == r.serial);
        f = resolved(f);
    }
    const vr::Frame& last = frames.back();
    REQUIRE(last.width == kSize);
    CHECK(px(last, 64, 64).a == doctest::Approx(1.0f));
    CHECK(px(last, 2, 2).a == 0.0f);
    CHECK(depthAt(last, 64, 64) == doctest::Approx(550.0f).epsilon(0.02));

    // Converging: consecutive frames differ less and less.
    auto change = [&](const vr::Frame& a, const vr::Frame& b) {
        double d = 0.0;
        for (size_t i = 0; i < a.rgba32f.size() && i < b.rgba32f.size(); ++i)
            d += std::abs(a.rgba32f[i] - b.rgba32f[i]);
        return d;
    };
    CHECK(change(frames[frames.size() - 2], frames.back()) < change(frames[0], frames[1]));

    // A newer request stops the old one's refinement.
    const size_t before = frames.size();
    vr::Request moved = requests.request({50.0, 0.0, 600.0});
    std::vector<vr::Frame> newer;
    moved.deliver = onMain([&newer](vr::Frame f) { newer.push_back(std::move(f)); });
    session->render(moved);
    for (int i = 0; i < 50; ++i)
    {
        mainq::drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(frames.size() == before);
    CHECK(!newer.empty());
}

TEST_CASE("hydra: delegates other than Storm get smooth preview meshes subdivided, and their render settings")
{
    fresh();
    cube("Round", 100.0);
    const vr::Renderer* embree = vr::registry().find("hydraEmbree");
    REQUIRE(embree);
    std::unique_ptr<vr::Session> session = embree->open(kPanel);
    REQUIRE(session);
    Storm requests;
    // The coverage of the frame Embree settles on, once half a second passes without a new one.
    auto covered = [&] {
        std::optional<vr::Frame> frame;
        uint64_t frames = 0;
        vr::Request r = requests.request();
        r.deliver = onMain([&, serial = r.serial](vr::Frame f) {
            if (f.serial != serial)
                return;
            frame = std::move(f);
            ++frames;
        });
        session->render(r);
        auto quietSince = std::chrono::steady_clock::now();
        const auto start = quietSince;
        for (uint64_t seen = 0; std::chrono::steady_clock::now() - quietSince < std::chrono::milliseconds(500) &&
                                std::chrono::steady_clock::now() - start < std::chrono::seconds(30);)
        {
            mainq::drain();
            if (frames != seen)
            {
                seen = frames;
                quietSince = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(frame);
        return coveredIn(resolved(*frame), 0, kSize);
    };
    const int cage = covered();
    graph().setValue({node("Round").id(), scene::MeshNode::kSmoothPreview, -1}, int64_t(2));
    const int smooth = covered();
    CHECK(smooth < cage * 0.9);
    CHECK(smooth > cage * 0.4);

    const std::vector<vr::Setting> settings = session->settings();
    auto value = [&](const std::string& key) {
        for (const vr::Setting& s : session->settings())
            if (s.key == key)
                return s.value;
        return nlohmann::json();
    };
    REQUIRE(!settings.empty());
    REQUIRE(value("enableSceneColors").is_boolean());
    const bool colors = value("enableSceneColors").get<bool>();
    CHECK(session->setSetting("enableSceneColors", !colors));
    CHECK(value("enableSceneColors") == !colors);
    // A value of another type than the setting's is refused.
    CHECK_FALSE(session->setSetting("enableSceneColors", "yes"));
}

TEST_CASE("hydra: the frame composites under Lila's overlays, depth tested against them")
{
    fresh();
    render::Renderer& gpu = render::Renderer::get();
    if (!gpu.running() && !gpu.start())
    {
        MESSAGE("no GPU for Lila's compositing");
        return;
    }
    cube("Box", 100.0);
    {
        Storm probe;
        if (!probe.session)
            return;
    }
    REQUIRE(vr::select(kPanel, kStorm));
    render::FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.background = glm::vec3(0.0f);
    s.view = glm::lookAt(kEye, glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0));
    s.proj = ui::Camera().proj(1.0);
    s.eye = kEye;
    // A white line through the origin, behind the box's front face.
    s.lines.push_back({glm::vec3(-1000.0f, 0.0f, 0.0f), glm::vec4(1.0f)});
    s.lines.push_back({glm::vec3(1000.0f, 0.0f, 0.0f), glm::vec4(1.0f)});
    Storm req;
    vr::Request r = req.request();
    r.deliver = {};
    REQUIRE(vr::apply(kPanel, r, s));
    for (int i = 0; i < 1000 && !s.externalColor; ++i)
    {
        mainq::drain();
        REQUIRE(vr::apply(kPanel, r, s));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(s.externalColor);
    CHECK(s.externalDepth);
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(gpu.renderOffscreen(s, kSize, kSize, bgra, &err), err);
    auto at = [&](int x, int y) {
        const uint8_t* p = &bgra[(size_t(y) * kSize + x) * 4];
        return glm::ivec3(p[2], p[1], p[0]);
    };
    image::Image img;
    img.width = img.height = kSize;
    for (size_t i = 0; i < size_t(kSize) * kSize; ++i)
        img.rgba.insert(img.rgba.end(), {bgra[i * 4 + 2] / 255.0f, bgra[i * 4 + 1] / 255.0f, bgra[i * 4] / 255.0f, 1.0f});
    image::write(fs::temp_directory_path() / "lila_usd_tests" / "hydra_composited.png", img, 8);

    // The box is Storm's grey, the line hidden where it passes behind the box and drawn either side of it.
    const glm::ivec3 box = at(64, 50);
    CHECK(box.r > 20);
    CHECK(std::abs(box.r - box.b) < 30);
    auto brighter = [&](int x0, int x1, int than) {
        int n = 0;
        for (int y = 58; y < 70; ++y)
            for (int x = x0; x < x1; ++x)
                n += at(x, y).g > than;
        return n;
    };
    CHECK(brighter(54, 74, box.g + 30) == 0);
    CHECK(brighter(0, 30, 100) > 20);
    CHECK(at(5, 20) == glm::ivec3(0));

    // Where Storm drew nothing the background shows, not what the target held from Lila's own frame before it.
    render::FrameState own = s;
    own.externalScene = false;
    own.externalColor = {};
    own.externalDepth = {};
    own.background = glm::vec3(1.0f);
    REQUIRE_MESSAGE(gpu.renderOffscreen(own, kSize, kSize, bgra, &err), err);
    CHECK(at(5, 20).g > 200);
    s.background = glm::vec3(0.25f, 0.5f, 0.75f);
    REQUIRE_MESSAGE(gpu.renderOffscreen(s, kSize, kSize, bgra, &err), err);
    const glm::ivec3 bg = at(5, 20);
    INFO("background " << bg.r << " " << bg.g << " " << bg.b);
    CHECK(std::abs(bg.r - 64) <= 3);
    CHECK(std::abs(bg.g - 128) <= 3);
    CHECK(std::abs(bg.b - 191) <= 3);
    REQUIRE(vr::select(kPanel, vr::kLila));
    vr::release(kPanel);
}
