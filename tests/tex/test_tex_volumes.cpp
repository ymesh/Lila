#include <doctest/doctest.h>

#include "render/Atmosphere.h"
#include "render/FrameState.h"
#include "render/Renderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>

using namespace lila::render;

namespace pathtest
{
bool tracing();
std::string uniformHdr(float value);
std::shared_ptr<RenderMesh> sphere(float radius, int rings, int segments);
}

namespace
{
constexpr int kFrameSize = 64;

/** Looking at the origin from +Z with nothing but the given volumes, under a uniform environment of radiance 1. */
FrameState volumeFrame(bool pathTraced)
{
    FrameState s;
    s.view = glm::lookAt(glm::dvec3(0, 0, 300), glm::dvec3(0), glm::dvec3(0, 1, 0));
    const double f = 1.0 / std::tan(glm::radians(20.0));
    glm::dmat4 p(0.0);
    p[0][0] = f;
    p[1][1] = f;
    p[2][3] = -1.0;
    p[3][2] = 1.0;
    s.proj = p;
    s.eye = glm::dvec3(0, 0, 300);
    s.grid = false;
    s.envPath = pathtest::uniformHdr(1.0f);
    s.envBackground = true;
    s.envBackgroundExposure = 1.0f;
    s.envLightSaturation = 1.0f;
    s.groundShadow = 0.0f;
    s.ambientOcclusion = false;
    s.msaaSamples = 1;
    s.taa = false;
    s.post.tonemapper = PostSettings::Linear;
    s.pathTrace.on = pathTraced;
    s.pathTrace.bounces = 16;
    s.pathTrace.samplesPerFrame = 32;
    s.pathTrace.maxSamples = 4096;
    s.pathTrace.denoise = false;
    s.pathTrace.seed = 5;
    if (pathTraced)
    {
        // The path tracer needs a scene to trace; a small sphere behind the camera stays out of the picture.
        MeshDraw d;
        d.mesh = pathtest::sphere(5.0f, 8, 16);
        d.world = glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, 2000.0));
        Material m;
        m.baseColor = glm::vec3(0.5f);
        d.materials = {m};
        s.meshes.push_back(d);
    }
    return s;
}

/** A cube of value 1, 8 voxels of 10 cm a side, centred on the origin. */
VolumeDraw cube()
{
    VolumeDraw v;
    v.dims = glm::ivec3(8);
    v.values = std::make_shared<std::vector<float>>(512, 1.0f);
    v.indexToWorld = glm::translate(glm::dmat4(1.0), glm::dvec3(-35.0)) * glm::scale(glm::dmat4(1.0), glm::dvec3(10.0));
    v.peak = 1.0f;
    return v;
}

std::vector<float> renderVolumes(const FrameState& s, int frames)
{
    Renderer::OffscreenResult out;
    std::string err;
    REQUIRE_MESSAGE(Renderer::get().renderOffscreenFrames({s}, kFrameSize, kFrameSize, frames, true, out, &err), err);
    REQUIRE(out.hdr.size() == size_t(kFrameSize) * kFrameSize * 4);
    return out.hdr;
}

/** Mean of the middle 8 x 8 pixels, all channels. */
double centre(const std::vector<float>& hdr)
{
    double sum = 0.0;
    for (int y = kFrameSize / 2 - 4; y < kFrameSize / 2 + 4; ++y)
        for (int x = kFrameSize / 2 - 4; x < kFrameSize / 2 + 4; ++x)
            for (int c = 0; c < 3; ++c)
                sum += hdr[(size_t(y) * kFrameSize + x) * 4 + c];
    return sum / (8 * 8 * 3);
}

/** Through the cube's middle: 70 cm at full density, then trilinear falloff to half over the 5 cm either side. */
constexpr double kOpticalLength = 70.0 + 2.0 * 5.0 * 0.75;
} // namespace

TEST_CASE("volumes: a pure absorber lets through exp(-optical depth), ray marched and path traced")
{
    if (!pathtest::tracing())
        return;
    for (bool traced : {false, true})
    {
        CAPTURE(traced);
        FrameState s = volumeFrame(traced);
        VolumeDraw v = cube();
        v.density = 0.01f;
        v.scatter = glm::vec3(0.0f);
        v.absorption = glm::vec3(1.0f);
        s.volumes.push_back(v);
        const double expected = std::exp(-0.01 * kOpticalLength);
        CHECK(centre(renderVolumes(s, traced ? 16 : 1)) == doctest::Approx(expected).epsilon(0.06));
    }
}

TEST_CASE("volumes: furnace, a white scattering volume under a uniform environment matches the environment")
{
    if (!pathtest::tracing())
        return;
    for (bool traced : {false, true})
    {
        CAPTURE(traced);
        FrameState s = volumeFrame(traced);
        VolumeDraw v = cube();
        v.density = 0.03f;
        v.scatter = glm::vec3(1.0f);
        v.absorption = glm::vec3(0.0f);
        s.volumes.push_back(v);
        CHECK(centre(renderVolumes(s, traced ? 32 : 1)) == doctest::Approx(1.0).epsilon(0.05));
    }
}

TEST_CASE("volumes: emission adds light where there is density")
{
    if (!pathtest::tracing())
        return;
    FrameState s = volumeFrame(false);
    VolumeDraw v = cube();
    v.density = 0.001f;
    v.scatter = glm::vec3(0.0f);
    v.absorption = glm::vec3(1.0f);
    v.emission = glm::vec3(10.0f, 0.0f, 0.0f);
    s.volumes.push_back(v);
    const std::vector<float> hdr = renderVolumes(s, 1);
    const float* mid = &hdr[(size_t(kFrameSize / 2) * kFrameSize + kFrameSize / 2) * 4];
    CHECK(mid[0] > mid[2] + 0.3f);
    // Outside the cube only the environment shows.
    CHECK(hdr[0] == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("atmosphere: the path tracer and the viewport draw the same sky behind")
{
    if (!pathtest::tracing())
        return;
    std::vector<double> blueOverRed;
    for (bool traced : {false, true})
    {
        FrameState s = volumeFrame(traced);
        AtmosphereFrame a;
        a.sunDir = glm::normalize(glm::dvec3(0.3, 0.6, -0.7));
        a.sunRadiance = glm::vec3(3.0f);
        s.atmosphere = a;
        s.envPath = atmosphereEnvironmentKey(a, s.eye);
        const std::vector<float> hdr = renderVolumes(s, traced ? 4 : 1);
        const float* mid = &hdr[(size_t(kFrameSize / 2) * kFrameSize + kFrameSize / 2) * 4];
        CHECK(mid[2] > mid[0]);
        blueOverRed.push_back(double(mid[2]) / std::max(double(mid[0]), 1e-6));
    }
    CHECK(blueOverRed[1] == doctest::Approx(blueOverRed[0]).epsilon(0.05));
}

TEST_CASE("volumes: the path tracer's denoiser smooths a volume like a surface")
{
    if (!pathtest::tracing())
        return;
    // Noise as the difference between two seeds, on the displayed picture: the denoiser works there, the linear readback is the plain average.
    auto picture = [](FrameState s, uint32_t seed) {
        s.pathTrace.seed = seed;
        Renderer::OffscreenResult out;
        std::string err;
        REQUIRE_MESSAGE(Renderer::get().renderOffscreenFrames({s}, kFrameSize, kFrameSize, 1, false, out, &err), err);
        REQUIRE(out.bgra.size() == size_t(kFrameSize) * kFrameSize * 4);
        return out.bgra;
    };
    std::vector<double> noise;
    for (bool denoise : {false, true})
    {
        FrameState s = volumeFrame(true);
        s.pathTrace.samplesPerFrame = 2;
        s.pathTrace.maxSamples = 2;
        s.pathTrace.denoise = denoise;
        s.post.dither = false;
        s.envPath = pathtest::uniformHdr(0.05f);
        LightDraw sun;
        sun.kind = LightDraw::Directional;
        sun.direction = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f));
        sun.radiance = glm::vec3(20.0f);
        s.lights.push_back(sun);
        VolumeDraw v = cube();
        v.density = 0.004f;
        v.scatter = glm::vec3(0.9f);
        s.volumes.push_back(v);
        const std::vector<uint8_t> a = picture(s, 5), b = picture(s, 6);
        double sq = 0.0;
        int n = 0;
        for (int y = kFrameSize / 2 - 6; y < kFrameSize / 2 + 6; ++y)
            for (int x = kFrameSize / 2 - 6; x < kFrameSize / 2 + 6; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const double d = (double(a[(size_t(y) * kFrameSize + x) * 4 + c]) - double(b[(size_t(y) * kFrameSize + x) * 4 + c])) / 255.0;
                    sq += d * d;
                    ++n;
                }
        noise.push_back(std::sqrt(sq / n));
    }
    CHECK(noise[0] > 0.0);
    CHECK(noise[1] < noise[0] * 0.5);
}

TEST_CASE("volumes: a scene of only volumes path traces")
{
    if (!pathtest::tracing())
        return;
    // Path traced, a lit volume is noisy from pixel to pixel at one sample; the raster view's march is smooth.
    auto spread = [](FrameState s) {
        s.meshes = {};
        s.pathTrace.samplesPerFrame = 1;
        s.pathTrace.maxSamples = 1;
        s.pathTrace.denoise = false;
        s.post.dither = false;
        LightDraw sun;
        sun.kind = LightDraw::Directional;
        sun.direction = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f));
        sun.radiance = glm::vec3(3.0f);
        s.lights.push_back(sun);
        VolumeDraw v = cube();
        v.density = 0.01f;
        v.scatter = glm::vec3(0.9f);
        s.volumes.push_back(v);
        Renderer::OffscreenResult out;
        std::string err;
        REQUIRE_MESSAGE(Renderer::get().renderOffscreenFrames({s}, kFrameSize, kFrameSize, 1, false, out, &err), err);
        double sq = 0.0;
        int n = 0;
        for (int y = kFrameSize / 2 - 6; y < kFrameSize / 2 + 6; ++y)
            for (int x = kFrameSize / 2 - 6; x < kFrameSize / 2 + 5; ++x)
            {
                const double d = (double(out.bgra[(size_t(y) * kFrameSize + x) * 4 + 1]) - double(out.bgra[(size_t(y) * kFrameSize + x + 1) * 4 + 1])) / 255.0;
                sq += d * d;
                ++n;
            }
        return std::sqrt(sq / n);
    };
    const double raster = spread(volumeFrame(false)), traced = spread(volumeFrame(true));
    CHECK(traced > raster * 4.0);
}

namespace pathtest
{
FrameState sphereFrame(const std::string& env, uint32_t seed, int samplesPerFrame);
}

TEST_CASE("atmosphere: path traced samples keep adding up from frame to frame under a sky")
{
    if (!pathtest::tracing())
        return;
    // Noise between two seeds after some one-sample draws: the sum restarting every frame would leave it at one draw's.
    auto noise = [](int draws) {
        std::vector<float> pictures[2];
        for (uint32_t seed : {1u, 2u})
        {
            FrameState s = pathtest::sphereFrame(pathtest::uniformHdr(1.0f), seed, 1);
            AtmosphereFrame a;
            a.sunDir = glm::normalize(glm::dvec3(0.3, 0.6, 0.7));
            a.sunRadiance = glm::vec3(3.0f);
            s.atmosphere = a;
            s.envPath = atmosphereEnvironmentKey(a, s.eye);
            s.pathTrace.maxSamples = 4096;
            Renderer::OffscreenResult out;
            std::string err;
            REQUIRE_MESSAGE(Renderer::get().renderOffscreenFrames({s}, kFrameSize, kFrameSize, draws, true, out, &err), err);
            pictures[seed - 1] = out.hdr;
        }
        double sq = 0.0;
        int n = 0;
        for (int y = kFrameSize / 2 - 8; y < kFrameSize / 2 + 8; ++y)
            for (int x = kFrameSize / 2 - 8; x < kFrameSize / 2 + 8; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const size_t i = (size_t(y) * kFrameSize + x) * 4 + c;
                    sq += double(pictures[0][i] - pictures[1][i]) * double(pictures[0][i] - pictures[1][i]);
                    ++n;
                }
        return std::sqrt(sq / n);
    };
    const double one = noise(1), sixteen = noise(16);
    CHECK(one > 0.0);
    CHECK(sixteen < one * 0.5);
}
