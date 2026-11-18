#include <doctest/doctest.h>

#include "render/Environment.h"
#include "render/FrameState.h"
#include "render/PathTracer.h"
#include "render/Renderer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace lila::render;

namespace pathtest
{
bool tracing()
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU for path tracing tests");
        return false;
    }
    if (!r.gpuInfo().rayTracing)
    {
        MESSAGE("the GPU can't trace rays");
        return false;
    }
    return true;
}

/** A uniform environment of the given radiance as a Radiance .hdr. */
std::string uniformHdr(float value)
{
    const auto path = std::filesystem::temp_directory_path() / ("lila_pt_uniform_" + std::to_string(int(value * 1000)) + ".hdr");
    std::ofstream f(path, std::ios::binary);
    const int w = 16, h = 8;
    f << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << h << " +X " << w << "\n";
    int e = 0;
    const float m = std::frexp(value, &e);
    const unsigned char rgbe[4] = {(unsigned char)(m * 256.0f), (unsigned char)(m * 256.0f), (unsigned char)(m * 256.0f), (unsigned char)(e + 128)};
    for (int i = 0; i < w * h; ++i)
        f.write(reinterpret_cast<const char*>(rgbe), 4);
    return path.string();
}

std::shared_ptr<RenderMesh> sphere(float radius, int rings, int segments)
{
    auto m = std::make_shared<RenderMesh>();
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= segments; ++s)
        {
            const float th = glm::pi<float>() * float(r) / float(rings), ph = 2.0f * glm::pi<float>() * float(s) / float(segments);
            MeshVertex v{};
            v.normal = {std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            v.pos = v.normal * radius;
            v.uv = {float(s) / float(segments), 1.0f - float(r) / float(rings)};
            v.tangent = {-std::sin(ph), 0.0f, std::cos(ph), 1.0f};
            m->vertices.push_back(v);
        }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s)
        {
            const uint32_t a = uint32_t(r * (segments + 1) + s), b = a + 1, c = a + uint32_t(segments + 1), d = c + 1;
            m->indices.insert(m->indices.end(), {a, c, b, b, c, d});
        }
    m->boundsMin = glm::vec3(-radius);
    m->boundsMax = glm::vec3(radius);
    return m;
}

/** A reversed-Z, infinite far perspective looking at the origin from +Z. */
FrameState sphereFrame(const std::string& env, uint32_t seed, int samplesPerFrame)
{
    FrameState s;
    s.view = glm::lookAt(glm::dvec3(0, 0, 300), glm::dvec3(0), glm::dvec3(0, 1, 0));
    const double f = 1.0 / std::tan(glm::radians(20.0)), nearZ = 1.0;
    glm::dmat4 p(0.0);
    p[0][0] = f;
    p[1][1] = f;
    p[2][3] = -1.0;
    p[3][2] = nearZ;
    s.proj = p;
    s.eye = glm::dvec3(0, 0, 300);
    s.grid = false;
    s.envPath = env;
    s.envBackground = true;
    s.envBackgroundExposure = 1.0f;
    s.envLightSaturation = 1.0f;
    s.groundShadow = 0.0f;
    s.ambientOcclusion = false;
    s.msaaSamples = 1;
    s.taa = false;
    s.post.tonemapper = PostSettings::Linear;
    s.pathTrace.on = true;
    s.pathTrace.bounces = 4;
    s.pathTrace.samplesPerFrame = samplesPerFrame;
    s.pathTrace.maxSamples = 4096;
    s.pathTrace.denoise = false;
    s.pathTrace.seed = seed;
    MeshDraw d;
    d.mesh = sphere(60.0f, 32, 64);
    Material m;
    m.baseColor = glm::vec3(1.0f);
    m.metallic = 0.0f;
    m.specular = 0.0f;
    m.roughness = 1.0f;
    m.flags = MatTwoSided;
    d.materials = {m};
    s.meshes.push_back(d);
    return s;
}

std::vector<float> trace(const FrameState& s, int frames, int size = 64)
{
    Renderer::OffscreenResult out;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreenFrames({s}, size, size, frames, true, out, &err), err);
    REQUIRE(out.hdr.size() == size_t(size) * size * 4);
    return out.hdr;
}
} // namespace pathtest

using namespace pathtest;

TEST_CASE("the environment table is a distribution over the cube's cells")
{
    EnvironmentData env = buildStudioEnvironment();
    int res = 0;
    std::vector<float> t = environmentTable(env, res);
    REQUIRE(res > 0);
    const size_t cells = size_t(6) * res * res;
    REQUIRE(t.size() == cells * 2);
    CHECK(t[cells - 1] == 1.0f);
    double total = 0.0;
    for (size_t c = 0; c < cells; ++c)
    {
        CHECK(t[cells + c] > 0.0f);
        if (c > 0)
            CHECK(t[c] >= t[c - 1]);
        total += t[cells + c];
    }
    CHECK(total == doctest::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("furnace: a white diffuse sphere under a uniform white environment converges to the environment")
{
    if (!tracing())
        return;
    const std::string env = uniformHdr(1.0f);
    std::vector<float> hdr = trace(sphereFrame(env, 7, 16), 16);
    const int size = 64;
    double sum = 0.0;
    int n = 0;
    for (int y = size / 2 - 8; y < size / 2 + 8; ++y)
        for (int x = size / 2 - 8; x < size / 2 + 8; ++x)
        {
            const float* p = &hdr[(size_t(y) * size + x) * 4];
            CHECK(p[3] == doctest::Approx(1.0f));
            for (int c = 0; c < 3; ++c)
                sum += p[c];
            n += 3;
        }
    const double mean = sum / n;
    CHECK(mean == doctest::Approx(1.0).epsilon(0.02));
    // The environment behind the sphere shows as it is.
    CHECK(hdr[0] == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("the path tracer gives the same image for the same seed and samples")
{
    if (!tracing())
        return;
    const std::string env = uniformHdr(1.0f);
    FrameState s = sphereFrame(env, 3, 4);
    s.meshes.edit()[0].materials[0].baseColor = glm::vec3(0.8f, 0.4f, 0.2f);
    s.meshes.edit()[0].materials[0].specular = 0.5f;
    s.meshes.edit()[0].materials[0].roughness = 0.3f;
    const std::vector<float> a = trace(s, 4), b = trace(s, 4);
    CHECK(a == b);
    s.pathTrace.seed = 4;
    const std::vector<float> c = trace(s, 4);
    CHECK(a != c);
}

TEST_CASE("the path tracer converges: more samples bring the image closer to a long render")
{
    if (!tracing())
        return;
    const std::string env = uniformHdr(1.0f);
    FrameState s = sphereFrame(env, 11, 8);
    s.meshes.edit()[0].materials[0].baseColor = glm::vec3(0.5f);
    s.meshes.edit()[0].materials[0].specular = 0.5f;
    s.meshes.edit()[0].materials[0].roughness = 0.4f;
    const std::vector<float> reference = trace(s, 64);
    auto error = [&](const std::vector<float>& img) {
        double e = 0.0;
        for (size_t i = 0; i < img.size(); ++i)
            e += std::abs(img[i] - reference[i]);
        return e / double(img.size());
    };
    s.pathTrace.seed = 12;
    const double few = error(trace(s, 1)), many = error(trace(s, 16));
    CHECK(many < few);
}

TEST_CASE("extended shading: coat and sheen change the path-traced and the shaded view, and conserve energy")
{
    if (!tracing())
        return;
    const std::string env = uniformHdr(1.0f);
    FrameState s = sphereFrame(env, 21, 16);
    const std::vector<float> plain = trace(s, 8);
    Material& m = s.meshes.edit()[0].materials[0];
    m.style.model = ShadingModel::Extended;
    m.ext.coat = 1.0f;
    m.ext.coatRoughness = 0.05f;
    m.ext.sheenColor = glm::vec3(0.5f);
    const std::vector<float> coated = trace(s, 8);
    double a = 0.0, b = 0.0;
    const int size = 64;
    for (int y = size / 2 - 8; y < size / 2 + 8; ++y)
        for (int x = size / 2 - 8; x < size / 2 + 8; ++x)
            for (int c = 0; c < 3; ++c)
            {
                a += plain[(size_t(y) * size + x) * 4 + c];
                b += coated[(size_t(y) * size + x) * 4 + c];
            }
    a /= 16.0 * 16.0 * 3.0;
    b /= 16.0 * 16.0 * 3.0;
    CHECK(plain != coated);
    // A white diffuse base under a coat reflects no more than the light that falls on it.
    CHECK(b < 1.05);

    // The rasterised view shades the Extended model too.
    s.pathTrace.on = false;
    s.post.tonemapper = PostSettings::Neutral;
    std::vector<uint8_t> shadedExt, shadedPlain;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, 64, 64, shadedExt, &err), err);
    s.meshes.edit()[0].materials[0].style.model = ShadingModel::DefaultLit;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreen(s, 64, 64, shadedPlain, &err), err);
    CHECK(shadedExt != shadedPlain);
}
