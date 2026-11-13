#include "render/DrawBatches.h"

#include "core/Profile.h"

#include <algorithm>
#include <unordered_map>

namespace lila::render
{
namespace
{
bool sameStyle(const MaterialStyle& a, const MaterialStyle& b)
{
    return a.model == b.model && a.painterlyStrength == b.painterlyStrength && a.strokeNormal == b.strokeNormal && a.strokeScale == b.strokeScale &&
           a.strokeLayerRatio == b.strokeLayerRatio && a.strokeMapping == b.strokeMapping && a.paletteCount == b.paletteCount &&
           a.paletteShadow == b.paletteShadow && a.paletteLight == b.paletteLight && a.paletteAccent == b.paletteAccent &&
           a.paletteContrast == b.paletteContrast && a.paletteSpread == b.paletteSpread && a.specularTint == b.specularTint &&
           a.boilOffset == b.boilOffset && a.toonShadowColor == b.toonShadowColor && a.toonLightTint == b.toonLightTint &&
           a.toonThreshold == b.toonThreshold && a.toonSoftness == b.toonSoftness && a.toonOuterOffset == b.toonOuterOffset &&
           a.toonOuterColor == b.toonOuterColor && a.rimColor == b.rimColor && a.rimWidth == b.rimWidth && a.rimStrength == b.rimStrength &&
           a.foliageBottomColor == b.foliageBottomColor && a.foliageTopColor == b.foliageTopColor && a.foliageGradient == b.foliageGradient &&
           a.foliageBottomShadow == b.foliageBottomShadow && a.foliageTopHighlight == b.foliageTopHighlight &&
           a.foliageTranslucency == b.foliageTranslucency && a.foliageWrap == b.foliageWrap && a.strokeMap == b.strokeMap;
}

bool sameMaterial(const Material& a, const Material& b)
{
    return a.baseColor == b.baseColor && a.opacity == b.opacity && a.metallic == b.metallic && a.roughness == b.roughness &&
           a.specular == b.specular && a.opacityClip == b.opacityClip && a.emissive == b.emissive && a.flags == b.flags &&
           a.baseColorMap == b.baseColorMap && a.normalMap == b.normalMap && a.ormMap == b.ormMap && a.emissiveMap == b.emissiveMap &&
           sameStyle(a.style, b.style);
}

/** Highlighted placements sort last within a wire batch. */
int wireRank(const MeshDraw& d)
{
    return (d.flags & DrawSelected) ? ((d.flags & DrawSelectedOther) ? 3 : 4) : (d.flags & DrawPreselect) ? 2 : (d.flags & DrawUnderSelected) ? 1 : 0;
}

/** Shaded draws can share a batch when these match and so do their materials. */
struct ShadedKey
{
    const RenderMesh* mesh;
    uint32_t display;      // vertex colour mode and channel
    glm::vec3 baseColor;   // used where a slot has no material
    float roughness;
    float wireWidth;       // 0 unless the mesh shader draws the wire

    bool operator==(const ShadedKey&) const = default;
};

struct WireKey
{
    const RenderMesh* mesh;
    bool depthTested;
    float width;

    bool operator==(const WireKey&) const = default;
};

struct KeyHash
{
    size_t operator()(const ShadedKey& k) const { return std::hash<const void*>()(k.mesh) ^ (size_t(k.display) << 1); }
    size_t operator()(const WireKey& k) const { return std::hash<const void*>()(k.mesh) ^ size_t(k.depthTested); }
};

template <class F> void forEachPlacement(const MeshDraw& d, F&& f)
{
    if (!d.instances || d.instances->empty())
    {
        f(d.world);
        return;
    }
    for (const glm::mat4& m : *d.instances)
        f(d.world * glm::dmat4(m));
}

size_t placements(const MeshDraw& d) { return d.instances && !d.instances->empty() ? d.instances->size() : 1; }
} // namespace

bool sameMaterials(const std::vector<Material>& a, const std::vector<Material>& b)
{
    if (&a == &b)
        return true;
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!sameMaterial(a[i], b[i]))
            return false;
    return true;
}

bool sameDraws(const std::vector<MeshDraw>& a, const std::vector<MeshDraw>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const MeshDraw& x = a[i];
        const MeshDraw& y = b[i];
        if (x.mesh != y.mesh || x.instances != y.instances || !x.materials.sameList(y.materials) || x.flags != y.flags ||
            x.world != y.world || x.baseColor != y.baseColor || x.roughness != y.roughness || x.vertexColors != y.vertexColors ||
            x.colorChannel != y.colorChannel || x.wireColor != y.wireColor || x.wireWidth != y.wireWidth || x.node != y.node ||
            x.lightingChannels != y.lightingChannels)
            return false;
    }
    return true;
}

void rebind(DrawBatches& batches, const std::vector<MeshDraw>& draws)
{
    for (std::vector<DrawBatch>* list : {&batches.shaded, &batches.wire})
        for (DrawBatch& b : *list)
            b.draw = &draws[b.drawIndex];
}

bool drawsWire(const MeshDraw& d) { return (d.flags & (DrawWire | DrawSelected | DrawUnderSelected | DrawPreselect)) != 0; }

bool wireInShader(const MeshDraw& d) { return (d.flags & DrawShaded) && drawsWire(d) && d.mesh && !d.mesh->edgeMask.empty(); }

glm::vec4 wireColor(const FrameState& s, const MeshDraw& d)
{
    return (d.flags & DrawSelected)        ? ((d.flags & DrawSelectedOther) ? s.wireSelectedOther : s.wireSelected)
           : (d.flags & DrawPreselect)     ? s.wirePreselect
           : (d.flags & DrawUnderSelected) ? s.wireUnderSelected
           : d.wireColor.a > 0.0f          ? d.wireColor
                                           : s.wireDefault;
}

DrawBatches batchDraws(const FrameState& s, bool shaderWire)
{
    LILA_ZONE();

    DrawBatches out;
    // Members of each batch in frame order; flattened into instances once grouping is done.
    std::vector<std::vector<const MeshDraw*>> shadedMembers, wireMembers;
    std::unordered_map<ShadedKey, std::vector<size_t>, KeyHash> shadedIndex;
    std::unordered_map<WireKey, size_t, KeyHash> wireIndex;
    for (const MeshDraw& d : s.meshes)
    {
        if (!d.mesh)
            continue;
        if ((d.flags & DrawShaded) && !d.mesh->indices.empty())
        {
            const ShadedKey key{d.mesh.get(), uint32_t(d.vertexColors) | uint32_t(d.colorChannel) << 8, d.baseColor, d.roughness,
                                shaderWire && wireInShader(d) ? std::max(d.wireWidth, 1.0f) : 0.0f};
            std::vector<size_t>& candidates = shadedIndex[key];
            size_t batch = SIZE_MAX;
            for (size_t c : candidates)
                if (shadedMembers[c].front()->materials.sameList(d.materials) || sameMaterials(shadedMembers[c].front()->materials, d.materials))
                {
                    batch = c;
                    break;
                }
            if (batch == SIZE_MAX)
            {
                batch = shadedMembers.size();
                candidates.push_back(batch);
                shadedMembers.emplace_back();
            }
            shadedMembers[batch].push_back(&d);
        }
        if (drawsWire(d) && !d.mesh->edgePoints.empty() && !(shaderWire && wireInShader(d)))
        {
            const WireKey key{d.mesh.get(), (d.flags & (DrawShaded | DrawWireDepth)) != 0, d.wireWidth};
            auto [it, added] = wireIndex.try_emplace(key, wireMembers.size());
            if (added)
                wireMembers.emplace_back();
            wireMembers[it->second].push_back(&d);
        }
    }

    size_t total = 0;
    for (const auto& members : shadedMembers)
        for (const MeshDraw* d : members)
            total += placements(*d);
    for (const auto& members : wireMembers)
        for (const MeshDraw* d : members)
            total += placements(*d);
    out.instances.reserve(total);
    out.translations.reserve(total);
    auto place = [&out](const glm::dmat4& world, const glm::vec4& color) {
        out.instances.push_back({glm::mat4(world), color});
        out.translations.push_back(glm::dvec3(world[3]));
    };

    for (const auto& members : shadedMembers)
    {
        DrawBatch b{members.front(), uint32_t(members.front() - s.meshes.data()), uint32_t(out.instances.size()), 0};
        for (const MeshDraw* d : members)
        {
            const bool wire = shaderWire && wireInShader(*d);
            b.wire = b.wire || wire;
            const glm::vec4 color = wire ? wireColor(s, *d) : glm::vec4(0.0f);
            forEachPlacement(*d, [&](const glm::dmat4& world) { place(world, color); });
        }
        b.count = uint32_t(out.instances.size()) - b.first;
        out.shaded.push_back(b);
    }
    for (auto& members : wireMembers)
    {
        std::ranges::stable_sort(members, [](const MeshDraw* a, const MeshDraw* b) { return wireRank(*a) < wireRank(*b); });
        DrawBatch b{members.front(), uint32_t(members.front() - s.meshes.data()), uint32_t(out.instances.size()), 0};
        for (const MeshDraw* d : members)
        {
            const glm::vec4 color = wireColor(s, *d);
            forEachPlacement(*d, [&](const glm::dmat4& world) { place(world, color); });
        }
        b.count = uint32_t(out.instances.size()) - b.first;
        out.wire.push_back(b);
    }
    return out;
}

void rebaseInstances(DrawBatches& batches, const glm::dvec3& origin)
{
    for (size_t i = 0; i < batches.instances.size(); ++i)
    {
        glm::vec4& t = batches.instances[i].world[3];
        t = glm::vec4(glm::vec3(batches.translations[i] - origin), t.w);
    }
}
} // namespace lila::render
