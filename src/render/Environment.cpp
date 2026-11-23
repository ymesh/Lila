#include "render/Environment.h"

#include "core/Profile.h"

#include <glm/gtc/packing.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lila::render
{
namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr int kCubeSize = 512;
constexpr int kMaxMips = 6;     // 512 down to 16: roughness 1 still has plenty of texels at 16
constexpr uint32_t kPrefilterSamples = 128; // at mip 1, doubled per mip so bright softboxes never ghost

/** Float RGB cube, faces back to back in Vulkan order. */
struct Cube
{
    int size = 0;
    std::vector<glm::vec3> px;

    explicit Cube(int n = 0) : size(n), px(size_t(n) * size_t(n) * 6) {}
    glm::vec3& at(int face, int x, int y) { return px[(size_t(face) * size_t(size) + size_t(y)) * size_t(size) + size_t(x)]; }
    const glm::vec3& at(int face, int x, int y) const
    {
        return px[(size_t(face) * size_t(size) + size_t(y)) * size_t(size) + size_t(x)];
    }
};

/** Direction through face coordinates u, v in [-1, 1] (v down), the inverse of Vulkan's cube face selection. */
glm::vec3 faceDir(int face, float u, float v)
{
    switch (face)
    {
    case 0: return glm::normalize(glm::vec3(1.0f, -v, -u));
    case 1: return glm::normalize(glm::vec3(-1.0f, -v, u));
    case 2: return glm::normalize(glm::vec3(u, 1.0f, v));
    case 3: return glm::normalize(glm::vec3(u, -1.0f, -v));
    case 4: return glm::normalize(glm::vec3(u, -v, 1.0f));
    default: return glm::normalize(glm::vec3(-u, -v, -1.0f));
    }
}

void dirToFace(const glm::vec3& d, int& face, float& s, float& t)
{
    glm::vec3 a = glm::abs(d);
    float sc, tc, ma;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0 ? 0 : 1;
        sc = d.x > 0 ? -d.z : d.z;
        tc = -d.y;
        ma = a.x;
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0 ? 2 : 3;
        sc = d.x;
        tc = d.y > 0 ? d.z : -d.z;
        ma = a.y;
    }
    else
    {
        face = d.z > 0 ? 4 : 5;
        sc = d.z > 0 ? d.x : -d.x;
        tc = -d.y;
        ma = a.z;
    }
    s = (sc / ma + 1.0f) * 0.5f;
    t = (tc / ma + 1.0f) * 0.5f;
}

/** Bilinear within the face; good enough as a source when many samples get averaged. */
glm::vec3 sample(const Cube& c, const glm::vec3& d)
{
    int face;
    float s, t;
    dirToFace(d, face, s, t);
    float x = std::clamp(s * float(c.size) - 0.5f, 0.0f, float(c.size - 1));
    float y = std::clamp(t * float(c.size) - 0.5f, 0.0f, float(c.size - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, c.size - 1), y1 = std::min(y0 + 1, c.size - 1);
    float fx = x - float(x0), fy = y - float(y0);
    glm::vec3 top = glm::mix(c.at(face, x0, y0), c.at(face, x1, y0), fx);
    glm::vec3 bottom = glm::mix(c.at(face, x0, y1), c.at(face, x1, y1), fx);
    return glm::mix(top, bottom, fy);
}

glm::vec3 sampleLod(const std::vector<Cube>& chain, const glm::vec3& d, float lod)
{
    lod = std::clamp(lod, 0.0f, float(chain.size() - 1));
    int l0 = int(lod);
    int l1 = std::min(l0 + 1, int(chain.size()) - 1);
    float f = lod - float(l0);
    glm::vec3 a = sample(chain[size_t(l0)], d);
    return f > 0.0f ? glm::mix(a, sample(chain[size_t(l1)], d), f) : a;
}

float areaElement(float x, float y)
{
    return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0f));
}

float texelSolidAngle(int size, int x, int y)
{
    float inv = 1.0f / float(size);
    float u = (float(x) + 0.5f) * 2.0f * inv - 1.0f;
    float v = (float(y) + 0.5f) * 2.0f * inv - 1.0f;
    float x0 = u - inv, x1 = u + inv, y0 = v - inv, y1 = v + inv;
    return areaElement(x0, y0) - areaElement(x0, y1) - areaElement(x1, y0) + areaElement(x1, y1);
}

/** Fills a cube from a radiance function, averaging ss x ss samples per texel. */
Cube renderCube(int size, int ss, const std::function<glm::vec3(const glm::vec3&)>& radiance)
{
    LILA_ZONE();

    Cube c(size);
    tbb::parallel_for(0, 6 * size, [&](int row) {
        int face = row / size, y = row % size;
        for (int x = 0; x < size; ++x)
        {
            glm::vec3 sum(0.0f);
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i)
                {
                    float u = (float(x) + (float(i) + 0.5f) / float(ss)) / float(size) * 2.0f - 1.0f;
                    float v = (float(y) + (float(j) + 0.5f) / float(ss)) / float(size) * 2.0f - 1.0f;
                    sum += radiance(faceDir(face, u, v));
                }
            c.at(face, x, y) = sum / float(ss * ss);
        }
    });
    return c;
}

std::vector<Cube> downsampleChain(Cube base)
{
    LILA_ZONE();

    std::vector<Cube> chain;
    chain.push_back(std::move(base));
    while (chain.back().size > 1)
    {
        const Cube& src = chain.back();
        Cube dst(src.size / 2);
        tbb::parallel_for(0, 6 * dst.size, [&](int row) {
            int face = row / dst.size, y = row % dst.size;
            for (int x = 0; x < dst.size; ++x)
                dst.at(face, x, y) = (src.at(face, 2 * x, 2 * y) + src.at(face, 2 * x + 1, 2 * y) + src.at(face, 2 * x, 2 * y + 1) +
                                      src.at(face, 2 * x + 1, 2 * y + 1)) *
                                     0.25f;
        });
        chain.push_back(std::move(dst));
    }
    return chain;
}

glm::vec2 hammersley(uint32_t i, uint32_t n)
{
    uint32_t b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return {float(i) / float(n), float(b) * 2.3283064365386963e-10f};
}

/** GGX half vector around +Z for alpha a. */
glm::vec3 sampleGgx(glm::vec2 xi, float a)
{
    float phi = 2.0f * kPi * xi.x;
    float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}

float dGgx(float nh, float a)
{
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

float vSmithGgx(float nv, float nl, float a)
{
    float a2 = a * a;
    float gv = nl * std::sqrt(nv * nv * (1.0f - a2) + a2);
    float gl = nv * std::sqrt(nl * nl * (1.0f - a2) + a2);
    return 0.5f / std::max(gv + gl, 1e-5f);
}

/** GGX prefilter with N = V = R, sampling a lower source mip per sample to keep the noise down (filtered IS). */
Cube prefilter(const std::vector<Cube>& src, int size, float roughness, uint32_t samples)
{
    LILA_ZONE();

    struct Tap
    {
        glm::vec3 l; // tangent space, +Z is the normal
        float lod;
    };
    float a = roughness * roughness;
    float texelSa = 4.0f * kPi / (6.0f * float(src[0].size) * float(src[0].size));
    std::vector<Tap> taps;
    for (uint32_t i = 0; i < samples; ++i)
    {
        glm::vec3 h = sampleGgx(hammersley(i, samples), a);
        glm::vec3 l = 2.0f * h.z * h - glm::vec3(0, 0, 1);
        if (l.z <= 0.0f)
            continue;
        float pdf = dGgx(h.z, a) * 0.25f;
        float sampleSa = 1.0f / (float(samples) * pdf + 1e-6f);
        taps.push_back({l, 0.5f * std::log2(sampleSa / texelSa) + 1.0f});
    }

    Cube c(size);
    tbb::parallel_for(0, 6 * size, [&](int row) {
        int face = row / size, y = row % size;
        for (int x = 0; x < size; ++x)
        {
            glm::vec3 n = faceDir(face, (float(x) + 0.5f) / float(size) * 2.0f - 1.0f, (float(y) + 0.5f) / float(size) * 2.0f - 1.0f);
            glm::vec3 up = std::abs(n.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            glm::vec3 t = glm::normalize(glm::cross(up, n));
            glm::vec3 b = glm::cross(n, t);
            glm::vec3 sum(0.0f);
            float weight = 0.0f;
            for (const Tap& tap : taps)
            {
                glm::vec3 l = t * tap.l.x + b * tap.l.y + n * tap.l.z;
                sum += sampleLod(src, l, tap.lod) * tap.l.z;
                weight += tap.l.z;
            }
            c.at(face, x, y) = sum / std::max(weight, 1e-6f);
        }
    });
    return c;
}

std::array<glm::vec4, 9> projectSh(const Cube& c)
{
    LILA_ZONE();

    std::array<glm::vec3, 9> sh{};
    for (int face = 0; face < 6; ++face)
        for (int y = 0; y < c.size; ++y)
            for (int x = 0; x < c.size; ++x)
            {
                glm::vec3 d = faceDir(face, (float(x) + 0.5f) / float(c.size) * 2.0f - 1.0f, (float(y) + 0.5f) / float(c.size) * 2.0f - 1.0f);
                glm::vec3 l = c.at(face, x, y) * texelSolidAngle(c.size, x, y);
                float basis[9] = {0.282095f,
                                  0.488603f * d.y,
                                  0.488603f * d.z,
                                  0.488603f * d.x,
                                  1.092548f * d.x * d.y,
                                  1.092548f * d.y * d.z,
                                  0.315392f * (3.0f * d.z * d.z - 1.0f),
                                  1.092548f * d.x * d.z,
                                  0.546274f * (d.x * d.x - d.y * d.y)};
                for (int i = 0; i < 9; ++i)
                    sh[size_t(i)] += l * basis[i];
            }
    // Cosine lobe convolution per band (pi, 2pi/3, pi/4), then / pi so the shader multiplies by albedo directly.
    const float band[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    std::array<glm::vec4, 9> out{};
    for (int i = 0; i < 9; ++i)
        out[size_t(i)] = glm::vec4(sh[size_t(i)] * band[i], 0.0f);
    return out;
}

uint16_t half(float v)
{
    return uint16_t(glm::packHalf1x16(std::min(v, 65000.0f)));
}

EnvironmentData finish(Cube base)
{
    LILA_ZONE();

    EnvironmentData env;
    env.size = base.size;
    int mips = std::min(kMaxMips, int(std::log2(float(base.size))) + 1);
    std::vector<Cube> chain = downsampleChain(std::move(base));
    const Cube& shSource = chain[std::min(chain.size() - 1, size_t(std::max(0, int(std::log2(float(env.size))) - 6)))];
    env.sh = projectSh(shSource);

    env.mips.resize(size_t(mips));
    for (int m = 0; m < mips; ++m)
    {
        Cube filtered = m == 0 ? chain[0] : prefilter(chain, env.size >> m, float(m) / float(mips - 1), kPrefilterSamples << (m - 1));
        std::vector<uint16_t>& out = env.mips[size_t(m)];
        out.resize(filtered.px.size() * 4);
        tbb::parallel_for(size_t(0), filtered.px.size(), [&](size_t i) {
            out[i * 4 + 0] = half(filtered.px[i].r);
            out[i * 4 + 1] = half(filtered.px[i].g);
            out[i * 4 + 2] = half(filtered.px[i].b);
            out[i * 4 + 3] = half(1.0f);
        });
    }
    return env;
}

struct Softbox
{
    glm::vec3 dir;
    float halfWidth, halfHeight; // tangent-plane half extents
    float radiance;
};

float softboxMask(const Softbox& box, const glm::vec3& d)
{
    glm::vec3 c = glm::normalize(box.dir);
    float facing = glm::dot(d, c);
    if (facing <= 0.0f)
        return 0.0f;
    glm::vec3 right = std::abs(c.y) < 0.999f ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), c)) : glm::vec3(1, 0, 0);
    glm::vec3 up = glm::cross(c, right);
    glm::vec3 p = d / facing - c;
    float x = std::abs(glm::dot(p, right)) / box.halfWidth;
    float y = std::abs(glm::dot(p, up)) / box.halfHeight;
    // Superellipse: a rectangle with rounded corners, edges softened like diffusion fabric.
    float q = std::pow(std::pow(x, 6.0f) + std::pow(y, 6.0f), 1.0f / 6.0f);
    float edge = 1.0f - glm::smoothstep(0.88f, 1.06f, q);
    return edge * (1.0f - 0.18f * std::min(q * q, 1.0f));
}

glm::vec3 studioRadiance(const glm::vec3& d)
{
    // Placed for the default camera (front right, above): key upper left, fill right, rim behind.
    static const Softbox boxes[] = {
        {{0.15f, 0.75f, 1.0f}, 0.48f, 0.34f, 7.0f},   // key
        {{1.0f, 0.22f, 0.12f}, 0.62f, 0.42f, 0.9f},   // fill
        {{-0.8f, 0.5f, -0.65f}, 0.16f, 0.62f, 4.0f},  // rim strip
        {{0.0f, 1.0f, 0.0f}, 0.55f, 0.55f, 1.0f},     // top
    };

    float y = d.y;
    float sky = glm::mix(0.10f, 0.20f, std::pow(glm::smoothstep(0.0f, 1.0f, y), 0.7f));
    float floor = glm::mix(0.05f, 0.025f, glm::smoothstep(0.0f, -0.6f, y));
    float l = glm::mix(floor, sky, glm::smoothstep(-0.06f, 0.04f, y));
    for (const Softbox& b : boxes)
        l += b.radiance * softboxMask(b, d);
    return glm::vec3(l);
}

glm::vec3 sampleEquirect(const HdrImage& img, const glm::vec3& d)
{
    float u = 0.5f + std::atan2(d.x, -d.z) / (2.0f * kPi);
    float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi;
    float x = u * float(img.width) - 0.5f;
    float y = std::clamp(v * float(img.height) - 0.5f, 0.0f, float(img.height - 1));
    int x0 = int(std::floor(x)), y0 = int(y);
    float fx = x - float(x0), fy = y - float(y0);
    auto wrap = [&](int i) { return ((i % img.width) + img.width) % img.width; };
    int y1 = std::min(y0 + 1, img.height - 1);
    glm::vec3 top = glm::mix(img.at(wrap(x0), y0), img.at(wrap(x0 + 1), y0), fx);
    glm::vec3 bottom = glm::mix(img.at(wrap(x0), y1), img.at(wrap(x0 + 1), y1), fx);
    return glm::mix(top, bottom, fy);
}
} // namespace

EnvironmentData buildStudioEnvironment()
{
    LILA_ZONE();
    return finish(renderCube(kCubeSize, 3, studioRadiance));
}

EnvironmentData buildEnvironment(const HdrImage& equirect)
{
    LILA_ZONE();

    // Supersample big panoramas so the cube does not alias them.
    int ss = std::clamp(int(std::round(float(equirect.width) / float(4 * kCubeSize))), 1, 4);
    return finish(renderCube(kCubeSize, ss, [&](const glm::vec3& d) { return sampleEquirect(equirect, d); }));
}

EnvironmentData buildEnvironment(int size, const std::function<glm::vec3(const glm::vec3&)>& radiance)
{
    LILA_ZONE();
    return finish(renderCube(size, 1, radiance));
}

std::vector<uint16_t> buildBrdfLut(int size)
{
    LILA_ZONE();

    constexpr uint32_t kSamples = 512;
    std::vector<uint16_t> out(size_t(size) * size_t(size) * 2);
    tbb::parallel_for(0, size, [&](int j) {
        float rough = (float(j) + 0.5f) / float(size);
        float a = rough * rough;
        for (int i = 0; i < size; ++i)
        {
            float nv = (float(i) + 0.5f) / float(size);
            glm::vec3 v(std::sqrt(1.0f - nv * nv), 0.0f, nv);
            float scale = 0.0f, bias = 0.0f;
            for (uint32_t s = 0; s < kSamples; ++s)
            {
                glm::vec3 h = sampleGgx(hammersley(s, kSamples), a);
                float vh = glm::dot(v, h);
                glm::vec3 l = 2.0f * vh * h - v;
                if (l.z <= 0.0f || vh <= 0.0f)
                    continue;
                float g = vSmithGgx(nv, l.z, a) * 4.0f * l.z * vh / h.z;
                float fc = std::pow(1.0f - vh, 5.0f);
                scale += (1.0f - fc) * g;
                bias += fc * g;
            }
            size_t o = (size_t(j) * size_t(size) + size_t(i)) * 2;
            out[o] = half(scale / float(kSamples));
            out[o + 1] = half(bias / float(kSamples));
        }
    });
    return out;
}
} // namespace lila::render
