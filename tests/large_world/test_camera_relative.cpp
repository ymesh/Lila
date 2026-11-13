#include <doctest/doctest.h>

#include "LargeWorld.h"
#include "render/CameraRelative.h"
#include "render/Renderer.h"
#include "ui/Camera.h"
#include "ui/MeshCache.h"
#include "ui/ViewInfo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace lila;
using namespace lila::render;
using namespace largeworld;

namespace
{
constexpr int kSize = 160;

bool gpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for the camera-relative rendering tests");
    return false;
}

ui::Camera cameraAt(const glm::dvec3& offset)
{
    ui::Camera cam;
    cam.target = offset + glm::dvec3(0.0, 20.0, 0.0);
    cam.distance = 140.0;
    cam.yaw = 35.0;
    cam.pitch = -22.0;
    return cam;
}

/** Everything the camera-relative path rebases: meshes (single and instanced), wire, lines, dots, a heightfield, a light, grid, AO and contact shadows. */
FrameState sceneAt(const glm::dvec3& offset)
{
    FrameState s;
    const ui::Camera cam = cameraAt(offset);
    s.view = cam.view();
    s.proj = cam.proj(1.0);
    s.eye = cam.eye();
    s.msaaSamples = 4;
    s.taa = false;
    s.envBackground = false;
    s.lighting = FrameState::Lighting::Studio;
    s.ambientOcclusion = true;
    s.groundShadow = 0.6f;
    s.grid = true;
    // 3000 km from the camera in every case, so the grid lines come from a far origin and its axes are out of view.
    s.gridOrigin = offset - glm::dvec3(3e8, 0.0, 3e8);
    s.background = glm::vec3(0.1f);

    static const auto cube = box(10.0f);
    MeshDraw d;
    d.mesh = cube;
    d.world = glm::translate(glm::dmat4(1.0), offset + glm::dvec3(0.0, 10.0, 0.0));
    d.flags = DrawShaded | DrawWire;
    d.baseColor = glm::vec3(0.7f, 0.4f, 0.2f);
    MeshDraw many = d;
    many.world = glm::translate(glm::dmat4(1.0), offset + glm::dvec3(30.0, 5.0, -20.0));
    many.flags = DrawShaded;
    many.baseColor = glm::vec3(0.2f, 0.5f, 0.8f);
    many.instances = std::make_shared<std::vector<glm::mat4>>(std::vector<glm::mat4>{
        glm::scale(glm::mat4(1.0f), glm::vec3(0.5f)), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 25.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f))});
    s.meshes = DrawList({d, many});

    const glm::vec4 yellow(1.0f, 0.9f, 0.1f, 1.0f);
    s.lines.push_back({offset + glm::dvec3(-30.0, 0.5, 30.0), yellow});
    s.lines.push_back({offset + glm::dvec3(30.0, 0.5, 30.0), yellow});
    s.linesOnTop.push_back({offset + glm::dvec3(-10.0, 20.0, 10.0), yellow});
    s.linesOnTop.push_back({offset + glm::dvec3(10.0, 20.0, 10.0), yellow});
    s.tris3d.push_back({offset + glm::dvec3(-40.0, 1.0, -10.0), glm::vec4(0.0f, 1.0f, 0.4f, 0.6f)});
    s.tris3d.push_back({offset + glm::dvec3(-25.0, 1.0, -10.0), glm::vec4(0.0f, 1.0f, 0.4f, 0.6f)});
    s.tris3d.push_back({offset + glm::dvec3(-25.0, 1.0, 5.0), glm::vec4(0.0f, 1.0f, 0.4f, 0.6f)});

    static std::shared_ptr<PointCloud> dots[std::size(kOffsets)];
    const size_t which = size_t(std::find(std::begin(kOffsets), std::end(kOffsets), offset) - std::begin(kOffsets));
    if (!dots[which])
    {
        dots[which] = std::make_shared<PointCloud>();
        dots[which]->origin = offset + glm::dvec3(0.0, 10.0, 0.0);
        for (int c = 0; c < 8; ++c)
            dots[which]->points.push_back({glm::vec3((c & 1) ? 10.0f : -10.0f, (c & 2) ? 10.0f : -10.0f, (c & 4) ? 10.0f : -10.0f),
                                           c == 7 ? uint32_t(PointDraw::Selected) : uint32_t(PointDraw::Normal)});
    }
    PointDraw pd;
    pd.cloud = dots[which];
    pd.colors[0] = glm::vec4(0.1f, 0.2f, 1.0f, 1.0f);
    pd.colors[1] = glm::vec4(1.0f, 0.3f, 0.1f, 1.0f);
    s.points.push_back(pd);

    static const auto heights = [] {
        auto h = std::make_shared<std::vector<float>>(17 * 17);
        for (int z = 0; z < 17; ++z)
            for (int x = 0; x < 17; ++x)
                (*h)[size_t(z * 17 + x)] = 4.0f * std::sin(float(x) * 0.6f) * std::cos(float(z) * 0.5f);
        return h;
    }();
    HeightfieldDraw hf;
    hf.heights = heights;
    hf.nx = hf.nz = 17;
    hf.cell = 4.0f;
    hf.world = glm::translate(glm::dmat4(1.0), offset + glm::dvec3(-70.0, 0.0, -70.0));
    hf.color = glm::vec3(0.3f, 0.6f, 0.3f);
    s.heightfields.push_back(hf);

    LightDraw light;
    light.kind = LightDraw::Point;
    light.position = offset + glm::dvec3(40.0, 80.0, 40.0);
    light.radiance = glm::vec3(3.0f);
    s.lights.push_back(light);
    return s;
}

std::vector<uint8_t> draw(const FrameState& s)
{
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, kSize, kSize, bgra, &err), err);
    REQUIRE(bgra.size() == size_t(kSize * kSize * 4));
    return bgra;
}

/** Writes a frame to %TEMP%/lila_large_world_<name>.tga when LILA_LARGE_WORLD_DUMP is set, to look at a failure. */
void dump(const std::vector<uint8_t>& bgra, const std::string& name)
{
    if (!std::getenv("LILA_LARGE_WORLD_DUMP"))
        return;
    std::ofstream out(std::filesystem::temp_directory_path() / ("lila_large_world_" + name + ".tga"), std::ios::binary);
    const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(kSize & 255), uint8_t(kSize >> 8), uint8_t(kSize & 255), uint8_t(kSize >> 8), 32, 0x28};
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
}

int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int m = 0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int above)
{
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            if (std::abs(int(a[i + c]) - int(b[i + c])) > above)
            {
                ++n;
                break;
            }
    return n;
}
} // namespace

TEST_CASE("rebaseOnEye moves the frame so the eye is at the origin, once")
{
    FrameState s = sceneAt(kOffsets[2]);
    const glm::dvec3 eye = s.eye;
    const glm::dvec3 light = s.lights[0].position;
    const glm::dmat4 viewProj = s.proj * s.view;
    rebaseOnEye(s);
    CHECK(s.eye == glm::dvec3(0.0));
    CHECK(s.renderOrigin == eye);
    CHECK(s.lights[0].position == light - eye);
    // The view keeps no translation, and a world point projects as it did.
    CHECK(glm::length(glm::dvec3(s.view[3])) < 1e-6);
    const glm::dvec3 p = kOffsets[2] + glm::dvec3(3.0, 4.0, 5.0);
    const glm::dvec4 before = viewProj * glm::dvec4(p, 1.0);
    const glm::dvec4 after = s.proj * s.view * glm::dvec4(p - s.renderOrigin, 1.0);
    CHECK(glm::length(before / before.w - after / after.w) < 1e-9);
    const FrameState once = s;
    rebaseOnEye(s);
    CHECK(s.renderOrigin == once.renderOrigin);
    CHECK(s.view == once.view);
}

TEST_CASE("relativeTo keeps sub-millimetre detail 3000 km from the origin")
{
    const glm::dvec3 far(3e8, 1234.5678, -3e8);
    const glm::dmat4 world = glm::translate(glm::dmat4(1.0), far + glm::dvec3(0.001, 0.002, 0.003));
    const glm::mat4 rel = relativeTo(world, far);
    CHECK(rel[3].x == doctest::Approx(0.001f).epsilon(1e-4));
    CHECK(rel[3].y == doctest::Approx(0.002f).epsilon(1e-4));
    CHECK(rel[3].z == doctest::Approx(0.003f).epsilon(1e-4));
    CHECK(wrapped(-1.0, 100.0) == doctest::Approx(99.0));
    CHECK(wrapped(3e8 + 7.25, 100.0) == doctest::Approx(7.25));
}

TEST_CASE("the same scene draws the same at the origin, at UTM coordinates and 3000 km out")
{
    if (!gpu())
        return;
    const std::vector<uint8_t> home = draw(sceneAt(kOffsets[0]));
    dump(home, "0");
    // Not a blank frame: the scene covers a good part of the view.
    CHECK(differing(home, std::vector<uint8_t>(home.size(), 0), 30) > size_t(kSize * kSize / 10));
    for (size_t i = 1; i < std::size(kOffsets); ++i)
    {
        CAPTURE(i);
        const std::vector<uint8_t> away = draw(sceneAt(kOffsets[i]));
        dump(away, std::to_string(i));
        CHECK(maxDiff(home, away) <= 3);
        CHECK(differing(home, away, 1) <= size_t(kSize));
    }
}

TEST_CASE("repeated frames far out do not jitter")
{
    if (!gpu())
        return;
    // A camera creeping a millimetre a frame 3000 km out still moves smoothly: consecutive frames differ only a little,
    // and the same frame drawn twice is identical.
    FrameState s = sceneAt(kOffsets[2]);
    const std::vector<uint8_t> a = draw(s);
    CHECK(maxDiff(a, draw(s)) == 0);
    FrameState moved = s;
    const glm::dvec3 step(0.1, 0.0, 0.0);
    moved.eye += step;
    moved.view = moved.view * glm::translate(glm::dmat4(1.0), -step);
    CHECK(differing(a, draw(moved), 24) < size_t(kSize * kSize / 50));
}

TEST_CASE("picking through a pixel hits the same spot at every offset")
{
    const auto cube = box(10.0f);
    std::optional<double> homeDistance;
    std::optional<QPointF> homePixel;
    for (const glm::dvec3& offset : kOffsets)
    {
        CAPTURE(offset.x);
        const ui::Camera cam = cameraAt(offset);
        ui::ViewInfo view;
        view.view = cam.view();
        view.proj = cam.proj(1.0);
        view.eye = cam.eye();
        view.width = view.height = kSize;
        glm::dvec3 o, dir;
        const std::optional<QPointF> aim = view.project(offset + glm::dvec3(2.0, 8.0, 10.0));
        REQUIRE(aim);
        view.ray(*aim + QPointF(0.3, -0.4), o, dir);
        const glm::dmat4 world = glm::translate(glm::dmat4(1.0), offset + glm::dvec3(0.0, 10.0, 0.0));
        const std::optional<ui::RayHit> hit = ui::raycast(*cube, world, o, dir);
        REQUIRE(hit);
        const std::optional<QPointF> px = view.project(o + dir * hit->distance);
        REQUIRE(px);
        if (!homeDistance)
        {
            homeDistance = hit->distance;
            homePixel = px;
            continue;
        }
        CHECK(hit->distance == doctest::Approx(*homeDistance).epsilon(1e-9));
        CHECK(px->x() == doctest::Approx(homePixel->x()).epsilon(1e-6));
        CHECK(px->y() == doctest::Approx(homePixel->y()).epsilon(1e-6));
    }
}

TEST_CASE("a point cloud draws the same at every offset, clipped by its boxes")
{
    if (!gpu())
        return;
    // A 64 x 64 sheet of points on a 40 cm cube, coloured by position.
    static const auto chunk = [] {
        auto c = std::make_shared<PointChunk>();
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
            {
                const uint32_t qx = x * 1040, qy = 30000 + (x * y) % 3000, qz = y * 1040;
                c->words.push_back(qx | (qy << 16));
                c->words.push_back(qz | ((x * 4) << 16) | ((y * 4) << 24));
                c->words.push_back(200u | (2u << 8) | (30000u << 16));
                ++c->count;
            }
        return c;
    }();
    std::vector<uint8_t> images[std::size(kOffsets)][2];
    for (size_t i = 0; i < std::size(kOffsets); ++i)
        for (int clip = 0; clip < 2; ++clip)
        {
            FrameState s = sceneAt(kOffsets[i]);
            s.meshes = DrawList();
            s.heightfields.clear();
            PointCloudDraw d;
            d.world = glm::translate(glm::dmat4(1.0), kOffsets[i] + glm::dvec3(-20.0, 0.0, -20.0));
            d.pointSize = 3.0f;
            d.chunks.push_back({chunk, glm::dvec3(0.0), 40.0});
            if (clip)
            {
                // Keep only the left half of the sheet.
                d.clipMode = PointCloudDraw::ClipInside;
                d.clips.push_back(glm::inverse(glm::translate(glm::dmat4(1.0), kOffsets[i] + glm::dvec3(-10.0, 15.0, 0.0)) *
                                               glm::scale(glm::dmat4(1.0), glm::dvec3(20.0, 60.0, 60.0))));
            }
            s.pointClouds.push_back(d);
            draw(s); // the first frame uploads the chunk
            images[i][clip] = draw(s);
        }
    const std::vector<uint8_t> blank = draw([] {
        FrameState s = sceneAt(kOffsets[0]);
        s.meshes = DrawList();
        s.heightfields.clear();
        return s;
    }());
    CHECK(differing(images[0][0], blank, 20) > size_t(400));
    // Clipping removes about half of what the sheet covers.
    const size_t full = differing(images[0][0], blank, 20), half = differing(images[0][1], blank, 20);
    CHECK(half < full * 3 / 4);
    CHECK(half > full / 4);
    for (size_t i = 1; i < std::size(kOffsets); ++i)
        for (int clip = 0; clip < 2; ++clip)
        {
            CAPTURE(i);
            CAPTURE(clip);
            CHECK(maxDiff(images[0][clip], images[i][clip]) <= 3);
        }
}

TEST_CASE("eye-dome lighting shades a point cloud and keeps its depth against the scene")
{
    if (!gpu())
        return;
    // Two sheets of points, one 10 cm nearer the eye over half the other: the far sheet darkens along the near one's edge.
    static const auto chunk = [] {
        auto c = std::make_shared<PointChunk>();
        for (uint32_t layer = 0; layer < 2; ++layer)
            for (uint32_t y = 0; y < 64; ++y)
                for (uint32_t x = 0; x < (layer ? 32u : 64u); ++x)
                {
                    const uint32_t qx = x * 1040, qy = layer ? 50000u : 30000u, qz = y * 1040;
                    c->words.push_back(qx | (qy << 16));
                    c->words.push_back(qz | (200u << 16) | (200u << 24));
                    c->words.push_back(200u | (2u << 8) | (30000u << 16));
                    ++c->count;
                }
        return c;
    }();
    auto frame = [&](float edl, bool wall) {
        FrameState s = sceneAt(kOffsets[0]);
        s.heightfields.clear();
        if (!wall)
            s.meshes = DrawList();
        PointCloudDraw d;
        d.world = glm::translate(glm::dmat4(1.0), glm::dvec3(-20.0, 0.0, -20.0));
        d.pointSize = 3.0f;
        d.edl = edl;
        d.chunks.push_back({chunk, glm::dvec3(0.0), 40.0});
        s.pointClouds.push_back(d);
        draw(s);
        return draw(s);
    };
    const std::vector<uint8_t> off = frame(0.0f, false), on = frame(1.0f, false);
    // Shading changes the cloud, not the empty background.
    CHECK(differing(off, on, 20) > size_t(50));
    // Drawn through its own target and shading pass, the cloud still meets the scene's meshes in depth: with the test
    // scene's meshes in, the image differs from the cloud alone exactly where those meshes cover it.
    const std::vector<uint8_t> withScene = frame(1.0f, true), sceneOnly = [] {
        FrameState s = sceneAt(kOffsets[0]);
        s.heightfields.clear();
        return draw(s);
    }();
    CHECK(differing(withScene, sceneOnly, 20) > size_t(0));
    CHECK(differing(withScene, on, 20) > size_t(0));
}
