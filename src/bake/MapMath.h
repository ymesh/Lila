#pragma once

#include "bake/BakeTypes.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

/**
 * Math the CPU map kernels share, kept header-only and free of library types so a GPU kernel can mirror it line for
 * line: sample hashing, low-discrepancy sequences, hemisphere and cone sampling, frames and sRGB.
 */
namespace lila::bake::mapmath
{
inline glm::vec3 v3(const float* f) { return {f[0], f[1], f[2]}; }

inline glm::vec3 unit(const glm::vec3& v, const glm::vec3& fallback = {0.0f, 0.0f, 1.0f})
{
    const float l = glm::length(v);
    return l > 1e-20f ? v / l : fallback;
}

inline uint32_t hashBits(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline float radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

/** An orthonormal t, b around unit n (Duff et al.). */
inline void basis(const glm::vec3& n, glm::vec3& t, glm::vec3& b)
{
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z), c = n.x * n.y * a;
    t = {1.0f + s * n.x * n.x * a, s * c, -s * n.x};
    b = {c, s + n.y * n.y * a, -n.y};
}

/** A projection miss retries on rings of probes widening around its ray: kProbeRings rings of kProbePerRing. */
inline constexpr uint32_t kProbeRings = 22;
inline constexpr uint32_t kProbePerRing = 16;

/** Probe j of ring k around unit axis d (t, b its basis), ring k at k + 1 degrees off it. Same on the GPU. */
inline glm::vec3 probeDirection(uint32_t k, uint32_t j, const glm::vec3& d, const glm::vec3& t, const glm::vec3& b)
{
    const float a = glm::radians(float(k + 1));
    const float phi = 6.28318530718f * (float(j) + 0.5f * float(k & 1u)) / float(kProbePerRing);
    return glm::normalize(d * std::cos(a) + (t * std::cos(phi) + b * std::sin(phi)) * std::sin(a));
}

/** A per-sample seed that is the same on every run and thread count. */
inline uint32_t sampleSeed(const TexelSample& s) { return hashBits(s.texel * 64u + s.sub) ^ hashBits(s.tile + 0x9e3779b9u); }

/** Ray k of n for a sample: a stratified 2D point rotated by the seed (Cranley-Patterson). */
inline glm::vec2 rayPoint(uint32_t k, uint32_t n, uint32_t seed)
{
    const float r1 = float(seed & 0xffff) / 65536.0f, r2 = float(seed >> 16) / 65536.0f;
    return {std::fmod((float(k) + 0.5f) / float(n) + r1, 1.0f), std::fmod(radicalInverse(k) + r2, 1.0f)};
}

/**
 * A cosine-weighted direction about axis within a cone of halfAngle (radians): pi / 2 is the full cosine hemisphere,
 * 0 the axis itself.
 */
inline glm::vec3 coneDirection(const glm::vec3& axis, const glm::vec2& u, float halfAngle)
{
    glm::vec3 t, b;
    basis(axis, t, b);
    const float r = std::sqrt(u.x) * std::sin(std::clamp(halfAngle, 0.0f, 1.57079632679f));
    const float phi = 6.28318530718f * u.y;
    return unit(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + axis * std::sqrt(std::max(0.0f, 1.0f - r * r)));
}

/** A surface offset that clears float noise at the scale of the bounds' diagonal. */
inline float rayEpsilon(float diagonal) { return std::max(1e-4f * diagonal, 1e-4f); }

inline float linearToSrgb(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline float srgbToLinear(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline glm::vec3 linearToSrgb(const glm::vec3& c) { return {linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b)}; }
inline glm::vec3 srgbToLinear(const glm::vec3& c) { return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)}; }

/** Distance from p to the segment ab. */
inline float segmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b)
{
    const glm::vec2 ab = b - a;
    const float l2 = glm::dot(ab, ab);
    const float t = l2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

inline float smoothstep(float e0, float e1, float x)
{
    const float t = e1 > e0 ? std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f) : (x >= e0 ? 1.0f : 0.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace lila::bake::mapmath
