#include "ui/MaterialDraw.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/ColorManagement.h"
#include "scene/Materials.h"
#include "ui/TexLive.h"
#include "ui/TextureCache.h"
#include "ui/ViewportTool.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace lila::ui
{
namespace
{
using namespace scene::material;

const std::string& str(const scene::Node& n, int attr) { return n.as<std::string>(attr); }
float floatAttr(const scene::Node& n, int attr) { return float(n.as<double>(attr)); }
glm::vec3 colorAttr(const scene::Node& n, int attr) { return glm::vec3(n.as<glm::dvec3>(attr)); }
std::string file(const scene::Node& n, int attr) { return scene::materials::resolvePath(str(n, attr)); }

/** A stepped pseudo-random offset per boil frame, so strokes jump between frames and scrub deterministically. */
glm::vec2 boilOffset(double seconds, double fps)
{
    const int64_t frame = int64_t(std::floor(seconds * std::max(fps, 0.1) + 1e-6));
    uint32_t h = uint32_t(frame) * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return {float(h & 0xffffu) / 65536.0f, float(h >> 16u) / 65536.0f};
}

render::MaterialStyle style(const scene::Node& n)
{
    render::MaterialStyle s;
    s.model = render::ShadingModel(std::clamp<int64_t>(n.as<int64_t>(kShadingModel), 0, int64_t(render::ShadingModel::Eye)));
    if (s.model == render::ShadingModel::DefaultLit || s.model == render::ShadingModel::Unlit)
        return s;
    s.painterlyStrength = floatAttr(n, kPainterlyStrength);
    s.strokeNormal = floatAttr(n, kStrokeNormal);
    s.strokeScale = floatAttr(n, kStrokeScale);
    s.strokeLayerRatio = floatAttr(n, kStrokeLayerRatio);
    s.strokeMapping = uint32_t(std::clamp<int64_t>(n.as<int64_t>(kStrokeMapping), 0, 3));
    s.paletteCount = int(n.as<int64_t>(kPaletteCount));
    s.paletteShadow = colorAttr(n, kPaletteShadow);
    s.paletteLight = colorAttr(n, kPaletteLight);
    s.paletteAccent = colorAttr(n, kPaletteAccent);
    s.paletteContrast = floatAttr(n, kPaletteContrast);
    s.paletteSpread = floatAttr(n, kPaletteSpread);
    s.specularTint = floatAttr(n, kSpecularTint);
    if (n.as<bool>(kBoil))
        s.boilOffset = boilOffset(scene::Graph::current().time(), n.as<double>(kBoilFps));
    s.toonShadowColor = colorAttr(n, kToonShadowColor);
    s.toonLightTint = colorAttr(n, kToonLightTint);
    s.toonThreshold = floatAttr(n, kToonThreshold);
    s.toonSoftness = floatAttr(n, kToonSoftness);
    s.toonOuterOffset = floatAttr(n, kToonOuterOffset);
    s.toonOuterColor = colorAttr(n, kToonOuterColor);
    s.rimColor = colorAttr(n, kRimColor);
    s.rimWidth = floatAttr(n, kRimWidth);
    s.rimStrength = floatAttr(n, kRimStrength);
    s.foliageBottomColor = colorAttr(n, kFoliageBottomColor);
    s.foliageTopColor = colorAttr(n, kFoliageTopColor);
    s.foliageGradient = floatAttr(n, kFoliageGradient);
    s.foliageBottomShadow = floatAttr(n, kFoliageBottomShadow);
    s.foliageTopHighlight = floatAttr(n, kFoliageTopHighlight);
    s.foliageTranslucency = floatAttr(n, kFoliageTranslucency);
    s.foliageWrap = floatAttr(n, kFoliageWrap);
    s.subsurfaceColor = colorAttr(n, kSubsurfaceColor);
    s.subsurfaceAmount = floatAttr(n, kSubsurfaceAmount);
    s.sheenColor = colorAttr(n, kSheenColor);
    s.sheenRoughness = floatAttr(n, kSheenRoughness);
    s.clearcoat = floatAttr(n, kClearcoat);
    s.clearcoatRoughness = floatAttr(n, kClearcoatRoughness);
    s.anisotropy = floatAttr(n, kAnisotropy);
    s.anisotropyRotation = floatAttr(n, kAnisotropyRotation);
    s.hairShift = floatAttr(n, kHairShift);
    s.irisRadius = floatAttr(n, kIrisRadius);
    s.irisDepth = floatAttr(n, kIrisDepth);
    s.limbalRing = floatAttr(n, kLimbalRing);
    if (s.model == render::ShadingModel::Painterly)
        s.strokeMap = textures::get(scene::materials::strokeSheetPath(str(n, kStrokeMap)), false);
    return s;
}

/** A texture set bound to the material replaces the maps it has channels for. */
void applyTextureSet(const scene::Node& n, render::Material& m)
{
    std::optional<texLive::Maps> live = texLive::mapsFor(n.name());
    if (!live || !live->ready)
        return;
    if (live->baseColor)
    {
        m.baseColorMap = live->baseColor;
        m.flags |= render::MatBaseColorMap;
    }
    if (live->normal)
    {
        m.normalMap = live->normal;
        m.flags |= render::MatNormalMap | render::MatNormalGreenUp;
    }
    if (live->orm)
    {
        m.ormMap = live->orm;
        m.flags &= ~uint32_t(render::MatOcclusionMap | render::MatRoughnessMap | render::MatMetallicMap);
        m.flags |= (live->occlusion ? uint32_t(render::MatOcclusionMap) : 0u) | (live->roughness ? uint32_t(render::MatRoughnessMap) : 0u) |
                   (live->metallic ? uint32_t(render::MatMetallicMap) : 0u);
    }
    if (live->emissive)
    {
        m.emissiveMap = live->emissive;
        m.flags |= render::MatEmissiveMap;
        m.emissive = glm::vec3(floatAttr(n, kEmissiveIntensity));
    }
}

/** render::Material::sampling bits of one map. */
uint32_t samplingBits(const scene::Node& n, int mapAttr)
{
    const TextureSampling s = scene::materials::sampling(n, mapAttr);
    return uint32_t(s.wrapU) | uint32_t(s.wrapV) << 2 | (s.filter == TextureFilter::Nearest ? 16u : 0u);
}

/** The sampling of the four viewport maps; the packed map follows the first separate map set when there is no ORM map. */
uint32_t samplingOf(const scene::Node& n)
{
    int orm = kOrmMap;
    if (str(n, kOrmMap).empty())
        for (int a : {kAmbientOcclusionMap, kRoughnessMap, kMetallicMap})
            if (!str(n, a).empty())
            {
                orm = a;
                break;
            }
    return samplingBits(n, kBaseColorMap) | samplingBits(n, kNormalMap) << 8 | samplingBits(n, orm) << 16 | samplingBits(n, kEmissiveMap) << 24;
}

/** Which of the packed map's sources (packed, occlusion, roughness, metallic as bits 0 to 3) are sRGB-encoded. */
uint32_t packedSrgb(const scene::Node& n)
{
    uint32_t mask = 0;
    const int maps[4] = {kOrmMap, kAmbientOcclusionMap, kRoughnessMap, kMetallicMap};
    for (int i = 0; i < 4; ++i)
        if (scene::materials::srgb(n, maps[i]))
            mask |= 1u << i;
    return mask;
}

render::Material convert(const scene::Node& n)
{
    render::Material m;
    m.baseColor = colorAttr(n, kBaseColor);
    m.metallic = floatAttr(n, kMetallic);
    m.roughness = floatAttr(n, kRoughness);
    m.specular = floatAttr(n, kSpecular);
    m.bevelRadius = floatAttr(n, kBevelRadius);
    m.opacity = floatAttr(n, kOpacity);
    m.opacityClip = floatAttr(n, kOpacityMaskClip);
    m.flags = 0;
    m.sampling = samplingOf(n);

    // With OCIO on, a colour map may name the config colour space it is in.
    namespace cm = scene::colorManagement;
    const cm::Settings colour = cm::settings(scene::Graph::current(), PrefStore::get().value<std::string>("general", cm::kConfigPref, ""));
    auto colourMap = [&](int map, int space) {
        const std::string ocio = colour.ocio ? n.as<std::string>(space) : std::string();
        return ocio.empty() ? textures::get(file(n, map), scene::materials::srgb(n, map), true)
                            : textures::getOcio(file(n, map), colour.config, ocio, scene::materials::srgb(n, map));
    };
    if ((m.baseColorMap = colourMap(kBaseColorMap, kBaseColorOcioSpace)))
        m.flags |= render::MatBaseColorMap;
    const bool greenUp = NormalFormat(n.as<int64_t>(kNormalFormat)) == NormalFormat::OpenGL;
    const std::string height = n.as<bool>(kNormalFromHeight) ? file(n, kHeightMap) : std::string();
    if (!height.empty())
    {
        const double depth = floatAttr(n, kHeightDepth);
        m.normalMap = textures::heightNormal(file(n, kNormalMap), height, depth / std::max(double(floatAttr(n, kHeightUvWidth)), 1e-3),
                                             depth / std::max(double(floatAttr(n, kHeightUvHeight)), 1e-3), greenUp);
    }
    else
        m.normalMap = textures::get(file(n, kNormalMap), scene::materials::srgb(n, kNormalMap), true);
    if (m.normalMap)
    {
        m.flags |= render::MatNormalMap;
        if (greenUp)
            m.flags |= render::MatNormalGreenUp;
    }

    std::string orm = file(n, kOrmMap), ao = file(n, kAmbientOcclusionMap), rough = file(n, kRoughnessMap),
                metal = file(n, kMetallicMap);
    if ((m.ormMap = textures::packed(orm, ao, rough, metal,
                                     scene::materials::packedChannels(PackedLayout(n.as<int64_t>(kPackedLayout))), packedSrgb(n))))
    {
        if (!orm.empty())
            m.flags |= render::MatOcclusionMap | render::MatRoughnessMap | render::MatMetallicMap;
        else
            m.flags |= (ao.empty() ? 0u : uint32_t(render::MatOcclusionMap)) | (rough.empty() ? 0u : uint32_t(render::MatRoughnessMap)) |
                       (metal.empty() ? 0u : uint32_t(render::MatMetallicMap));
    }

    float intensity = floatAttr(n, kEmissiveIntensity);
    if ((m.emissiveMap = colourMap(kEmissiveMap, kEmissiveOcioSpace)))
    {
        m.flags |= render::MatEmissiveMap;
        m.emissive = glm::vec3(intensity);
    }
    else
        m.emissive = colorAttr(n, kEmissiveColor) * intensity;
    applyTextureSet(n, m);

    switch (BlendMode(n.as<int64_t>(kBlendMode)))
    {
    case BlendMode::Masked: m.flags |= render::MatMasked; break;
    case BlendMode::Translucent: m.flags |= render::MatTranslucent; break;
    default: break;
    }
    if (n.as<bool>(kTwoSided))
        m.flags |= render::MatTwoSided;
    if (n.as<int64_t>(kUvSet) >= 1)
        m.flags |= render::MatUv1;
    m.style = style(n);
    return m;
}

size_t slotCount(const geo::Mesh& mesh)
{
    return std::max<size_t>(mesh.materialNames().size(), 1);
}

bool sameMaps(const std::optional<texLive::Maps>& a, const std::optional<texLive::Maps>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a || (a->baseColor == b->baseColor && a->normal == b->normal && a->orm == b->orm && a->emissive == b->emissive &&
                  a->occlusion == b->occlusion && a->roughness == b->roughness && a->metallic == b->metallic && a->ready == b->ready);
}

/**
 * One slot name's converted material. Reused while the material node has not recomputed, no texture has arrived and
 * its texture set's maps are the same; checked again only when the scene, a texture or a redraw request moved the token.
 */
struct SlotEntry
{
    const scene::Node* node = nullptr;
    uint64_t pass = 0;
    uint64_t textures = 0;
    uint64_t version = 0, redraws = 0;
    bool timed = false; // boils: follows the scene time
    double time = 0.0;
    std::optional<texLive::Maps> live;
    render::Material material;
    bool normalMap = false;
};

struct SlotCache
{
    std::unordered_map<std::string, SlotEntry> slots;
    scene::Graph* graph = nullptr;
    int graphListener = 0;
    uint64_t textures = 1; // images that finished loading, which convert() picks up
    uint64_t redraws = 1;  // texture sets announce new maps this way
    uint64_t epoch = 1;    // bumped when a known slot's material changes or the slots are dropped
};

SlotCache& slotCache()
{
    static SlotCache c;
    static const bool listening = [] {
        textures::addListener([] { ++slotCache().textures; });
        viewportTools::addListener([] { ++slotCache().redraws; });
        return true;
    }();
    (void)listening;
    scene::Graph& g = scene::Graph::current();
    if (c.graph != &g)
    {
        if (c.graph)
            c.graph->removeListener(c.graphListener);
        c.graph = &g;
        c.slots.clear();
        ++c.epoch;
        // Which node a slot name finds changes only when nodes come, go or change names.
        c.graphListener = g.addListener(
            [](scene::Change change, scene::NodeId) {
                if (change == scene::Change::Created || change == scene::Change::Deleted || change == scene::Change::Renamed ||
                    change == scene::Change::Cleared || change == scene::Change::Reset)
                {
                    slotCache().slots.clear();
                    ++slotCache().epoch;
                }
            },
            true);
    }
    return c;
}

const SlotEntry& slotEntry(const std::string& slot)
{
    SlotCache& c = slotCache();
    scene::Graph& g = *c.graph;
    auto [it, added] = c.slots.try_emplace(slot);
    SlotEntry& e = it->second;
    if (!added && e.version == g.version() && e.redraws == c.redraws && e.textures == c.textures)
        return e;
    if (added)
        e.node = scene::materials::find(g, slot);
    std::optional<texLive::Maps> live = e.node ? texLive::mapsFor(e.node->name()) : std::nullopt;
    if (added || (e.node && e.node->evaluatedPass() != e.pass) || e.textures != c.textures || !sameMaps(live, e.live) || (e.timed && e.time != g.time()))
    {
        if (!added)
            ++c.epoch;
        e.material = e.node ? convert(*e.node) : render::Material{};
        e.pass = e.node ? e.node->evaluatedPass() : 0;
        e.live = std::move(live);
        e.timed = e.node && e.material.style.model != render::ShadingModel::DefaultLit && e.node->as<bool>(kBoil);
        e.time = g.time();
        e.textures = c.textures;
        e.normalMap = e.node && (!str(*e.node, kNormalMap).empty() || (e.live && e.live->normal));
    }
    e.version = g.version();
    e.redraws = c.redraws;
    return e;
}
} // namespace

bool materialsShown()
{
    return PrefStore::get().value<bool>("general", "/viewport/materials", true);
}

void setMaterialsShown(bool on)
{
    PrefStore::get().set("general", "/viewport/materials", on);
    viewportTools::requestRedraw();
}

uint64_t materialsEpoch()
{
    SlotCache& c = slotCache();
    for (auto& [slot, e] : c.slots)
        slotEntry(slot);
    return c.epoch;
}

bool needsTangents(const geo::Mesh& mesh)
{
    for (size_t i = 0; i < slotCount(mesh); ++i)
        if (slotEntry(scene::materials::slotName(mesh, int32_t(i))).normalMap)
            return true;
    return false;
}

std::vector<render::Material> drawMaterials(const geo::Mesh& mesh)
{
    std::vector<render::Material> out(slotCount(mesh));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = slotEntry(scene::materials::slotName(mesh, int32_t(i))).material;
    return out;
}
} // namespace lila::ui
