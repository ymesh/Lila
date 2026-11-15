#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class Graph;
}

namespace lila::bake
{
/**
 * An immutable, unwelded triangle soup in bake (world) space: everything a backend needs from a mesh, flat and uploadable.
 * Per-corner arrays hold 3 entries per triangle, in winding order; front faces wind counter-clockwise.
 */
struct MeshSoup
{
    std::string name;
    std::vector<glm::vec3> positions;
    /** Shading (split) normals. */
    std::vector<glm::vec3> normals;
    /** uv0, empty when the mesh has none. */
    std::vector<glm::vec2> uvs;
    /** The first colour set, empty when the mesh has none. */
    std::vector<glm::vec4> colors;
    /** Every colour set's name in the mesh's order; the first one's values are colors. */
    std::vector<std::string> colorSetNames;
    /** Per corner values of colorSetNames[1...], each empty when unusable. */
    std::vector<std::vector<glm::vec4>> moreColors;
    /** Material slot per triangle, indexing materialNames. */
    std::vector<uint32_t> materials;
    std::vector<std::string> materialNames;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    uint32_t triangleCount() const { return uint32_t(materials.size()); }
};

/** A low: the soup plus what the raster and the auto cage need. */
struct LowInput
{
    MeshSoup soup;
    /** MikkTSpace tangents per corner (xyz, w bitangent sign), on the export triangulation. */
    std::vector<glm::vec4> tangents;
    /** Smooth point normals per corner (hard edges ignored): the auto cage pushes along these. */
    std::vector<glm::vec3> cageNormals;
    /** Per corner "bakeOffset" point attribute, 1 where absent. */
    std::vector<float> cageScale;
    /** Per corner "bakeSkew" point attribute, added to the group's skew; 0 where absent. */
    std::vector<float> cageSkew;
    /** Explicit cage positions per corner, empty for the auto cage. */
    std::vector<glm::vec3> cagePositions;
    /** Source polygon per triangle (triangles of one polygon share it). */
    std::vector<uint32_t> faces;
    /** Source point per corner. */
    std::vector<uint32_t> points;
    /** UV island per triangle, numbered from 0 in face order. */
    std::vector<uint32_t> islands;
    uint32_t islandCount = 0;
};

/** A high after its bake-only modifier stack. */
struct HighInput
{
    MeshSoup soup;
    /** The node it came from (the low's name for self). */
    std::string source;
    /** Triangles before the bake modifiers, for reporting. */
    uint32_t sourceTriangles = 0;
};

/** One group's snapshots and its resolved projection distances. */
struct GroupInputs
{
    std::string name;
    GroupSettings settings;
    std::vector<LowInput> lows;
    std::vector<HighInput> highs;
    /** Resolved frontal and rear distances (auto replaced), cm. */
    float frontal = 0.0f;
    float rear = 0.0f;
    /** The Auto distances were measured against the highs (resolveAutoDistances), not the 3% size fallback. */
    bool measured = false;
    glm::vec3 highMin{0.0f};
    glm::vec3 highMax{0.0f};
    /** Per high: the node it came from (the low's path for self, the file for file highs), and its low when self, else -1. */
    std::vector<std::string> highNodes;
    std::vector<int32_t> highSelfLow;
    /** Index of this group's first high among every group's highs (the order a backend numbers them, scope all). */
    uint32_t highBase = 0;
    /**
     * Highs each low's rays skip, as bits over every group's highs: linkWords words per low and RayKind, low-major. Empty
     * when nothing is skipped. Self highs skip the other lows' projection; unlinks add the rest (buildLinks).
     */
    std::vector<uint32_t> links;
    uint32_t linkWords = 0;
};

/** Whether the low's rays of this kind pass through the high (numbered over every group's highs). */
inline bool skips(const GroupInputs& gi, uint32_t low, uint32_t globalHigh, RayKind kind)
{
    if (gi.links.empty() || low == 0xffffffffu)
        return false;
    const size_t word = (size_t(low) * 2 + size_t(kind)) * gi.linkWords + globalHigh / 32;
    return word < gi.links.size() && (gi.links[word] >> (globalHigh % 32) & 1u) != 0;
}

/** A material's inputs as the bake saw them (scene/Materials.h conventions: linear colours, sRGB colour maps). */
struct MaterialInput
{
    /** A material node of this name exists; otherwise these are the renderer's neutral defaults. */
    bool found = false;
    glm::vec3 baseColor{0.5f};
    float metallic = 0.0f;
    float roughness = 0.55f;
    glm::vec3 emissive{0.0f};
    float emissiveIntensity = 1.0f;
    float opacity = 1.0f;
    /** 0 opaque, 1 masked, 2 translucent. */
    int blendMode = 0;
    bool normalOpenGL = false;
    /** 0 ORM, 1 MRO. */
    int packedLayout = 0;
    /** Resolved absolute texture paths, empty when unset. */
    std::string baseColorMap, metallicMap, roughnessMap, normalMap, ormMap, emissiveMap, occlusionMap;
    /** Whether each map's values are sRGB-encoded (its colorSpace, auto resolved). */
    bool baseColorSrgb = true, metallicSrgb = false, roughnessSrgb = false, ormSrgb = false, emissiveSrgb = true;
};

/** Every group of one bake. Immutable once built; backends and kernels only read it. */
struct BakeInputs
{
    std::string project;
    std::vector<GroupInputs> groups;
    /** Union of every group's highs, for position maps. */
    glm::vec3 highMin{0.0f};
    glm::vec3 highMax{0.0f};
    /** By slot name: every material the highs use. */
    std::map<std::string, MaterialInput, std::less<>> materials;
    std::vector<std::string> warnings;
    /** Seconds spent building these: modifiers (bake-only stacks), snapshot (soups); snapshotCacheHits counts reused meshes. */
    json timings = json::object();
};

/** The named material node's inputs, or found = false and defaults. Main thread. */
LILA_SCENE_API MaterialInput materialInput(scene::Graph& g, const std::string& slot);

/** A world-space soup of a mesh on its export triangulation. */
LILA_SCENE_API MeshSoup soupFromMesh(const geo::Mesh& mesh, const glm::dmat4& world, std::string name);

/** A low input: soup, tangents, cage normals and scale, islands; cage (same topology, already in world space) is optional. */
LILA_SCENE_API LowInput lowFromMesh(const geo::Mesh& mesh, const glm::dmat4& world, std::string name, const geo::Mesh* cage = nullptr,
                                    const glm::dmat4& cageWorld = glm::dmat4(1.0));

/**
 * The UDIM tile (u, v) each triangle bakes into: the tile holding its island's area-weighted UV centre, so a whole
 * island lands in one tile and a face nudged just past a tile edge doesn't start another.
 */
LILA_SCENE_API std::vector<glm::ivec2> triangleTiles(const LowInput& low);

/**
 * A bake-only modifier stack ([{"kind", "params"}]) instantiated on a private graph, never the scene's: nothing it does
 * reaches the scene graph, the file or undo. Build and apply on the main thread; applyPure runs anywhere. A subdivision
 * here blends its uvs unless its params name another way.
 */
class LILA_SCENE_API BakeModifierStack
{
public:
    /** Unknown kinds and bad params are skipped with a warning. */
    explicit BakeModifierStack(const json& stack);
    ~BakeModifierStack();
    BakeModifierStack(const BakeModifierStack&) = delete;
    BakeModifierStack& operator=(const BakeModifierStack&) = delete;

    bool empty() const { return mNodes.empty(); }
    /** The mesh through every modifier in order. A modifier that throws passes its input through, with a warning. */
    geo::Mesh apply(const geo::Mesh& mesh);
    /** Every modifier is a plain function of its input mesh and settings, so applyPure may run on any thread, several at once. */
    bool pure() const;
    /** apply() for a pure() stack, touching nothing shared: warnings go to the caller's list. */
    geo::Mesh applyPure(const geo::Mesh& mesh, std::vector<std::string>& warnings) const;
    const std::vector<std::string>& warnings() const { return mWarnings; }

private:
    std::unique_ptr<scene::Graph> mGraph;
    std::vector<scene::NodeId> mNodes;
    std::vector<std::string> mWarnings;
};

/** Built-in bake modifier presets: "smooth" / "Smooth x2" (subdivision 2), "smooth1".."smooth4", "autoBevelSmooth" / "Auto Bevel + Smooth". Null when unknown. */
LILA_SCENE_API json modifierPreset(std::string_view name);

/**
 * Snapshots the given bakeGroup nodes (all of the project's when empty): lows as exported (their export-enabled modifiers),
 * highs through their bake-only stacks, all in world space. withHighs false skips the highs (dry runs). Main thread;
 * the per-mesh conversions run in parallel. Throws std::runtime_error for a group naming a missing mesh.
 */
LILA_SCENE_API BakeInputs buildInputs(scene::Graph& g, scene::NodeId project, const std::vector<scene::NodeId>& groups, bool withHighs = true);

/** Fills every group's highBase and links from its self highs and unlinks (buildInputs calls it). */
LILA_SCENE_API void buildLinks(BakeInputs& in);
} // namespace lila::bake
