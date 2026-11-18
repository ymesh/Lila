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
} // namespace lila::render
