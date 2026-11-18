#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/Compute.h"
#include "render/Displacement.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "render/VulkanContext.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace lila::render;
using lila::json;
namespace dp = lila::render::displacement;

namespace displacetest
{
/** A plane of n x n quads over x, z in [0, size], uv spanning [0, uvSpan] along u and [0, 1] along v, facing +Y. */
std::shared_ptr<RenderMesh> plane(int n, float size, float uvSpan)
{
    auto m = std::make_shared<RenderMesh>();
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            MeshVertex v{};
            const float s = float(i) / float(n), t = float(j) / float(n);
            v.pos = {s * size, 0.0f, t * size};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.uv = {s * uvSpan, t};
            v.uv1 = {t, s};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            m->vertices.push_back(v);
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const uint32_t a = uint32_t(j * (n + 1) + i), b = a + 1, c = a + uint32_t(n + 1), d = c + 1;
            m->indices.insert(m->indices.end(), {a, c, b, b, c, d});
        }
    m->boundsMin = {0.0f, 0.0f, 0.0f};
    m->boundsMax = {size, 0.0f, size};
    return m;
}

std::vector<uint8_t> pattern(int w, int h, float phase)
{
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float v = 0.5f + 0.45f * std::sin(0.71f * float(x) + phase) * std::cos(0.37f * float(y) - phase);
            std::memcpy(&out[(size_t(y) * w + x) * 4], &v, 4);
        }
    return out;
}

bool displaceGpu()
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for displacement tests");
        return false;
    }
    return true;
}

GpuImageRef heightImage(int w, int h, float phase)
{
    GpuImageDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = TextureFormat::R32F;
    desc.name = "test height";
    GpuImageRef img = GpuImage::create(desc);
    std::vector<uint8_t> rgba = pattern(w, h, phase);
    std::vector<uint8_t> r32(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        std::memcpy(&r32[i * 4], &rgba[i * 4], 4);
    img->update(0, 0, w, h, std::move(r32));
    return img;
}

dp::HeightImage cpuImage(const std::vector<uint8_t>& texels, int w, int h)
{
    return {w, h, TextureFormat::R32F, std::span<const uint8_t>(texels)};
}

std::vector<uint8_t> r32Pattern(int w, int h, float phase)
{
    std::vector<uint8_t> rgba = pattern(w, h, phase), r32(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        std::memcpy(&r32[i * 4], &rgba[i * 4], 4);
    return r32;
}
} // namespace displacetest

using namespace displacetest;

TEST_CASE("displacement tessellation splits every triangle into four per level, grouped by section")
{
    auto base = plane(2, 10.0f, 1.0f);
    base->sections = {{0, 0, 12}, {1, 12, 12}};
    auto t = dp::tessellate(*base, 2);
    CHECK(t->indices.size() == base->indices.size() * 16);
    REQUIRE(t->sections.size() == 2);
    REQUIRE(t->displaceRanges.size() == 2);
    CHECK(t->displaceRanges[0].x == 0);
    CHECK(t->displaceRanges[1].x == t->displaceRanges[0].y);
    CHECK(t->displaceRanges[1].x + t->displaceRanges[1].y == t->vertices.size());
    CHECK(t->displaceDirs.size() == t->vertices.size());
    // Each section's triangles index only its own vertices.
    for (size_t si = 0; si < 2; ++si)
        for (uint32_t i = t->sections[si].firstIndex; i < t->sections[si].firstIndex + t->sections[si].indexCount; ++i)
        {
            CHECK(t->indices[i] >= t->displaceRanges[si].x);
            CHECK(t->indices[i] < t->displaceRanges[si].x + t->displaceRanges[si].y);
        }
    for (const glm::vec4& d : t->displaceDirs)
        CHECK(d.y == doctest::Approx(1.0f));
    // Level 0 copies the mesh.
    CHECK(dp::tessellate(*base, 0)->indices.size() == base->indices.size());
}

TEST_CASE("displacement directions are shared by vertices at one point, so hard edges stay closed")
{
    // Two triangles meeting at a right angle with split normals along the shared edge.
    auto m = std::make_shared<RenderMesh>();
    MeshVertex a{}, b{}, c{}, d{}, e{}, f{};
    a.pos = {0, 0, 0}, b.pos = {1, 0, 0}, c.pos = {0, 0, 1};
    a.normal = b.normal = c.normal = {0, 1, 0};
    d.pos = {0, 0, 0}, e.pos = {0, -1, 0}, f.pos = {0, 0, 1};
    d.normal = e.normal = f.normal = {-1, 0, 0};
    m->vertices.assign({a, b, c, d, e, f});
    m->indices = {0, 2, 1, 3, 4, 5};
    auto t = dp::tessellate(*m, 1);
    for (size_t i = 0; i < t->vertices.size(); ++i)
        for (size_t j = 0; j < t->vertices.size(); ++j)
            if (t->vertices[i].pos == t->vertices[j].pos)
                CHECK(glm::length(glm::vec3(t->displaceDirs[i] - t->displaceDirs[j])) < 1e-6f);
}

TEST_CASE("displacement level follows the mesh's size on screen")
{
    auto m = plane(4, 100.0f, 1.0f);
    glm::dmat4 view = glm::lookAt(glm::dvec3(50, 200, 50), glm::dvec3(50, 0, 50), glm::dvec3(0, 0, -1));
    glm::dmat4 proj = glm::perspective(glm::radians(45.0), 1.0, 1.0, 1e5);
    const int near = dp::levelFor(*m, glm::dmat4(1.0), view, proj, 1000.0, 4.0);
    view = glm::lookAt(glm::dvec3(50, 2000, 50), glm::dvec3(50, 0, 50), glm::dvec3(0, 0, -1));
    const int far = dp::levelFor(*m, glm::dmat4(1.0), view, proj, 1000.0, 4.0);
    CHECK(near > far);
    CHECK(near <= 6);
    // The triangle budget caps it.
    CHECK(dp::levelFor(*m, glm::dmat4(1.0), view, proj, 1e6, 1.0, 8, 32 * 16) <= 2);
}

TEST_CASE("displaced vertex positions on the GPU match the CPU reference")
{
    if (!displaceGpu())
        return;
    auto base = plane(3, 20.0f, 1.0f);
    base->sections = {{0, 0, 30}, {0, 30, 24}};
    auto mesh = dp::tessellate(*base, 3);
    const int w = 37, h = 23;
    GpuImageRef img = heightImage(w, h, 0.3f);
    const std::vector<uint8_t> texels = r32Pattern(w, h, 0.3f);
    for (bool uv1 : {false, true})
    {
        const dp::SectionParams params{4.0f, 0.5f, uv1};
        std::vector<MeshVertex> gpuOut = dp::displaceNow(*mesh, {{img, glm::ivec2(-1)}}, params);
        std::vector<MeshVertex> cpuOut(mesh->vertices.begin(), mesh->vertices.end());
        for (const glm::uvec2& r : mesh->displaceRanges)
            dp::displaceCpu(cpuOut, mesh->displaceDirs, r.x, r.y, cpuImage(texels, w, h), params);
        float worst = 0.0f;
        float moved = 0.0f;
        for (size_t i = 0; i < cpuOut.size(); ++i)
        {
            worst = std::max(worst, glm::length(gpuOut[i].pos - cpuOut[i].pos));
            moved = std::max(moved, std::abs(cpuOut[i].pos.y));
            CHECK(gpuOut[i].uv == cpuOut[i].uv);
            CHECK(gpuOut[i].normal == cpuOut[i].normal);
        }
        CHECK(worst < 1e-4f);
        CHECK(moved > 0.5f);
    }
}

TEST_CASE("displacement reads each UDIM tile's own height")
{
    if (!displaceGpu())
        return;
    auto mesh = dp::tessellate(*plane(4, 20.0f, 2.0f), 2);
    const int w = 16, h = 16;
    GpuImageRef left = heightImage(w, h, 0.0f), right = heightImage(w, h, 1.7f);
    const std::vector<uint8_t> leftTexels = r32Pattern(w, h, 0.0f), rightTexels = r32Pattern(w, h, 1.7f);
    const dp::SectionParams params{2.0f, 0.5f, false};
    std::vector<MeshVertex> gpuOut = dp::displaceNow(*mesh, {{left, glm::ivec2(0, 0)}, {right, glm::ivec2(1, 0)}}, params);
    float worst = 0.0f;
    for (size_t i = 0; i < mesh->vertices.size(); ++i)
    {
        MeshVertex v = mesh->vertices[i];
        const float u = v.uv.x;
        // u = 2 and v = 1 fall in tiles with no image.
        if (std::floor(u) >= 2.0f || std::floor(v.uv.y) >= 1.0f)
        {
            CHECK(gpuOut[i].pos == v.pos);
            continue;
        }
        const std::vector<uint8_t>& texels = std::floor(u) < 1.0f ? leftTexels : rightTexels;
        std::vector<MeshVertex> one{v};
        const std::vector<glm::vec4> dir{mesh->displaceDirs[i]};
        dp::displaceCpu(one, dir, 0, 1, cpuImage(texels, w, h), params);
        worst = std::max(worst, glm::length(gpuOut[i].pos - one[0].pos));
    }
    CHECK(worst < 1e-4f);
}

TEST_CASE("the viewport draws a displaced mesh moved by its material's height")
{
    if (!displaceGpu())
        return;
    // An edge-on plane in clip space covers no pixels until its vertices move up and down the screen.
    auto flat = std::make_shared<RenderMesh>();
    const int n = 8;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            MeshVertex v{};
            const float s = float(i) / float(n), t = float(j) / float(n);
            v.pos = {-0.8f + 1.6f * s, 0.0f, 0.2f + 0.6f * t};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.uv = {s, t};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            flat->vertices.push_back(v);
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const uint32_t a = uint32_t(j * (n + 1) + i), b = a + 1, c = a + uint32_t(n + 1), d = c + 1;
            flat->indices.insert(flat->indices.end(), {a, c, b, b, c, d});
        }
    flat->boundsMin = {-0.8f, 0.0f, 0.2f};
    flat->boundsMax = {0.8f, 0.0f, 0.8f};
    auto mesh = dp::tessellate(*flat, 2);
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.taa = false;
    s.lighting = FrameState::Lighting::Flat;
    s.background = {0.0f, 0.0f, 0.0f};
    MeshDraw d;
    d.mesh = mesh;
    Material m;
    m.baseColor = glm::vec3(0.8f);
    m.flags = MatTwoSided;
    d.materials = {m};
    s.meshes.push_back(d);
    auto covered = [](const std::vector<uint8_t>& bgra) {
        size_t c = 0;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            c += (bgra[i] > 40 || bgra[i + 1] > 40 || bgra[i + 2] > 40) ? 1 : 0;
        return c;
    };
    std::vector<uint8_t> plain, displaced;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, 128, 128, plain, &err), err);
    s.meshes.edit()[0].materials[0].heightMap = heightImage(32, 32, 0.4f)->asTexture();
    s.meshes.edit()[0].materials[0].displacementDepth = 0.5f;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, 128, 128, displaced, &err), err);
    CHECK(covered(plain) < 200);
    CHECK(covered(displaced) > 1500);
}

TEST_CASE("displacementPreview turns on per mesh, keeps its detail, and undoes")
{
    lila::ops::execute("newScene", json::object());
    lila::UndoStack::get().clear();
    lila::ops::execute("polyCube", {{"name", "Rock"}});
    json r = lila::ops::execute("displacementPreview", {{"args", {"Rock"}}, {"enable", true}, {"detail", 3.0}});
    REQUIRE(r.size() == 1);
    CHECK(r[0]["enabled"] == true);
    CHECK(r[0]["detail"].get<double>() == doctest::Approx(3.0));
    CHECK(lila::ops::execute("displacementPreview", {{"args", {"Rock"}}})[0]["enabled"] == true);
    lila::UndoStack::get().undo();
    CHECK(lila::ops::execute("displacementPreview", {{"args", {"Rock"}}})[0]["enabled"] == false);
    CHECK_THROWS(lila::ops::execute("displacementPreview", {{"args", {"nothing"}}, {"enable", true}}));
}
