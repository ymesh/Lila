#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

/**
 * The baker's data contract: the settings a bake project and its groups store (as versioned JSON on their nodes), and the
 * POD records the raster, projection and map kernels pass between them. TexelSample and HitRecord are plain 16-byte
 * aligned structs with no pointers so a GPU backend can upload them as they are (std430 layout).
 *
 * Pipeline, per group and low mesh and UDIM tile, a band of texel rows at a time:
 *
 *   TexelRaster (stratified samples on the low's UVs)
 *     -> Backend::traceProjection (one HitRecord per sample, against the group's own highs only)
 *     -> Backend::shade, once per map, reusing the same hits
 *     -> accumulate into the output texture the map's file name resolves to (several groups may share one)
 *   then per texture: resolve (average, exact vote or coverage), MapDef::finalize, Backend::dilate, write.
 */
namespace lila::bake
{
using json = nlohmann::json;

/**
 * Version written into every settings blob; readers upgrade older blobs in fromJson. 2: groups hold their meshes and
 * project by node, so renames follow (files still name them, as version 1 did).
 */
inline constexpr int kSettingsVersion = 2;

/** Where and how a project's textures are written. */
struct LILA_SCENE_API OutputSettings
{
    /** Relative paths are under the workspace, else the scene file's folder, else the current folder (resolveFolder). */
    std::string folder = "bake";
    /** File name tokens: {project} {group} {mesh} {material} {map} {udim} {res} {ext}. {ext} is appended when absent. */
    std::string name = "T_{[!M_]material}_{map}";
    /** png | exr | tga | tif | psd (one layered file per texture set: maps whose names differ only by {map} become its layers). */
    std::string format = "png";
    /** PNG, PSD: 8 or 16. TIFF: 8, 16 or 32 (float). EXR: 16 (half) or 32 (float). TGA: 8. Other values round to the nearest the format has. */
    int bits = 8;
    /** Triangular dither when writing 8 bit. Exact maps (IDs, islands) never dither. */
    bool dither = false;
    /** deflate | none: TIFF strips deflate, TGA run-length encodes. */
    std::string compression = "deflate";
    /** Deflate level for PNG, EXR and TIFF: 1 fastest to 9 smallest. Bakes favour speed; files are a little larger. */
    int compressionLevel = 2;
    /** How {udim} reads: 1001 (Mari, Unreal), uv (u1_v1, Mudbox) or uv0 (u0_v0). */
    std::string udimNaming = "1001";
    /** Write <texture>.json beside each texture: map, colour space, bits, decode data, the inputs' hash. */
    bool sidecar = false;
    int width = 2048;
    int height = 2048;
    /** Samples per texel: 1, 4, 16 or 64 (a stratified n x n grid). */
    int supersampling = 4;
    /** Texels to dilate past island borders; -1 fills the whole texture. */
    int padding = 16;
    /**
     * Per texture set ({material}: the material slot, or the mesh for a mesh without materials): overrides of width,
     * height, supersampling, padding, bits and format. Needs {material} in the name, so each set writes its own files.
     */
    std::map<std::string, json, std::less<>> materials;

    /** These settings with the material's overrides applied (and no per-material table). */
    OutputSettings forMaterial(std::string_view material) const;

    json toJson() const;
    /** Missing keys keep their defaults. Throws std::invalid_argument on a bad value. */
    static OutputSettings fromJson(const json& j);
};

/** The keys a per-material output override may set. */
inline constexpr const char* kMaterialOutputKeys[] = {"width", "height", "supersampling", "padding", "bits", "format"};

/** One map a project bakes. */
struct LILA_SCENE_API MapSettings
{
    /** A registered map id (see Maps.h): normal, objectNormal, position, height, id, alpha, uvIslands, ao, ... */
    std::string map;
    bool enabled = true;
    /** The {map} token; empty uses the map's default suffix. */
    std::string suffix;
    /** 0 uses the output's. */
    int bits = 0;
    /** Empty uses the output's. */
    std::string format;
    /**
     * Map specific, see each MapDef's doc. normal: greenUp (OpenGL), flipX. height: range (cm, 0 = auto). id: source
     * (material | object | group | vertexColor), palette {name: [r, g, b] or "#rrggbb"}. ao: rays, distance, scope.
     */
    json options = json::object();

    json toJson() const;
    static MapSettings fromJson(const json& j);
};

/**
 * A high-poly entry of a group: a mesh node, the group's lows themselves ("self as high"), or a mesh file (FBX, OBJ or an
 * Alembic frame) streamed in only while baking, so very dense highs never enter the scene.
 */
struct LILA_SCENE_API HighEntry
{
    /** Mesh node path; empty when self or a file. */
    std::string node;
    bool self = false;
    /** A mesh file: .fbx, .obj or .abc. Relative paths are under the workspace. */
    std::string file;
    /** Only the file's meshes whose name or path contains this; empty takes them all. */
    std::string object;
    /** Alembic: the frame to read (at the scene's fps); other formats ignore it. */
    double frame = 0.0;
    /**
     * Bake-only modifier stack, applied (in order, in object space) to the transient high snapshot and never to the scene:
     * [{"kind": "subdivision", "params": {"levels": 2}}]. Kinds are scene::modifiers::kinds().
     */
    json modifiers = json::array();

    json toJson() const;
    static HighEntry fromJson(const json& j);
};

/**
 * Meshes kept out of some of a group's lows' rays. A high or occluder matches by its node (a self high by its low), or by
 * a material any of its faces use: a high is skipped whole.
 */
struct LILA_SCENE_API Unlink
{
    /** The lows it applies to; empty is every low of the group. */
    std::vector<std::string> lows;
    /** The meshes those lows' rays ignore. */
    std::vector<std::string> meshes;
    /** Material slot names: every high and occluder with faces of one is ignored too. */
    std::vector<std::string> materials;
    bool empty() const { return meshes.empty() && materials.empty(); }
    /** Secondary rays (AO, thickness, bent normals, rounded edges) pass through them. */
    bool occlusion = true;
    /** Projection rays pass through them: they bake nothing onto these lows. */
    bool projection = false;

    json toJson() const;
    static Unlink fromJson(const json& j);
};

/** One bake group: lows that receive maps, the highs they are projected from, and the projection settings. */
struct LILA_SCENE_API GroupSettings
{
    std::vector<std::string> lows;
    std::vector<HighEntry> highs;
    /** Optional cage mesh with the low's topology (one low only); empty uses the auto cage. */
    std::string cage;
    /**
     * Ray start distance outside the low along its averaged normals, cm. 0 is auto: measured against the highs when they
     * are traced (resolveAutoDistances), else 3% of the low's bounds diagonal.
     */
    double frontal = 0.0;
    /** How far rays continue inside the low, cm; 0 is auto (measured like frontal, else the frontal distance). */
    double rear = 0.0;
    /** Scales the auto cage push (per group); a "bakeOffset" point attribute on the low scales it per point. */
    double cageOffset = 1.0;
    /**
     * 0 to 1: blends ray directions from the averaged smooth normals (0) toward each low face's geometric normal (1). A
     * "bakeSkew" point attribute on the low adds to it per point.
     */
    double skew = 0.0;
    /** Rays only hit high faces facing them. */
    bool ignoreBackfaces = true;
    /** Occluders for AO: self (the high that was hit), group (this group's highs) or all (every group's). */
    std::string aoScope = "group";
    std::vector<Unlink> unlinks;

    json toJson() const;
    static GroupSettings fromJson(const json& j);
};

/** A bake project's stored settings. Groups are separate bakeGroup nodes naming the project. */
struct LILA_SCENE_API ProjectSettings
{
    OutputSettings output;
    std::vector<MapSettings> maps;
    /** The target profile last applied (bakeForTarget), informational. */
    std::string target;
    /** Saving the scene rebakes the project when its inputs' hash changed since the last bake (see BakePipeline.h). */
    bool rebakeOnSave = false;
    /** Every bake wires its outputs into the lows' materials (bakeAssign). */
    bool autoAssign = false;
    /**
     * What new groups start with, from an applied target or preset (see Targets.h groupDefaults): frontal, rear,
     * cageOffset, skew, aoScope, ignoreBackfaces, modifiers (a stack for each new high) and self.
     */
    json groupDefaults = json::object();

    json toJson() const;
    static ProjectSettings fromJson(const json& j);
    /** The default map list: normal, height, id, ao. */
    static ProjectSettings defaults();
};

inline constexpr uint32_t kNoHit = 0xffffffffu;

/** Which of a low's rays a link applies to (GroupInputs::links). */
enum class RayKind : uint32_t
{
    Projection = 0,
    /** Secondary rays: AO, thickness, bent normals, rounded edges. */
    Occlusion = 1,
};

enum HitFlags : uint32_t
{
    kHitValid = 1,
    /** The nearest surface along the ray faced away (counted as a miss when backfaces are ignored). */
    kHitBackface = 2,
    /** The ray met the far side of a thin part (a surface facing away from the low) and was re-aimed along the face normal. */
    kHitReaimed = 4,
    /** No ray along the normal found the high; the nearest one facing the low's way, found by probes, was used. */
    kHitProbed = 8,
};

/** One stratified sample on a low's UVs, with the low's interpolated surface there. World (bake) space. */
struct alignas(16) TexelSample
{
    float position[3];
    /** Texel index y * width + x in the tile's texture. */
    uint32_t texel;
    /** Interpolated low shading normal (not normalised). */
    float normal[3];
    /** Triangle of the low's soup. */
    uint32_t lowTriangle;
    /** Interpolated MikkTSpace tangent (xyz, not normalised) and bitangent sign. */
    float tangent[4];
    /** Ray start point (cage point) and the inward ray direction (unit). */
    float origin[3];
    /** Ray length from the origin. */
    float rayLength;
    float direction[3];
    /** Sub-sample index within the texel, 0 to supersampling - 1. */
    uint32_t sub;
    float uv[2];
    uint32_t tile;
    /** Index of the low in its group; kNoHit applies no per-low links. */
    uint32_t low = 0xffffffffu;
};
static_assert(sizeof(TexelSample) == 96);

/** The projection result for one sample. high is kNoHit on a miss. */
struct alignas(16) HitRecord
{
    float position[3];
    /** Index of the high in its group's highs. */
    uint32_t high;
    /** Interpolated high shading normal (unit). */
    float normal[3];
    /** Triangle of the high's soup. */
    uint32_t triangle;
    /** Weights of the triangle's corners 1 and 2 (corner 0 is 1 - u - v). */
    float bary[2];
    /** Ray parameter from the sample's origin. */
    float distance;
    /** Signed distance of the hit from the low surface along the outward ray direction, cm. */
    float height;
    uint32_t flags;
    uint32_t pad[3];
};
static_assert(sizeof(HitRecord) == 64);

/** UDIM number of a tile: 1001 + u + 10 v. */
inline int udimOf(int tileU, int tileV) { return 1001 + tileU + 10 * tileV; }
} // namespace lila::bake
