#include <doctest/doctest.h>

#include "core/ImageIO.h"
#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "ui/Camera.h"
#include "ui/ViewportRenderers.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>

using namespace lila;
using namespace lila::render;
namespace vr = lila::ui::viewportRenderers;

namespace
{
constexpr int kSize = 128;
constexpr int kPanel = 7;
const glm::dvec3 kEye(0.0, 0.0, 600.0);

bool gpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for viewport renderer tests");
    return false;
}

/** A closed box, half size h, with face normals and wire edges. */
std::shared_ptr<RenderMesh> box(float h)
{
    auto m = std::make_shared<RenderMesh>();
    for (int axis = 0; axis < 3; ++axis)
        for (int sign = -1; sign <= 1; sign += 2)
        {
            glm::vec3 n(0.0f);
            n[axis] = float(sign);
            const glm::vec3 u = axis == 0 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 v = glm::cross(n, u);
            const uint32_t base = uint32_t(m->vertices.size());
            for (int c = 0; c < 4; ++c)
            {
                MeshVertex vert{};
                vert.pos = (n + u * ((c & 1) ? 1.0f : -1.0f) + v * ((c & 2) ? 1.0f : -1.0f)) * h;
                vert.normal = n;
                vert.tangent = glm::vec4(u, 1.0f);
                m->vertices.push_back(vert);
            }
            m->indices.insert(m->indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
            const glm::vec3 p[4] = {m->vertices[base].pos, m->vertices[base + 1].pos, m->vertices[base + 3].pos, m->vertices[base + 2].pos};
            for (int e = 0; e < 4; ++e)
                m->edgePoints.insert(m->edgePoints.end(), {p[e], p[(e + 1) % 4]});
        }
    m->boundsMin = glm::vec3(-h);
    m->boundsMax = glm::vec3(h);
    return m;
}

/** Looking down -Z at the origin from kEye, black background, nothing but what a case adds. */
FrameState baseFrame(int msaa = 1)
{
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = msaa;
    s.lighting = FrameState::Lighting::Studio;
    s.background = glm::vec3(0.0f);
    s.view = glm::lookAt(kEye, glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0));
    s.proj = ui::Camera().proj(1.0);
    s.eye = kEye;
    return s;
}

void addBox(FrameState& s, uint32_t flags)
{
    MeshDraw d;
    d.mesh = box(50.0f);
    d.flags = flags;
    s.meshes.push_back(d);
}

/** A white line across the view through the origin: depth tested, 600 cm away. */
void addLine(FrameState& s)
{
    const glm::vec4 white(1.0f);
    s.lines.push_back({glm::vec3(-1000.0f, 0.0f, 0.0f), white});
    s.lines.push_back({glm::vec3(1000.0f, 0.0f, 0.0f), white});
}

vr::Request requestFor(const FrameState& s)
{
    vr::Request r;
    r.width = r.height = kSize;
    r.view.view = s.view;
    r.view.proj = s.proj;
    r.view.eye = s.eye;
    r.view.width = r.view.height = kSize;
    return r;
}

/** One colour over the whole frame; depth from fn when given, per pixel. */
vr::Frame solid(int w, int h, glm::u8vec4 color, std::function<float(int x, int y)> depth = {}, uint64_t serial = 0)
{
    vr::Frame f;
    f.width = w;
    f.height = h;
    f.serial = serial;
    f.rgba8.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 4; ++c)
            f.rgba8[i * 4 + c] = color[c];
    if (depth)
    {
        f.depth.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                f.depth[size_t(y) * w + x] = depth(x, y);
    }
    return f;
}

/** What a test renderer saw and how it answers. */
struct Probe
{
    int renders = 0;
    std::vector<vr::Request> requests;
    /** A frame to deliver straight from render, or nullopt to answer later. */
    std::function<std::optional<vr::Frame>(const vr::Request&)> answer;
};

class ProbeSession final : public vr::Session
{
public:
    explicit ProbeSession(Probe& p) : mProbe(p) {}
    void render(const vr::Request& r) override
    {
        ++mProbe.renders;
        mProbe.requests.push_back(r);
        if (mProbe.answer)
            if (std::optional<vr::Frame> f = mProbe.answer(r))
            {
                f->serial = r.serial;
                r.deliver(std::move(*f));
            }
    }

private:
    Probe& mProbe;
};

/** Registers a probe renderer and picks it for kPanel; both go when it does. */
struct ProbeRenderer
{
    Probe probe;
    std::string id;
    explicit ProbeRenderer(std::string rid) : id(std::move(rid))
    {
        vr::add({.id = id, .name = id, .open = [this](int) { return std::make_unique<ProbeSession>(probe); }});
        REQUIRE(vr::select(kPanel, id));
    }
    ~ProbeRenderer()
    {
        vr::remove(id);
        vr::release(kPanel);
    }
};

std::vector<uint8_t> draw(const FrameState& s, const char* name)
{
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, kSize, kSize, bgra, &err), err);
    image::Image img;
    img.width = img.height = kSize;
    img.rgba.resize(size_t(kSize) * kSize * 4);
    for (size_t i = 0; i < size_t(kSize) * kSize; ++i)
    {
        img.rgba[i * 4 + 0] = bgra[i * 4 + 2] / 255.0f;
        img.rgba[i * 4 + 1] = bgra[i * 4 + 1] / 255.0f;
        img.rgba[i * 4 + 2] = bgra[i * 4 + 0] / 255.0f;
        img.rgba[i * 4 + 3] = 1.0f;
    }
    image::write(std::filesystem::temp_directory_path() / "lila_viewport_renderer_tests" / (std::string(name) + ".png"), img, 8);
    return bgra;
}

/** {r, g, b} of a pixel. */
glm::ivec3 at(const std::vector<uint8_t>& bgra, int x, int y)
{
    const uint8_t* p = &bgra[(size_t(y) * kSize + x) * 4];
    return {p[2], p[1], p[0]};
}

/** Pixels in [x0, x1) x [y0, y1) whose green is over 200: the white line and wires over a red image. */
int brightIn(const std::vector<uint8_t>& bgra, int x0, int x1, int y0, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            n += at(bgra, x, y).g > 200;
    return n;
}

bool closeTo(glm::ivec3 got, glm::ivec3 want, int tolerance = 3)
{
    const glm::ivec3 d = glm::abs(got - want);
    return std::max({d.x, d.y, d.z}) <= tolerance;
}
} // namespace

TEST_CASE("viewport renderer: its pixels replace Lila's shading and Lila's overlays draw over them")
{
    if (!gpu())
        return;
    ProbeRenderer pr("test.blue");
    pr.probe.answer = [](const vr::Request& r) { return solid(r.width, r.height, {0, 0, 255, 255}); };

    // MSAA on, as views run it: the image resolves like the scene it replaces.
    FrameState s = baseFrame(4);
    addBox(s, DrawShaded);
    // A HUD-style line in NDC across row 16.
    const glm::vec4 green(0.0f, 1.0f, 0.0f, 1.0f);
    const float row = 16.5f / kSize * 2.0f - 1.0f;
    s.overlay.push_back({glm::vec3(-1.0f, row, 0.5f), green});
    s.overlay.push_back({glm::vec3(1.0f, row, 0.5f), green});
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    CHECK(s.externalScene);
    CHECK(pr.probe.renders == 1);
    const auto img = draw(s, "overlays");

    CHECK(closeTo(at(img, 64, 64), {0, 0, 255}));
    CHECK(closeTo(at(img, 4, 120), {0, 0, 255}));
    CHECK(at(img, 64, 16).g > 200);
    CHECK(at(img, 64, 16).b < 60);
}

TEST_CASE("viewport renderer: its depth hides depth-tested lines and wires behind its surfaces")
{
    if (!gpu())
        return;
    ProbeRenderer pr("test.depth");
    // Red everywhere; the left half is a surface 300 cm away, in front of the line and box; the right half is 900 cm away, behind them.
    pr.probe.answer = [](const vr::Request& r) {
        const int w = r.width;
        return solid(r.width, r.height, {255, 0, 0, 255}, [w](int x, int) { return x < w / 2 ? 300.0f : 900.0f; });
    };
    FrameState s = baseFrame();
    addBox(s, DrawShaded | DrawSelected);
    addLine(s);
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    const auto img = draw(s, "depth");

    CHECK(closeTo(at(img, 20, 20), {255, 0, 0}));
    CHECK(brightIn(img, 0, kSize / 2 - 2, 0, kSize) == 0);
    // The line's right half and the selected box's right edges.
    CHECK(brightIn(img, kSize / 2 + 2, kSize, 60, 68) > 40);
    CHECK(brightIn(img, kSize / 2 + 2, kSize, 0, 60) > 5);

    // Without depth nothing is hidden: the line crosses the whole image.
    pr.probe.answer = [](const vr::Request& r) { return solid(r.width, r.height, {255, 0, 0, 255}); };
    FrameState flat = baseFrame();
    addLine(flat);
    flat.view = glm::translate(flat.view, glm::dvec3(0.0, 0.0, -1.0)); // a new request
    REQUIRE(vr::apply(kPanel, requestFor(flat), flat));
    const auto noDepth = draw(flat, "nodepth");
    CHECK(brightIn(noDepth, 0, kSize / 2 - 2, 60, 68) > 40);
    CHECK(brightIn(noDepth, kSize / 2 + 2, kSize, 60, 68) > 40);
}

TEST_CASE("viewport renderer: switching back to Lila draws Lila's shading again")
{
    if (!gpu())
        return;
    FrameState lila = baseFrame();
    addBox(lila, DrawShaded);
    const auto own = draw(lila, "lila");
    const glm::ivec3 shaded = at(own, 64, 64);
    REQUIRE(shaded.r + shaded.g + shaded.b > 30);

    ProbeRenderer pr("test.switch");
    pr.probe.answer = [](const vr::Request& r) { return solid(r.width, r.height, {0, 255, 0, 255}); };
    FrameState s = baseFrame();
    addBox(s, DrawShaded);
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    CHECK(closeTo(at(draw(s, "switched"), 64, 64), {0, 255, 0}));

    REQUIRE(vr::select(kPanel, vr::kLila));
    CHECK(vr::selected(kPanel) == vr::kLila);
    FrameState back = baseFrame();
    addBox(back, DrawShaded);
    CHECK_FALSE(vr::apply(kPanel, requestFor(back), back));
    CHECK_FALSE(back.externalScene);
    CHECK(closeTo(at(draw(back, "back"), 64, 64), shaded, 1));
}

TEST_CASE("viewport renderer: a late frame shows once it arrives, and a stale one never does")
{
    if (!gpu())
        return;
    ProbeRenderer pr("test.async");
    int redraws = 0;
    vr::setRedraw(kPanel, [&redraws] { ++redraws; });

    // Nothing delivered yet: the background, and none of Lila's shading.
    FrameState s = baseFrame();
    addBox(s, DrawShaded);
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    CHECK(s.externalScene);
    CHECK_FALSE(s.externalColor);
    CHECK(closeTo(at(draw(s, "waiting"), 64, 64), {0, 0, 0}));
    REQUIRE(pr.probe.renders == 1);
    const vr::Request first = pr.probe.requests.back();

    std::thread late([first] { first.deliver(solid(first.width, first.height, {0, 0, 255, 255}, {}, first.serial)); });
    late.join();
    mainq::drain();
    CHECK(redraws >= 1);

    FrameState again = baseFrame();
    addBox(again, DrawShaded);
    REQUIRE(vr::apply(kPanel, requestFor(again), again));
    CHECK(pr.probe.renders == 1); // the same view: no new request
    CHECK(closeTo(at(draw(again, "arrived"), 64, 64), {0, 0, 255}));

    // The camera moves: a new request; its frame arrives, then the old request's frame, which is dropped.
    FrameState moved = baseFrame();
    moved.view = glm::translate(moved.view, glm::dvec3(5.0, 0.0, 0.0));
    REQUIRE(vr::apply(kPanel, requestFor(moved), moved));
    REQUIRE(pr.probe.renders == 2);
    const vr::Request second = pr.probe.requests.back();
    CHECK(second.serial > first.serial);
    second.deliver(solid(kSize, kSize, {0, 255, 0, 255}, {}, second.serial));
    first.deliver(solid(kSize, kSize, {255, 0, 0, 255}, {}, first.serial));
    mainq::drain();
    FrameState shown = moved;
    REQUIRE(vr::apply(kPanel, requestFor(moved), shown));
    CHECK(pr.probe.renders == 2);
    CHECK(closeTo(at(draw(shown, "stale"), 64, 64), {0, 255, 0}));

    // A progressive renderer refines the same request: the newer delivery wins.
    second.deliver(solid(kSize, kSize, {255, 255, 0, 255}, {}, second.serial));
    FrameState refined = moved;
    REQUIRE(vr::apply(kPanel, requestFor(moved), refined));
    CHECK(closeTo(at(draw(refined, "refined"), 64, 64), {255, 255, 0}));

    vr::setRedraw(kPanel, {});
}

TEST_CASE("viewport renderer: GPU images, linear float pixels and frames of another size")
{
    if (!gpu())
        return;
    ProbeRenderer pr("test.formats");
    FrameState s = baseFrame();

    // A GPU image the renderer wrote itself.
    GpuImageRef image = GpuImage::create({.width = 32, .height = 32, .format = TextureFormat::RGBA8, .srgb = true, .name = "test"});
    std::vector<uint8_t> magenta(32 * 32 * 4);
    for (size_t i = 0; i < magenta.size(); i += 4)
    {
        magenta[i] = 255;
        magenta[i + 2] = 255;
        magenta[i + 3] = 255;
    }
    image->update(0, 0, 32, 32, magenta);
    pr.probe.answer = [image](const vr::Request&) {
        vr::Frame f;
        f.image = image;
        return f;
    };
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    CHECK(closeTo(at(draw(s, "gpuimage"), 64, 64), {255, 0, 255}));

    // Linear floats: 0.5 shows as its sRGB encoding.
    pr.probe.answer = [](const vr::Request& r) {
        vr::Frame f;
        f.width = r.width;
        f.height = r.height;
        f.rgba32f.resize(size_t(r.width) * r.height * 4);
        for (size_t i = 0; i < f.rgba32f.size(); i += 4)
        {
            f.rgba32f[i] = 0.5f;
            f.rgba32f[i + 3] = 1.0f;
        }
        return f;
    };
    FrameState linear = baseFrame();
    linear.view = glm::translate(linear.view, glm::dvec3(1.0, 0.0, 0.0));
    REQUIRE(vr::apply(kPanel, requestFor(linear), linear));
    CHECK(closeTo(at(draw(linear, "linear"), 64, 64), {188, 0, 0}));

    // Half the size, left red and right blue: stretched over the whole view.
    pr.probe.answer = [](const vr::Request&) {
        vr::Frame f = solid(kSize / 2, kSize / 2, {255, 0, 0, 255});
        for (int y = 0; y < kSize / 2; ++y)
            for (int x = kSize / 4; x < kSize / 2; ++x)
            {
                uint8_t* p = &f.rgba8[(size_t(y) * (kSize / 2) + x) * 4];
                p[0] = 0;
                p[2] = 255;
            }
        return f;
    };
    FrameState half = baseFrame();
    half.view = glm::translate(half.view, glm::dvec3(2.0, 0.0, 0.0));
    REQUIRE(vr::apply(kPanel, requestFor(half), half));
    const auto img = draw(half, "stretched");
    CHECK(closeTo(at(img, 20, 64), {255, 0, 0}));
    CHECK(closeTo(at(img, 108, 64), {0, 0, 255}));

    // A frame whose buffers don't match its size is dropped, keeping what was shown.
    pr.probe.answer = [](const vr::Request&) {
        vr::Frame f;
        f.width = 4;
        f.height = 4;
        f.rgba8.resize(3);
        return f;
    };
    FrameState bad = baseFrame();
    bad.view = glm::translate(bad.view, glm::dvec3(3.0, 0.0, 0.0));
    REQUIRE(vr::apply(kPanel, requestFor(bad), bad));
    CHECK(closeTo(at(draw(bad, "malformed"), 20, 64), {255, 0, 0}));
}

TEST_CASE("viewport renderer: buffers as a renderer keeps them, bottom up, premultiplied, unexposed, with OpenGL window depth")
{
    if (!gpu())
        return;
    ProbeRenderer pr("test.raw");
    // Stored bottom row first: the view's top half is red at half alpha, its bottom half opaque blue, both premultiplied.
    // The left half is a surface 300 cm away, in front of the line; the right half 900 cm away, behind it.
    const glm::dmat4 gl = glm::perspectiveRH_NO(1.0, 1.0, 1.0, 10000.0);
    auto window = [&gl](double distance) {
        const glm::dvec4 c = gl * glm::dvec4(0.0, 0.0, -distance, 1.0);
        return float(c.z / c.w * 0.5 + 0.5);
    };
    pr.probe.answer = [&](const vr::Request& r) {
        vr::Frame f;
        f.width = r.width;
        f.height = r.height;
        f.texelFormat = TextureFormat::RGBA16F;
        f.texels.resize(size_t(r.width) * r.height * 8);
        f.depth.resize(size_t(r.width) * r.height);
        auto* t = reinterpret_cast<uint16_t*>(f.texels.data());
        for (int sy = 0; sy < r.height; ++sy)
            for (int x = 0; x < r.width; ++x)
            {
                const size_t i = size_t(sy) * r.width + x;
                const glm::vec4 c = sy >= r.height / 2 ? glm::vec4(0.5f, 0.0f, 0.0f, 0.5f) : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
                for (int k = 0; k < 4; ++k)
                    t[i * 4 + k] = glm::packHalf1x16(c[k]);
                f.depth[i] = window(x < r.width / 2 ? 300.0 : 900.0);
            }
        f.bottomUp = true;
        f.premultiplied = true;
        f.exposure = 0.5f;
        f.glDepthProj = gl;
        return f;
    };
    FrameState s = baseFrame();
    addLine(s);
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    const auto img = draw(s, "raw");
    // Red 1 at alpha 0.5, halved by the exposure, over black: 0.25 linear.
    CHECK(closeTo(at(img, 20, 20), {137, 0, 0}));
    CHECK(closeTo(at(img, 20, 108), {0, 0, 188}));
    CHECK(brightIn(img, 0, kSize / 2 - 2, 60, 68) == 0);
    CHECK(brightIn(img, kSize / 2 + 2, kSize, 60, 68) > 40);
}

TEST_CASE("viewport renderer: the example extension rasterises the scene on its own thread, preview then full size")
{
    if (!gpu())
        return;
    ops::execute("newScene", json::object());
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    REQUIRE(em.load(LILA_TEST_VIEWPORT_RENDERER_PACKAGE) == "viewportRenderer");
    REQUIRE(vr::registry().contains("cpuShaded"));
    ops::execute("polyCube", {{"width", 100}, {"height", 100}, {"depth", 100}});
    REQUIRE(vr::select(kPanel, "cpuShaded"));
    // Redraws earlier tests posted would otherwise count as this renderer's frames.
    mainq::drain();
    int frames = 0;
    vr::setRedraw(kPanel, [&frames] { ++frames; });

    FrameState s = baseFrame();
    addLine(s);
    REQUIRE(vr::apply(kPanel, requestFor(s), s));
    // A quarter-size preview, then the full frame, both from its worker thread.
    FrameState shown = baseFrame();
    addLine(shown);
    for (int i = 0; i < 1000 && !(shown.externalColor && shown.externalColor->width == kSize); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        mainq::drain();
        shown = baseFrame();
        addLine(shown);
        REQUIRE(vr::apply(kPanel, requestFor(shown), shown));
    }
    CHECK(frames >= 2);
    REQUIRE(shown.externalColor);
    CHECK(shown.externalColor->width == kSize);
    REQUIRE(shown.externalDepth);
    const auto img = draw(shown, "example");

    // The cube's front face faces the camera: tinted by its +Z normal at full brightness. The background stays Lila's.
    CHECK(closeTo(at(img, 64, 50), {128, 128, 255}, 4));
    CHECK(closeTo(at(img, 4, 4), {0, 0, 0}));
    // The line 600 cm away passes behind the cube's front face (550 cm) and shows either side of it.
    CHECK(brightIn(img, 54, 74, 60, 68) == 0);
    CHECK(brightIn(img, 0, 30, 60, 68) > 20);

    vr::setRedraw(kPanel, {});
    em.unload("viewportRenderer");
    CHECK_FALSE(vr::registry().contains("cpuShaded"));
    CHECK(vr::selected(kPanel) == vr::kLila);
    vr::release(kPanel);
}
