#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class Graph;

/**
 * Surface materials, modeled on Unreal's default lit material.
 *
 * A `material` node (non-DAG) holds the inputs. Mesh faces pick one through their material slot: the face int
 * "material" indexes geo::Mesh::materialNames(), and a slot binds to the material node whose name equals the slot
 * name, exactly as FBX and Unreal match material slots by name. A slot with no node of that name renders with the
 * neutral default (mid gray, roughness 0.55). Renaming a material through renameMaterial rewrites the slots too.
 *
 * Conventions, all Unreal's:
 *   - baseColor and emissive are linear; color maps (base color, emissive) are sRGB, data maps are linear.
 *   - A connected map replaces its constant (emissive maps are scaled by emissiveIntensity).
 *   - Normal maps are tangent space, DirectX style by default (green points down the texture, Unreal's convention);
 *     normalFormat "opengl" (Flip Green) is for maps baked green-up (Blender, Substance's OpenGL preset).
 *   - Packed maps hold occlusion in R, roughness in G, metallic in B (ORM, Unreal's packing), or with packedLayout
 *     "mro" metallic in R and occlusion in B. A packed map wins over separate ones.
 *   - Lila UVs are V-up like Maya and FBX (v = 0 at the bottom of the image); the viewport samples at (u, 1 - v),
 *     which is what Unreal's FBX import does when it flips V, so a texture lands the same way in both.
 */
namespace material
{
enum Attr
{
    kBaseColor,
    kBaseColorMap,
    kMetallic,
    kMetallicMap,
    kSpecular,
    kRoughness,
    kRoughnessMap,
    kNormalMap,
    kNormalFormat,
    kAmbientOcclusionMap,
    kOrmMap,
    kPackedLayout,
    kEmissiveColor,
    kEmissiveMap,
    kEmissiveIntensity,
    kOpacity,
    kOpacityMaskClip,
    kBlendMode,
    kTwoSided,
    kUvSet,
    kShadingModel,
    // Painterly
    kPainterlyStrength,
    kStrokeNormal,
    kStrokeScale,
    kStrokeLayerRatio,
    kStrokeMapping,
    kStrokeMap,
    kPaletteCount,
    kPaletteShadow,
    kPaletteLight,
    kPaletteAccent,
    kPaletteContrast,
    kPaletteSpread,
    kSpecularTint,
    kBoil,
    kBoilFps,
    // Toon
    kToonShadowColor,
    kToonLightTint,
    kToonThreshold,
    kToonSoftness,
    kToonOuterOffset,
    kToonOuterColor,
    kRimColor,
    kRimWidth,
    kRimStrength,
    // Foliage
    kFoliageBottomColor,
    kFoliageTopColor,
    kFoliageGradient,
    kFoliageBottomShadow,
    kFoliageTopHighlight,
    kFoliageTranslucency,
    kFoliageWrap,
    // Skin, Cloth, Clearcoat, Hair, Eye
    kSubsurfaceColor,
    kSubsurfaceAmount,
    kSheenColor,
    kSheenRoughness,
    kClearcoat,
    kClearcoatRoughness,
    kAnisotropy,
    kAnisotropyRotation,
    kHairShift,
    kIrisRadius,
    kIrisDepth,
    kLimbalRing,
    kBevelRadius,
    // Normals from height
    kHeightMap,
    kNormalFromHeight,
    kHeightDepth,
    kHeightUvWidth,
    kHeightUvHeight,
    // Per-texture sampling: wrapU, wrapV and filter for each of materials::sampledMaps(), in that order.
    kTextureSampling,
    // The colour maps' OCIO input colour spaces (base colour, emissive): a config colour space, empty to follow ColorSpace.
    kBaseColorOcioSpace = kTextureSampling + 7 * 4,
    kEmissiveOcioSpace,
    // Position in the Material Editor's list; 0 sorts by name after the placed ones.
    kListOrder,
    // The texture set drawing every map it has, and per-map picks of one set's map ({channel: set}, JSON).
    kTextureSet,
    kTextureSetMaps,
    // The volume model: participating media filling the mesh (or its geometry nodes' density grid).
    kVolumeDensity,
    kVolumeScatterColor,
    kVolumeAbsorptionColor,
    kVolumeAnisotropy,
    kVolumeEmissionColor,
    kVolumeEmissionStrength,
    kVolumeVoxelSize,
    kCount
};

/** The viewport (and Unreal master material) lighting model; DefaultLit is Unreal's default lit. */
enum class ShadingModel
{
    DefaultLit,
    Painterly,
    Toon,
    Foliage,
    Unlit, // base color (plus emissive) as is, no lighting: Unreal's Unlit, glTF's KHR_materials_unlit
    Skin,      // light scattered under the surface: Unreal's Subsurface Profile
    Cloth,     // velvet sheen at grazing angles: Unreal's Cloth
    Clearcoat, // a glossy layer over the base: Unreal's Clear Coat (car paint, lacquer)
    Hair,      // highlights stretched across the strand: Unreal's Hair, for hair cards
    Eye,       // iris under a refracting cornea: Unreal's Eye
    Volume,    // fog, smoke or cloud filling the mesh: Unreal's Volume material domain, Blender's Principled Volume
};

/** Where Painterly stroke sheets are projected. */
enum class StrokeMapping
{
    Object, // triplanar in object space: strokes ride along with the object
    World,  // triplanar in world space
    Screen, // triplanar, rescaled by camera distance in octaves so strokes keep their size on screen
    Uv,     // the mesh's UVs, for output that bakes to textures
};

enum class BlendMode
{
    Opaque,
    Masked,
    Translucent,
};

enum class NormalFormat
{
    DirectX,
    OpenGL,
};

enum class PackedLayout
{
    ORM,
    MRO,
};

/** What a texture shows outside 0..1 along one axis. Black is a transparent black border (USD's "black"). */
enum class TextureWrap
{
    Repeat,
    Clamp,
    Mirror,
    Black,
};

enum class TextureFilter
{
    Linear, // trilinear, mipmapped
    Nearest,
};

/** How a texture's 8- and 16-bit values are read. EXR and HDR files are always linear. */
enum class TextureColorSpace
{
    Auto, // sRGB for the colour maps (base colour, emissive), linear for the rest
    Srgb,
    Linear,
};

/** How one texture of a material is sampled. */
struct TextureSampling
{
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureFilter filter = TextureFilter::Linear;
    TextureColorSpace colorSpace = TextureColorSpace::Auto;

    bool operator==(const TextureSampling&) const = default;
    bool isDefault() const { return *this == TextureSampling{}; }
};
} // namespace material

namespace materials
{
/** Slot that faces fall back to when a mesh first gets a second material; the FBX exporter uses the same name. */
inline constexpr const char* kDefaultSlot = "Default";

/** The material node a slot binds to, or null. */
LILA_SCENE_API Node* find(Graph& g, std::string_view slot);
LILA_SCENE_API std::vector<Node*> all(Graph& g);

/** Creates a material node named exactly `name` when it is free (else made unique). Undoable. */
LILA_SCENE_API Node* create(Graph& g, std::string_view name);

/**
 * Renames any node like Graph::rename; a material's slots on every mesh follow it, and the materials using a texture set
 * follow the set. Returns the name it got.
 */
LILA_SCENE_API std::string rename(Graph& g, NodeId id, const std::string& name);

/** The texture set the material picks whole (its textureSet attr); empty for none. */
LILA_SCENE_API std::string textureSet(const Node& material);
/** The material's per-map picks: {textureChannels name: set} for the maps that take one map of a set. */
LILA_SCENE_API std::map<std::string, std::string> textureSetMaps(const Node& material);
/** The set one of the material's maps (a textureChannels name) comes from: its own pick, else the whole set; empty for none. */
LILA_SCENE_API std::string textureSetFor(const Node& material, std::string_view channel);
/** Every set the material uses: the whole set first, then the per-map picks in textureChannels order. */
LILA_SCENE_API std::vector<std::string> textureSetsUsed(const Node& material);

/** Source channel (0 R, 1 G, 2 B) of occlusion, roughness and metallic in a packed map of this layout. */
LILA_SCENE_API std::array<int, 3> packedChannels(material::PackedLayout layout);

/** Texture channel names for setMaterialTexture ("baseColor", "normal", "orm", ...) and their attrs. */
LILA_SCENE_API const std::vector<std::pair<std::string, int>>& textureChannels();

/**
 * The setMaterialTexture channel a texture file's name suggests, from its last name token after dropping numeric
 * suffixes: T_Rock_BaseColor_1.png and T_Rock_D.png are "baseColor", T_Rock_CRM.png is "orm", T_Rock_MRO.png is "mro".
 * "" when nothing matches.
 */
LILA_SCENE_API std::string guessTextureChannel(std::string_view fileName);
/** The map attrs with sampling settings: base color, metallic, roughness, normal, occlusion, packed and emissive. */
LILA_SCENE_API std::span<const int> sampledMaps();
/** The first of a map attr's four sampling attrs (wrapU, wrapV, filter, colorSpace), or -1 for an attr without them. */
LILA_SCENE_API int samplingAttr(int mapAttr);
/** How a material samples one of its maps; the default for an attr without sampling settings. */
LILA_SCENE_API material::TextureSampling sampling(const Node& material, int mapAttr);
/** Sets a map's sampling, touching only the attrs that change. Undoable. */
LILA_SCENE_API void setSampling(Graph& g, NodeId material, int mapAttr, const material::TextureSampling& s);
/** "repeat", "clamp", "mirror" or "black". */
LILA_SCENE_API const char* wrapName(material::TextureWrap w);
/** True for the maps that hold colour (base colour, emissive), read as sRGB unless their colour space says otherwise. */
LILA_SCENE_API bool colorMap(int mapAttr);
/** Whether a map's 8- and 16-bit values are sRGB-encoded: its colorSpace, with auto resolved by colorMap. */
LILA_SCENE_API bool srgb(const Node& material, int mapAttr);
/** "auto", "sRGB" or "linear": the colorSpace enum's names. */
LILA_SCENE_API const char* colorSpaceName(material::TextureColorSpace c);
/**
 * Whether another app's colour space name means sRGB-encoded values (true) or linear ones (false): Maya's, Blender's,
 * OCIO's and MaterialX's ("sRGB", "Utility - sRGB - Texture", "Raw", "Non-Color", "scene-linear Rec.709-sRGB",
 * "ACEScg", "lin_rec709", "srgb_texture"). Empty when the name is unknown.
 */
LILA_SCENE_API std::optional<bool> srgbFromName(std::string_view name);
/** The colorSpace that reads a map as srgb says: auto when that is the map's own default. */
LILA_SCENE_API material::TextureColorSpace colorSpaceFor(int mapAttr, bool srgb);

/** The stroke sheets shipped in resources/strokes, by file stem; a material's strokeMap may name one instead of a path. */
LILA_SCENE_API std::vector<std::string> builtinStrokeSheets();
/** The stroke sheet file a strokeMap value means: the default sheet when empty, a built-in by stem, else resolvePath. */
LILA_SCENE_API std::string strokeSheetPath(const std::string& strokeMap);

/** paths::resolvePath of a stored texture path, as UTF-8. */
LILA_SCENE_API std::string resolvePath(const std::string& path);

/** The scene's materials in the Material Editor's order: by listOrder, then unplaced ones by name. */
LILA_SCENE_API std::vector<Node*> ordered(Graph& g);
/** Slot index for name, added when missing. */
LILA_SCENE_API int32_t slotIndex(geo::Mesh& mesh, const std::string& name);
/** Points every face (faces empty) or the given faces at the named slot, then drops slots no face uses. */
LILA_SCENE_API void assign(geo::Mesh& mesh, const std::string& slot, const std::vector<uint32_t>& faces);
/** Drops slots no face uses and renumbers the rest, keeping their order. */
LILA_SCENE_API void pruneSlots(geo::Mesh& mesh);
/** Slot name per face id; ids outside the names get "Material<id>", like the FBX exporter. */
LILA_SCENE_API std::string slotName(const geo::Mesh& mesh, int32_t id);
/**
 * Puts the named slots first, in the given order, the rest after them in their current order, and renumbers the
 * faces to match. Unknown names are ignored. False when nothing moved.
 */
LILA_SCENE_API bool reorderSlots(geo::Mesh& mesh, const std::vector<std::string>& order);
/** Faces per slot name, in slot order. */
LILA_SCENE_API std::vector<std::pair<std::string, uint32_t>> slotUsage(const geo::Mesh& mesh);

LILA_SCENE_API void registerTypes();
/** createMaterial, assignMaterial, listMaterials, materialInfo, setMaterialTexture, renameMaterial. */
LILA_SCENE_API void registerCommands();
} // namespace materials
} // namespace lila::scene
