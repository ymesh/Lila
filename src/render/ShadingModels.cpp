#include "render/RenderMesh.h"

#include <algorithm>
#include <bit>

namespace lila::render
{
std::array<uint32_t, kStyleWords> packStyle(const MaterialStyle& s, uint32_t strokeHandle, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                            bool zUp)
{
    std::array<float, kStyleWords> f{};
    auto vec = [&](int i, glm::vec3 v, float w) {
        f[size_t(i) * 4 + 0] = v.x;
        f[size_t(i) * 4 + 1] = v.y;
        f[size_t(i) * 4 + 2] = v.z;
        f[size_t(i) * 4 + 3] = w;
    };
    auto pair = [&](int i, float z, float w) {
        f[size_t(i) * 4 + 2] = z;
        f[size_t(i) * 4 + 3] = w;
    };
    switch (s.model)
    {
    case ShadingModel::Painterly:
        pair(0, s.painterlyStrength, s.strokeNormal);
        f[4] = 1.0f / std::max(s.strokeScale, 0.01f);
        f[5] = std::max(s.strokeLayerRatio, 1.0f);
        f[6] = float(s.strokeMapping);
        f[7] = s.paletteContrast;
        vec(2, s.paletteShadow, s.specularTint);
        vec(3, s.paletteLight, float(std::clamp(s.paletteCount, 2, 4)));
        vec(4, s.paletteAccent, s.paletteSpread);
        f[20] = s.boilOffset.x;
        f[21] = s.boilOffset.y;
        break;
    case ShadingModel::Toon:
        pair(0, s.toonThreshold, s.toonSoftness);
        f[4] = s.toonOuterOffset;
        f[5] = s.rimWidth;
        f[6] = s.rimStrength;
        vec(2, s.toonShadowColor, 0.0f);
        vec(3, s.toonLightTint, 0.0f);
        vec(4, s.toonOuterColor, 0.0f);
        vec(5, s.rimColor, 0.0f);
        break;
    case ShadingModel::Foliage:
        pair(0, s.foliageGradient, s.foliageWrap);
        f[4] = s.foliageBottomShadow;
        f[5] = s.foliageTopHighlight;
        f[6] = s.foliageTranslucency;
        vec(2, s.foliageBottomColor, 0.0f);
        vec(3, s.foliageTopColor, 0.0f);
        break;
    case ShadingModel::Skin:
        vec(2, s.subsurfaceColor, s.subsurfaceAmount);
        break;
    case ShadingModel::Cloth:
        vec(2, s.sheenColor, s.sheenRoughness);
        break;
    case ShadingModel::Clearcoat:
        pair(0, s.clearcoat, s.clearcoatRoughness);
        break;
    case ShadingModel::Hair:
        pair(0, s.anisotropy, glm::radians(s.anisotropyRotation));
        f[4] = s.hairShift;
        break;
    case ShadingModel::Eye:
        pair(0, s.irisRadius, s.irisDepth);
        f[4] = s.limbalRing;
        f[5] = s.clearcoat;
        f[6] = s.clearcoatRoughness;
        break;
    default: break;
    }
    vec(6, boundsMin, zUp ? 2.0f : 1.0f);
    vec(7, boundsMax, 0.0f);

    std::array<uint32_t, kStyleWords> out;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = std::bit_cast<uint32_t>(f[i]);
    out[0] = uint32_t(s.model);
    out[1] = strokeHandle;
    return out;
}
std::array<uint32_t, kStyleWords> packExtended(const ExtendedShading& e, const std::array<uint32_t, kExtendedMaps>& handles)
{
    std::array<float, kStyleWords> f{};
    auto vec = [&](int i, glm::vec3 v, float w) {
        f[size_t(i) * 4 + 0] = v.x;
        f[size_t(i) * 4 + 1] = v.y;
        f[size_t(i) * 4 + 2] = v.z;
        f[size_t(i) * 4 + 3] = w;
    };
    f[2] = e.coat;
    f[3] = e.coatRoughness;
    vec(1, e.sheenColor, e.sheenRoughness);
    vec(2, e.subsurfaceColor, e.subsurfaceRadius);
    vec(3, e.specularColor, e.specularWeight);
    vec(4, glm::vec3(e.anisotropy, e.anisotropyRotation, e.transmission), e.ior);
    std::array<uint32_t, kStyleWords> out;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = std::bit_cast<uint32_t>(f[i]);
    out[0] = uint32_t(ShadingModel::Extended);
    out[1] = 0xffffffffu;
    // Handles from word 20 on, two to a word.
    for (uint32_t k = 0; k < kExtendedMaps; ++k)
    {
        const uint32_t word = 20 + k / 2, shift = (k & 1) * 16;
        if (k % 2 == 0)
            out[word] = 0xffffffffu;
        out[word] = (out[word] & ~(0xffffu << shift)) | ((handles[k] & 0xffffu) << shift);
    }
    return out;
}

std::array<uint32_t, kGrungeBlockWords> packGrunge(const GrungeLook& g, uint64_t address)
{
    std::array<float, kGrungeBlockWords> f{};
    auto vec = [&](size_t at, glm::vec3 v, float w) {
        f[at] = v.x;
        f[at + 1] = v.y;
        f[at + 2] = v.z;
        f[at + 3] = w;
    };
    uint32_t enabled = 0;
    vec(4, g.outlineColor, 0.0f);
    for (size_t k = 0; k < g.layers.size(); ++k)
    {
        const GrungeLayer& l = g.layers[k];
        if (l.enabled && l.strength > 0.0f)
            enabled |= 1u << k;
        const size_t b = 8 + k * 16;
        f[b] = l.level;
        f[b + 1] = std::max(l.softness, 1e-4f);
        f[b + 2] = l.breakup;
        f[b + 3] = 1.0f / std::max(l.noiseScale, 0.01f);
        // Multiply rides on the bands word's sign: -(bands + 1).
        f[b + 4] = l.multiply ? -float(std::max(l.bands, 0) + 1) : float(std::max(l.bands, 0));
        f[b + 5] = l.cut;
        f[b + 6] = l.outline;
        f[b + 7] = std::clamp(l.strength, 0.0f, 1.0f);
        vec(b + 8, l.color, l.roughness);
        // The seed moves the noise domain, so two seeds read unrelated noise.
        vec(b + 12, l.low, float(l.seed % 4096u) * 17.31f);
    }
    std::array<uint32_t, kGrungeBlockWords> out;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = std::bit_cast<uint32_t>(f[i]);
    out[0] = uint32_t(address);
    out[1] = uint32_t(address >> 32);
    out[2] = enabled;
    out[3] = std::bit_cast<uint32_t>(std::max(g.edgeWidth, 1e-3f));
    const size_t reach = 8 + g.layers.size() * 16;
    out[reach] = std::bit_cast<uint32_t>(std::max(g.cavityWidth, 1e-3f));
    out[reach + 1] = std::bit_cast<uint32_t>(std::max(g.aoWidth, 1e-3f));
    out[reach + 2] = std::bit_cast<uint32_t>(std::max(g.fadeHeight, 1e-3f));
    out[reach + 3] = std::bit_cast<uint32_t>(std::max(g.streakLength, 1e-3f));
    return out;
}
} // namespace lila::render
